#!/usr/bin/env bash
# tests/test_phase8_residuals.sh — Phase 8 residual parity gaps
#
#   P8-01  --mode symbolic (u+x,go-w)
#   P8-02  -N/--newer uses ctime; --newer-mtime uses mtime
#   P8-03  sparse mid-file multi-volume round-trip
#   P8-04  --warning=no-KEYWORD silences more sites
#   P8-05  -g reads GNU tar listed-incremental snapshot (format 2)
#   P8-06  compressed remote create/extract via mock rmt
#
# Usage: bash test_phase8_residuals.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
MOCK_RSH="$TESTDIR/mock_rsh.sh"
MOCK_RMT="$TESTDIR/mock_rmt.sh"
TMPBASE="$(mktemp -d /tmp/mutar_p8.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

# ── P8-01: --mode symbolic ────────────────────────────────────────────────────
echo "[P8-01 --mode symbolic]"
{
  D="$TMPBASE/mode"
  mkdir -p "$D"
  echo hi > "$D/f.txt"
  chmod 644 "$D/f.txt"
  # 644 + u+x → 744; go-w is no-op (no group/other write)
  if ! "$MUTAR" -cf "$D/a.tar" --mode=u+x,go-w -C "$D" f.txt 2>"$D/err.txt"; then
    fail "P8-01" "create failed: $(cat "$D/err.txt")"
  else
    line=$("$MUTAR" -tvf "$D/a.tar" 2>/dev/null | head -1)
    if echo "$line" | grep -q '^-rwxr--r--'; then
      pass "P8-01: --mode=u+x,go-w → -rwxr--r--"
    else
      fail "P8-01" "mode line: $line"
    fi
  fi
  # pure octal still works
  "$MUTAR" -cf "$D/b.tar" --mode=640 -C "$D" f.txt 2>/dev/null
  line=$("$MUTAR" -tvf "$D/b.tar" 2>/dev/null | head -1)
  if echo "$line" | grep -q '^-rw-r-----'; then
    pass "P8-01b: --mode=640 octal still works"
  else
    fail "P8-01b" "octal mode line: $line"
  fi
}

# ── P8-02: --newer ctime vs --newer-mtime ─────────────────────────────────────
echo "[P8-02 --newer ctime vs --newer-mtime]"
{
  D="$TMPBASE/newer"
  mkdir -p "$D"
  echo old > "$D/old.txt"
  touch -d '2020-01-01' "$D/old.txt"
  # Bump ctime without changing mtime
  chmod 600 "$D/old.txt"
  # mtime still 2020 → --newer-mtime=2021 should skip
  "$MUTAR" -cf "$D/by_mtime.tar" --newer-mtime='2021-01-01' -C "$D" old.txt 2>/dev/null
  m_members=$("$MUTAR" -tf "$D/by_mtime.tar" 2>/dev/null | wc -l)
  # ctime is now → --newer=2021 should include
  "$MUTAR" -cf "$D/by_ctime.tar" --newer='2021-01-01' -C "$D" old.txt 2>/dev/null
  c_members=$("$MUTAR" -tf "$D/by_ctime.tar" 2>/dev/null | wc -l)
  if [ "$m_members" -eq 0 ] && [ "$c_members" -eq 1 ]; then
    pass "P8-02: --newer-mtime skips (mtime old); --newer includes (ctime new)"
  else
    fail "P8-02" "mtime_members=$m_members ctime_members=$c_members"
  fi
}

# ── P8-03: sparse mid-file multi-volume ───────────────────────────────────────
echo "[P8-03 sparse multi-volume]"
{
  D="$TMPBASE/sp_mvol"
  mkdir -p "$D/src" "$D/out"
  truncate -s 50k "$D/src/sp.bin"
  dd if=/dev/urandom of="$D/src/sp.bin" bs=1k count=8 conv=notrunc status=none
  dd if=/dev/urandom of="$D/src/sp.bin" bs=1k count=2 seek=40 conv=notrunc status=none
  md5_src=$(md5sum < "$D/src/sp.bin" | awk '{print $1}')

  # -L 10 matches default blocking (20×512) so mid-file split is clean
  if ! "$MUTAR" -c -S -M -L 10 -f "$D/s.tar" -C "$D/src" sp.bin 2>"$D/c.err"; then
    fail "P8-03" "create failed: $(head -c 300 "$D/c.err")"
  else
    nvol=$(find "$D" -maxdepth 1 \( -name 's.tar' -o -name 's.tar.*' \) -type f | wc -l)
    has_m=0
    for v in "$D"/s.tar "$D"/s.tar.*; do
      [ -f "$v" ] || continue
      tf=$(dd if="$v" bs=1 skip=156 count=1 status=none 2>/dev/null | od -An -t x1 | tr -d ' \n')
      if [ "$tf" = "4d" ]; then has_m=1; break; fi
    done
    "$MUTAR" -x -M -f "$D/s.tar" -C "$D/out" 2>"$D/x.err"
    rc_x=$?
    md5_out=$(md5sum < "$D/out/sp.bin" 2>/dev/null | awk '{print $1}')
    if [ "$rc_x" -eq 0 ] && [ "$nvol" -ge 2 ] && [ "$has_m" -eq 1 ] && [ "$md5_src" = "$md5_out" ]; then
      pass "P8-03: sparse multi-vol $nvol volumes, type M, content OK"
    else
      fail "P8-03" "rc_x=$rc_x nvol=$nvol has_m=$has_m md5 $md5_src vs $md5_out; xerr=$(head -c 200 "$D/x.err")"
    fi
  fi
}

# ── P8-04: --warning broader coverage ─────────────────────────────────────────
echo "[P8-04 --warning coverage]"
{
  D="$TMPBASE/warn"
  mkdir -p "$D"
  echo x > "$D/f.txt"
  # no-newer should silence the "not newer" warning
  "$MUTAR" -cf "$D/w.tar" --warning=no-newer --newer='2099-01-01' -C "$D" f.txt 2>"$D/err.txt"
  if grep -qi 'not newer\|not dumped' "$D/err.txt"; then
    fail "P8-04" "no-newer did not silence: $(cat "$D/err.txt")"
  else
    pass "P8-04: --warning=no-newer silences newer filter warning"
  fi
  # compress category for --seekable solid stream
  "$MUTAR" -czf "$D/s.tar.gz" --seekable --warning=no-compress -C "$D" f.txt 2>"$D/err2.txt"
  if grep -qi 'seekable' "$D/err2.txt"; then
    fail "P8-04b" "no-compress did not silence seekable warn: $(cat "$D/err2.txt")"
  else
    pass "P8-04b: --warning=no-compress silences seekable solid-stream warning"
  fi
}

# ── P8-05: GNU snapshot interop (read) ────────────────────────────────────────
echo "[P8-05 GNU snapshot read]"
{
  D="$TMPBASE/gnu_snap"
  mkdir -p "$D/tree/sub"
  echo a > "$D/tree/a.txt"
  echo b > "$D/tree/sub/b.txt"
  if ! command -v tar >/dev/null 2>&1; then
    skip "P8-05" "system tar not available"
  else
    # GNU level-0 snapshot
    if ! tar --listed-incremental="$D/snap.gnu" -cf "$D/l0.tar" -C "$D" tree 2>"$D/gnu.err"; then
      skip "P8-05" "GNU tar -g failed: $(head -c 200 "$D/gnu.err")"
    else
      # mutar level-1 with GNU snapshot should skip unchanged files
      if ! "$MUTAR" -c --level=1 -g "$D/snap.gnu" -f "$D/l1.tar" -C "$D" tree 2>"$D/m.err"; then
        fail "P8-05" "mutar -g with GNU snap failed: $(cat "$D/m.err")"
      else
        members=$("$MUTAR" -tf "$D/l1.tar" 2>/dev/null)
        # dirs always dumped; unchanged files should be absent
        if echo "$members" | grep -q 'a\.txt'; then
          fail "P8-05" "expected a.txt skipped; members: $members"
        elif ! echo "$members" | grep -q 'tree/'; then
          fail "P8-05" "expected directory members; got: $members"
        else
          pass "P8-05: mutar -g reads GNU snapshot; skips unchanged files"
        fi
      fi
    fi
  fi
}

# ── P8-06: compressed remote ──────────────────────────────────────────────────
echo "[P8-06 compressed remote]"
{
  D="$TMPBASE/rcmp"
  mkdir -p "$D" "$D/out"
  echo 'remote-gz-payload' > "$D/f.txt"
  if [ ! -x "$MOCK_RSH" ] || [ ! -x "$MOCK_RMT" ]; then
    skip "P8-06" "mock rsh/rmt not executable"
  else
    ARCH="localhost:$D/arch.tar.gz"
    RMT_FLAGS=(--rsh-command="$MOCK_RSH" --rmt-command="$MOCK_RMT")
    if ! "$MUTAR" -czf "$ARCH" -C "$D" f.txt "${RMT_FLAGS[@]}" 2>"$D/c.err"; then
      fail "P8-06" "remote compress create failed: $(cat "$D/c.err")"
    elif [ ! -s "$D/arch.tar.gz" ]; then
      fail "P8-06" "remote archive empty or missing"
    else
      if ! "$MUTAR" -xzf "$ARCH" -C "$D/out" "${RMT_FLAGS[@]}" 2>"$D/x.err"; then
        fail "P8-06" "remote compress extract failed: $(cat "$D/x.err")"
      elif cmp -s "$D/f.txt" "$D/out/f.txt"; then
        pass "P8-06: compressed remote create+extract via rmt"
      else
        fail "P8-06" "content mismatch after remote compress round-trip"
      fi
    fi
  fi
}

echo
echo "=== phase8 residuals summary: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ]
