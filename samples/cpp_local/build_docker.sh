#!/usr/bin/env bash
# Builds local_access + stress_local in the client framework's Docker image
# (fountainer_client: Boost/OpenSSL/nlohmann available). The build folder is the
# named volume fproj_build (a CIFS share loses exec bits).
#   bash build_docker.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WS="$(cd "$HERE/../../.." && pwd)"
docker run --rm --entrypoint bash \
    -v "$WS/fountainer_client_framework:/app" \
    -v "$WS/fountainer_project:/proj" \
    -v fproj_build:/build \
    fountainer_client -c "
        cmake -S /proj/samples/cpp_local -B /build -DFOUNTAINER_FRAMEWORK_DIR=/app -DCMAKE_BUILD_TYPE=Release >/dev/null &&
        cmake --build /build -j\$(nproc) --target local_access stress_local"
