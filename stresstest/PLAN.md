# Stress Test Plan: Fountainer Access via Web, C++ (Local) and CANopen

As of: 2026-09-23 · Test device: **FNT-000003** (`esp32-94a990ddc1a8`, HW2.0,
firmware 4.40.0, WLAN IP DEVICE_IP, CAN node 3) · CAN master: Raspberry Pi
`PI_HOST` (PI_IP, MCP2515 HAT, can0 @ 250 kbit/s) · Cloud server:
Docker `fountain_server` on SERVER_IP (web UI/API :8010, WSS :8443).

**Never touched:** production pump `esp32-a1b2c3d4e5f6` @ PROD_PUMP_IP.

## 1. Goal

Demonstrate that the device remains stable under simultaneous load on all
three access paths and that the **pump output** (`Fon_Relay_Output`)
remains controllable and observable at all times:

| Path | Transport | Tool in the test |
|---|---|---|
| **Web** | Browser/HTTP -> cloud server -> signed WSS (8443) -> device | `stress_web.py` (REST API of the server: `/api/dp_read`, `/api/dp_write`, `/api/command`, `/api/devices`) |
| **C++** | `fountainer_client_framework` -> local WSS server of the device (4443, mTLS, 1 slot) | `stress_local` (C++, public API of the framework) |
| **CAN** | CANopen SDO/PDO via SocketCAN from the Pi | `canopen_sdo bench` (C++, raw SocketCAN) |

## 2. Load Profiles

Limits from the firmware that the test respects or deliberately approaches:

- local server: 5 frames/s (burst 20), 3 violations/10 s -> disconnect,
  1 client slot, TX frame <= 4096 B, idle 300 s
- cloud path: no explicit rate limit, but the prototype's WLAN link
  is weak (`Net_Link_Score` 11-13) -> timeouts are to be expected and
  are counted, not concealed
- CAN: SDO ~2 ms latency (measured 521 reads/s), writes ~35 ms (NVS)
- pump: `Fon_Min_On_Time` = `Fon_Min_Off_Time` = 30 s -> output is
  switched at most every 45 s

| Phase | Duration | Web | C++ local | CAN | Purpose |
|---|---|---|---|---|---|
| P0 Baseline | - | 1 snapshot | connect + read_all | info + read_all | reachability, counter values (heap, stack, CAN counters, uptime) |
| P1 Single load Web | 60 s | 3 workers, dp_read/dp_write/devices alternating | - | - | cloud path alone: latency, timeouts |
| P2 Single load C++ | 60 s | - | reads 4/s + write every 5 s | - | local path alone, just below the rate limit |
| P3 Single load CAN | 60 s | - | - | SDO round-robin as fast as possible + writes | CAN alone |
| P4 **Full load + output** | 6 x 45 s = 270 s | as P1 | as P2 | as P3 | all three simultaneously; every 45 s a different path switches the output (Web -> C++ -> CAN -> ...), **all three** paths must see the new state within 10 s |
| P5 Churn | 20 x | - | connect/disconnect | load keeps running | slot release, handshake under load |
| P6 Post-run | - | snapshot | read_all | info | compare counters: no reboot (uptime monotonic), heap/stack not collapsed, `Can_Bus_Errors` = 0, no new alerts |

## 3. Output Test (P4) in Detail

Preparation (the prototype has no pressure sensor connected, fault 1
"sensor" is latched):

1. `Fon_Pressure_Manual = 1`, `Fon_Pressure_Value = 2.5` (simulation, volatile),
   `Fon_Dry_Run_Min_Rise = 0` (otherwise the constant simulation curve latches
   as dry run, fault 4, after 30 s; reset to 100 at the end)
2. `Fon_Fault_Ack = 1` -> fault acknowledged, state Off (1)
3. `set_state Manual`

Switching sequence, each step triggered by the named path:

| t | Path | Action | Expectation |
|---|---|---|---|
| 0 s | Web | `command set_state On` | relay 1 on all paths <= 10 s |
| 45 s | C++ | `commands().set_state(Off)` | relay 0 |
| 90 s | CAN | SDO `Fon_Pump_Command = 1` | relay 1 |
| 135 s | Web | `set_state Off` | relay 0 |
| 180 s | C++ | `set_state(On)` | relay 1 |
| 225 s | CAN | `Fon_Pump_Command = 2` | relay 0 |

Observation: Web via `/api/dp_read Fon_Relay_Output` (1/s), C++ via
`read(dp::Fon_Relay_Output)` in the load loop, CAN via TPDO1 (byte 5) and SDO.
Each path writes timestamp + value as JSONL; `analyze.py` measures the
visibility latency per path for each switching operation.

Cleanup: `set_state Off`, `Fon_Pressure_Manual = 0` (sensor operation) —
the fault then returns (no sensor), which is the initial state.

## 4. Metrics / Pass Criteria

- no device reboot (`System_Uptime` monotonic), no watchdog reset
- `System_Min_Memory_Free` / `System_Largest_Free_Block` after the test
  not below 50 % of the baseline value (fragmentation)
- `Can_Bus_Errors` = 0, `Can_Bus_Off_Count` unchanged
- local path: 0 disconnects due to the rate limit, no session aborts
  except those caused by the churn test itself
- cloud path: timeouts are counted; the criterion is recovery
  without intervention (session comes back, no permanent disruption)
- output: each of the 6 switching operations visible on all 3 paths <= 10 s

## 5. Tools

```
stresstest/
  run_stress.sh        orchestration (VM): starts web load, C++ load (Docker)
                       and the CAN load on the Pi (ssh), collects JSONL
  stress_web.py        web load + web output observer (requests, threads)
  analyze.py           evaluates the JSONL logs -> results/<date>_stresstest.md
samples/cpp_local/     stress_local.cpp (C++ load, framework API)
samples/canopen_cpp/   canopen_sdo.cpp   (bench mode = CAN load)
```

All raw data is located under `stresstest/results/`.
