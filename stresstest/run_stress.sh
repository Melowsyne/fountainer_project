#!/usr/bin/env bash
# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
#
# run_stress.sh — runs the stress test from PLAN.md (phases P0..P6) and
# stores all raw data under results/<timestamp>/.
#
#   bash run_stress.sh [--quick]           # --quick: shortened phases (smoke)
#
# Prerequisites: C++ programs built (samples/cpp_local/build_docker.sh),
# canopen_sdo on the Pi under ~/fountainer_project/canopen_cpp, SSH access
# to the Pi (PI_SSH/PI_SCP overridable, default: ssh/scp pi@PI_IP).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
WEB="$PROJ/samples/web/web_api_client.py"
RUN="$PROJ/samples/cpp_local/run_docker.sh"
CFG="/proj/samples/cpp_local/client.fnt-000003.json"
DEV="${DEVICE:-esp32-94a990ddc1a8}"
URL="${SERVER_URL:-http://localhost:8010}"
PI_SSH="${PI_SSH:-ssh pi@PI_IP}"
PI_SCP="${PI_SCP:-scp}"
PI_HOST="${PI_HOST:-pi@PI_IP}"
PI_BIN="~/fountainer_project/canopen_cpp/canopen_sdo"

[ "$DEV" = "esp32-a1b2c3d4e5f6" ] && { echo "production pump is off-limits"; exit 2; }

P1=60; P2=60; P3=60; P4=270; P5_CHURN=20; P5=60
if [ "${1:-}" = "--quick" ]; then P1=15; P2=15; P3=15; P4=60; P5_CHURN=3; P5=15; fi
TOG_WEB="0:On,135:Off"; TOG_CPP="45:off,180:on"; TOG_CAN="90:on,225:off"
[ "${1:-}" = "--quick" ] && { TOG_WEB="0:On"; TOG_CPP="10:off"; TOG_CAN="20:on"; }

OUT="$HERE/results/$(date +%Y-%m-%d_%H%M)"
mkdir -p "$OUT"; cd "$OUT"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a run.log; }
web() { python3 "$WEB" --url "$URL" --device "$DEV" "$@"; }
snapshot() {   # $1 = file name
    web read System_Uptime System_Min_Memory_Free System_Largest_Free_Block System_Min_Stack_Free \
        Fon_Current_State Fon_Relay_Output Fon_Fault_Code Can_Bus_Errors Can_Bus_Off_Count \
        Can_Sdo_Count Can_Rx_Frames Can_Tx_Frames Can_Emcy_Count Net_Link_Score Log_Dropped \
        Fon_Pressure_Manual > "$1" 2>&1 || log "snapshot $1 ERROR"
}

log "Stress test -> $OUT  (device $DEV)"

# ---- Clock offset VM - Pi (Pi timestamps are corrected in analyze.py) --------
T0=$(date +%s%3N); TP=$($PI_SSH 'date +%s%3N'); T1=$(date +%s%3N)
echo $(( (T0 + T1) / 2 - TP )) > pi_offset_ms
log "Pi clock offset: $(cat pi_offset_ms) ms (RTT $((T1 - T0)) ms)"

# ---- P0 Baseline ---------------------------------------------------------------
log "P0 Baseline"
snapshot p0_baseline.json
$PI_SSH "$PI_BIN info" > p0_can_info.txt 2>&1 || log "P0 can info ERROR"
cat p0_baseline.json | tr -d '\n' | tee -a run.log; echo

# ---- Output preparation: pressure simulation, acknowledge faults, Manual -----------
log "Preparation: Fon_Pressure_Manual=1, Fon_Pressure_Value=2.5, Dry_Run_Min_Rise=0, Fault_Ack, set_state Manual"
# Fon_Dry_Run_Min_Rise=0: otherwise the constant simulated pressure curve would be
# latched as dry run (fault 4) after Fon_Dry_Run_Detect_Time (30 s).
web write Fon_Pressure_Manual=true Fon_Pressure_Value=2.5 Fon_Dry_Run_Min_Rise=0 > prep_1.json 2>&1
sleep 1
web write Fon_Fault_Ack=1 > prep_2.json 2>&1
sleep 1
web command set_state Manual > prep_3.json 2>&1
sleep 2
web read Fon_Current_State Fon_Relay_Output Fon_Fault_Code | tr -d '\n' | tee -a run.log; echo

# ---- P1 Web alone -------------------------------------------------------------------
log "P1 web-only load ${P1}s"
python3 "$HERE/stress_web.py" --url "$URL" --device "$DEV" --seconds $P1 --workers 3 \
    --jsonl web_p1.jsonl > p1_web.summary.json 2>&1; log "P1 rc=$?"

# ---- P2 C++ local alone -------------------------------------------------------------------
log "P2 C++-only load ${P2}s"
bash "$RUN" stress_local "$CFG" $P2 --jsonl /proj/stresstest/results/$(basename "$OUT")/cpp_p2.jsonl \
    > p2_cpp.summary.json 2>&1; log "P2 rc=$?"

# ---- P3 CAN alone -----------------------------------------------------------------------------
log "P3 CAN-only load ${P3}s"
$PI_SSH "cd ~/fountainer_project/canopen_cpp && rm -f can_p3.jsonl && ./canopen_sdo bench $P3 --jsonl can_p3.jsonl" \
    > p3_can.summary.json 2>&1; log "P3 rc=$?"
$PI_SCP "$PI_HOST:~/fountainer_project/canopen_cpp/can_p3.jsonl" . 2>/dev/null

# ---- P4 Full load + output -----------------------------------------------------------------------
log "P4 full load ${P4}s with output schedule (Web $TOG_WEB | C++ $TOG_CPP | CAN $TOG_CAN)"
$PI_SSH "cd ~/fountainer_project/canopen_cpp && rm -f can_p4.jsonl && ./canopen_sdo bench $P4 --jsonl can_p4.jsonl --toggle $TOG_CAN" \
    > p4_can.summary.json 2>&1 &
PID_CAN=$!
bash "$RUN" stress_local "$CFG" $P4 --jsonl /proj/stresstest/results/$(basename "$OUT")/cpp_p4.jsonl --toggle "$TOG_CPP" \
    > p4_cpp.summary.json 2>&1 &
PID_CPP=$!
sleep 4                                   # C++/CAN are connected, then web with t=0 toggle
python3 "$HERE/stress_web.py" --url "$URL" --device "$DEV" --seconds $P4 --workers 3 \
    --jsonl web_p4.jsonl --toggle "$TOG_WEB" > p4_web.summary.json 2>&1; log "P4 web rc=$?"
wait $PID_CPP; log "P4 cpp rc=$?"
wait $PID_CAN; log "P4 can rc=$?"
$PI_SCP "$PI_HOST:~/fountainer_project/canopen_cpp/can_p4.jsonl" . 2>/dev/null

# ---- P5 Churn C++ under Web+CAN load -------------------------------------------------------------
log "P5 Churn ${P5_CHURN}x C++ connect/disconnect under web+CAN load (${P5}s)"
$PI_SSH "cd ~/fountainer_project/canopen_cpp && rm -f can_p5.jsonl && ./canopen_sdo bench $P5 --jsonl can_p5.jsonl" \
    > p5_can.summary.json 2>&1 &
PID_CAN=$!
python3 "$HERE/stress_web.py" --url "$URL" --device "$DEV" --seconds $P5 --workers 3 \
    --jsonl web_p5.jsonl > p5_web.summary.json 2>&1 &
PID_WEB=$!
sleep 2
bash "$RUN" stress_local "$CFG" 0 --churn $P5_CHURN --jsonl /proj/stresstest/results/$(basename "$OUT")/cpp_p5.jsonl \
    > p5_cpp.summary.json 2>&1; log "P5 churn rc=$?"
wait $PID_WEB; wait $PID_CAN
$PI_SCP "$PI_HOST:~/fountainer_project/canopen_cpp/can_p5.jsonl" . 2>/dev/null

# ---- P6 Wind-down + cleanup ----------------------------------------------------------------------------
log "P6 Post-run"
web command set_state Off > p6_off.json 2>&1
sleep 2
snapshot p6_after.json
cat p6_after.json | tr -d '\n' | tee -a run.log; echo
web write Fon_Pressure_Manual=false Fon_Dry_Run_Min_Rise=100 > p6_cleanup.json 2>&1
$PI_SSH "$PI_BIN nmt preop" >/dev/null 2>&1
$PI_SSH "$PI_BIN read Fon_Current_State Fon_Relay_Output Can_Bus_Errors" >> run.log 2>&1

python3 "$HERE/analyze.py" "$OUT" > analysis.md 2>&1
log "done: $OUT/analysis.md"
