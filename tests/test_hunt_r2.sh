#!/usr/bin/env bash
# tests/test_hunt_r2.sh — Hunt Round 2 CRITICAL/HIGH crash regressions
#
# R2-1  --delete must not reserve(e.size) on INT64_MAX / 1TiB (no ASan OOM)
# R2-2  --record-size huge (16MiB+) must error, not ASan abort
# R2-3  base-256 size 2^80 must be rejected (not decode as 0 / exit 0)
# R2-4  multiple -C applies each directory to following names (create)
# R2-5  -C + -M writes later volumes next to -f (original CWD), not under -C
#
# Usage: ./test_hunt_r2.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
MUTAR="$(cd "$(dirname "$MUTAR")" && pwd)/$(basename "$MUTAR")"
TMPBASE="$(mktemp -d /tmp/mutar_hunt_r2.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 required to craft malformed headers" >&2
  exit 1
fi

TIMEOUT_SECS="${HUNT_TIMEOUT:-8}"
run_to() { timeout --signal=TERM --kill-after=2 "$TIMEOUT_SECS" "$@"; }

# Craft a single ustar header (optional base-256 size) + optional payload/EOF.
# Args: outpath name size typeflag [base256=0] [payload_bytes=0] [eof_blocks=2]
write_hdr() {
  python3 - "$1" "$2" "$3" "$4" "${5:-0}" "${6:-0}" "${7:-2}" <<'PY'
import sys
path, name, size_s, tf, b256, pay, eof = sys.argv[1:8]
size = int(size_s)
base256 = int(b256)
pay_n = int(pay)
eof_n = int(eof)
blk = bytearray(512)
nb = name.encode("utf-8", "surrogateescape")[:99]
blk[0:len(nb)] = nb
blk[100:108] = b"0000644\0"
blk[108:116] = b"0000000\0"
blk[116:124] = b"0000000\0"
if base256:
    v = size
    raw = bytearray(12)
    for i in range(11, -1, -1):
        raw[i] = v & 0xFF
        v >>= 8
    raw[0] |= 0x80
    blk[124:136] = raw
else:
    if size < 0:
        raise SystemExit("octal size cannot be negative")
    blk[124:136] = f"{size:011o}\0".encode("ascii")
blk[136:148] = b"00000000000\0"
blk[156] = ord(tf) if len(tf) == 1 else tf.encode("ascii")[0]
blk[257:265] = b"ustar  \0"
csum = 0
for i, b in enumerate(blk):
    csum += (0x20 if 148 <= i < 156 else b)
blk[148:156] = f"{csum:06o}\0 ".encode("ascii")
with open(path, "wb") as f:
    f.write(blk)
    if pay_n:
        f.write(b"\0" * pay_n)
    if eof_n:
        f.write(b"\0" * (512 * eof_n))
PY
}

echo "=== mutar hunt R2 crash regressions ==="

# ── R2-1: --delete must not allocate claimed size ─────────────────────────────
echo "[R2-1] --delete huge claimed size (no reserve OOM)"
{
  D="$TMPBASE/r2_1"
  mkdir -p "$D"
  ok=1
  for spec in "max:9223372036854775807" "tib:1099511627776"; do
    kind="${spec%%:*}"
    sz="${spec#*:}"
    write_hdr "$D/${kind}.tar" "huge.bin" "$sz" "0" 1 0 2
    set +e
    run_to "$MUTAR" --delete -f "$D/${kind}.tar" "other" \
      >"$D/${kind}.out" 2>"$D/${kind}.err"
    rc=$?
    set -e
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      fail "R2-1-$kind" "timed out / killed (OOM or hang) rc=$rc"
      ok=0
    elif [ "$rc" -ge 128 ]; then
      fail "R2-1-$kind" "aborted/signaled rc=$rc err=$(head -c 200 "$D/${kind}.err")"
      ok=0
    elif [ "$rc" -eq 0 ]; then
      fail "R2-1-$kind" "exit 0 while copying truncated huge member"
      ok=0
    fi
  done
  # Sanity: delete of a real small member still works
  printf 'keep\n' >"$D/keep.txt"
  printf 'gone\n' >"$D/gone.txt"
  run_to "$MUTAR" -cf "$D/small.tar" -C "$D" keep.txt gone.txt \
    >"$D/c.out" 2>"$D/c.err"
  set +e
  run_to "$MUTAR" --delete -f "$D/small.tar" gone.txt \
    >"$D/d.out" 2>"$D/d.err"
  rc=$?
  set -e
  list=$(run_to "$MUTAR" -tf "$D/small.tar" 2>/dev/null || true)
  if [ "$rc" -ne 0 ]; then
    fail "R2-1-small" "normal --delete failed rc=$rc err=$(head -c 160 "$D/d.err")"
    ok=0
  elif echo "$list" | grep -q gone; then
    fail "R2-1-small" "gone.txt still listed: $list"
    ok=0
  elif ! echo "$list" | grep -q keep; then
    fail "R2-1-small" "keep.txt missing after delete: $list"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R2-1: --delete rejects huge size without crash; small delete works"
  fi
}

# ── R2-2: --record-size cap ───────────────────────────────────────────────────
echo "[R2-2] --record-size huge must not ASan-abort"
{
  D="$TMPBASE/r2_2"
  mkdir -p "$D"
  printf 'x\n' >"$D/f.txt"
  ok=1
  for sz in 16777216 167772160 999999999999; do
    set +e
    run_to "$MUTAR" -cf "$D/out.tar" --record-size="$sz" -C "$D" f.txt \
      >"$D/s$sz.out" 2>"$D/s$sz.err"
    rc=$?
    set -e
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      fail "R2-2-$sz" "timed out / killed rc=$rc"
      ok=0
    elif [ "$rc" -ge 128 ]; then
      fail "R2-2-$sz" "signaled rc=$rc err=$(head -c 200 "$D/s$sz.err")"
      ok=0
    elif [ "$rc" -eq 0 ]; then
      fail "R2-2-$sz" "accepted huge record-size"
      ok=0
    elif ! grep -qiE 'record-size|memory exhausted|too large|invalid' "$D/s$sz.err"; then
      fail "R2-2-$sz" "rc=$rc but missing size error: $(head -c 200 "$D/s$sz.err")"
      ok=0
    fi
  done
  # Valid record-size still works
  set +e
  run_to "$MUTAR" -cf "$D/ok.tar" --record-size=1024 -C "$D" f.txt \
    >"$D/ok.out" 2>"$D/ok.err"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    fail "R2-2-ok" "record-size=1024 failed rc=$rc err=$(head -c 160 "$D/ok.err")"
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    pass "R2-2: huge --record-size rejected; 1024 still works"
  fi
}

# ── R2-3: base-256 2^80 must not list as empty / exit 0 ───────────────────────
echo "[R2-3] base-256 size 2^80 rejected"
{
  D="$TMPBASE/r2_3"
  mkdir -p "$D"
  # 2^80 = 1208925819614629174706176
  write_hdr "$D/p80.tar" "huge.bin" "1208925819614629174706176" "0" 1 0 2
  set +e
  run_to "$MUTAR" -tf "$D/p80.tar" >"$D/p80.out" 2>"$D/p80.err"
  rc=$?
  set -e
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    fail "R2-3" "timed out / killed"
  elif [ "$rc" -ge 128 ]; then
    fail "R2-3" "signaled rc=$rc err=$(head -c 200 "$D/p80.err")"
  elif [ "$rc" -eq 0 ]; then
    fail "R2-3" "exit 0 on 2^80 size (decoded as 0?); out=$(head -c 80 "$D/p80.out")"
  else
    pass "R2-3: 2^80 size rejected rc=$rc"
  fi
}

# ── R2-4: multiple -C ─────────────────────────────────────────────────────────
echo "[R2-4] multiple -C applies to following names"
{
  D="$TMPBASE/r2_4"
  mkdir -p "$D/a" "$D/b" "$D/out"
  printf 'FROM-A\n' >"$D/a/x"
  printf 'FROM-B\n' >"$D/b/y"
  printf 'WRONG\n' >"$D/b/x"
  set +e
  run_to "$MUTAR" -cf "$D/m.tar" -C "$D/a" x -C "$D/b" y \
    >"$D/c.out" 2>"$D/c.err"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    fail "R2-4" "create rc=$rc err=$(head -c 200 "$D/c.err")"
  else
    run_to "$MUTAR" -xf "$D/m.tar" -C "$D/out" >"$D/x.out" 2>"$D/x.err"
    list=$(run_to "$MUTAR" -tf "$D/m.tar" 2>/dev/null | tr '\n' ' ')
    gotx=$(cat "$D/out/x" 2>/dev/null || echo '')
    goty=$(cat "$D/out/y" 2>/dev/null || echo '')
    if [ "$gotx" != "FROM-A" ]; then
      fail "R2-4" "x='$gotx' expected FROM-A; list=$list"
    elif [ "$goty" != "FROM-B" ]; then
      fail "R2-4" "y='$goty' expected FROM-B; list=$list"
    else
      pass "R2-4: -C a x -C b y archived the right files"
    fi
  fi
}

# ── R2-5: -C + -M later volumes stay at original CWD ──────────────────────────
echo "[R2-5] -C + -M volumes written at original CWD"
{
  D="$TMPBASE/r2_5"
  mkdir -p "$D/src" "$D/cwd"
  # Enough data to force at least one extra volume at -L 10 (10 KiB)
  for i in $(seq 1 40); do
    dd if=/dev/urandom of="$D/src/f$(printf '%02d' "$i").bin" bs=400 count=1 \
      status=none 2>/dev/null
  done
  set +e
  (
    cd "$D/cwd" || exit 1
    run_to "$MUTAR" -c -M -L 10 -f archive.tar -C "$D/src" .
  ) >"$D/c.out" 2>"$D/c.err"
  rc=$?
  set -e
  n_cwd=$(find "$D/cwd" -maxdepth 1 -name 'archive.tar*' -type f | wc -l)
  n_src=$(find "$D/src" -maxdepth 1 -name 'archive.tar*' -type f | wc -l)
  if [ "$rc" -ne 0 ]; then
    fail "R2-5" "create rc=$rc err=$(head -c 300 "$D/c.err")"
  elif [ "$n_cwd" -lt 2 ]; then
    fail "R2-5" "expected >=2 volumes in CWD, got $n_cwd; src_vols=$n_src err=$(head -c 200 "$D/c.err")"
  elif [ "$n_src" -ne 0 ]; then
    fail "R2-5" "volumes leaked into -C tree ($n_src); cwd=$n_cwd"
  else
    pass "R2-5: $n_cwd volumes in original CWD, none under -C"
  fi
}

echo "=== hunt R2 summary: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
