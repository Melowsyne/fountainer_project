#!/usr/bin/env bash
# Runs a built program in the client image with the testbed PKI and HMAC keys.
#   bash run_docker.sh local_access /proj/samples/cpp_local/client.fnt-000003.json [on|off]
#   bash run_docker.sh stress_local /proj/samples/cpp_local/client.fnt-000003.json 60 --jsonl /proj/stresstest/results/cpp.jsonl
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WS="$(cd "$HERE/../../.." && pwd)"
BIN="$1"; shift
docker run --rm --entrypoint "/build/$BIN" \
    -v "$WS/fountainer_client_framework:/app:ro" \
    -v "$WS/fountainer_project:/proj" \
    -v fproj_build:/build \
    -v "$WS/DO_NOT_COMMIT/CA:/certs:ro" \
    -v "$WS/DO_NOT_COMMIT/production_secrets:/secrets:ro" \
    fountainer_client "$@"
