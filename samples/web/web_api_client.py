#!/usr/bin/env python3
# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
"""web_api_client.py — access to a Fountainer via the REST API of the
cloud server (fountainer_server, admin web on port 8010).

The server holds the signed WSS session to the device; the REST API triggers
exactly the protocol RPCs that the buttons of the web UI use as well:

    GET  /api/devices              device list + shadow state (dp, alerts, events)
    POST /api/dp_read  {device_id, names[]}          -> dp_report
    POST /api/dp_write {device_id, dp{name: value}}  -> dp_write_result (signed)
    POST /api/command  {device_id, command, target_state?, duration_steps?}
                                                     -> command_result (signed)

Login: form POST /login (admin/admin in the testbed) sets the cookie
"session"; all /api/ calls need it (401 otherwise).

    python3 web_api_client.py --device esp32-94a990ddc1a8 devices
    python3 web_api_client.py --device esp32-94a990ddc1a8 read Fon_Current_State Fon_Relay_Output
    python3 web_api_client.py --device esp32-94a990ddc1a8 write Fon_Event_Label=3
    python3 web_api_client.py --device esp32-94a990ddc1a8 command set_state On
"""
import argparse
import json
import sys

import requests


class FountainerWebClient:
    """Thin wrapper around the server REST API (one session = one login)."""

    def __init__(self, base_url="http://SERVER_IP:8010", user="admin",
                 password="admin", timeout=30.0):
        self.base = base_url.rstrip("/")
        self.timeout = timeout
        self.http = requests.Session()
        r = self.http.post(f"{self.base}/login",
                           data={"username": user, "password": password},
                           allow_redirects=False, timeout=timeout)
        # Success = redirect to "/" with session cookie; failure = redirect to /login?error=1
        if r.status_code != 302 or "session" not in self.http.cookies:
            raise RuntimeError(f"Login failed ({r.status_code})")

    # ---- Reading ----------------------------------------------------------
    def devices(self):
        """All registered devices incl. online flag and dp shadow."""
        r = self.http.get(f"{self.base}/api/devices", timeout=self.timeout)
        r.raise_for_status()
        return r.json()["devices"]

    def device(self, device_id):
        for d in self.devices():
            if d["device_id"] == device_id:
                return d
        raise KeyError(device_id)

    def dp_read(self, device_id, names):
        """Fresh dp_read on the device (not the shadow). Empty list =
        full snapshot. Returns {name: value}."""
        return self._post("/api/dp_read", {"device_id": device_id,
                                           "names": list(names)})["dp"]

    # ---- Writing / control (server-signed, scope=control) -----------------
    def dp_write(self, device_id, **values):
        """Write config datapoints, atomic batch. Result contains
        status ("applied"/"rejected"), errors{} and readback{}."""
        return self._post("/api/dp_write", {"device_id": device_id, "dp": values})

    def command(self, device_id, command, target_state=None, duration_steps=None):
        """set_state On|Off|Auto|Manual, turn_on_duration (30 s steps),
        restart, reboot. Result: status applied/rejected (+ error)."""
        body = {"device_id": device_id, "command": command}
        if target_state is not None:
            body["target_state"] = target_state
        if duration_steps is not None:
            body["duration_steps"] = int(duration_steps)
        return self._post("/api/command", body)

    def set_state(self, device_id, state):
        return self.command(device_id, "set_state", target_state=state)

    # ---- internal ---------------------------------------------------------
    def _post(self, path, body):
        r = self.http.post(f"{self.base}{path}", json=body, timeout=self.timeout)
        # 409 = device not connected, 504 = device did not respond,
        # 400 = parameter error. The body is always JSON with "error".
        data = r.json()
        if r.status_code != 200 or not data.get("ok", False):
            raise RuntimeError(f"{path}: HTTP {r.status_code} {data.get('error')}")
        return data["result"]


def _parse_value(text):
    try:
        return json.loads(text)          # 1, 2.5, true, "text"
    except json.JSONDecodeError:
        return text


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--url", default="http://SERVER_IP:8010")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    ap.add_argument("--device", default="esp32-94a990ddc1a8")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("devices")
    p = sub.add_parser("read"); p.add_argument("names", nargs="*")
    p = sub.add_parser("write"); p.add_argument("assign", nargs="+", metavar="NAME=VALUE")
    p = sub.add_parser("command"); p.add_argument("command")
    p.add_argument("arg", nargs="?", help="target_state or duration_steps")
    args = ap.parse_args()

    c = FountainerWebClient(args.url, args.user, args.password)
    if args.cmd == "devices":
        for d in c.devices():
            print(f"{d['device_id']}  serial={d['serial']}  online={d['online']}  "
                  f"fw={d['fw_version']}  uptime={d['uptime_s']}  fault={d['fault_active']}")
    elif args.cmd == "read":
        print(json.dumps(c.dp_read(args.device, args.names), indent=1, sort_keys=True))
    elif args.cmd == "write":
        values = {k: _parse_value(v) for k, _, v in (a.partition("=") for a in args.assign)}
        print(json.dumps(c.dp_write(args.device, **values), indent=1))
    elif args.cmd == "command":
        kw = {}
        if args.command == "set_state":
            kw["target_state"] = args.arg
        elif args.command == "turn_on_duration":
            kw["duration_steps"] = args.arg
        print(json.dumps(c.command(args.device, args.command, **kw), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
