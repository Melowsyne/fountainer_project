# Stress Test 2026-09-23 — Results

Device FNT-000003 (`esp32-94a990ddc1a8`, HW2.0), cloud server SERVER_IP,
CAN master Raspberry Pi PI_IP. Plan: [`../PLAN.md`](../PLAN.md).
Raw data and automatic evaluation (`analysis.md`) per run in the
subfolders.

| Run | Firmware | Scope | Folder |
|---|---|---|---|
| 1 (smoke) | 4.40.0 | `--quick`: P1-P3 15 s each, P4 60 s (3 toggles), P5 3x | `2026-09-23_1025/` |
| 2 (full) | 4.40.0 | P1-P3 60 s each, P4 270 s (6 toggles), P5 20x + 60 s | `2026-09-23_1035/` |
| 3 (full, after fix) | **4.40.1** | as run 2 | `2026-09-23_1050/` |

## 1. Throughput and Latency per Access Path (Run 2, Firmware 4.40.0)

| Phase | Web (server REST -> WSS) | C++ local (WSS 4443) | CAN (SDO) |
|---|---|---|---|
| Single load 60 s | 323 reads, 15 writes, 57 devices; read avg 30 / p95 66 / max 112 ms; **0 errors** | 200 reads (3.3/s), 11 writes; read avg 21 / p95 26 / max 258 ms; write avg 88 ms; 0 errors | 50 191 reads (**836/s**), 11 writes; read avg 1.18 / p95 1.23 / max 10.7 ms; write avg 26.8 ms; 0 errors |
| Full load 270 s (all three in parallel) | 1402 reads, 63 writes, 252 devices, 2 toggles; read avg 41 / p95 73 / max 1318 ms; 0 errors | 880 reads, 52 writes, 2 toggles; read avg 33 / p95 49 / max 1466 ms; write avg 102 ms; 0 errors, 0 disconnects | 207 369 reads (**768/s**), 53 writes, 2 toggles; read avg 1.28 / p95 1.59 / max 35 ms; 0 errors; 273 TPDO1 |
| Churn (20x C++ connect/read/disconnect under web+CAN load) | 321 reads, 0 errors | **19/20 ok**, cycle avg 3.5 s (mTLS handshake on the ESP32); 1x `tls_handshake_failed` (socket timeout 10 s), next attempt ok | 46 556 reads (776/s), 0 errors |

Under full load the CAN path loses about 8 % throughput (836 -> 768 reads/s),
the WSS paths become ~10 ms slower on average; outliers up to 1.5 s
come from the WLAN (prototype link score 0-13).

## 2. Output (Relay) under Full Load

Switching sequence every 45 s, each step triggered by a different path; visibility
of the new `Fon_Relay_Output` value on the other paths (web observer
1 read/s, C++ 3 reads/s, CAN TPDO1 event-driven + SDO):

| Triggered by | Action | Web sees it after | C++ sees it after | CAN sees it after |
|---|---|---|---|---|
| Web `set_state On` | On | 0.9 s | 0.2 s | 0.0 s |
| C++ `set_state(Off)` | Off | 0.8 s | 0.2 s | 0.0 s |
| CAN `Fon_Pump_Command=1` | On | 0.8 s | 0.2 s | 0.0 s |
| Web `set_state Off` | Off | 0.1 s | 0.4 s | 0.0 s |
| C++ `set_state(On)` | On | 1.0 s | 0.2 s | 0.0 s |
| CAN `Fon_Pump_Command=2` | Off | 1.7 s | 1.6 s | 0.2 s |

All 6 switching operations were accepted (`applied`) and became visible on all three
paths within 1.7 s (criterion: 10 s). The CAN timestamps were
recalibrated via the relay edges because the VM clock drifts against the Pi
by ~1.2 s/min (the offset measured via SSH alone is not sufficient).

Device: `System_Uptime` monotonic (no reboot), `System_Min_Memory_Free`
constant 31 120 B, `System_Largest_Free_Block` 31 744 B (no
fragmentation), `Can_Bus_Errors` 0, `Can_Bus_Off_Count` 0.

## 3. Findings

### B1 — Firmware: `set_state On` immediately latches fault 3 (max runtime) — FIXED

In the smoke run (run 1), the web `set_state On` in state Off led
directly to `Fon_Current_State=5`, `Fon_Fault_Code=3`, EMCY 0xFF03, without
the relay ever switching on (device log: `pump state 1 -> 7`,
`pump fault [3]` in the same cycle). The command itself reported `applied`.
Via CAN (`Fon_Pump_Command`) this never occurred.

Cause (`src/device/pump_task.c` / `pump_manager.c`): the pump cycle
takes `ullNow` at the start of the cycle but only acquires the lock after the ADC
measurement and filtering. A remote request (`pump_request_on` from the cloud/local
command context) in this window stamps `pump_start_ms` with a
**newer** `now_ms()`. `pm_update` then computes `ullNow - pump_start_ms`
unsigned -> ~2^64 ms >= `max_on_ms` -> `PM_FAULT_MAX_RUNTIME`. The
CAN path (`poll_command_dps`) runs in the cycle with the same `ullNow` and
is therefore not affected. Hit probability = share of the
window in the cycle (in full run 2 it did not hit on 3 web/C++ switch-ons,
in the smoke run it hit on the first attempt).

Fix (firmware 4.40.1, not committed): `run_ms()` helper in
`pump_manager.c` (runtime without underflow, at all three locations) and
`ullNow = now_ms()` only under the lock in `pump_task.c`.
Host regression test `test_request_newer_than_cycle_time` in
`test/host/test_pump_manager.c` (old code: 3 FAIL, new: PASS; `bash
test/host/run.sh` fully green). Rolled out via OTA to FNT-000003
(10:48, download+apply 35 s); verification: run 3.

### B2 — Client framework: datapoint catalog outdated — FIXED

`fountainer_client_framework/include/fountainer/datapoints/generated.hpp`
knew 104 datapoints (firmware 4.40.0: 126; `Fon_Pump_Command` and all
`Can_*` were missing; `generate_datapoints.py --check` flagged it). Regenerated
(schema hash `0ed6ba7b…`), not committed.

### B3 — CAN sample: U64 decoded via `double` — FIXED

`Device_Serial_Number` (0x00464E5400000003) arrived as `…0004`: 64-bit
values > 2^53 get rounded in `double`. Dedicated `to_u64` path in
`canopen_sdo.cpp`; verified against `fcm read` and the web path.

### B4 — Cloud path: sporadic HTTP 504 (device does not respond within 5 s) — OPEN (environment)

In the smoke run, 2 of 85 web operations (dp_read, dp_write) returned 504 after
5 s; in the 60 s/270 s phases of the full runs 0 errors. The prototype's
`Net_Link_Score` was 0-13 (WLAN alert `link_poor`). No firmware or
server error discernible; recommendation: repeat the call (client retry) and
improve the prototype's WLAN position.

### B5 — Local path: mTLS handshake timeout under load (1/20 churn) — OPEN (known)

On the 7th of 20 connection setups under simultaneous web+CAN load
the TLS handshake aborted after 10 s (`tls/tls_handshake_failed`); the
next setup 4 s later succeeded. Matches the known behavior
"latecomer mTLS handshake fails under load";
handshake duration on the ESP32 ~3-3.8 s. No slot leak (all subsequent
connections ok).

### B6 — Observations requiring no action

- `Log_Dropped` increased during the runs (0 -> 662): overwrites in the
  RAM log ring because the server's 60 s log poller could not keep up with
  weak WLAN; no loss of error information for the test.
- `System_Min_Stack_Free` 888 B (unchanged before/after the test).
- Constant simulation pressure correctly triggers the dry-run protection
  (fault 4) after `Fon_Dry_Run_Detect_Time` (30 s) — for the test
  `Fon_Dry_Run_Min_Rise=0` was set and afterwards reset to 100.
- A `set_state Off` within `Fon_Min_On_Time` is recorded as a deadline
  (not rejected); the schedule with 45 s spacing respects the
  minimum times.

## 4. Run 3 (Firmware 4.40.1)

Same procedure as run 2, started ~2 min after the OTA reboot (uptime 105 s).

| Phase | Web | C++ local | CAN |
|---|---|---|---|
| Single load 60 s | 265 reads, 12 writes; **5x HTTP 504** (burst around 10:51:04, ~70 s after the reboot: WLAN link still coming up, link score 0); read avg 35 ms | 199 reads, avg 18 / p95 23 ms, 0 errors | 50 295 reads (838/s), avg 1.18 ms, 0 errors |
| Full load 270 s | 1410 reads, 63 writes, 2 toggles, **0 errors**; avg 38 / p95 70 / max 4442 ms | 873 reads, 52 writes, 2 toggles, 0 errors, 0 disconnects; avg 33 / p95 45 / max 4288 ms | 207 896 reads (770/s), 53 writes, 2 toggles, 0 errors, 0 EMCY |
| Churn 20x | 322 reads, 0 errors | **20/20 ok**, cycle 3.05-3.87 s | 46 654 reads (778/s), 0 errors |

Output: all 6 switching operations `applied`, visibility Web 0.1-1.1 s,
C++ 0.1-0.4 s, CAN 0.0-0.3 s — **no more spurious max-runtime faults**,
no EMCY, `Fon_Fault_Code` stayed 0 (run 1 on 4.40.0: fault 3 on the
first web `On`). Device: no reboot (uptime 105 -> 627 s), min free heap
39 104 -> 31 120 B (low-water mark during the mTLS handshakes of the churn test,
as in run 2), largest free block once transiently 29 696 B, afterwards
31 744 B again; `Can_Bus_Errors` 0.

Overall result: **passed** on 4.40.1 (criteria from PLAN.md §4). Only
the environment-related web timeouts with weak WLAN (B4) and
the known handshake outlier under load (B5, did not occur in run 3)
remain open.

