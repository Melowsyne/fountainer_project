# fountainer_project — Overall Documentation: Accessing the Fountainer

This project bundles the documentation of the Fountainer system (ESP32-S3
pump controller) from an integrator's point of view: **how do I access the
device**, which paths exist, which limits apply, and how stable the device
is when all paths are used at the same time (stress test).

| Repo | Contents |
|---|---|
| `../fountainer_firmware` | ESP32-S3 firmware (cloud client, local WSS server, CANopen slave); docs under `DOKU/` |
| `../fountainer_server` | Cloud server (Python): WSS endpoint for devices, admin web UI + REST API, OTA, log/history poller |
| `../fountainer_client_framework` | C++20 framework for local mTLS access (Boost.Beast, OpenSSL, nlohmann-json) |
| `../fountainer_can_master_linux` | CANopen master in Python (python-canopen/SocketCAN), CLI `fcm` |
| **this project** | Access documentation with source code samples (`samples/`), stress test plan/tools/results (`stresstest/`) |

```
fountainer_project/
  README.md                 this documentation
  samples/
    web/                    web_api_client.py, web_api_curl.sh   (Python / curl against the server REST API)
    cpp_local/              local_access.cpp, stress_local.cpp, CMakeLists.txt, client.fnt-000003.json,
                            build_docker.sh, run_docker.sh       (C++ on the client framework, local WSS server)
    canopen_cpp/            canopen_sdo.cpp                      (C++ CANopen master with raw SocketCAN)
  doku/                     CAN_Signals.md + img/: oscilloscope captures of the CAN frames, decoded bit by bit
  stresstest/
    PLAN.md                 test plan
    run_stress.sh           orchestration of all phases
    stress_web.py           web load / output observer
    analyze.py              evaluation of the JSONL logs
    results/<date>/         raw data + analysis.md per run
```

Testbed (as of 2026-09-23): prototype **FNT-000003** (`esp32-94a990ddc1a8`,
HW2.0, firmware 4.40.1 since the OTA on 2026-09-23) with WLAN IP DEVICE_IP and CAN node 3; cloud server
on SERVER_IP (`fountain_server`, Docker); CAN master Raspberry Pi 3B
`PI_HOST` (PI_IP, MCP2515 HAT, `can0` 250 kbit/s). The
production pump `esp32-a1b2c3d4e5f6` (PROD_PUMP_IP) is not addressed by any
tool of this project.

---

## 1. The Three Access Paths at a Glance

```
                 +-----------------------------+
  Browser/HTTP   |  Cloud server SERVER_IP  |   WSS 8443, mTLS, HMAC-signed
  REST :8010 --->|  fountain_server (Python)   |------------------------------+
                 +-----------------------------+                              |
                                                                              v
  C++ app (fountainer_client_framework)   WSS 4443, mTLS, HMAC     +---------------------+
  ------------------------------------------------------------->   |  Fountainer ESP32   |
                                                                   |  FNT-000003         |
  CANopen master (Pi, SocketCAN can0)      CiA 301 SDO/PDO/NMT     |  Node 3 @ 250 kbit  |
  ------------------------------------------------------------->   +---------------------+
```

| | Web (server REST) | C++ local (framework) | CANopen |
|---|---|---|---|
| Transport | HTTP to the server, server holds a WSS to the device | WSS directly to the device, port 4443 | CAN bus, SocketCAN |
| Auth | Web login (cookie); server signs control RPCs (HMAC-SHA256, scope `control`) | mTLS client certificate + HMAC key of the device (kid) | none (bus unauthenticated; passwords write-only) |
| Reach | anywhere the server is reachable | same LAN/WLAN as the device | bus wiring (J3 CANH/CANL) |
| Read | `dp_read` (all 126 datapoints) | `dp_read` typed / dynamic, polling, subscriptions | SDO upload per datapoint (`0x2000+i`), TPDO1-4 cyclic/event-driven |
| Write | `dp_write` (batch, validated, NVS) | `dp_write` (compile-time type check) | SDO download (same validated path), RPDO |
| Control pump | `command set_state/turn_on_duration/restart/reboot` | `commands().set_state(...)` etc. | `Fon_Pump_Command` = 1 On, 2 Off, 3 Auto, 4 Manual, 5 Restart |
| Limits | latency depends on the device's WLAN link; a timeout = HTTP 504 | 1 client slot, 5 frames/s (burst 20), TX frame <= 4 KB, idle 300 s | SDO ~1-2 ms; writes ~30 ms (NVS); 1 master per bus is sensible |
| Measured (stress test) | ~5-6 reads/s, avg 30-60 ms | 3.3 reads/s (budget 4/s), avg ~20 ms | 850 reads/s, avg 1.2 ms |

All three paths end in the device in the **same** validated datapoint path
(`task_com_apply_dp_write` / command logic of the `pump_task`); value range,
cross-field rules (e.g. `Fon_Min_Pressure < Fon_Max_Pressure`), min-on/off
times and fault latch apply identically everywhere.

---

## 2. Access via Web Server (REST API of the Cloud Server)

The server (`fountainer_server`) holds the Fountain v2.2 session (WSS 8443,
mTLS, HMAC) to each device. The admin web interface on **port 8010**
offers the same functions as a REST API; the UI buttons call exactly
these endpoints.

| Endpoint | Method | Body / Query | Response |
|---|---|---|---|
| `/login` | POST (form) | `username`, `password` (testbed: admin/admin) | 302 + cookie `session` |
| `/api/devices` | GET | – | `{devices:[{device_id, serial, online, fw_version, uptime_s, dp{}, dp_ages{}, config{}, alerts[], events[], logs[]}], firmware:[]}` |
| `/api/dp_read` | POST | `{device_id, names:[...]}` (empty = snapshot) | `{ok, result:{dp:{name:value}}}` |
| `/api/dp_write` | POST | `{device_id, dp:{name:value,...}}` | `{ok, result:{status:"applied"/"rejected", errors{}, readback{}}}` |
| `/api/command` | POST | `{device_id, command, target_state?, duration_steps?}` | `{ok, result:{status:"applied"/"rejected", error?}}` |
| `/api/history` | GET | `?device_id=&since_i=` | 1 Hz pressure history |
| `/health` | GET | – | `{status, devices_online}` (without login) |

Error patterns: **401** without login, **409** device not connected, **504**
device did not respond within the RPC timeout (typical with weak WLAN —
the call then simply needs to be repeated), **400** parameter error.

Sample: [`samples/web/web_api_client.py`](samples/web/web_api_client.py)
(class `FountainerWebClient` + CLI) and
[`samples/web/web_api_curl.sh`](samples/web/web_api_curl.sh).

```python
from web_api_client import FountainerWebClient

c = FountainerWebClient("http://SERVER_IP:8010", "admin", "admin")   # login -> cookie
dev = "esp32-94a990ddc1a8"

for d in c.devices():                                   # device list + shadow
    print(d["device_id"], d["online"], d["fw_version"])

dp = c.dp_read(dev, ["Fon_Current_State", "Fon_Relay_Output", "Fon_Current_Pressure"])
print(dp)                                               # {'Fon_Current_State': 5, 'Fon_Relay_Output': False, ...}

res = c.dp_write(dev, Fon_Max_Pressure=3.5)             # signed dp_write, atomic batch
assert res["status"] == "applied", res["errors"]

res = c.set_state(dev, "On")                            # signed command set_state
print(res["status"], res.get("error"))                  # applied | rejected, e.g. not_permitted
```

The same with curl:

```bash
curl -s -c cj -d "username=admin&password=admin" http://SERVER_IP:8010/login
curl -s -b cj http://SERVER_IP:8010/api/devices | python3 -m json.tool | head
curl -s -b cj -H "Content-Type: application/json" http://SERVER_IP:8010/api/dp_read \
     -d '{"device_id":"esp32-94a990ddc1a8","names":["Fon_Relay_Output"]}'
curl -s -b cj -H "Content-Type: application/json" http://SERVER_IP:8010/api/command \
     -d '{"device_id":"esp32-94a990ddc1a8","command":"set_state","target_state":"Off"}'
```

What happens on the wire (server -> device): `dp_read` unsigned,
`dp_write`/`command` as an HMAC-signed control message with a running
`seq` (anti-replay); the device responds with `dp_report` /
`dp_write_result` / `command_result` (`in_reply_to`). Details:
`fountainer_firmware/DOKU/Protocol_Reference.md`.

---

## 3. Access via C++ (fountainer_client_framework, Local WSS Server)

The device runs a local WSS server (port **4443**, path `/ws`,
subprotocol `fountain`, **mTLS mandatory**, 1 client slot). The C++ framework
handles transport, handshake (`hello` -> `hello_ack` -> signed
`ota_check` proof -> `ota_none`), HMAC signing of the control messages,
rate budget (4/s, below the firmware's 5/s), keepalive and reconnect.

Required: the root CA of the testbed PKI, a client certificate/key
(`service-laptop-01`), the device's HMAC key (`hmac.key`, kid "1") and
the expected `device_id`. In Docker operation these are mounted under `/certs`
and `/secrets`; the configuration only names paths
([`samples/cpp_local/client.fnt-000003.json`](samples/cpp_local/client.fnt-000003.json)).

Sample: [`samples/cpp_local/local_access.cpp`](samples/cpp_local/local_access.cpp)

```cpp
#include <fountainer/client.hpp>
#include <fountainer/config.hpp>
#include <fountainer/datapoints/generated.hpp>
using namespace fountainer;

Config cfg = load_config("client.fnt-000003.json");

auto hmac = HmacCredentials::from_file(cfg.fountain.kid, cfg.fountain.hmac_key_file);
auto built = Client::builder(Endpoint{cfg.device.host, cfg.device.port})
    .with_tls(TlsCredentials::mutual_tls(cfg.tls.ca_file, cfg.tls.client_cert_file,
                                         cfg.tls.client_key_file,
                                         EndpointIdentityPolicy::VerifyCertificateChainOnly)) // IP instead of DNS name
    .with_hmac(std::move(*hmac))
    .with_expected_device(cfg.fountain.device_id)
    .build();                                  // Result<Client>: config errors surface here
Client client = std::move(*built);

auto conn = client.connect();                  // success == Fountain session RUNNING
std::cout << conn->device_id << " fw=" << conn->firmware_version << '\n';

auto p = client.datapoints().read(dp::Fon_Current_Pressure);           // Result<float>
auto s = client.datapoints().read(dp::Fon_Current_State, dp::Fon_Relay_Output);   // snapshot
auto v = client.datapoints().read("Can_State");                          // dynamic by name

auto wr = client.datapoints().write(dp::Fon_Event_Label, std::uint8_t{2});   // RO datapoint = compile error
if (wr && !wr->applied()) { /* firmware rejected it: wr->errors */ }

auto cr = client.commands().set_state(FountainState::On);               // signed command
if (cr && !cr->applied()) std::cerr << cr->error.value_or("") << '\n';  // e.g. not_permitted

auto sub = client.datapoints().subscribe(dp::Fon_Relay_Output,
    [](const DatapointChange<bool>& c) { std::cout << "Relay " << c.value << '\n'; });
client.polling().every(std::chrono::seconds(1), dp::Fon_Relay_Output, dp::Fon_Current_State);
client.polling().start();
// ...
client.disconnect();
```

Build and run (in the framework's Docker image, build folder in the named
volume `fproj_build`, because the CIFS share loses exec bits):

```bash
bash samples/cpp_local/build_docker.sh
bash samples/cpp_local/run_docker.sh local_access /proj/samples/cpp_local/client.fnt-000003.json [on|off|auto|manual]
```

Output against FNT-000003:

```
connected: esp32-94a990ddc1a8 fw=4.40.0
Fon_Current_Pressure = 2.5 bar
  Fon_Current_State = 5
  Fon_Fault_Code = 1
  Fon_Relay_Output = false
  System_Uptime = 1755
  Can_State = 2
Fon_Event_Label written, readback ok
```

Note: only **one** local client at a time (the second one is rejected at the
slot; a client with `auto_reconnect` holds the slot permanently);
the framework CLI (`fountainer-cli <config> read|write|command|watch`) and
the Python maintenance tool `fountainer_server/local_maintenance_client.py`
use the same slot. The datapoint catalog `generated.hpp` is generated from
the firmware's `dp_list.def` and must match the firmware
(`python3 tools/generate_datapoints.py --check`).

---

## 4. Access via CANopen (C++ Program with SocketCAN)

The firmware (from 4.40.0, HW2.x with TJA1051 transceiver only) is a CiA 301
slave: NMT, heartbeat (`0x700+id`), SDO server (`0x600+id` -> `0x580+id`,
expedited + segmented), 4 TPDO / 2 RPDO, EMCY (`0x80+id`). **Every
datapoint i is object `0x2000+i` (sub 0)**, the count is in `0x2FFF`,
and `0x3000+i` provides a descriptor record (sub 1 name, sub 2 type code,
sub 3 access) — a master can learn its object dictionary directly from the
device without an EDS. Defaults: node ID 3, 250 kbit/s, heartbeat 1 s
(`Can_Node_Id`, `Can_Bitrate`, `Can_Heartbeat_Ms`, `Can_Enabled`).

Sample: [`samples/canopen_cpp/canopen_sdo.cpp`](samples/canopen_cpp/canopen_sdo.cpp)
— a standalone C++17 program without third-party libraries (raw
SocketCAN) that learns the catalog via discovery and reads/writes typed values.

```bash
# on the Pi (can0 is brought up by the systemd unit fountainer-can0)
g++ -O2 -std=c++17 -o canopen_sdo canopen_sdo.cpp
./canopen_sdo info                                   # identity 0x1018, name, SW, heartbeat
./canopen_sdo list | head                            # catalog (discovery 0.8 s, then od_cache.txt)
./canopen_sdo read Fon_Current_State Fon_Relay_Output Device_Serial_Number
./canopen_sdo write Fon_Max_Pressure 3.8             # -> SDO abort 0x06090030 on range error
./canopen_sdo pump on                                # Fon_Pump_Command = 1
./canopen_sdo nmt start && ./canopen_sdo monitor 10  # TPDO1..4, heartbeat, EMCY
./canopen_sdo bench 60 --jsonl can.jsonl --toggle 10:on,55:off   # stress load
```

**Terminology:** CANopen names the SDO direction from the *slave's* point of view:
**SDO upload** = the master reads an object (the slave "uploads"),
**SDO download** = the master writes. For process data the terms are **TPDO**
(transmit PDO: the slave sends, e.g. pressure/state cyclically or on
change) and **RPDO** (receive PDO: the slave receives, e.g.
`Fon_Fault_Ack`). "transmit/receive" is the level of the CAN frames
(SocketCAN `write()`/`read()`). The sample follows this convention:
`sdo_upload()`/`sdo_download()` are the protocol primitives, on top of them
sit typed readers (`read_f32`, `read_u32`, `read_i32`, `read_str`)
and `read()`/`write()` by datapoint name.

Core of the access (abridged from `canopen_sdo.cpp`):

```cpp
// socket on can0, filter on the node's COB-IDs (SDO response, TPDO1-4, HB, EMCY)
int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
ifreq ifr{}; std::strncpy(ifr.ifr_name, "can0", IFNAMSIZ - 1); ioctl(fd, SIOCGIFINDEX, &ifr);
sockaddr_can addr{AF_CAN, ifr.ifr_ifindex}; bind(fd, (sockaddr*)&addr, sizeof addr);

// SDO upload (master reads): request 0x600+node = [0x40, idx_lo, idx_hi, sub, 0,0,0,0]
// response 0x580+node: 0x4F/0x4B/0x47/0x43 (1..4 bytes expedited in bytes 4..7),
// 0x41 = segmented (strings, U64), 0x80 = abort with error code in bytes 4..7
std::vector<std::uint8_t> sdo_upload(std::uint16_t index, std::uint8_t sub);

// SDO download (master writes, 1..4 bytes): cs = 0x23 | ((4-n) << 2); response 0x60
void sdo_download(std::uint16_t index, std::uint8_t sub, const std::uint8_t* d, std::size_t n);

// learn the catalog from the device: 0x2FFF = count, 0x3000+i = {name, type code, access}
int count = to_u64(sdo_upload(0x2FFF, 0), 2);
for (int i = 0; i < count; ++i)
    od[str(sdo_upload(0x3000 + i, 1))] = Entry{0x2000 + i, sdo_upload(0x3000 + i, 2)[0], sdo_upload(0x3000 + i, 3)[0]};
// type codes: 0 BOOL 1 U8 2 U16 3 U32 4 U64 5 I8 6 I16 8 F32 9 ENUM 10 STR, all little-endian
```

**Concrete examples** (`./canopen_sdo demo`, excerpt from `run_demo()`):

```cpp
// pressure sensor: F32 in bar (object 0x2019), filtered pressure, sensor voltage U32 mV
float bar  = m.read_f32("Fon_Current_Pressure");
float filt = m.read_f32("Fon_Pressure_Filtered");
std::uint32_t mv = m.read_u32("Fon_Sensor_Voltage_mV");

// CPU temperature of the ESP32-S3 (F32 deg C, object 0x2004) and utilization (U8 %)
float temp = m.read_f32("System_Temperature");
std::uint32_t load = m.read_u32("System_Utilization");

// system values: uptime U32 s, free heap U32 B, WLAN RSSI I8 dBm
std::uint32_t up = m.read_u32("System_Uptime");
std::uint32_t heap = m.read_u32("System_Memory_Free");
std::int32_t rssi = m.read_i32("System_RSSI");

// pump: state ENUM (1 Off 2 On 3 Auto 4 Manual 5 Fault), relay BOOL, fault code U8
std::uint32_t state = m.read_u32("Fon_Current_State");
bool relay = m.read_u32("Fon_Relay_Output") != 0;

// identity: strings arrive segmented (2 round trips), U64 as 8 bytes
std::string sw = m.read_str("Device_SW_Version");

// writing configuration = SDO download; the slave validates and persists (NVS)
m.write("Fon_Event_Label", "3");                       // ok, readback 3
try { m.write("Fon_Max_Pressure", "99"); }             // -> SDO abort 0x06090030 (out of range)
catch (const SdoError& e) { std::puts(e.what()); }

// control pump: Fon_Pump_Command (U8) 1 On, 2 Off, 3 Auto, 4 Manual, 5 Restart
std::uint8_t on = 1; m.sdo_download(m.entry("Fon_Pump_Command").index, 0, &on, 1);

// process data without polling: NMT Operational (0x000 [0x01, node]) -> TPDO1 on 0x180+node
// = F32 pressure | U8 state | U8 relay | U8 fault code (7 bytes), on change + at least 1x/s
m.on_async = [&](const can_frame& fr) { Tpdo1 t{}; if (decode_tpdo1(fr, m.node(), t)) /* ... */; };
m.nmt(0x01);  while (...) m.pump(200);  m.nmt(0x80);   // back to Pre-Operational
```

Output against FNT-000003 (firmware 4.40.1, pressure simulation 2.5 bar active,
no sensor connected):

```
$ ./canopen_sdo demo
Pressure         2.50 bar (filtered 2.50 bar, sensor 71 mV)
CPU temperature  35.2 C, utilization 4 %
Uptime           5820 s (1 h 37 min)
Heap free        81368 B (minimum 31120 B, largest block 31744 B)
WLAN             RSSI -86 dBm, link score 11
Pump             state 5 (Fault), relay off, fault code 1
Device           4.40.1, HW HW2.0, Serial 00464E5400000003
Fon_Event_Label  written, readback 3
Fon_Max_Pressure=99 -> rejected: SDO abort 0x06090030 value out of range (0x2031:0)
Listening to TPDO1 for 3 s (NMT start):
  TPDO1  2.50 bar  state=5 relay=0 fault=1
  TPDO1  2.50 bar  state=5 relay=0 fault=1
  TPDO1  2.50 bar  state=5 relay=0 fault=1
```

The same accesses on the bus (`candump can0`), node 3:

```
603  [8]  40 19 20 00 00 00 00 00     SDO upload request  0x2019:0 (Fon_Current_Pressure)
583  [8]  43 19 20 00 00 00 20 40     response: 4 bytes expedited, F32 LE 0x40200000 = 2.5 bar
603  [8]  40 1A 20 00 00 00 00 00     0x201A:0 Fon_Sensor_Voltage_mV
583  [8]  43 1A 20 00 47 00 00 00     U32 = 0x47 = 71 mV
603  [8]  40 04 20 00 00 00 00 00     0x2004:0 System_Temperature
583  [8]  43 04 20 00 CD CC 0C 42     F32 0x420CCCCD = 35.2 C
603  [8]  40 05 20 00 00 00 00 00     0x2005:0 System_Utilization
583  [8]  4F 05 20 00 04 00 00 00     1 byte expedited (0x4F), U8 = 4 %
000  [2]  01 03                       NMT start node 3 -> Operational
183  [7]  00 00 20 40 05 00 01        TPDO1: 2.5 bar, state 5, relay 0, fault 1
703  [1]  05                          heartbeat: Operational
```

Further commands of the sample: `info`, `list`, `read NAME...`, `write NAME VALUE`,
`pump on|off|auto|manual|restart`, `nmt start|stop|preop|reset`, `monitor SEC`,
`bench SEC [--jsonl F] [--toggle T:on,...]` (stress load). First output of
`info`/`read`:

```
node 3 on can0
  name       Fountainer
  hw/sw      HW2.0 / 4.40.1
  product    0x464E5400
  heartbeat  1000 ms
  datapoints 126
Device_Serial_Number         = 00464E5400000003
ERROR: SDO abort 0x06010001 write-only object (read refused) (0x204C:0)   <- Network_Password
```

Alternative without C++: the Python master `fountainer_can_master_linux`
(`fcm scan|info|read|write|pdo-map|monitor|bench`, API `FountainerMaster`).
Two SDO clients on the same node at the same time should be avoided (the
responses cannot be told apart).

Value ranges and semantics of all datapoints: `fountainer_firmware/DOKU/Datapoints.md`;
CANopen details: `fountainer_firmware/DOKU/CANopen.md`. What the frames physically
look like on the bus (heartbeat, TPDO1/3, SDO request/response decoded bit by
bit, CAN_H/CAN_L levels, 4 µs bits): [`doku/CAN_Signals.md`](doku/CAN_Signals.md)
with oscilloscope captures from 2026-09-23.

---

## 5. Stress Test: All Paths Simultaneously, Output Remains Controllable

Plan: [`stresstest/PLAN.md`](stresstest/PLAN.md). Execution:

```bash
bash samples/cpp_local/build_docker.sh                       # once
scp samples/canopen_cpp/canopen_sdo.cpp pi@PI_IP:~/fountainer_project/canopen_cpp/ && ssh ... g++ ...
bash stresstest/run_stress.sh [--quick]                      # results: stresstest/results/<date>/
```

Three runs on 2026-09-23 (smoke on 4.40.0, full run on 4.40.0, full run
on 4.40.1 after the fix), details and raw data in
[`stresstest/results/2026-09-23_stresstest.md`](stresstest/results/2026-09-23_stresstest.md).

**Result (full run on 4.40.1, run of 2026-09-23 10:50)**

| | Web (REST -> server -> WSS) | C++ local (WSS 4443) | CAN (SDO/TPDO) |
|---|---|---|---|
| Full load 270 s, all three in parallel | 1410 reads, 63 writes, 0 errors; avg 38 / p95 70 ms | 873 reads, 52 writes, 0 errors, 0 disconnects; avg 33 / p95 45 ms | 207 896 reads (770/s), 53 writes, 0 errors; avg 1.3 / p95 1.6 ms |
| Switching the output (6x, taking turns Web/C++/CAN) | 6/6 `applied`; new relay state visible on all paths within <= 1.1 s (criterion 10 s) | | |
| Churn 20x connect/read/disconnect under load | – | 20/20 ok, cycle 3.1-3.9 s (mTLS on the ESP32) | – |
| Device | no reboot, heap low-water mark 31.1 kB (mTLS handshakes), no fragmentation, `Can_Bus_Errors` 0 | | |

**Findings and Measures**

| # | Finding | Status |
|---|---|---|
| B1 | Firmware: `set_state On` from cloud/local sporadically latched fault 3 (max runtime) immediately: cycle timestamp older than `pump_start_ms` of the remote request -> unsigned underflow of the runtime. CAN path never affected. | **fixed** in 4.40.1 (`run_ms()` + timestamp under the lock, host regression test), rolled out via OTA to FNT-000003 |
| B2 | C++ framework: `generated.hpp` outdated (104 instead of 126 datapoints) | **fixed** (regenerated) |
| B3 | CAN sample: U64 decoded via `double` (serial number wrong) | **fixed** |
| B4 | Web path: sporadic HTTP 504 (device does not respond within 5 s) at WLAN link score 0 of the prototype; 5x in 60 s right after a reboot, otherwise 0 | open (environment): repeat the call, improve WLAN position |
| B5 | Local path: 1/20 mTLS handshake timeout under web+CAN load (run 2), next attempt ok; run 3: 20/20 | open (known latecomer behavior), no slot leak |


---

## 6. Operating Notes

- **Production pump** `esp32-a1b2c3d4e5f6` / PROD_PUMP_IP: never address it with test tools.
- Device IPs are DHCP-dynamic (FNT-000003 most recently DEVICE_IP); check before every run (`/api/devices` does not show the IP; `ip neigh` or the router).
- `Fon_Pressure_Manual`/`Fon_Pressure_Value` are volatile: after a reboot the real sensor is active again; without a sensor (prototype on the test bench) fault 1 latches and `Fon_Fault_Ack` only takes effect with a healthy (or simulated) sensor.
- Min-on/min-off time (default 30 s) applies on all paths: a `set_state Off` within the min-on time is accepted and recorded as a deadline (the relay drops out once it expires); an `On` within the min-off time, in the fault state or above `Fon_Max_Pressure` is rejected with `not_permitted`.
- The CIFS share loses exec bits: start scripts with `bash script.sh`; C++ builds in the Docker volume.

## License

Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.

This repository is published as a reference project. You may review, compile
and run the code to evaluate it; see [LICENSE.md](LICENSE.md) for the terms.
Production use, integration into products or custom development on this code
base is available under a separate agreement: info@melowsyne.com
