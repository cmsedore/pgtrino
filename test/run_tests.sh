#!/usr/bin/env bash
# Run the trino_fdw regression suite against the mock Trino server.
#
# Requires: the extension installed (make install) and a running PostgreSQL
# that the current user can connect to as a superuser (PGHOST/PGUSER/... are
# honoured, as for any libpq client).
set -euo pipefail

cd "$(dirname "$0")/.."
PORT=18080

python3 -I test/mock_trino.py "$PORT" > test/mock_trino.log 2>&1 &
MOCK_PID=$!
# a second instance that answers with spooled-protocol segments
python3 -I test/mock_trino.py "$((PORT + 1))" spooling > test/mock_trino_spooling.log 2>&1 &
MOCK2_PID=$!
trap 'kill $MOCK_PID $MOCK2_PID 2>/dev/null || true' EXIT

# wait for the mock to accept connections
for _ in $(seq 1 50); do
    if curl -s -o /dev/null -X POST -H 'X-Trino-User: probe' --data 'SELECT 1' \
            "http://127.0.0.1:$PORT/v1/statement"; then
        break
    fi
    sleep 0.1
done
# the probe above is logged by the mock; consume it so the suite starts clean
curl -s -o /dev/null -X POST -H 'X-Trino-User: probe' --data 'SELECT sql FROM sqllog' \
    "http://127.0.0.1:$PORT/v1/statement"

make installcheck "$@"
