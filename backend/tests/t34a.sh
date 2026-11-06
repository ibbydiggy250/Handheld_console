#!/usr/bin/env bash
# Phase 3.4a: pairing backend, validated with curl only.
# Usage (Git Bash, from repo root):  bash backend/tests/t34a.sh
# Needs backend/sb.env and the two test users from 3.3.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/backend/sb.env"
T=$(mktemp -d)
F="$SB_URL/functions/v1"
ALPHA="23456789ABCDEFGHJKLMNPQRSTUVWXYZ"

jget() { python -c "import json,sys;d=json.load(open(sys.argv[1]));print(eval(sys.argv[2]))" "$1" "$2" 2>/dev/null; }
signin() { curl -s "$SB_URL/auth/v1/token?grant_type=password" -H "apikey: $SB_KEY" -H "Content-Type: application/json" -d "{\"email\":\"$1\",\"password\":\"$2\"}"; }
post() { local url=$1 body=$2; shift 2; curl -s -o $T/body -w "%{http_code}" -X POST "$url" -H "apikey: $SB_KEY" -H "Content-Type: application/json" "$@" -d "$body"; }

pass=0; fail=0
# check NAME RESULT DETAIL -- RESULT must be captured as r=$? right after the
# test; never put $(...) in NAME (it would reset $? -- see NOTES 3.3d).
check() { if [ "$2" = 0 ]; then echo "PASS  $1"; pass=$((pass+1)); else echo "FAIL  $1  -- $3"; fail=$((fail+1)); fi; }

signin "$SB_TEST_A_EMAIL" "$SB_TEST_A_PASS" > $T/a.json; signin "$SB_TEST_B_EMAIL" "$SB_TEST_B_PASS" > $T/b.json
TOKEN_A=$(jget $T/a.json "d['access_token']"); UID_A=$(jget $T/a.json "d['user']['id']")
TOKEN_B=$(jget $T/b.json "d['access_token']")
[ -n "$TOKEN_A" ] && [ -n "$TOKEN_B" ]; r=$?; check "both test users sign in" $r "check sb.env / passwords"

# ---- issue
c=$(post "$F/device-issue" '{}'); cp $T/body $T/issue.json
DC=$(jget $T/issue.json "d['device_code']"); UC=$(jget $T/issue.json "d['user_code']")
EXP=$(jget $T/issue.json "d['expires_in']"); INT=$(jget $T/issue.json "d['interval']")
ok_chars=0; for ((i=0;i<${#UC};i++)); do [[ "$ALPHA" == *"${UC:$i:1}"* ]] || ok_chars=1; done
[ "$c" = 200 ] && [ ${#DC} -ge 43 ] && [ ${#UC} = 6 ] && [ $ok_chars = 0 ] && [ "$EXP" = 600 ] && [ "$INT" = 5 ]; r=$?
check "issue: 43+ char device_code, 6-char user_code from the font-safe alphabet, 600 s, 5 s" $r "$c $(cat $T/body)"
echo "      user_code = $UC"

# ---- poll before approval
c=$(post "$F/device-poll" "{\"device_code\":\"$DC\"}"); st=$(jget $T/body "d.get('status')")
[ "$c" = 200 ] && [ "$st" = pending ]; r=$?; check "poll before approval -> pending" $r "$c $(cat $T/body)"

# ---- approve: negatives first
c=$(post "$F/device-approve" "{\"code\":\"$UC\"}")
[ "$c" = 401 ]; r=$?; check "approve without sign-in -> 401" $r "$c $(cat $T/body)"

c=$(post "$F/device-approve" '{"code":"0O1I23"}' -H "Authorization: Bearer $TOKEN_A")
[ "$c" = 400 ]; r=$?; check "approve with chars outside the alphabet -> 400" $r "$c $(cat $T/body)"

WRONG=$( [ "$UC" = 222222 ] && echo 333333 || echo 222222 )
c=$(post "$F/device-approve" "{\"code\":\"$WRONG\"}" -H "Authorization: Bearer $TOKEN_A")
[ "$c" = 404 ]; r=$?; check "approve a code that doesn't exist -> 404" $r "$c $(cat $T/body)"

# ---- approve: A, typed the way a person would (lowercase, dash in the middle)
TYPED="$(echo ${UC:0:3} | tr A-Z a-z)-$(echo ${UC:3:3} | tr A-Z a-z)"
c=$(post "$F/device-approve" "{\"code\":\"$TYPED\"}" -H "Authorization: Bearer $TOKEN_A")
[ "$c" = 200 ]; r=$?; check "A approves (typed as '$TYPED')" $r "$c $(cat $T/body)"

c=$(post "$F/device-approve" "{\"code\":\"$UC\"}" -H "Authorization: Bearer $TOKEN_B")
[ "$c" = 404 ]; r=$?; check "B approving the same code afterwards -> 404 (can't steal the device)" $r "$c $(cat $T/body)"

# ---- collect
c=$(post "$F/device-poll" "{\"device_code\":\"$DC\"}"); cp $T/body $T/poll.json
st=$(jget $T/poll.json "d.get('status')"); uid=$(jget $T/poll.json "d.get('user_id')")
AT=$(jget $T/poll.json "d.get('access_token')"); RT=$(jget $T/poll.json "d.get('refresh_token')")
[ "$c" = 200 ] && [ "$st" = approved ] && [ "$uid" = "$UID_A" ] && [ -n "$AT" ] && [ -n "$RT" ]; r=$?
check "poll after approval -> session for A" $r "$c $(cat $T/body)"

c=$(curl -s -o $T/body -w "%{http_code}" "$SB_URL/auth/v1/user" -H "apikey: $SB_KEY" -H "Authorization: Bearer $AT"); id=$(jget $T/body "d.get('id')")
[ "$c" = 200 ] && [ "$id" = "$UID_A" ]; r=$?; check "collected access token authenticates as A" $r "$c $id"

c=$(curl -s -o $T/body -w "%{http_code}" "$SB_URL/rest/v1/save_states?select=id" -H "apikey: $SB_KEY" -H "Authorization: Bearer $AT"); n=$(jget $T/body "len(d)")
[ "$c" = 200 ] && [ "$n" -ge 1 ]; r=$?; check "collected token reads A's saves ($n)" $r "$c $(cat $T/body)"

c=$(post "$F/device-poll" "{\"device_code\":\"$DC\"}"); st=$(jget $T/body "d.get('status')")
[ "$c" = 404 ] && [ "$st" = invalid ]; r=$?; check "second poll -> invalid (session handed out exactly once)" $r "$c $(cat $T/body)"

c=$(post "$F/device-poll" '{"device_code":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}'); st=$(jget $T/body "d.get('status')")
[ "$c" = 404 ] && [ "$st" = invalid ]; r=$?; check "poll with a made-up device_code -> invalid" $r "$c $(cat $T/body)"

# ---- the table itself is sealed
c=$(curl -s -o $T/body -w "%{http_code}" "$SB_URL/rest/v1/device_codes?select=*" -H "apikey: $SB_KEY" -H "Authorization: Bearer $TOKEN_A"); n=$(jget $T/body "len(d)")
[ "$c" = 200 ] && [ "$n" = 0 ]; r=$?; check "signed-in user reading device_codes directly -> [] (no policies)" $r "$c $(cat $T/body)"

c=$(post "$SB_URL/rest/v1/device_codes" '{"device_code_hash":"x","user_code":"ZZZZZZ","expires_at":"2099-01-01T00:00:00Z"}' -H "Authorization: Bearer $TOKEN_A")
[ "$c" != 201 ]; r=$?; check "signed-in user inserting into device_codes directly -> denied ($c)" $r "$c $(cat $T/body)"

# ---- browser preflight for the approve page
c=$(curl -s -o /dev/null -D $T/hdr -w "%{http_code}" -X OPTIONS "$F/device-approve" -H "Origin: http://localhost:5173" -H "Access-Control-Request-Method: POST")
grep -qi "access-control-allow-origin" $T/hdr; h=$?
[ "$c" = 200 ] && [ $h = 0 ]; r=$?; check "CORS preflight on device-approve" $r "$c"

echo; echo "$pass passed, $fail failed"
rm -rf $T
