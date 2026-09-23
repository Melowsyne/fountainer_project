// Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
//
// stress_local — sustained load on the local WSS path (fountainer_client_framework)
// for the stress test. Reads at a rate just below the firmware's rate limit
// (5 frames/s), writes a config datapoint every 5 s, switches the
// output according to the schedule and logs everything as JSONL (epoch ms).
//
//   ./stress_local <config.json> <seconds> [--jsonl FILE] [--toggle T:on,T:off,...]
//                  [--rate-ms 300] [--churn N]
//
// --churn N: instead of sustained load, N times connect -> read -> disconnect (slot
// release and handshake under load), also logged.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <fountainer/client.hpp>
#include <fountainer/config.hpp>
#include <fountainer/datapoints/generated.hpp>
#include <fountainer/logging/logger.hpp>

using namespace fountainer;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::int64_t epoch_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

double as_double(const DatapointValue& v)
{
    return std::visit([](const auto& x) -> double {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) return 0.0;
        else return static_cast<double>(x);
    }, v);
}

struct Toggle { double at_s; std::string action; };

std::vector<Toggle> parse_toggles(const std::string& spec)
{
    std::vector<Toggle> out;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        auto comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        auto colon = item.find(':');
        if (colon != std::string::npos) out.push_back({std::stod(item.substr(0, colon)), item.substr(colon + 1)});
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

std::string stats(std::vector<double> v)
{
    if (v.empty()) return "{}";
    std::sort(v.begin(), v.end());
    double sum = 0; for (double x : v) sum += x;
    char b[160];
    std::snprintf(b, sizeof b, "{\"min_ms\":%.1f,\"avg_ms\":%.1f,\"p95_ms\":%.1f,\"max_ms\":%.1f}",
                  v.front(), sum / v.size(), v[static_cast<std::size_t>(v.size() * 0.95) - (v.size() > 1 ? 1 : 0)], v.back());
    return b;
}

class Log {
public:
    explicit Log(const std::string& path) { if (!path.empty()) out_.open(path, std::ios::app); }
    void line(const std::string& s) { std::lock_guard<std::mutex> g(m_); if (out_) out_ << s << '\n'; }
private:
    std::ofstream out_;
    std::mutex m_;
};

Result<Client> make_client(const Config& cfg, bool reconnect)
{
    auto hmac = HmacCredentials::from_file(cfg.fountain.kid, cfg.fountain.hmac_key_file);
    if (!hmac) return fail(hmac.error());
    return Client::builder(Endpoint{cfg.device.host, cfg.device.port, cfg.device.path, cfg.device.subprotocol})
        .with_tls(TlsCredentials::mutual_tls(cfg.tls.ca_file, cfg.tls.client_cert_file, cfg.tls.client_key_file,
                                             EndpointIdentityPolicy::VerifyCertificateChainOnly))
        .with_hmac(std::move(*hmac))
        .with_expected_device(cfg.fountain.device_id)
        .with_reconnect({.enabled = reconnect, .initial_delay = 1s, .max_delay = 5s})
        .build();
}

int run_churn(const Config& cfg, int n, Log& log)
{
    int ok = 0, failed = 0;
    std::vector<double> lat;
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        auto built = make_client(cfg, false);
        if (!built) { std::cerr << built.error().to_string() << '\n'; return 1; }
        Client client = std::move(*built);
        auto conn = client.connect();
        std::string err;
        if (conn) {
            auto r = client.datapoints().read(dp::Fon_Relay_Output);
            if (!r) err = r.error().to_string();
        } else {
            err = conn.error().to_string();
        }
        (void)client.disconnect();
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (err.empty()) { ++ok; lat.push_back(ms); } else ++failed;
        char b[400];
        std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"cpp\",\"op\":\"churn\",\"i\":%d,\"ok\":%s,\"ms\":%.0f,\"error\":\"%s\"}",
                      static_cast<long long>(epoch_ms()), i, err.empty() ? "true" : "false", ms, err.c_str());
        log.line(b);
        std::printf("[churn %2d] %s %.0f ms %s\n", i, err.empty() ? "ok " : "ERR", ms, err.c_str());
        std::this_thread::sleep_for(500ms);        // wait for the firmware's slot release
    }
    std::printf("{\"path\":\"cpp\",\"mode\":\"churn\",\"n\":%d,\"ok\":%d,\"failed\":%d,\"cycle\":%s}\n",
                n, ok, failed, stats(lat).c_str());
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <config.json> <seconds> [--jsonl F] [--toggle SPEC] [--rate-ms N] [--churn N]\n";
        return 2;
    }
    log::set_level("warn");
    double seconds = std::stod(argv[2]);
    std::string jsonl, toggle_spec;
    int rate_ms = 300, churn = 0;
    for (int i = 3; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--jsonl") jsonl = argv[++i];
        else if (a == "--toggle") toggle_spec = argv[++i];
        else if (a == "--rate-ms") rate_ms = std::stoi(argv[++i]);
        else if (a == "--churn") churn = std::stoi(argv[++i]);
    }
    Config cfg;
    try { cfg = load_config(argv[1]); } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 2; }
    Log log(jsonl);
    if (churn > 0) return run_churn(cfg, churn, log);

    auto built = make_client(cfg, true);
    if (!built) { std::cerr << built.error().to_string() << '\n'; return 1; }
    Client client = std::move(*built);

    int disconnects = 0, reconnects = 0;
    auto sub = client.events().on_connection_state([&](const ConnectionStateChange& c) {
        char b[300];
        std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"cpp\",\"op\":\"conn\",\"state\":%d,\"cause\":\"%s\"}",
                      static_cast<long long>(epoch_ms()), static_cast<int>(c.current),
                      c.cause ? c.cause->to_string().c_str() : "");
        log.line(b);
        if (c.current == ClientState::Ready) ++reconnects;
        else if (c.cause) ++disconnects;
    });

    auto conn = client.connect();
    if (!conn) { std::cerr << "connect: " << conn.error().to_string() << '\n'; return 1; }
    reconnects = 0;                                  // the first connection does not count
    std::printf("connected: %s fw=%s\n", conn->device_id.c_str(), conn->firmware_version.c_str());

    auto toggles = parse_toggles(toggle_spec);
    std::size_t next_toggle = 0;
    std::vector<double> lat, wlat;
    unsigned long reads = 0, writes = 0, errors = 0;
    auto t_start = Clock::now();
    auto last_write = t_start;
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t_start).count(); };

    while (elapsed() < seconds) {
        auto tick = Clock::now();
        // ---- Read (one dp_read with 5 names) ----
        auto t0 = Clock::now();
        auto snap = client.datapoints().read(dp::Fon_Relay_Output, dp::Fon_Current_State, dp::System_Uptime,
                                             dp::System_Min_Memory_Free, dp::System_Largest_Free_Block);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        char b[400];
        if (snap) {
            ++reads; lat.push_back(ms);
            auto g = [&](const char* n) { auto p = snap->find(n); return p ? as_double(*p) : -1.0; };
            std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"cpp\",\"op\":\"read\",\"ms\":%.0f,\"relay\":%g,\"state\":%g,"
                          "\"uptime\":%g,\"min_free\":%g,\"largest\":%g}",
                          static_cast<long long>(epoch_ms()), ms, g("Fon_Relay_Output"), g("Fon_Current_State"),
                          g("System_Uptime"), g("System_Min_Memory_Free"), g("System_Largest_Free_Block"));
        } else {
            ++errors;
            std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"cpp\",\"op\":\"read\",\"ms\":%.0f,\"error\":\"%s\"}",
                          static_cast<long long>(epoch_ms()), ms, snap.error().to_string().c_str());
        }
        log.line(b);

        // ---- Write every 5 s (signed, firmware NVS path) ----
        if (Clock::now() - last_write > 5s) {
            last_write = Clock::now();
            auto w0 = Clock::now();
            auto wr = client.datapoints().write(dp::Fon_Event_Label, static_cast<std::uint8_t>(writes % 8));
            double wms = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
            if (wr && wr->applied()) { ++writes; wlat.push_back(wms); }
            else {
                ++errors;
                std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"cpp\",\"op\":\"write\",\"error\":\"%s\"}",
                              static_cast<long long>(epoch_ms()), wr ? wr->status.c_str() : wr.error().to_string().c_str());
                log.line(b);
            }
        }

        // ---- Switch the output according to the schedule (signed command) ----
        if (next_toggle < toggles.size() && elapsed() >= toggles[next_toggle].at_s) {
            const Toggle& tg = toggles[next_toggle++];
            FountainState st = tg.action == "on" ? FountainState::On : tg.action == "off" ? FountainState::Off :
                               tg.action == "auto" ? FountainState::Auto : FountainState::Manual;
            auto cr = client.commands().set_state(st);
            std::string status = !cr ? cr.error().to_string() : cr->applied() ? "applied" : "rejected:" + cr->error.value_or("");
            if (status != "applied") ++errors;
            std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"cpp\",\"op\":\"toggle\",\"action\":\"%s\",\"status\":\"%s\"}",
                          static_cast<long long>(epoch_ms()), tg.action.c_str(), status.c_str());
            log.line(b);
            std::printf("[%6.1fs] toggle %s -> %s\n", elapsed(), tg.action.c_str(), status.c_str());
        }
        std::this_thread::sleep_until(tick + std::chrono::milliseconds(rate_ms));
    }

    std::printf("{\"path\":\"cpp\",\"seconds\":%.0f,\"reads\":%lu,\"reads_per_s\":%.2f,\"writes\":%lu,\"errors\":%lu,"
                "\"disconnects\":%d,\"reconnects\":%d,\"read_latency\":%s,\"write_latency\":%s}\n",
                seconds, reads, reads / seconds, writes, errors, disconnects, reconnects,
                stats(lat).c_str(), stats(wlat).c_str());
    (void)client.disconnect();
    return errors ? 1 : 0;
}
