#!/usr/bin/env bash
# Phase 3.4a: code expiry. Takes ~10.5 minutes -- run in the background.
# Usage (Git Bash, from repo root):  bash backend/tests/t34a_expiry.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/backend/sb.env"
T=$(mktemp -d)
F="$SB_URL/functions/v1"

jget() { python -c "import json,sys;d=json.load(open(sys.argv[1]));print(eval(sys.argv[2]))" "$1" "$2" 2>/dev/null; }
post() { local url=$1 body=$2; shift 2; curl -s -o $T/body -w "%{http_code}" -X POST "$url" -H "apikey: $SB_KEY" -H "Content-Type: application/json" "$@" -d "$body"; }
pass=0; fail=0
check() { if [ "$2" = 0 ]; then echo "PASS  $1"; pass=$((pass+1)); else echo "FAIL  $1  -- $3"; fail=$((fail+1)); fi; }

TOKEN_A=$(curl -s "$SB_URL/auth/v1/token?grant_type=password" -H "apikey: $SB_KEY" -H "Content-Type: application/json" \
  -d "{\"email\":\"$SB_TEST_A_EMAIL\",\"password\":\"$SB_TEST_A_PASS\"}" > $T/a.json; jget $T/a.json "d['access_token']")

c=$(post "$F/device-issue" '{}'); DC=$(jget $T/body "d['device_code']"); UC=$(jget $T/body "d['user_code']")
[ "$c" = 200 ]; r=$?; check "issue ($UC)" $r "$c"

c=$(post "$F/device-poll" "{\"device_code\":\"$DC\"}"); st=$(jget $T/body "d.get('status')")
[ "$st" = pending ]; r=$?; check "pending at t=0" $r "$c $(cat $T/body)"

echo "waiting 610 s for the code to expire..."
python -c "import time; time.sleep(610)"

c=$(post "$F/device-poll" "{\"device_code\":\"$DC\"}"); st=$(jget $T/body "d.get('status')")
[ "$c" = 410 ] && [ "$st" = expired ]; r=$?; check "poll after 610 s -> expired (410)" $r "$c $(cat $T/body)"

c=$(post "$F/device-approve" "{\"code\":\"$UC\"}" -H "Authorization: Bearer $TOKEN_A")
[ "$c" = 404 ]; r=$?; check "approve after expiry -> 404" $r "$c $(cat $T/body)"

echo; echo "$pass passed, $fail failed"
rm -rf $T
