#!/usr/bin/env bash
# Kills any prior echo_server, rebuilds the project, starts the server,
# runs the client once, then cleans up the server.
#
# Usage: ./scripts/run_echo_demo.sh ["message to echo"]

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Kill any leftover echo_server from a previous run so bind on 9000 succeeds.
pkill -f 'build/echo_server' 2>/dev/null || true

# One-time configure. Preserve any flags you passed on first configure
# (e.g. -DCMAKE_CXX_COMPILER=g++-14) by only running if build/ is absent.
if [[ ! -d build ]]; then
    cmake -S . -B build
fi

cmake --build build

./build/echo_server &
SERVER_PID=$!

# Ensure the server gets killed even if the client fails.
trap 'kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true' EXIT

# Give the server a moment to bind before the client sends.
sleep 0.2

MESSAGE="${1:-hello world}"
./build/echo_client "$MESSAGE"
