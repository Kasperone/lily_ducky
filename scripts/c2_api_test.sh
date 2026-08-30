#!/usr/bin/env bash
# =============================================================================
# c2_api_test.sh — LilyDucky WiFi C2 end-to-end test suite
# =============================================================================
# Run from any machine JOINED to the dongle's SoftAP:
#   ssid: LilyC2   pass: quackquack
#
# Usage:
#   ./scripts/c2_api_test.sh [HOST_IP] [TOKEN] [HELLO_DD_PATH]
# Defaults: 192.168.4.1, prompts for token, payloads/hello.dd next to repo.
#
# Covers plan Tasks 11, 12, 14:
#   status, payload PUT/GET round-trip, list, auth-negative (401),
#   run-to-completion, mid-run stop, DETECT_OS cooperative pause with
#   C2 responsiveness check during the detection window.
# =============================================================================
set -u

IP="${1:-192.168.4.1}"
TOKEN="${2:-}"
HELLO="${3:-$(dirname "$0")/../payloads/hello.dd}"
BASE="http://$IP"

if [ -z "$TOKEN" ]; then
    echo "Usage: $0 <ip> <token> [hello.dd path]"
    echo "Token is printed on the serial console at boot."
    exit 2
fi

PASS=***
FAIL=0

check() {  # check <name> <condition-result: 0=ok>
    if [ "$2" -eq 0 ]; then
        echo "  [PASS] $1"
        PASS=***
    else
        echo "  [FAIL] $1"
        FAIL=$((FAIL+1))
    fi
}

curlj() { curl -s -m 5 "$@"; }

echo "== LilyDucky C2 test suite against $BASE =="

# ── 1. status endpoint ──────────────────────────────────────────────────────
echo "[1] GET /api/status"
S=$(curlj "$BASE/api/status")
echo "    $S"
echo "$S" | grep -q '"state"' && echo "$S" | grep -q '"ap_ip":"192.168.4.1"'
check "status returns JSON with state+ap_ip" $?

# ── 2. upload payload (auth via header) ────────────────────────────────────
echo "[2] PUT /api/payload/payload.dd (hello.dd)"
if [ ! -f "$HELLO" ]; then
    echo "    hello.dd not found at $HELLO — aborting"
    exit 3
fi
R=$(curlj -X PUT -H "X-Auth-Token: $TOKEN" --data-binary "@$HELLO" \
    "$BASE/api/payload/payload.dd")
[ "$R" = "OK" ]
check "PUT returns OK" $?

# ── 3. round-trip read-back ─────────────────────────────────────────────────
echo "[3] GET /api/payload/payload.dd round-trip"
curlj "$BASE/api/payload/payload.dd" > /tmp/c2_roundtrip.dd
cmp -s "$HELLO" /tmp/c2_roundtrip.dd
check "downloaded payload identical to source" $?

# ── 4. payload listing ──────────────────────────────────────────────────────
echo "[4] GET /api/payloads"
L=$(curlj "$BASE/api/payloads")
echo "    $L"
echo "$L" | grep -q 'payload.dd'
check "payload list contains payload.dd" $?

# ── 5. negative auth: PUT without token must be rejected ───────────────────
echo "[5] PUT without token"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 5 -X PUT \
    --data-binary 'EVIL' "$BASE/api/payload/evil.dd")
echo "    HTTP $CODE"
[ "$CODE" = "401" ]
check "unauthenticated PUT rejected with 401" $?

# ── 6. run to completion ────────────────────────────────────────────────────
echo "[6] POST /api/run/payload.dd → poll until complete"
R=$(curlj -X POST -H "X-Auth-Token: $TOKEN" "$BASE/api/run/payload.dd")
[ "$R" = "Running" ]
check "run returns Running" $?
STATE=""
for i in $(seq 1 20); do
    sleep 0.5
    STATE=$(curlj "$BASE/api/status" | sed -n 's/.*"state":"\([a-z]*\)".*/\1/p')
    case "$STATE" in
        complete|error|stopped|idle) break ;;
    esac
done
echo "    final state: $STATE"
[ "$STATE" = "complete" ]
check "hello.dd ran to completion (typing is no-op on C5)" $?

# ── 7. mid-run stop ─────────────────────────────────────────────────────────
echo "[7] run again, stop within ~300 ms"
curlj -X POST -H "X-Auth-Token: $TOKEN" "$BASE/api/run/payload.dd" >/dev/null
sleep 0.3
R=$(curlj -X POST -H "X-Auth-Token: $TOKEN" "$BASE/api/stop")
[ "$R" = "Stopped" ]
check "stop returns Stopped" $?
sleep 0.5
STATE=$(curlj "$BASE/api/status" | sed -n 's/.*"state":"\([a-z]*\)".*/\1/p')
echo "    state after stop: $STATE"
[ "$STATE" = "stopped" ]
check "status reports stopped" $?

# ── 8. DETECT_OS payload + C2 responsiveness during detection ──────────────
echo "[8] DETECT_OS payload: cooperative pause, C2 must stay responsive"
cat > /tmp/c2_detect.dd <<'EOF'
REM DETECT_OS test for the C5 lab node
DETECT_OS
STRINGLN os detection done
EOF
R=$(curlj -X PUT -H "X-Auth-Token: $TOKEN" --data-binary @/tmp/c2_detect.dd \
    "$BASE/api/payload/detect.dd")
[ "$R" = "OK" ]
check "PUT detect.dd returns OK" $?

curlj -X POST -H "X-Auth-Token: $TOKEN" "$BASE/api/run/detect.dd" >/dev/null
# During the 3 s detection window the server MUST still answer fast.
SLOW=0
for i in 1 2 3 4; do
    T=$(curl -s -o /dev/null -m 3 -w '%{time_total}' "$BASE/api/status")
    echo "    poll $i: /api/status answered in ${T}s"
    OK=$(awk -v t="$T" 'BEGIN{print (t<1.0)?0:1}')
    SLOW=$((SLOW+OK))
    sleep 0.6
done
[ "$SLOW" -eq 0 ]
check "C2 responsive during detection window (all answers < 1 s)" $?

STATE=""
for i in $(seq 1 12); do
    sleep 0.5
    STATE=$(curlj "$BASE/api/status" | sed -n 's/.*"state":"\([a-z]*\)".*/\1/p')
    case "$STATE" in
        complete|error|stopped|idle) break ;;
    esac
done
echo "    final state: $STATE"
[ "$STATE" = "complete" ]
check "detect.dd ran to completion (serial should show \$_OS=0)" $?

# ── summary ─────────────────────────────────────────────────────────────────
echo
echo "== RESULT: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
