#!/usr/bin/env bash
# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
# Fountainer via the server REST API with curl (same as web_api_client.py).
#   bash web_api_curl.sh [server-url] [device_id]
set -euo pipefail
URL="${1:-http://SERVER_IP:8010}"
DEV="${2:-esp32-94a990ddc1a8}"
CJ="$(mktemp)"; trap 'rm -f "$CJ"' EXIT

# 1) Login -> session cookie (302 to "/" on success)
curl -s -c "$CJ" -o /dev/null -w "login: HTTP %{http_code}\n" \
     -d "username=admin&password=admin" "$URL/login"

# 2) Device list with shadow state
curl -s -b "$CJ" "$URL/api/devices" | python3 -c '
import json,sys
for d in json.load(sys.stdin)["devices"]:
    print(d["device_id"], "online=%s" % d["online"], "fw=%s" % d["fw_version"])'

# 3) Fresh dp_read on the device
curl -s -b "$CJ" -H "Content-Type: application/json" "$URL/api/dp_read" \
     -d "{\"device_id\":\"$DEV\",\"names\":[\"Fon_Current_State\",\"Fon_Relay_Output\",\"Fon_Current_Pressure\"]}"
echo

# 4) Write a config datapoint (server-signed dp_write)
curl -s -b "$CJ" -H "Content-Type: application/json" "$URL/api/dp_write" \
     -d "{\"device_id\":\"$DEV\",\"dp\":{\"Fon_Event_Label\":0}}"
echo

# 5) Control the pump (server-signed command). Others: Off|Auto|Manual,
#    "restart", "reboot", "turn_on_duration" + duration_steps (x30 s)
# curl -s -b "$CJ" -H "Content-Type: application/json" "$URL/api/command" \
#      -d "{\"device_id\":\"$DEV\",\"command\":\"set_state\",\"target_state\":\"On\"}"
