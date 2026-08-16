#!/usr/bin/env bash
# tests/test_extract_safety.sh — extract path safety (symlink zip-slip, etc.)
#
# T-SAFE-01: symlink-mediated zip-slip
#   Archive: member "evil" is a symlink to an absolute victim path, then a
#   second member "evil" is a regular file with payload "pwned".
#   Extract must NOT overwrite the victim; extracted "evil" should be a
#   regular file with "pwned" (or extract must refuse without touching victim).
#
# Usage: ./test_extract_safety.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
# Absolute path so subshells that cd still find the binary
MUTAR="$(cd "$(dirname "$MUTAR")" && pwd)/$(basename "$MUTAR")"
TMPBASE="$(mktemp -d /tmp/mutar_extract_safety.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

echo "=== mutar extract safety ==="

# ── T-SAFE-01: symlink then regular same-name must not follow link ────────────
echo "[T-SAFE-01] symlink-mediated zip-slip"
{
  D="$TMPBASE/zipslip"
  mkdir -p "$D/stage" "$D/extract" "$D/victim_dir"
  printf 'safe-content\n' >"$D/victim_dir/victim"
  VICTIM="$D/victim_dir/victim"

  # Build archive from stage CWD: -r does not honor -C for member open paths.
  (
    cd "$D/stage" || exit 1
    ln -s "$VICTIM" evil
    "$MUTAR" -cf "$D/a.tar" evil 2>"$D/c1.err"
    echo $? >"$D/rc1"
    rm -f evil
    printf 'pwned\n' >evil
    "$MUTAR" -rf "$D/a.tar" evil 2>"$D/c2.err"
    echo $? >"$D/rc2"
  )
  rc1=$(cat "$D/rc1" 2>/dev/null || echo 1)
  rc2=$(cat "$D/rc2" 2>/dev/null || echo 1)

  if [ "$rc1" -ne 0 ] || [ "$rc2" -ne 0 ]; then
    fail "T-SAFE-01" "archive build failed rc1=$rc1 rc2=$rc2; $(head -c 200 "$D/c1.err" 2>/dev/null; head -c 200 "$D/c2.err" 2>/dev/null)"
  else
    "$MUTAR" -xf "$D/a.tar" -C "$D/extract" 2>"$D/x.err"
    rc_x=$?
    victim_now=$(cat "$VICTIM" 2>/dev/null || echo '')
    extracted="$D/extract/evil"

    if [ "$victim_now" != "safe-content" ]; then
      fail "T-SAFE-01" "VICTIM OVERWRITTEN (got '$victim_now'); extract rc=$rc_x err=$(head -c 300 "$D/x.err")"
    elif [ -L "$extracted" ]; then
      fail "T-SAFE-01" "extracted evil still a symlink (victim safe); want regular file or refuse"
    elif [ -f "$extracted" ]; then
      if grep -qx 'pwned' "$extracted" 2>/dev/null; then
        pass "T-SAFE-01: victim intact; evil is regular file with pwned (rc=$rc_x)"
      else
        payload=$(tr -d '\n' <"$extracted" 2>/dev/null || echo '')
        if [ "$rc_x" -ne 0 ]; then
          pass "T-SAFE-01: victim intact; extract refused safely (rc=$rc_x)"
        else
          fail "T-SAFE-01" "extracted payload='$payload' expected pwned; rc=$rc_x"
        fi
      fi
    else
      if [ "$rc_x" -ne 0 ]; then
        pass "T-SAFE-01: victim intact; extract refused (no evil written, rc=$rc_x)"
      else
        fail "T-SAFE-01" "no extracted evil and extract rc=0; err=$(head -c 200 "$D/x.err")"
      fi
    fi
  fi
}

# ── T-SAFE-02: relative symlink zip-slip (evil -> ../outside/victim) ──────────
echo "[T-SAFE-02] relative symlink zip-slip"
{
  D="$TMPBASE/relslip"
  mkdir -p "$D/stage" "$D/extract" "$D/outside"
  printf 'safe-rel\n' >"$D/outside/victim"
  (
    cd "$D/stage" || exit 1
    # From extract dir, ../outside/victim is the target
    ln -s "../outside/victim" evil
    "$MUTAR" -cf "$D/a.tar" evil 2>/dev/null
    rm -f evil
    printf 'pwned-rel\n' >evil
    "$MUTAR" -rf "$D/a.tar" evil 2>/dev/null
  )

  "$MUTAR" -xf "$D/a.tar" -C "$D/extract" 2>"$D/x.err"
  victim_now=$(cat "$D/outside/victim" 2>/dev/null || echo '')
  if [ "$victim_now" != "safe-rel" ]; then
    fail "T-SAFE-02" "relative victim overwritten: '$victim_now'"
  elif [ -f "$D/extract/evil" ] && [ ! -L "$D/extract/evil" ]; then
    pass "T-SAFE-02: relative victim intact; evil is regular"
  elif [ ! -e "$D/extract/evil" ]; then
    pass "T-SAFE-02: relative victim intact; no evil written"
  else
    fail "T-SAFE-02" "victim safe but evil still symlink or unexpected type"
  fi
}

# ── T-SAFE-04: directory symlink then nested regular must not escape ──────────
echo "[T-SAFE-04] intermediate directory symlink zip-slip"
{
  D="$TMPBASE/dirslip"
  mkdir -p "$D/stage" "$D/extract" "$D/outside"
  printf 'safe-dir\n' >"$D/outside/keep"
  (
    cd "$D/stage" || exit 1
    ln -s "$D/outside" d
    "$MUTAR" -cf "$D/a.tar" d 2>/dev/null
    rm -f d
    mkdir -p d
    printf 'pwned-dir\n' >d/evil
    "$MUTAR" -rf "$D/a.tar" d/evil 2>/dev/null
  )

  "$MUTAR" -xf "$D/a.tar" -C "$D/extract" 2>"$D/x.err"
  rc_x=$?
  outside_evil="$D/outside/evil"
  keep_now=$(cat "$D/outside/keep" 2>/dev/null || echo '')

  if [ -f "$outside_evil" ]; then
    fail "T-SAFE-04" "WROTE OUTSIDE via dir symlink: $(cat "$outside_evil"); rc=$rc_x"
  elif [ "$keep_now" != "safe-dir" ]; then
    fail "T-SAFE-04" "outside/keep corrupted: '$keep_now'"
  elif [ -f "$D/extract/d/evil" ] && [ ! -L "$D/extract/d" ]; then
    # Intermediate symlink replaced with real dir; payload in-tree
    if grep -qx 'pwned-dir' "$D/extract/d/evil" 2>/dev/null; then
      pass "T-SAFE-04: no outside write; d is real dir with evil (rc=$rc_x)"
    else
      pass "T-SAFE-04: no outside write; in-tree extract present (rc=$rc_x)"
    fi
  elif [ ! -e "$outside_evil" ]; then
    pass "T-SAFE-04: no outside write (rc=$rc_x, extract layout may vary)"
  else
    fail "T-SAFE-04" "unexpected state; err=$(head -c 200 "$D/x.err")"
  fi
}

# ── T-SAFE-03: make_volume_name must not treat % as printf format ─────────────
echo "[T-SAFE-03] volume name with percent (no format-string crash)"
{
  D="$TMPBASE/volpct"
  mkdir -p "$D/src"
  for i in $(seq 1 30); do
    printf 'x%.0s' {1..300} >"$D/src/f$i.txt"
  done
  # Path containing %s / %n style sequences must not crash or mis-format
  "$MUTAR" -c -M -L 10 -f "$D/arch%s%n%d.tar" -C "$D" src/ 2>"$D/err.txt"
  rc=$?
  # Expect either multi-vol files with literal %s%n and substituted first %d, or success
  if [ "$rc" -ne 0 ]; then
    # May fail for other reasons; only fail if segfault-like
    if grep -qi 'segfault\|stack smashing\|format' "$D/err.txt" 2>/dev/null; then
      fail "T-SAFE-03" "format-related failure: $(head -c 200 "$D/err.txt")"
    else
      # Still check process completed (rc is from mutar, not signal)
      if [ "$rc" -ge 128 ]; then
        fail "T-SAFE-03" "died with signal-ish rc=$rc"
      else
        pass "T-SAFE-03: no format crash (rc=$rc)"
      fi
    fi
  else
    pass "T-SAFE-03: volume create with % in name ok (rc=0)"
  fi
}

echo "=== extract safety summary: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
