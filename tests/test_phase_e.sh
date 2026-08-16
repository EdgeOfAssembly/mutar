#!/usr/bin/env bash
# tests/test_phase_e.sh — GOAL_NEXT Phase E (G10–G15 polish)
# Covers: --restrict, --backup CONTROL (none/simple/numbered), --quoting-style,
#         --check-device flag parse, listed-incremental snapshot dirs+dev.
#
# Usage: ./test_phase_e.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TMPDIR_BASE="$(mktemp -d /tmp/mutar_phase_e.XXXXXX)"
trap 'rm -rf "$TMPDIR_BASE"' EXIT

PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

T() {
    local name="$1"; shift
    echo "[$name]"
    "$@"
}

if [[ ! -x "$MUTAR" ]]; then
    echo "mutar binary not found/executable: $MUTAR" >&2
    exit 1
fi

GROUND="$TMPDIR_BASE/ground"
mkdir -p "$GROUND/subdir"
echo "hello" > "$GROUND/file1.txt"
echo "world" > "$GROUND/file with spaces.txt"
echo "nested" > "$GROUND/subdir/nested.txt"

# ── G13 --restrict ────────────────────────────────────────────────────────────

T "T-E-01: --restrict alone is accepted" \
bash -c "
  ARCH=\"$TMPDIR_BASE/r1.tar\"
  \"$MUTAR\" --restrict -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  [ -f \"\$ARCH\" ]
" && pass "T-E-01 restrict alone" || fail "T-E-01 restrict alone" "create failed"

T "T-E-02: --restrict rejects -P / --absolute-names" \
bash -c "
  ARCH=\"$TMPDIR_BASE/r2.tar\"
  set +e
  OUT=\$(\"$MUTAR\" --restrict -c -P -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  RC=\$?
  set -e
  [ \"\$RC\" -ne 0 ] || { echo \"expected non-zero, got \$RC\"; echo \"\$OUT\"; exit 1; }
  echo \"\$OUT\" | grep -qi 'restrict' || { echo \"no restrict message: \$OUT\"; exit 1; }
  echo \"  rejected -P with: \$(echo \"\$OUT\" | head -1)\"
" && pass "T-E-02 restrict vs -P" || fail "T-E-02 restrict vs -P" "did not reject"

T "T-E-03: --restrict rejects --to-command" \
bash -c "
  ARCH=\"$TMPDIR_BASE/r3.tar\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  set +e
  OUT=\$(\"$MUTAR\" --restrict -x -f \"\$ARCH\" --to-command='cat >/dev/null' -C \"$TMPDIR_BASE/out3\" 2>&1)
  RC=\$?
  set -e
  [ \"\$RC\" -ne 0 ] || { echo \"expected non-zero\"; echo \"\$OUT\"; exit 1; }
  echo \"\$OUT\" | grep -qi 'to-command\|restrict' || { echo \"\$OUT\"; exit 1; }
" && pass "T-E-03 restrict vs to-command" || fail "T-E-03 restrict vs to-command" "did not reject"

T "T-E-04: --restrict rejects multi-volume (-M)" \
bash -c "
  ARCH=\"$TMPDIR_BASE/r4.tar\"
  set +e
  OUT=\$(\"$MUTAR\" --restrict -c -M -L 1 -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  RC=\$?
  set -e
  [ \"\$RC\" -ne 0 ] || { echo \"expected non-zero\"; echo \"\$OUT\"; exit 1; }
  echo \"\$OUT\" | grep -qi 'restrict\|multi-volume' || { echo \"\$OUT\"; exit 1; }
" && pass "T-E-04 restrict vs -M" || fail "T-E-04 restrict vs -M" "did not reject"

# ── G15 --backup CONTROL ──────────────────────────────────────────────────────

T "T-E-05: --backup=simple renames with suffix" \
bash -c "
  ARCH=\"$TMPDIR_BASE/b_simple.tar\"
  EX=\"$TMPDIR_BASE/ex_simple\"
  mkdir -p \"\$EX/ground\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  echo 'old' > \"\$EX/ground/file1.txt\"
  \"$MUTAR\" -x --backup=simple -f \"\$ARCH\" -C \"\$EX\" >/dev/null 2>&1
  [ -f \"\$EX/ground/file1.txt~\" ] || { ls -la \"\$EX/ground/\"; exit 1; }
  grep -q 'old' \"\$EX/ground/file1.txt~\"
  grep -q 'hello' \"\$EX/ground/file1.txt\"
" && pass "T-E-05 backup=simple" || fail "T-E-05 backup=simple" "suffix backup missing"

T "T-E-06: --backup=none does not create backup" \
bash -c "
  ARCH=\"$TMPDIR_BASE/b_none.tar\"
  EX=\"$TMPDIR_BASE/ex_none\"
  mkdir -p \"\$EX/ground\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  echo 'old' > \"\$EX/ground/file1.txt\"
  \"$MUTAR\" -x --backup=none -f \"\$ARCH\" -C \"\$EX\" >/dev/null 2>&1
  [ ! -f \"\$EX/ground/file1.txt~\" ] || { echo 'unexpected simple backup'; exit 1; }
  [ ! -f \"\$EX/ground/file1.txt.~1~\" ] || { echo 'unexpected numbered backup'; exit 1; }
  grep -q 'hello' \"\$EX/ground/file1.txt\"
" && pass "T-E-06 backup=none" || fail "T-E-06 backup=none" "backup created or extract failed"

T "T-E-07: --backup=numbered creates file.~1~" \
bash -c "
  ARCH=\"$TMPDIR_BASE/b_num.tar\"
  EX=\"$TMPDIR_BASE/ex_num\"
  mkdir -p \"\$EX/ground\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  echo 'old1' > \"\$EX/ground/file1.txt\"
  \"$MUTAR\" -x --backup=numbered -f \"\$ARCH\" -C \"\$EX\" >/dev/null 2>&1
  [ -f \"\$EX/ground/file1.txt.~1~\" ] || { ls -la \"\$EX/ground/\"; exit 1; }
  grep -q 'old1' \"\$EX/ground/file1.txt.~1~\"
  # second extract should create .~2~
  echo 'old2' > \"\$EX/ground/file1.txt\"
  \"$MUTAR\" -x --backup=numbered -f \"\$ARCH\" -C \"\$EX\" >/dev/null 2>&1
  [ -f \"\$EX/ground/file1.txt.~2~\" ] || { ls -la \"\$EX/ground/\"; exit 1; }
  grep -q 'old2' \"\$EX/ground/file1.txt.~2~\"
" && pass "T-E-07 backup=numbered" || fail "T-E-07 backup=numbered" "numbered backup missing"

# ── G12 --quoting-style ───────────────────────────────────────────────────────

T "T-E-08: --quoting-style=literal lists raw names" \
bash -c "
  ARCH=\"$TMPDIR_BASE/q_lit.tar\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  OUT=\$(\"$MUTAR\" -t --quoting-style=literal -f \"\$ARCH\" 2>/dev/null)
  echo \"\$OUT\" | grep -qF 'file with spaces.txt' || { echo \"\$OUT\"; exit 1; }
" && pass "T-E-08 quoting literal" || fail "T-E-08 quoting literal" "raw name missing"

T "T-E-09: --quoting-style=escape escapes spaces" \
bash -c "
  ARCH=\"$TMPDIR_BASE/q_esc.tar\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  OUT=\$(\"$MUTAR\" -t --quoting-style=escape -f \"\$ARCH\" 2>/dev/null)
  echo \"\$OUT\" | grep -qE 'file\\\\ with\\\\ spaces\\.txt|file\\ with\\ spaces' \
    || { echo \"OUT=\$OUT\"; exit 1; }
" && pass "T-E-09 quoting escape" || fail "T-E-09 quoting escape" "no escaped spaces"

T "T-E-10: --quoting-style=c wraps specials in double quotes" \
bash -c "
  ARCH=\"$TMPDIR_BASE/q_c.tar\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  OUT=\$(\"$MUTAR\" -t --quoting-style=c -f \"\$ARCH\" 2>/dev/null)
  echo \"\$OUT\" | grep -qF '\"' || { echo \"OUT=\$OUT\"; exit 1; }
  echo \"\$OUT\" | grep -q 'file with spaces' || { echo \"OUT=\$OUT\"; exit 1; }
" && pass "T-E-10 quoting c" || fail "T-E-10 quoting c" "no c-style quotes"

T "T-E-11: --quoting-style=shell quotes specials" \
bash -c "
  ARCH=\"$TMPDIR_BASE/q_sh.tar\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  OUT=\$(\"$MUTAR\" -t --quoting-style=shell -f \"\$ARCH\" 2>/dev/null)
  echo \"\$OUT\" | grep -q \"'\" || { echo \"OUT=\$OUT\"; exit 1; }
" && pass "T-E-11 quoting shell" || fail "T-E-11 quoting shell" "no shell quotes"

# ── G14 --check-device + G10 snapshot ──────────────────────────────────────────

T "T-E-12: --check-device / --no-check-device accepted" \
bash -c "
  ARCH=\"$TMPDIR_BASE/cd.tar\"
  SNAR=\"$TMPDIR_BASE/cd.snar\"
  \"$MUTAR\" -c --check-device --level=0 -g \"\$SNAR\" -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  \"$MUTAR\" -c --no-check-device --level=0 -g \"\$SNAR\" -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  [ -f \"\$SNAR\" ]
" && pass "T-E-12 check-device flags" || fail "T-E-12 check-device flags" "rejected"

T "T-E-13: listed-incremental snapshot V2 has dirs and device field" \
bash -c "
  ARCH=\"$TMPDIR_BASE/snap.tar\"
  SNAR=\"$TMPDIR_BASE/snap.snar\"
  rm -f \"\$SNAR\"
  \"$MUTAR\" -c --level=0 -g \"\$SNAR\" -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  head -1 \"\$SNAR\" | grep -q 'MUTAR_SNAPSHOT_V2' || { echo 'header:'; head -3 \"\$SNAR\"; exit 1; }
  # directory entry present
  grep -E '^(ground/subdir|ground)\t' \"\$SNAR\" >/dev/null \
    || grep -E 'subdir' \"\$SNAR\" >/dev/null \
    || { echo 'snapshot:'; cat \"\$SNAR\"; exit 1; }
  # third field (device) present on at least one line
  awk -F'\t' 'NR>1 && NF>=3 { found=1 } END { exit !found }' \"\$SNAR\" \
    || { echo 'no device field:'; cat \"\$SNAR\"; exit 1; }
  echo '  snapshot header + dir + dev OK'
" && pass "T-E-13 snapshot V2 dirs+dev" || fail "T-E-13 snapshot V2 dirs+dev" "format wrong"

T "T-E-14: --help mentions restrict / backup CONTROL / quoting / rmt lseek limit" \
bash -c "
  HELP=\$(\"$MUTAR\" --help 2>&1)
  echo \"\$HELP\" | grep -q 'restrict' || exit 1
  echo \"\$HELP\" | grep -qi 'numbered' || exit 1
  echo \"\$HELP\" | grep -q 'quoting-style' || exit 1
  echo \"\$HELP\" | grep -qi 'lseek' || exit 1
" && pass "T-E-14 help text" || fail "T-E-14 help text" "missing phrases"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "========================================="
echo " Results: PASS=$PASS   FAIL=$FAIL   SKIP=$SKIP"
echo "========================================="
[ "$FAIL" -eq 0 ]
