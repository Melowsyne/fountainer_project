// Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
//
// canopen_sdo — CANopen access to the Fountainer slave (CiA 301) with raw
// SocketCAN, without any third-party library. Shows how a master reads,
// writes and controls the device:
//
//   - learn the object dictionary from the device itself (descriptor records
//     0x3000+i: name / type code / access) -> no EDS needed
//   - SDO upload (expedited + segmented), SDO download (expedited)
//   - NMT commands, heartbeat, TPDO1 decoding, EMCY
//   - bench: sustained load for the stress test (reads round-robin, writes,
//     switching the output via Fon_Pump_Command, JSONL log)
//
//   g++ -O2 -std=c++17 -o canopen_sdo canopen_sdo.cpp
//   ./canopen_sdo [--if can0] [--node 3] [--od od_cache.txt] COMMAND ...
//
//   info                         identity 0x1018, name, SW version, heartbeat
//   list                         datapoint catalog (index, type, access)
//   read NAME...                 SDO upload, typed
//   write NAME VALUE             SDO download (slave validates: range, NVS)
//   pump on|off|auto|manual|restart    Fon_Pump_Command = 1..5
//   nmt start|stop|preop|reset|reset-comm
//   monitor SEC                  listen to heartbeat, TPDO1..4 and EMCY
//   bench SEC [--jsonl FILE] [--toggle T:on,T:off,...]   stress load
//   demo                         concrete examples: pressure sensor, CPU temperature,
//                                system values, writing config, listening to TPDO1
//
// Object dictionary (firmware DOKU/CANopen.md): datapoint i = 0x2000+i sub0,
// count 0x2FFF, descriptor 0x3000+i (sub1 name, sub2 type code, sub3 access).
// Type codes (dp_type_t): 0 BOOL 1 U8 2 U16 3 U32 4 U64 5 I8 6 I16 8 F32 9 ENUM 10 STR.
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t epoch_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- Object dictionary --------------------------------------------------------
enum TypeCode { T_BOOL = 0, T_U8 = 1, T_U16 = 2, T_U32 = 3, T_U64 = 4, T_I8 = 5,
                T_I16 = 6, T_F32 = 8, T_ENUM = 9, T_STR = 10 };

struct Entry {
    std::string name;
    std::uint16_t index = 0;
    int type = 0;
    int access = 0;              // 0 RO, 1 RW, 2 WO
};

const char* type_name(int t)
{
    switch (t) {
    case T_BOOL: return "BOOL"; case T_U8: return "U8"; case T_U16: return "U16";
    case T_U32: return "U32"; case T_U64: return "U64"; case T_I8: return "I8";
    case T_I16: return "I16"; case T_F32: return "F32"; case T_ENUM: return "ENUM";
    case T_STR: return "STR"; default: return "?";
    }
}
const char* access_name(int a) { return a == 0 ? "RO" : a == 1 ? "RW" : "WO"; }

std::size_t type_size(int t)
{
    switch (t) {
    case T_BOOL: case T_U8: case T_I8: case T_ENUM: return 1;
    case T_U16: case T_I16: return 2;
    case T_U32: case T_F32: return 4;
    case T_U64: return 8;
    default: return 0;           // STR: variable
    }
}

// ---- SDO errors ----------------------------------------------------------------
struct SdoError : std::runtime_error {
    std::uint32_t abort;
    SdoError(std::uint32_t code, const std::string& what)
        : std::runtime_error(what), abort(code) {}
};

const char* abort_text(std::uint32_t code)
{
    switch (code) {
    case 0x05040000: return "SDO protocol timeout";
    case 0x06010001: return "write-only object (read refused)";
    case 0x06010002: return "read-only object (write refused)";
    case 0x06020000: return "object does not exist";
    case 0x06040043: return "cross-field constraint violated";
    case 0x06070010: return "data length mismatch";
    case 0x06070012: return "data too long";
    case 0x06070013: return "data too short";
    case 0x06090011: return "sub-index does not exist";
    case 0x06090030: return "value out of range";
    case 0x08000000: return "general error";
    default: return "";
    }
}

// ---- CANopen master via SocketCAN --------------------------------------------------
class CanopenMaster {
public:
    // Asynchronous frames (TPDO, heartbeat, EMCY) are passed through to this
    // handler while waiting for SDO responses.
    std::function<void(const can_frame&)> on_async;

    CanopenMaster(const std::string& ifname, int node) : node_(node)
    {
        fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (fd_ < 0) throw std::runtime_error("socket(PF_CAN) failed");
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
        if (ioctl(fd_, SIOCGIFINDEX, &ifr) < 0)
            throw std::runtime_error("Interface " + ifname + " not found");
        sockaddr_can addr{};
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("bind(can) failed");
        // Only frames of this node: SDO response, TPDO1-4, heartbeat, EMCY
        std::vector<can_filter> f;
        for (std::uint32_t base : {0x580u, 0x180u, 0x280u, 0x380u, 0x480u, 0x700u, 0x080u})
            f.push_back({base + static_cast<std::uint32_t>(node_), CAN_SFF_MASK});
        setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, f.data(),
                   static_cast<socklen_t>(f.size() * sizeof(can_filter)));
    }
    ~CanopenMaster() { if (fd_ >= 0) close(fd_); }

    int node() const { return node_; }

    // ---- Raw send / receive --------------------------------------------------------
    void send(std::uint32_t id, const std::uint8_t* data, std::uint8_t len)
    {
        can_frame fr{};
        fr.can_id = id;
        fr.can_dlc = len;
        std::memcpy(fr.data, data, len);
        if (::write(fd_, &fr, sizeof(fr)) != static_cast<ssize_t>(sizeof(fr)))
            throw std::runtime_error("CAN write failed (bus offline?)");
        ++tx_frames;
    }

    // Waits up to timeout_ms for a frame; asynchronous frames are dispatched.
    bool recv(can_frame& fr, int timeout_ms)
    {
        pollfd p{fd_, POLLIN, 0};
        int r = poll(&p, 1, timeout_ms);
        if (r <= 0) return false;
        if (::read(fd_, &fr, sizeof(fr)) != static_cast<ssize_t>(sizeof(fr))) return false;
        ++rx_frames;
        return true;
    }

    // Processes all pending asynchronous frames (non-blocking).
    void pump(int timeout_ms = 0)
    {
        can_frame fr{};
        while (recv(fr, timeout_ms)) {
            dispatch_async(fr);
            timeout_ms = 0;
        }
    }

    // ---- SDO (Service Data Objects, CiA 301 §7.2.4) ------------------------------------
    // CANopen names the direction from the SLAVE's point of view: "upload" = the
    // slave uploads data to the master (master READS), "download" = the master
    // writes into the slave. The master sends requests on 0x600+node and
    // receives responses on 0x580+node; every access addresses
    // index:subindex of the object dictionary. "transmit/receive" is the
    // level of the CAN frames (SocketCAN write/read); for PDOs: TPDO =
    // transmit PDO of the slave (slave sends cyclically/on change), RPDO =
    // receive PDO (slave receives process data from the master).
    std::vector<std::uint8_t> sdo_upload(std::uint16_t index, std::uint8_t sub)
    {
        std::uint8_t req[8] = {0x40, static_cast<std::uint8_t>(index & 0xFF),
                               static_cast<std::uint8_t>(index >> 8), sub, 0, 0, 0, 0};
        send(0x600 + node_, req, 8);
        can_frame fr = wait_sdo(index, sub);
        std::uint8_t cs = fr.data[0];
        if ((cs & 0xE0) == 0x40) {                     // response to upload init
            if (cs & 0x02) {                            // expedited
                std::size_t n = (cs & 0x01) ? 4 - ((cs >> 2) & 0x03) : 4;
                return {fr.data + 4, fr.data + 4 + n};
            }
            // segmented: size in bytes 4..7, then fetch the segments
            std::uint32_t total = fr.data[4] | (fr.data[5] << 8) | (fr.data[6] << 16) |
                                  (static_cast<std::uint32_t>(fr.data[7]) << 24);
            std::vector<std::uint8_t> out;
            std::uint8_t toggle = 0;
            for (;;) {
                std::uint8_t seg[8] = {static_cast<std::uint8_t>(0x60 | toggle), 0, 0, 0, 0, 0, 0, 0};
                send(0x600 + node_, seg, 8);
                can_frame sf = wait_sdo_segment();
                std::uint8_t scs = sf.data[0];
                if ((scs & 0x10) != toggle)
                    throw SdoError(0x05030000, "SDO toggle bit mismatch");
                std::size_t n = 7 - ((scs >> 1) & 0x07);
                out.insert(out.end(), sf.data + 1, sf.data + 1 + n);
                if (scs & 0x01) break;                  // last segment
                toggle ^= 0x10;
            }
            if (total && out.size() > total) out.resize(total);
            return out;
        }
        throw SdoError(0x08000000, "unexpected SDO response");
    }

    void sdo_download(std::uint16_t index, std::uint8_t sub, const std::uint8_t* data,
                      std::size_t n)
    {
        if (n < 1 || n > 4) throw std::runtime_error("expedited SDO download: 1..4 Bytes");
        std::uint8_t req[8] = {static_cast<std::uint8_t>(0x23 | ((4 - n) << 2)),
                               static_cast<std::uint8_t>(index & 0xFF),
                               static_cast<std::uint8_t>(index >> 8), sub, 0, 0, 0, 0};
        std::memcpy(req + 4, data, n);
        send(0x600 + node_, req, 8);
        can_frame fr = wait_sdo(index, sub);
        if (fr.data[0] != 0x60) throw SdoError(0x08000000, "unexpected SDO response");
    }

    // ---- NMT --------------------------------------------------------------------------
    void nmt(std::uint8_t cs)
    {
        std::uint8_t d[2] = {cs, static_cast<std::uint8_t>(node_)};
        send(0x000, d, 2);
    }

    // ---- Object dictionary ---------------------------------------------------------------
    // Learns the catalog from the device (0x2FFF + descriptor records) or loads it
    // from the cache file; the cache is written after discovery.
    void load_od(const std::string& cache)
    {
        if (!cache.empty()) {
            std::ifstream in(cache);
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ss(line);
                Entry e;
                if (ss >> e.name >> e.index >> e.type >> e.access) od_[e.name] = e;
            }
            if (!od_.empty()) return;
        }
        auto cnt = sdo_upload(0x2FFF, 0);
        int n = cnt[0] | (cnt[1] << 8);
        for (int i = 0; i < n; ++i) {
            Entry e;
            auto name = sdo_upload(0x3000 + i, 1);
            e.name.assign(name.begin(), name.end());
            e.index = static_cast<std::uint16_t>(0x2000 + i);
            e.type = sdo_upload(0x3000 + i, 2)[0];
            e.access = sdo_upload(0x3000 + i, 3)[0];
            od_[e.name] = e;
        }
        if (!cache.empty()) {
            std::ofstream out(cache);
            for (auto& [k, e] : od_) out << e.name << ' ' << e.index << ' ' << e.type << ' ' << e.access << '\n';
        }
    }

    const Entry& entry(const std::string& name) const
    {
        auto it = od_.find(name);
        if (it == od_.end()) throw std::runtime_error("unknown datapoint: " + name);
        return it->second;
    }
    const std::map<std::string, Entry>& od() const { return od_; }

    // ---- Typed access -------------------------------------------------------------------------
    std::string read(const std::string& name)
    {
        const Entry& e = entry(name);
        return decode(e.type, sdo_upload(e.index, 0));
    }

    double read_number(const std::string& name)
    {
        const Entry& e = entry(name);
        auto raw = sdo_upload(e.index, 0);
        return to_number(e.type, raw);
    }

    // Typed readers for the most common cases (the type is checked against the
    // catalog so that e.g. an F32 is not interpreted as U32).
    float read_f32(const std::string& name)
    {
        const Entry& e = entry(name);
        if (e.type != T_F32) throw std::runtime_error(name + " is not F32");
        float f; auto raw = sdo_upload(e.index, 0); std::memcpy(&f, raw.data(), 4); return f;
    }
    std::uint32_t read_u32(const std::string& name)
    {
        const Entry& e = entry(name);
        if (e.type != T_U32 && e.type != T_U16 && e.type != T_U8 && e.type != T_BOOL && e.type != T_ENUM)
            throw std::runtime_error(name + " is not an unsigned integer type");
        return static_cast<std::uint32_t>(to_u64(sdo_upload(e.index, 0), type_size(e.type)));
    }
    std::int32_t read_i32(const std::string& name)
    {
        const Entry& e = entry(name);
        auto raw = sdo_upload(e.index, 0);
        if (e.type == T_I8)  return static_cast<std::int8_t>(raw[0]);
        if (e.type == T_I16) return static_cast<std::int16_t>(to_u64(raw, 2));
        throw std::runtime_error(name + " is not a signed type");
    }
    std::string read_str(const std::string& name)
    {
        const Entry& e = entry(name);
        if (e.type != T_STR) throw std::runtime_error(name + " is not a string");
        auto raw = sdo_upload(e.index, 0); return std::string(raw.begin(), raw.end());
    }

    void write(const std::string& name, const std::string& text)
    {
        const Entry& e = entry(name);
        std::uint8_t buf[8] = {0};
        std::size_t n = type_size(e.type);
        if (e.type == T_STR || n == 0 || n > 4)
            throw std::runtime_error("write: only numeric types up to 4 bytes (" + name + ")");
        if (e.type == T_F32) {
            float f = std::stof(text);
            std::memcpy(buf, &f, 4);
        } else {
            long long v = (text == "true") ? 1 : (text == "false") ? 0 : std::stoll(text);
            for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
        sdo_download(e.index, 0, buf, n);
    }

    // Little-endian raw bytes -> integer (U64 NOT via double: > 2^53 rounds)
    static std::uint64_t to_u64(const std::vector<std::uint8_t>& d, std::size_t n)
    {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n && i < d.size(); ++i) v |= static_cast<std::uint64_t>(d[i]) << (8 * i);
        return v;
    }

    static double to_number(int type, const std::vector<std::uint8_t>& d)
    {
        auto u = [&](std::size_t n) { return to_u64(d, n); };
        switch (type) {
        case T_BOOL: case T_U8: case T_ENUM: return static_cast<double>(u(1));
        case T_U16: return static_cast<double>(u(2));
        case T_U32: return static_cast<double>(u(4));
        case T_U64: return static_cast<double>(u(8));
        case T_I8: return static_cast<double>(static_cast<std::int8_t>(u(1)));
        case T_I16: return static_cast<double>(static_cast<std::int16_t>(u(2)));
        case T_F32: { float f; std::uint32_t r = static_cast<std::uint32_t>(u(4)); std::memcpy(&f, &r, 4); return f; }
        default: return 0;
        }
    }

    static std::string decode(int type, const std::vector<std::uint8_t>& d)
    {
        char buf[64];
        switch (type) {
        case T_STR: return std::string(d.begin(), d.end());
        case T_U64: std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(to_u64(d, 8))); return buf;
        case T_F32: std::snprintf(buf, sizeof buf, "%.3f", to_number(type, d)); return buf;
        case T_BOOL: return to_number(type, d) != 0 ? "1" : "0";
        default: std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(to_number(type, d))); return buf;
        }
    }

    unsigned long tx_frames = 0, rx_frames = 0;

private:
    can_frame wait_sdo(std::uint16_t index, std::uint8_t sub)
    {
        auto deadline = Clock::now() + std::chrono::milliseconds(500);
        can_frame fr{};
        while (Clock::now() < deadline) {
            int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (!recv(fr, std::max(left, 1))) continue;
            if (fr.can_id != static_cast<canid_t>(0x580 + node_)) { dispatch_async(fr); continue; }
            std::uint16_t ri = fr.data[1] | (fr.data[2] << 8);
            if (fr.data[0] == 0x80) {
                std::uint32_t code = fr.data[4] | (fr.data[5] << 8) | (fr.data[6] << 16) |
                                     (static_cast<std::uint32_t>(fr.data[7]) << 24);
                char msg[96];
                std::snprintf(msg, sizeof msg, "SDO abort 0x%08X %s (0x%04X:%u)", code, abort_text(code), ri, fr.data[3]);
                throw SdoError(code, msg);
            }
            if (ri != index || fr.data[3] != sub) continue;   // stale response
            return fr;
        }
        throw SdoError(0x05040000, "SDO timeout (no response in 500 ms)");
    }

    can_frame wait_sdo_segment()
    {
        auto deadline = Clock::now() + std::chrono::milliseconds(500);
        can_frame fr{};
        while (Clock::now() < deadline) {
            if (!recv(fr, 50)) continue;
            if (fr.can_id != static_cast<canid_t>(0x580 + node_)) { dispatch_async(fr); continue; }
            if (fr.data[0] == 0x80) throw SdoError(0x08000000, "SDO abort in segment");
            return fr;
        }
        throw SdoError(0x05040000, "SDO segment timeout");
    }

    void dispatch_async(const can_frame& fr) { if (on_async) on_async(fr); }

    int fd_ = -1;
    int node_;
    std::map<std::string, Entry> od_;
};

// ---- Decoding of asynchronous frames ---------------------------------------------------------
struct Tpdo1 { float pressure; std::uint8_t state, relay, fault; };

bool decode_tpdo1(const can_frame& fr, int node, Tpdo1& out)
{
    if (fr.can_id != static_cast<canid_t>(0x180 + node) || fr.can_dlc < 7) return false;
    std::memcpy(&out.pressure, fr.data, 4);
    out.state = fr.data[4]; out.relay = fr.data[5]; out.fault = fr.data[6];
    return true;
}

const char* nmt_name(std::uint8_t s)
{
    return s == 0 ? "BOOT-UP" : s == 4 ? "STOPPED" : s == 5 ? "OPERATIONAL" : s == 127 ? "PRE-OPERATIONAL" : "?";
}

void print_async(const can_frame& fr, int node)
{
    Tpdo1 t{};
    if (decode_tpdo1(fr, node, t)) {
        std::printf("TPDO1  pressure=%.2f bar state=%u relay=%u fault=%u\n", t.pressure, t.state, t.relay, t.fault);
    } else if (fr.can_id == static_cast<canid_t>(0x700 + node)) {
        std::printf("HB     %s\n", nmt_name(fr.data[0]));
    } else if (fr.can_id == static_cast<canid_t>(0x080 + node)) {
        std::printf("EMCY   code=0x%04X reg=0x%02X fault=%u state=%u\n",
                    fr.data[0] | (fr.data[1] << 8), fr.data[2], fr.data[3], fr.data[4]);
    } else {
        std::printf("PDO    id=0x%03X dlc=%u\n", fr.can_id, fr.can_dlc);
    }
}

// ---- bench: stress load + output observation ---------------------------------------------------------
struct Toggle { double at_s; std::string action; };

int run_bench(CanopenMaster& m, double seconds, const std::string& jsonl,
              const std::vector<Toggle>& toggles)
{
    std::ofstream log;
    if (!jsonl.empty()) log.open(jsonl, std::ios::app);
    auto emit = [&](const std::string& line) { if (log) log << line << '\n'; };

    // Listen to TPDO1: every relay/state change is logged
    unsigned long pdo_frames = 0, emcy = 0;
    int last_relay = -1, last_state = -1;
    m.on_async = [&](const can_frame& fr) {
        Tpdo1 t{};
        if (decode_tpdo1(fr, m.node(), t)) {
            ++pdo_frames;
            if (t.relay != last_relay || t.state != last_state) {
                last_relay = t.relay; last_state = t.state;
                char b[160];
                std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"can\",\"op\":\"tpdo1\",\"relay\":%u,\"state\":%u,\"fault\":%u}",
                              static_cast<long long>(epoch_ms()), t.relay, t.state, t.fault);
                emit(b);
            }
        } else if (fr.can_id == static_cast<canid_t>(0x080 + m.node())) {
            ++emcy;
            char b[120];
            std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"can\",\"op\":\"emcy\",\"code\":%u}",
                          static_cast<long long>(epoch_ms()), fr.data[0] | (fr.data[1] << 8));
            emit(b);
        }
    };
    m.nmt(0x01);                                     // OPERATIONAL -> TPDOs running

    const std::vector<std::string> names = {"Fon_Relay_Output", "Fon_Current_State", "System_Uptime",
                                            "Fon_Current_Pressure", "System_Min_Memory_Free",
                                            "System_Largest_Free_Block", "Can_Bus_Errors"};
    std::vector<double> lat, wlat;
    unsigned long reads = 0, writes = 0, errors = 0;
    std::size_t next_toggle = 0;
    auto t_start = Clock::now();
    auto last_write = t_start;
    std::size_t i = 0;
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t_start).count(); };

    while (elapsed() < seconds) {
        const std::string& name = names[i++ % names.size()];
        auto t0 = Clock::now();
        try {
            double v = m.read_number(name);
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            lat.push_back(ms); ++reads;
            if (name == "Fon_Relay_Output" || name == "Fon_Current_State" || name == "System_Uptime") {
                char b[200];
                std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"can\",\"op\":\"read\",\"name\":\"%s\",\"value\":%g,\"ms\":%.2f}",
                              static_cast<long long>(epoch_ms()), name.c_str(), v, ms);
                emit(b);
            }
        } catch (const std::exception& ex) {
            ++errors;
            char b[240];
            std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"can\",\"op\":\"read\",\"name\":\"%s\",\"error\":\"%s\"}",
                          static_cast<long long>(epoch_ms()), name.c_str(), ex.what());
            emit(b);
        }
        // every 5 s a validated write (NVS path)
        if (Clock::now() - last_write > std::chrono::seconds(5)) {
            last_write = Clock::now();
            auto w0 = Clock::now();
            try {
                m.write("Fon_Event_Label", std::to_string(writes % 8));
                wlat.push_back(std::chrono::duration<double, std::milli>(Clock::now() - w0).count());
                ++writes;
            } catch (const std::exception& ex) {
                ++errors;
                char b[240];
                std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"can\",\"op\":\"write\",\"error\":\"%s\"}",
                              static_cast<long long>(epoch_ms()), ex.what());
                emit(b);
            }
        }
        // switch the output according to the schedule
        if (next_toggle < toggles.size() && elapsed() >= toggles[next_toggle].at_s) {
            const Toggle& tg = toggles[next_toggle++];
            const char* cmd = tg.action == "on" ? "1" : tg.action == "off" ? "2" : tg.action == "auto" ? "3" :
                              tg.action == "manual" ? "4" : "5";
            std::string status = "applied";
            try { m.write("Fon_Pump_Command", cmd); } catch (const std::exception& ex) { status = ex.what(); ++errors; }
            char b[240];
            std::snprintf(b, sizeof b, "{\"t\":%lld,\"path\":\"can\",\"op\":\"toggle\",\"action\":\"%s\",\"status\":\"%s\"}",
                          static_cast<long long>(epoch_ms()), tg.action.c_str(), status.c_str());
            emit(b);
            std::printf("[%6.1fs] toggle %s -> %s\n", elapsed(), tg.action.c_str(), status.c_str());
        }
        m.pump(0);
    }

    auto stats = [](std::vector<double> v) {
        if (v.empty()) return std::string("{}");
        std::sort(v.begin(), v.end());
        double sum = 0; for (double x : v) sum += x;
        char b[160];
        std::snprintf(b, sizeof b, "{\"min_ms\":%.2f,\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"max_ms\":%.2f}",
                      v.front(), sum / v.size(), v[static_cast<std::size_t>(v.size() * 0.95) - (v.size() > 1 ? 1 : 0)], v.back());
        return std::string(b);
    };
    std::printf("{\"path\":\"can\",\"seconds\":%.0f,\"reads\":%lu,\"reads_per_s\":%.1f,\"writes\":%lu,\"errors\":%lu,"
                "\"pdo_frames\":%lu,\"emcy\":%lu,\"read_latency\":%s,\"write_latency\":%s,\"tx\":%lu,\"rx\":%lu}\n",
                seconds, reads, reads / seconds, writes, errors, pdo_frames, emcy,
                stats(lat).c_str(), stats(wlat).c_str(), m.tx_frames, m.rx_frames);
    return errors ? 1 : 0;
}


// ---- demo: concrete read examples -----------------------------------------------------------
// Every value is an SDO upload (master reads) of object 0x2000+i, sub 0;
// the types come from the device's descriptor record 0x3000+i.
int run_demo(CanopenMaster& m)
{
    // 1) Pressure sensor: raw value in bar (F32) + sensor voltage (U32 mV) + filtered pressure
    float bar  = m.read_f32("Fon_Current_Pressure");
    float filt = m.read_f32("Fon_Pressure_Filtered");
    std::uint32_t mv = m.read_u32("Fon_Sensor_Voltage_mV");
    bool sim = m.read_u32("Fon_Pressure_Manual") != 0;
    std::printf("Pressure         %.2f bar (filtered %.2f bar, sensor %u mV%s)\n", bar, filt, mv,
                sim ? ", SIMULATION active" : "");

    // 2) CPU temperature of the ESP32-S3 (F32 degrees C) and utilization (U8 %)
    std::printf("CPU temperature  %.1f C, utilization %u %%\n",
                m.read_f32("System_Temperature"), m.read_u32("System_Utilization"));

    // 3) System values: uptime (U32 s), free heap (U32 B), Wi-Fi RSSI (I8 dBm)
    std::uint32_t up = m.read_u32("System_Uptime");
    std::printf("Uptime           %u s (%u h %02u min)\n", up, up / 3600, (up % 3600) / 60);
    std::printf("Heap free        %u B (minimum %u B, largest block %u B)\n",
                m.read_u32("System_Memory_Free"), m.read_u32("System_Min_Memory_Free"),
                m.read_u32("System_Largest_Free_Block"));
    std::printf("WLAN             RSSI %d dBm, Link-Score %u\n",
                m.read_i32("System_RSSI"), m.read_u32("Net_Link_Score"));

    // 4) Pump: state (ENUM: 1 Off 2 On 3 Auto 4 Manual 5 Fault), relay, fault code
    static const char* kState[] = {"Init", "Off", "On", "Auto", "Manual", "Fault"};
    std::uint32_t st = m.read_u32("Fon_Current_State");
    std::printf("Pump             state %u (%s), relay %s, fault code %u\n", st,
                st < 6 ? kState[st] : "?", m.read_u32("Fon_Relay_Output") ? "on" : "off",
                m.read_u32("Fon_Fault_Code"));

    // 5) Identity (strings arrive segmented, U64 as 8 bytes)
    std::printf("Device           %s, HW %s, Serial %s\n", m.read_str("Device_SW_Version").c_str(),
                m.read_str("Device_HW_Version").c_str(), m.read("Device_Serial_Number").c_str());

    // 6) Writing configuration = SDO download; the slave validates (range,
    //    cross-field rules) and persists to NVS. Here a harmless point:
    m.write("Fon_Event_Label", "3");
    std::printf("Fon_Event_Label  written, readback %s\n", m.read("Fon_Event_Label").c_str());
    try { m.write("Fon_Max_Pressure", "99"); }
    catch (const SdoError& e) { std::printf("Fon_Max_Pressure=99 -> rejected: %s\n", e.what()); }

    // 7) Process data without polling: NMT Operational, then TPDO1
    //    (0x180+node) delivers pressure/state/relay/fault on change and at least 1x/s.
    std::printf("Listening to TPDO1 for 3 s (NMT start):\n");
    m.on_async = [&](const can_frame& fr) {
        Tpdo1 t{};
        if (decode_tpdo1(fr, m.node(), t))
            std::printf("  TPDO1  %.2f bar  state=%u relay=%u fault=%u\n", t.pressure, t.state, t.relay, t.fault);
    };
    m.nmt(0x01);
    auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end) m.pump(200);
    m.nmt(0x80);                                     // back to Pre-Operational
    return 0;
}

std::vector<Toggle> parse_toggles(const std::string& spec)
{
    std::vector<Toggle> out;
    std::istringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto p = item.find(':');
        if (p == std::string::npos) continue;
        out.push_back({std::stod(item.substr(0, p)), item.substr(p + 1)});
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string ifname = "can0", od_cache = "od_cache.txt";
    int node = 3;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--if" && i + 1 < argc) ifname = argv[++i];
        else if (a == "--node" && i + 1 < argc) node = std::stoi(argv[++i]);
        else if (a == "--od" && i + 1 < argc) od_cache = argv[++i];
        else args.push_back(a);
    }
    if (args.empty()) { std::cerr << "usage: see file header\n"; return 2; }

    try {
        CanopenMaster m(ifname, node);
        const std::string& cmd = args[0];

        if (cmd == "info") {
            auto name = m.sdo_upload(0x1008, 0), sw = m.sdo_upload(0x100A, 0), hw = m.sdo_upload(0x1009, 0);
            auto prod = m.sdo_upload(0x1018, 2), rev = m.sdo_upload(0x1018, 3), ser = m.sdo_upload(0x1018, 4);
            auto hb = m.sdo_upload(0x1017, 0), cnt = m.sdo_upload(0x2FFF, 0);
            std::printf("node %d on %s\n  name       %s\n  hw/sw      %s / %s\n  product    0x%08llX\n"
                        "  revision   0x%08llX\n  serial     %llu\n  heartbeat  %llu ms\n  datapoints %llu\n",
                        node, ifname.c_str(), std::string(name.begin(), name.end()).c_str(),
                        std::string(hw.begin(), hw.end()).c_str(), std::string(sw.begin(), sw.end()).c_str(),
                        static_cast<unsigned long long>(CanopenMaster::to_number(T_U32, prod)),
                        static_cast<unsigned long long>(CanopenMaster::to_number(T_U32, rev)),
                        static_cast<unsigned long long>(CanopenMaster::to_number(T_U32, ser)),
                        static_cast<unsigned long long>(CanopenMaster::to_number(T_U16, hb)),
                        static_cast<unsigned long long>(CanopenMaster::to_number(T_U16, cnt)));
            return 0;
        }
        if (cmd == "nmt") {
            std::string s = args.size() > 1 ? args[1] : "";
            std::uint8_t cs = s == "start" ? 0x01 : s == "stop" ? 0x02 : s == "preop" ? 0x80 :
                              s == "reset" ? 0x81 : s == "reset-comm" ? 0x82 : 0;
            if (!cs) { std::cerr << "nmt start|stop|preop|reset|reset-comm\n"; return 2; }
            m.nmt(cs);
            return 0;
        }
        if (cmd == "monitor") {
            double sec = args.size() > 1 ? std::stod(args[1]) : 10;
            m.on_async = [&](const can_frame& fr) { print_async(fr, node); };
            auto end = Clock::now() + std::chrono::duration<double>(sec);
            while (Clock::now() < end) m.pump(200);
            return 0;
        }

        m.load_od(od_cache);                     // from here on: access by name

        if (cmd == "list") {
            std::vector<const Entry*> v;
            for (auto& [k, e] : m.od()) v.push_back(&e);
            std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->index < b->index; });
            for (auto e : v) std::printf("0x%04X  %-28s %-5s %s\n", e->index, e->name.c_str(), type_name(e->type), access_name(e->access));
            return 0;
        }
        if (cmd == "read") {
            for (std::size_t i = 1; i < args.size(); ++i)
                std::printf("%-28s = %s\n", args[i].c_str(), m.read(args[i]).c_str());
            return 0;
        }
        if (cmd == "write" && args.size() == 3) {
            m.write(args[1], args[2]);
            std::printf("%s = %s  (readback %s)\n", args[1].c_str(), args[2].c_str(), m.read(args[1]).c_str());
            return 0;
        }
        if (cmd == "pump" && args.size() == 2) {
            const std::string& a = args[1];
            const char* v = a == "on" ? "1" : a == "off" ? "2" : a == "auto" ? "3" : a == "manual" ? "4" : a == "restart" ? "5" : nullptr;
            if (!v) { std::cerr << "pump on|off|auto|manual|restart\n"; return 2; }
            m.write("Fon_Pump_Command", v);
            usleep(300 * 1000);                  // pump_task polls the command
            std::printf("Fon_Pump_Command=%s -> state=%s relay=%s\n", v,
                        m.read("Fon_Current_State").c_str(), m.read("Fon_Relay_Output").c_str());
            return 0;
        }
        if (cmd == "demo") return run_demo(m);
        if (cmd == "bench") {
            double sec = args.size() > 1 ? std::stod(args[1]) : 30;
            std::string jsonl, toggles;
            for (std::size_t i = 2; i + 1 < args.size(); ++i) {
                if (args[i] == "--jsonl") jsonl = args[++i];
                else if (args[i] == "--toggle") toggles = args[++i];
            }
            return run_bench(m, sec, jsonl, parse_toggles(toggles));
        }
        std::cerr << "unknown command: " << cmd << '\n';
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << '\n';
        return 1;
    }
}
