#!/usr/bin/env bash
# tests/test_hunt_r3.sh — Hunt Round 3 leftover HIGH regressions
#
# R3-1   -T - and -X - read stdin (not a file named "-")
# R3-2   -f '' must error (not treat as stdout)
# R3-3   -r / -u / --delete on compressed archive must refuse
# R3-4   --mtime=@SECONDS and --mtime=1970-01-01 (no year-2242 wrap)
# R3-5   ustar exact 100-byte name round-trip
# R3-6   --exclude=sub/*.o matches dir/sub/bar.o (GNU no-anchored)
# R3-7   --to-command SIGPIPE must not kill mutar (rc != -13 / 141)
# R3-8   0-byte / non-tar extract is nonzero
# R3-9   --one-top-level must not double-prefix (only/only/...)
# R3-10  --strip-components 2^31 / 2^32 rejected (not wrap to 0)
# R3-11  --owner=NAME:UID and --owner=+UID
# R3-12  -k keeps existing symlink / fifo / hardlink dest
# R3-13  -P create keeps leading /
#
# Usage: ./test_hunt_r3.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
MUTAR="$(cd "$(dirname "$MUTAR")" && pwd)/$(basename "$MUTAR")"
TMPBASE="$(mktemp -d /tmp/mutar_hunt_r3.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

TIMEOUT_SECS="${HUNT_TIMEOUT:-8}"
run_to() { timeout --signal=TERM --kill-after=2 "$TIMEOUT_SECS" "$@"; }

echo "=== mutar hunt R3 leftover HIGH ==="

# ── R3-1: -T - / -X - read stdin ──────────────────────────────────────────────
echo "[R3-1] -T - and -X - read stdin"
{
  D="$TMPBASE/r3_1"
  mkdir -p "$D"
  printf 'keep\n' >"$D/keep.txt"
  printf 'drop\n' >"$D/drop.o"
  printf 'also\n' >"$D/also.txt"
  ok=1
  set +e
  printf 'keep.txt\nalso.txt\n' | run_to "$MUTAR" -cf "$D/from_t.tar" -C "$D" -T -
  rc=$?
  set -e
  list=$(run_to "$MUTAR" -tf "$D/from_t.tar" 2>/dev/null || true)
  if [ "$rc" -ne 0 ]; then
    fail "R3-1-T" "create -T - failed rc=$rc"
    ok=0
  elif ! echo "$list" | grep -q keep.txt || ! echo "$list" | grep -q also.txt; then
    fail "R3-1-T" "missing members: $list"
    ok=0
  elif echo "$list" | grep -q drop; then
    fail "R3-1-T" "unexpected drop.o in -T - archive: $list"
    ok=0
  fi
  set +e
  printf '*.o\n' | run_to "$MUTAR" -cf "$D/from_x.tar" -C "$D" -X - keep.txt drop.o also.txt
  rc=$?
  set -e
  list=$(run_to "$MUTAR" -tf "$D/from_x.tar" 2>/dev/null || true)
  if [ "$rc" -ne 0 ]; then
    fail "R3-1-X" "create -X - failed rc=$rc"
    ok=0
  elif echo "$list" | grep -q 'drop.o'; then
    fail "R3-1-X" "drop.o not excluded via -X -: $list"
    ok=0
  elif ! echo "$list" | grep -q keep.txt; then
    fail "R3-1-X" "keep.txt missing: $list"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-1: -T - and -X - read stdin"
  fi
}

# ── R3-2: -f '' errors ────────────────────────────────────────────────────────
echo "[R3-2] -f '' must error"
{
  D="$TMPBASE/r3_2"
  mkdir -p "$D"
  printf 'x\n' >"$D/f.txt"
  set +e
  run_to "$MUTAR" -cf '' -C "$D" f.txt >"$D/out" 2>"$D/err"
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    fail "R3-2" "empty -f succeeded (treated as stdout?)"
  elif [ "$rc" -ge 128 ]; then
    fail "R3-2" "signaled rc=$rc err=$(head -c 160 "$D/err")"
  else
    pass "R3-2: -f '' errors (rc=$rc)"
  fi
}

# ── R3-3: compressed -r/-u/--delete refuse ────────────────────────────────────
echo "[R3-3] refuse -r/-u/--delete on compressed archive"
{
  D="$TMPBASE/r3_3"
  mkdir -p "$D"
  printf 'a\n' >"$D/a.txt"
  printf 'b\n' >"$D/b.txt"
  run_to "$MUTAR" -czf "$D/a.tar.gz" -C "$D" a.txt
  orig_md5=$(md5sum "$D/a.tar.gz" | awk '{print $1}')
  ok=1
  for op in "-r" "-u" "--delete"; do
    set +e
    run_to "$MUTAR" $op -f "$D/a.tar.gz" -C "$D" b.txt >"$D/${op}.out" 2>"$D/${op}.err"
    rc=$?
    set -e
    new_md5=$(md5sum "$D/a.tar.gz" | awk '{print $1}')
    if [ "$rc" -eq 0 ]; then
      fail "R3-3-$op" "compressed $op exited 0"
      ok=0
    elif [ "$rc" -ge 128 ]; then
      fail "R3-3-$op" "signaled rc=$rc"
      ok=0
    elif [ "$new_md5" != "$orig_md5" ]; then
      fail "R3-3-$op" "archive mutated under refused $op"
      ok=0
    fi
  done
  if [ "$ok" -eq 1 ]; then
    pass "R3-3: compressed -r/-u/--delete refused; archive unchanged"
  fi
}

# ── R3-4: --mtime=@SECONDS and 1970-01-01 ─────────────────────────────────────
echo "[R3-4] --mtime=@SECONDS and 1970-01-01"
{
  D="$TMPBASE/r3_4"
  mkdir -p "$D"
  printf 'x\n' >"$D/f.txt"
  ok=1
  run_to "$MUTAR" -cf "$D/epoch.tar" --mtime=@0 -C "$D" f.txt
  ts=$(run_to "$MUTAR" --utc -tvf "$D/epoch.tar" 2>/dev/null | grep f.txt | awk '{print $4}')
  if [ "$ts" != "1970-01-01" ]; then
    fail "R3-4-at" "mtime=@0 listed as '$ts' (want 1970-01-01)"
    ok=0
  fi
  run_to "$MUTAR" -cf "$D/date.tar" --mtime=1970-01-01 -C "$D" f.txt
  ts=$(run_to "$MUTAR" --utc -tvf "$D/date.tar" 2>/dev/null | grep f.txt | awk '{print $4}')
  if echo "$ts" | grep -q 2242; then
    fail "R3-4-date" "1970-01-01 wrapped to $ts"
    ok=0
  elif [ "$ts" != "1970-01-01" ]; then
    fail "R3-4-date" "mtime=1970-01-01 listed as '$ts'"
    ok=0
  fi
  run_to "$MUTAR" -cf "$D/sec.tar" --mtime=@1000000000 -C "$D" f.txt
  ts=$(run_to "$MUTAR" --utc -tvf "$D/sec.tar" 2>/dev/null | grep f.txt | awk '{print $4}')
  if [ "$ts" != "2001-09-09" ]; then
    fail "R3-4-sec" "mtime=@1000000000 listed as '$ts' (want 2001-09-09)"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-4: --mtime=@SECONDS and 1970-01-01 (UTC, no wrap)"
  fi
}

# ── R3-5: ustar 100-byte name ─────────────────────────────────────────────────
echo "[R3-5] ustar exact 100-byte name round-trip"
{
  D="$TMPBASE/r3_5"
  mkdir -p "$D"
  name=$(python3 -c 'print("n"*100)')
  printf 'payload\n' >"$D/$name"
  ok=1
  set +e
  run_to "$MUTAR" --format=ustar -cf "$D/a.tar" -C "$D" "$name" >"$D/c.out" 2>"$D/c.err"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    fail "R3-5-create" "ustar create failed rc=$rc err=$(head -c 160 "$D/c.err")"
    ok=0
  else
    listed=$(run_to "$MUTAR" -tf "$D/a.tar" 2>/dev/null | tr -d '\n')
    if [ "${#listed}" -ne 100 ]; then
      fail "R3-5-list" "listed name length ${#listed} (want 100): '$listed'"
      ok=0
    fi
    mkdir -p "$D/out"
    run_to "$MUTAR" -xf "$D/a.tar" -C "$D/out"
    if [ ! -f "$D/out/$name" ]; then
      fail "R3-5-extract" "100-byte name not extracted"
      ok=0
    elif ! cmp -s "$D/$name" "$D/out/$name"; then
      fail "R3-5-extract" "payload mismatch"
      ok=0
    fi
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-5: ustar 100-byte name round-trips"
  fi
}

# ── R3-6: --exclude=sub/*.o ───────────────────────────────────────────────────
echo "[R3-6] --exclude=sub/*.o matches dir/sub/bar.o"
{
  D="$TMPBASE/r3_6"
  mkdir -p "$D/dir/sub"
  printf 'o\n' >"$D/dir/sub/bar.o"
  printf 'c\n' >"$D/dir/sub/bar.c"
  run_to "$MUTAR" -cf "$D/a.tar" --exclude='sub/*.o' -C "$D" dir
  list=$(run_to "$MUTAR" -tf "$D/a.tar" 2>/dev/null || true)
  if echo "$list" | grep -q 'bar.o'; then
    fail "R3-6" "bar.o not excluded: $list"
  elif ! echo "$list" | grep -q 'bar.c'; then
    fail "R3-6" "bar.c missing: $list"
  else
    pass "R3-6: --exclude=sub/*.o matches dir/sub/bar.o"
  fi
}

# ── R3-7: --to-command SIGPIPE ────────────────────────────────────────────────
echo "[R3-7] --to-command SIGPIPE does not kill mutar"
{
  D="$TMPBASE/r3_7"
  mkdir -p "$D/out"
  dd if=/dev/zero of="$D/big.bin" bs=1024 count=64 status=none
  run_to "$MUTAR" -cf "$D/a.tar" -C "$D" big.bin
  ok=1
  set +e
  run_to "$MUTAR" -xf "$D/a.tar" -C "$D/out" --to-command='true' \
    >"$D/t.out" 2>"$D/t.err"
  rc=$?
  set -e
  if [ "$rc" -eq 141 ] || [ "$rc" -eq 243 ] || [ "$rc" -eq 13 ]; then
    fail "R3-7-true" "died from SIGPIPE rc=$rc"
    ok=0
  elif [ "$rc" -ge 128 ]; then
    fail "R3-7-true" "signaled rc=$rc err=$(head -c 160 "$D/t.err")"
    ok=0
  fi
  set +e
  run_to "$MUTAR" -xf "$D/a.tar" -C "$D/out" \
    --ignore-command-error --to-command='head -c 1 >/dev/null' \
    >"$D/h.out" 2>"$D/h.err"
  rc=$?
  set -e
  if [ "$rc" -eq 141 ] || [ "$rc" -eq 243 ] || [ "$rc" -eq 13 ]; then
    fail "R3-7-head" "died from SIGPIPE rc=$rc"
    ok=0
  elif [ "$rc" -ge 128 ]; then
    fail "R3-7-head" "signaled rc=$rc err=$(head -c 160 "$D/h.err")"
    ok=0
  elif [ "$rc" -ne 0 ]; then
    fail "R3-7-head" "ignore-command-error still failed rc=$rc err=$(head -c 160 "$D/h.err")"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-7: --to-command SIGPIPE ignored / ignore-command-error succeeds"
  fi
}

# ── R3-8: 0-byte / non-tar extract ────────────────────────────────────────────
echo "[R3-8] 0-byte / non-tar extract nonzero"
{
  D="$TMPBASE/r3_8"
  mkdir -p "$D/out"
  : >"$D/empty.tar"
  printf 'not a tar archive\n' >"$D/junk.tar"
  ok=1
  set +e
  run_to "$MUTAR" -xf "$D/empty.tar" -C "$D/out" >"$D/e.out" 2>"$D/e.err"
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    fail "R3-8-empty" "0-byte extract exited 0"
    ok=0
  elif [ "$rc" -ge 128 ]; then
    fail "R3-8-empty" "signaled rc=$rc"
    ok=0
  fi
  set +e
  run_to "$MUTAR" -xf "$D/junk.tar" -C "$D/out" >"$D/j.out" 2>"$D/j.err"
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    fail "R3-8-junk" "non-tar extract exited 0"
    ok=0
  elif [ "$rc" -ge 128 ]; then
    fail "R3-8-junk" "signaled rc=$rc"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-8: 0-byte and non-tar extract fail"
  fi
}

# ── R3-9: --one-top-level no double-prefix ────────────────────────────────────
echo "[R3-9] --one-top-level does not double-prefix"
{
  D="$TMPBASE/r3_9"
  mkdir -p "$D/only" "$D/out"
  printf 'x\n' >"$D/only/f.txt"
  run_to "$MUTAR" -cf "$D/a.tar" -C "$D" only/f.txt
  run_to "$MUTAR" -xf "$D/a.tar" --one-top-level=only -C "$D/out"
  if [ -f "$D/out/only/only/f.txt" ]; then
    fail "R3-9" "double-prefixed only/only/f.txt"
  elif [ ! -f "$D/out/only/f.txt" ]; then
    fail "R3-9" "missing only/f.txt; tree=$(find "$D/out" -type f)"
  else
    pass "R3-9: --one-top-level=only does not create only/only/"
  fi
}

# ── R3-10: --strip-components overflow ────────────────────────────────────────
echo "[R3-10] --strip-components 2^31 / 2^32 rejected"
{
  D="$TMPBASE/r3_10"
  mkdir -p "$D/out"
  printf 'x\n' >"$D/f.txt"
  run_to "$MUTAR" -cf "$D/a.tar" -C "$D" f.txt
  ok=1
  for n in 2147483648 4294967296; do
    set +e
    run_to "$MUTAR" -xf "$D/a.tar" -C "$D/out" --strip-components="$n" \
      >"$D/s$n.out" 2>"$D/s$n.err"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
      fail "R3-10-$n" "accepted huge strip-components (would wrap)"
      ok=0
    elif [ "$rc" -ge 128 ]; then
      fail "R3-10-$n" "signaled rc=$rc"
      ok=0
    fi
  done
  if [ "$ok" -eq 1 ]; then
    pass "R3-10: --strip-components 2^31/2^32 rejected"
  fi
}

# ── R3-11: --owner=NAME:UID and --owner=+UID ──────────────────────────────────
echo "[R3-11] --owner=NAME:UID and --owner=+UID"
{
  D="$TMPBASE/r3_11"
  mkdir -p "$D"
  printf 'x\n' >"$D/f.txt"
  ok=1
  run_to "$MUTAR" -cf "$D/colon.tar" --owner=nobody:12345 --numeric-owner -C "$D" f.txt
  line=$(run_to "$MUTAR" --numeric-owner -tvf "$D/colon.tar" 2>/dev/null | grep f.txt || true)
  if ! echo "$line" | grep -q '12345/'; then
    fail "R3-11-colon" "uid 12345 not in listing: $line"
    ok=0
  fi
  run_to "$MUTAR" -cf "$D/plus.tar" --owner=+99 --numeric-owner -C "$D" f.txt
  line=$(run_to "$MUTAR" --numeric-owner -tvf "$D/plus.tar" 2>/dev/null | grep f.txt || true)
  if ! echo "$line" | grep -q '99/'; then
    fail "R3-11-plus" "uid 99 not in listing: $line"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-11: --owner=NAME:UID and --owner=+UID"
  fi
}

# ── R3-12: -k symlink / fifo / hardlink ───────────────────────────────────────
echo "[R3-12] -k keeps existing symlink/fifo/hardlink dest"
{
  D="$TMPBASE/r3_12"
  mkdir -p "$D/src" "$D/out"
  printf 'orig\n' >"$D/src/target"
  ln -s target "$D/src/thelink"
  mkfifo "$D/src/thefifo"
  printf 'hard\n' >"$D/src/ha"
  ln "$D/src/ha" "$D/src/hb"
  run_to "$MUTAR" -cf "$D/a.tar" -C "$D/src" thelink thefifo ha hb
  printf 'KEEP-L\n' >"$D/out/thelink"
  printf 'KEEP-F\n' >"$D/out/thefifo"
  printf 'KEEP-A\n' >"$D/out/ha"
  printf 'KEEP-B\n' >"$D/out/hb"
  set +e
  run_to "$MUTAR" -xkf "$D/a.tar" -C "$D/out" >"$D/k.out" 2>"$D/k.err"
  set -e
  ok=1
  got=$(cat "$D/out/thelink" 2>/dev/null || true)
  if [ "$got" != "KEEP-L" ]; then
    fail "R3-12-symlink" "dest overwritten: '$got'"
    ok=0
  fi
  got=$(cat "$D/out/thefifo" 2>/dev/null || true)
  if [ "$got" != "KEEP-F" ]; then
    fail "R3-12-fifo" "dest overwritten: '$got'"
    ok=0
  fi
  got=$(cat "$D/out/hb" 2>/dev/null || true)
  if [ "$got" != "KEEP-B" ]; then
    fail "R3-12-hardlink" "dest overwritten: '$got'"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R3-12: -k keeps symlink/fifo/hardlink destinations"
  fi
}

# ── R3-13: -P create keeps leading / ──────────────────────────────────────────
echo "[R3-13] -P create keeps leading /"
{
  D="$TMPBASE/r3_13"
  mkdir -p "$D"
  printf 'x\n' >"$D/abs.txt"
  abs="$D/abs.txt"
  run_to "$MUTAR" -cPf "$D/p.tar" "$abs"
  list=$(run_to "$MUTAR" -tf "$D/p.tar" 2>/dev/null || true)
  if echo "$list" | grep -q "^/"; then
    pass "R3-13: -P create keeps leading /"
  else
    fail "R3-13" "listed without leading /: $list"
  fi
}

echo
echo "hunt R3: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
exit 0
