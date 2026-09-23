// Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
//
// local_access — access to a Fountainer via the C++ API of the
// fountainer_client_framework: directly on the device's local WSS server
// (port 4443, mTLS, HMAC-signed control), without a cloud server.
//
//   ./local_access <config.json> [on|off|auto|manual]
//
// The JSON config is the framework's format (device/tls/fountain), see
// client.fnt-000003.json next to it. The optional state is sent as a signed
// set_state command; without an argument only reads/writes are performed.
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

#include <fountainer/client.hpp>
#include <fountainer/config.hpp>
#include <fountainer/datapoints/generated.hpp>
#include <fountainer/logging/logger.hpp>

using namespace fountainer;
using namespace std::chrono_literals;

namespace {

// Print an arbitrary datapoint value (variant).
std::string to_text(const DatapointValue& v)
{
    return std::visit([](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) return x;
        else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else return std::to_string(+x);        // +x: uint8_t as a number, not as a character
    }, v);
}

// Config file -> client: mTLS against the testbed CA (device IP is DHCP,
// hence chain verification without hostname), HMAC key from file, expected identity.
Result<Client> make_client(const Config& cfg)
{
    auto hmac = HmacCredentials::from_file(cfg.fountain.kid, cfg.fountain.hmac_key_file);
    if (!hmac) return fail(hmac.error());
    return Client::builder(Endpoint{cfg.device.host, cfg.device.port, cfg.device.path,
                                    cfg.device.subprotocol})
        .with_tls(TlsCredentials::mutual_tls(cfg.tls.ca_file, cfg.tls.client_cert_file,
                                             cfg.tls.client_key_file,
                                             EndpointIdentityPolicy::VerifyCertificateChainOnly))
        .with_hmac(std::move(*hmac))
        .with_expected_device(cfg.fountain.device_id)
        .with_reconnect({.enabled = false})
        .build();
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <config.json> [on|off|auto|manual]\n";
        return 2;
    }
    log::set_level("warn");                    // keep library logging quiet

    Config cfg;
    try {
        cfg = load_config(argv[1]);
    } catch (const std::exception& ex) {
        std::cerr << "Config: " << ex.what() << '\n';
        return 2;
    }

    auto built = make_client(cfg);
    if (!built) { std::cerr << built.error().to_string() << '\n'; return 1; }
    Client client = std::move(*built);

    // 1) Connect: success means Fountain session RUNNING (handshake,
    //    device's session proof verified, ota_none sent).
    auto conn = client.connect();
    if (!conn) { std::cerr << "connect: " << conn.error().to_string() << '\n'; return 1; }
    std::cout << "connected: " << conn->device_id << " fw=" << conn->firmware_version << '\n';

    // 2) Typed single read (Result<float>) ...
    if (auto p = client.datapoints().read(dp::Fon_Current_Pressure))
        std::cout << "Fon_Current_Pressure = " << *p << " bar\n";
    else
        std::cerr << "read: " << p.error().to_string() << '\n';

    // ... and a multi-read as a snapshot (one dp_read message).
    auto snap = client.datapoints().read(dp::Fon_Current_State, dp::Fon_Relay_Output,
                                         dp::Fon_Fault_Code, dp::System_Uptime);
    if (snap) {
        for (const auto& [name, value] : snap->values())
            std::cout << "  " << name << " = " << to_text(value) << '\n';
    }

    // 3) Dynamic read by name (e.g. from a configuration).
    if (auto v = client.datapoints().read("Can_State"))
        std::cout << "  Can_State = " << to_text(*v) << '\n';

    // 4) Signed write. An RO datapoint would be a compile error; a
    //    rejection by the firmware is a result (applied()==false).
    auto wr = client.datapoints().write(dp::Fon_Event_Label, std::uint8_t{2});
    if (!wr) {
        std::cerr << "write: " << wr.error().to_string() << '\n';
    } else if (!wr->applied()) {
        for (const auto& e : wr->errors) std::cerr << "  rejected " << e.datapoint << ": " << e.reason << '\n';
    } else {
        std::cout << "Fon_Event_Label written, readback ok\n";
    }

    // 5) Control the pump (signed command set_state).
    if (argc > 2) {
        std::string s = argv[2];
        FountainState st = s == "on" ? FountainState::On : s == "off" ? FountainState::Off :
                           s == "auto" ? FountainState::Auto : FountainState::Manual;
        auto cr = client.commands().set_state(st);
        if (!cr) std::cerr << "command: " << cr.error().to_string() << '\n';
        else std::cout << "set_state " << to_string(st) << " -> "
                       << (cr->applied() ? "applied" : "rejected: " + cr->error.value_or("")) << '\n';
    }

    // 6) Observe: subscription to changes + poll scheduler (1 s).
    auto sub = client.datapoints().subscribe(dp::Fon_Relay_Output,
        [](const DatapointChange<bool>& c) {
            std::cout << "  Relay -> " << (c.value ? "ON" : "OFF") << '\n';
        });
    client.polling().every(1s, dp::Fon_Relay_Output, dp::Fon_Current_State);
    client.polling().start();
    std::this_thread::sleep_for(5s);
    client.polling().stop();

    (void)client.disconnect();
    return 0;
}
