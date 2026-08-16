#!/usr/bin/env bash
# tests/test_pax_option.sh — --pax-option=delete=KEYWORD (Phase B / G1)
#
# Verifies that when creating a POSIX/PAX archive with
#   --pax-option=delete=uname --pax-option=delete=gname
# the extended header payload does not contain uname=/gname= records,
# while a baseline create without those options does emit them.
#
# Usage:
#   bash test_pax_option.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL + 1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

WORK="$(mktemp -d /tmp/mutar_pax_opt.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Long name forces a PAX extended header (path keyword) under --posix.
# That path also causes mutar to emit uname/gname in the same 'x' header.
LONG_NAME="$(printf 'paxopt_%0120d' 1)"
mkdir -p "$WORK/src"
printf 'pax-option payload\n' >"$WORK/src/$LONG_NAME"
# Stable owner names so strings(1) has something to match when not deleted.
OWNER_NAME="paxowner"
GROUP_NAME="paxgroup"

BASE_TAR="$WORK/base.tar"
DEL_TAR="$WORK/deleted.tar"

echo "=== --pax-option delete=uname,gname ==="

# Baseline: PAX header should mention uname= / gname= (owner override forces names)
"$MUTAR" --posix -cf "$BASE_TAR" \
  --owner="$OWNER_NAME" --group="$GROUP_NAME" \
  -C "$WORK/src" "$LONG_NAME"

if strings "$BASE_TAR" | grep -q "uname=${OWNER_NAME}"; then
  pass "baseline archive contains uname=${OWNER_NAME} in PAX data"
else
  fail "baseline uname" "expected uname=${OWNER_NAME} in strings of $BASE_TAR"
fi
if strings "$BASE_TAR" | grep -q "gname=${GROUP_NAME}"; then
  pass "baseline archive contains gname=${GROUP_NAME} in PAX data"
else
  fail "baseline gname" "expected gname=${GROUP_NAME} in strings of $BASE_TAR"
fi
if strings "$BASE_TAR" | grep -q "path="; then
  pass "baseline archive contains path= in PAX data"
else
  fail "baseline path" "expected path= keyword for long name"
fi

# With delete=: uname/gname must be absent from extended header data
"$MUTAR" --posix -cf "$DEL_TAR" \
  --owner="$OWNER_NAME" --group="$GROUP_NAME" \
  --pax-option=delete=uname \
  --pax-option=delete=gname \
  -C "$WORK/src" "$LONG_NAME"

if strings "$DEL_TAR" | grep -q "uname=${OWNER_NAME}"; then
  fail "delete uname" "uname=${OWNER_NAME} still present after --pax-option=delete=uname"
else
  pass "delete=uname suppresses uname= in PAX data"
fi
if strings "$DEL_TAR" | grep -q "gname=${GROUP_NAME}"; then
  fail "delete gname" "gname=${GROUP_NAME} still present after --pax-option=delete=gname"
else
  pass "delete=gname suppresses gname= in PAX data"
fi
# path must still be present (not deleted)
if strings "$DEL_TAR" | grep -q "path="; then
  pass "path= still emitted when only uname/gname deleted"
else
  fail "path retained" "path= missing after delete=uname,gname"
fi

# Comma-separated form in a single --pax-option
COMMA_TAR="$WORK/comma.tar"
"$MUTAR" --posix -cf "$COMMA_TAR" \
  --owner="$OWNER_NAME" --group="$GROUP_NAME" \
  --pax-option=delete=uname,delete=gname \
  -C "$WORK/src" "$LONG_NAME"
if strings "$COMMA_TAR" | grep -Eq "uname=${OWNER_NAME}|gname=${GROUP_NAME}"; then
  fail "comma form" "uname/gname still present with --pax-option=delete=uname,delete=gname"
else
  pass "comma-separated delete=uname,delete=gname works"
fi

# delete= with empty keyword is ignored (must not crash; uname still emitted)
EMPTY_TAR="$WORK/empty_del.tar"
"$MUTAR" --posix -cf "$EMPTY_TAR" \
  --owner="$OWNER_NAME" --group="$GROUP_NAME" \
  --pax-option=delete= \
  -C "$WORK/src" "$LONG_NAME"
if strings "$EMPTY_TAR" | grep -q "uname=${OWNER_NAME}"; then
  pass "empty delete= is ignored (uname still present)"
else
  fail "empty delete=" "unexpectedly suppressed uname"
fi

# Round-trip extract still works with deleted keywords (names live in ustar fields)
EXDIR="$WORK/extract"
mkdir -p "$EXDIR"
"$MUTAR" -xf "$DEL_TAR" -C "$EXDIR"
if [ -f "$EXDIR/$LONG_NAME" ]; then
  pass "extract succeeds after delete=uname,gname"
else
  fail "extract" "missing $LONG_NAME after extract"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
exit 0
