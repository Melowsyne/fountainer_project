# CAN Signals on the Fountainer — Oscilloscope Captures (2026-09-23)

Setup: Fountainer Rev. 1 (FNT-000003, ESP32-S3 TWAI + TJA1051T/3) ↔ Raspberry Pi
PI_HOST (MCP2515 + MCP2562), 250 kbit/s, both ends terminated with 120 Ω.
Tektronix MSO2002B via USBTMC on the Pi: CH1 = CAN_L, CH2 = CAN_H, ground at
board GND, 1 V/div, 100 000 samples (4–20 ns). The raw waveforms were
downloaded, decoded bit by bit in Python (sampling at bit center,
stuff-bit removal, CRC-15 checked, ACK evaluated) and annotated with fields,
values and levels (`tools/render_can.py`, captured with
`tools/scope_capture.py`).

| Image | Contents |
|---|---|
| `img/can_01_heartbeat_frame.png` | Complete heartbeat frame 0x703, DLC 1, data 0x05 = Operational (55 bits incl. 3 stuff bits, 220 µs). Levels CAN_H 2560 → 3720 mV, CAN_L 2520 → 1480 mV, V_diff 40 → 2240 mV. ACK from the Pi visible in the ACK slot. |
| `img/can_02_bit_level_sof_identifier.png` | Zoom on SOF and identifier: individual 4.00 µs bits as a square wave, bit values and field boundaries. |
| `img/can_03_tpdo1_frame.png` | Process data TPDO1 0x183, DLC 7: pressure 2.50 bar (float LE `00 00 20 40`), state 5 (Fault), relay off, fault code 1 — 110 bits, 440 µs. |
| `img/can_03c_tpdo3_frame.png` | TPDO3 0x383: uptime and pump runtime (2 × UINT32). |
| `img/can_04_sdo_request_response.png` | SDO traffic under `fcm bench`: request from the master 0x603 (upload 0x200B System_Uptime) and response from the slave 0x583 with value; response time from end of frame to response SOF 48 µs. The Pi drives CAN_H to 3600 mV, the ESP32 to 3720 mV. |
| `img/can_05_sdo_request_frame.png` | SDO request 0x603 in detail (8 data bytes, command 0x40, index/subindex). |
| `img/can_06_sdo_response_frame.png` | SDO response 0x583 in detail (0x43, index, 4 data bytes = Fon_Current_Pressure 2.50 bar). |
| `img/scope_original/*.png` | Unedited screenshots of the oscilloscope (480×240): whole frame, bit level, difference (MATH CH2−CH1), request/response, bus traffic 2 ms/div, edges 10 µs/div. |

Key figures from the captures: bit time 4.00 µs (250 kbit/s, deviation < 0.1 %),
dominant differential voltage 2.1–2.2 V, recessive 40 mV, clean edges without
overshoot, no CRC errors in any of the decoded frames.
