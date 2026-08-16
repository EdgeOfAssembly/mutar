#!/usr/bin/env bash
# tests/test_hunt_fixes.sh — hunt-bug regressions (crash / hang / empty create / gzip)
#
# F1  checksum-valid L/K/x/g with size -1 or INT64_MAX must not abort
# F2  skip_entry on a huge claimed size + short file must not hang
# CLI-1  create with no members and no -T must refuse (must not archive CWD)
# F4  compressor/decompressor child failure must not exit 0
# E2  extract regular must not O_TRUNC through an existing hardlink
# E1  hardlink must not follow an extracted directory symlink
#
# Usage: ./test_hunt_fixes.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
MUTAR="$(cd "$(dirname "$MUTAR")" && pwd)/$(basename "$MUTAR")"
TMPBASE="$(mktemp -d /tmp/mutar_hunt_fixes.XXXXXX)"
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

echo "=== mutar hunt-fix regressions ==="

# ── F1: huge / negative metadata size must not throw or abort ─────────────────
echo "[F1] read_data_string huge/negative L/K/x/g size"
{
  D="$TMPBASE/f1"
  mkdir -p "$D"
  ok=1
  for spec in "L:max:1" "K:neg:1" "x:max:1" "g:neg:1"; do
    tf="${spec%%:*}"
    rest="${spec#*:}"
    kind="${rest%%:*}"
    if [ "$kind" = "max" ]; then
      sz="9223372036854775807"  # INT64_MAX
      b256=1
    else
      sz="-1"
      b256=1
    fi
    write_hdr "$D/${tf}_${kind}.tar" "././@LongLink" "$sz" "$tf" "$b256" 0 2
    set +e
    run_to "$MUTAR" -tf "$D/${tf}_${kind}.tar" >"$D/${tf}_${kind}.out" 2>"$D/${tf}_${kind}.err"
    rc=$?
    set -e
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      fail "F1-$tf-$kind" "timed out / killed (hang or stuck alloc) rc=$rc"
      ok=0
    elif [ "$rc" -ge 128 ]; then
      fail "F1-$tf-$kind" "aborted/signaled rc=$rc err=$(head -c 200 "$D/${tf}_${kind}.err")"
      ok=0
    elif [ "$rc" -eq 0 ]; then
      fail "F1-$tf-$kind" "exit 0 on insane $tf size=$sz; out=$(head -c 80 "$D/${tf}_${kind}.out")"
      ok=0
    fi
  done
  if [ "$ok" -eq 1 ]; then
    pass "F1: insane L/K/x/g sizes rejected without crash/hang"
  fi
}

# ── F2: skip_entry must stop at EOF (4GiB / INT64_MAX claimed size) ───────────
echo "[F2] skip_entry ignores EOF"
{
  D="$TMPBASE/f2"
  mkdir -p "$D"
  ok=1
  # 4GiB regular, header only (no data)
  write_hdr "$D/4g.tar" "huge.bin" "4294967296" "0" 0 0 0
  set +e
  run_to "$MUTAR" -tf "$D/4g.tar" >"$D/4g.out" 2>"$D/4g.err"
  rc=$?
  set -e
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    fail "F2-4g" "hang (timeout) listing 4GiB-claimed short archive"
    ok=0
  elif [ "$rc" -ge 128 ]; then
    fail "F2-4g" "signaled rc=$rc"
    ok=0
  elif [ "$rc" -eq 0 ]; then
    fail "F2-4g" "exit 0 on truncated 4GiB member"
    ok=0
  fi

  write_hdr "$D/max.tar" "huge.bin" "9223372036854775807" "0" 1 0 0
  set +e
  run_to "$MUTAR" -tf "$D/max.tar" >"$D/max.out" 2>"$D/max.err"
  rc=$?
  set -e
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    fail "F2-max" "hang (timeout) listing INT64_MAX-claimed short archive"
    ok=0
  elif [ "$rc" -ge 128 ]; then
    fail "F2-max" "signaled rc=$rc"
    ok=0
  elif [ "$rc" -eq 0 ]; then
    fail "F2-max" "exit 0 on truncated INT64_MAX member"
    ok=0
  fi

  if [ "$ok" -eq 1 ]; then
    pass "F2: skip_entry stops on EOF and fails (no hang)"
  fi
}

# ── CLI-1: create with no members must refuse (and not truncate) ──────────────
echo "[CLI-1] create with no members"
{
  D="$TMPBASE/cli1"
  mkdir -p "$D/cwd"
  printf 'KEEP\n' >"$D/existing.tar"
  printf 'cwd-file\n' >"$D/cwd/should_not_archive.txt"
  set +e
  (
    cd "$D/cwd" || exit 1
    run_to "$MUTAR" -cf "$D/existing.tar"
  ) >"$D/out" 2>"$D/err"
  rc=$?
  set -e
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    fail "CLI-1" "timed out"
  elif [ "$rc" -eq 0 ]; then
    fail "CLI-1" "exit 0; archived CWD or created empty? err=$(head -c 200 "$D/err")"
  elif ! grep -qi 'empty archive\|refusing' "$D/err"; then
    fail "CLI-1" "rc=$rc but missing refuse message: $(head -c 200 "$D/err")"
  elif [ "$(cat "$D/existing.tar")" != "KEEP" ]; then
    fail "CLI-1" "existing archive was truncated/overwritten"
  else
    pass "CLI-1: refuse empty create; did not truncate existing.tar"
  fi

  # -T empty is allowed (GNU writes an empty archive)
  : >"$D/empty.list"
  set +e
  run_to "$MUTAR" -cf "$D/from_T.tar" -T "$D/empty.list" >"$D/t.out" 2>"$D/t.err"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    fail "CLI-1-T" "empty -T should create empty archive rc=$rc err=$(head -c 160 "$D/t.err")"
  else
    pass "CLI-1-T: empty --files-from writes empty archive (rc=0)"
  fi
}

# ── F4: compressor / decompressor child failure must fail ─────────────────────
echo "[F4] compressor/decompressor child status"
{
  D="$TMPBASE/f4"
  mkdir -p "$D"
  printf 'hello\n' >"$D/f.txt"
  cat >"$D/fail.sh" <<'EOF'
#!/bin/sh
# Consume stdin so the parent write does not SIGPIPE, then fail.
cat >/dev/null
exit 1
EOF
  chmod +x "$D/fail.sh"

  set +e
  run_to "$MUTAR" -cf "$D/out.tar" --use-compress-program="$D/fail.sh" -C "$D" f.txt \
    >"$D/c.out" 2>"$D/c.err"
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    fail "F4-create" "create with failing compressor exited 0"
  elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    fail "F4-create" "timed out"
  else
    pass "F4-create: failing compressor rc=$rc"
  fi

  if ! command -v gzip >/dev/null 2>&1; then
    fail "F4-gzip" "gzip not installed"
  else
    run_to "$MUTAR" -czf "$D/ok.tar.gz" -C "$D" f.txt >"$D/g.out" 2>"$D/g.err"
    # Truncate so gzip -d fails (CRC / unexpected EOF)
    dd if="$D/ok.tar.gz" of="$D/bad.tar.gz" bs=1 count=16 status=none 2>/dev/null \
      || head -c 16 "$D/ok.tar.gz" >"$D/bad.tar.gz"
    set +e
    run_to "$MUTAR" -tzf "$D/bad.tar.gz" >"$D/l.out" 2>"$D/l.err"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
      fail "F4-gzip" "list of truncated gzip exited 0"
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      fail "F4-gzip" "timed out"
    else
      pass "F4-gzip: truncated gzip list rc=$rc"
    fi
  fi
}

# ── E2: extract regular through existing hardlink ─────────────────────────────
echo "[E2] extract regular must not write through hardlink"
{
  D="$TMPBASE/e2"
  mkdir -p "$D/src" "$D/dst"
  printf 'pwned\n' >"$D/src/file"
  printf 'orig\n' >"$D/dst/keep"
  ln "$D/dst/keep" "$D/dst/file"
  run_to "$MUTAR" -cf "$D/a.tar" -C "$D/src" file >"$D/c.out" 2>"$D/c.err"
  run_to "$MUTAR" -xf "$D/a.tar" -C "$D/dst" >"$D/x.out" 2>"$D/x.err"
  keep=$(cat "$D/dst/keep" 2>/dev/null || echo '')
  got=$(cat "$D/dst/file" 2>/dev/null || echo '')
  if [ "$keep" != "orig" ]; then
    fail "E2" "hardlink peer overwritten (keep='$keep')"
  elif [ "$got" != "pwned" ]; then
    fail "E2" "extracted file='$got' expected pwned; keep intact"
  else
    pass "E2: unlinked hardlink before extract; peer intact"
  fi
}

# ── E1: hardlink must not follow a directory symlink ──────────────────────────
echo "[E1] hardlink through dir symlink must not escape"
{
  D="$TMPBASE/e1"
  mkdir -p "$D/src/d" "$D/dst" "$D/outside"
  printf 'payload\n' >"$D/src/innocent"
  ln "$D/src/innocent" "$D/src/d/escaped"
  printf 'safe\n' >"$D/outside/keep"
  ln -s "$D/outside" "$D/dst/d"
  run_to "$MUTAR" -cf "$D/a.tar" -C "$D/src" innocent d/escaped >"$D/c.out" 2>"$D/c.err"
  run_to "$MUTAR" -xf "$D/a.tar" -C "$D/dst" >"$D/x.out" 2>"$D/x.err"
  if [ -e "$D/outside/escaped" ]; then
    fail "E1" "wrote outside via dir symlink: $(ls -l "$D/outside")"
  elif [ "$(cat "$D/outside/keep")" != "safe" ]; then
    fail "E1" "outside/keep corrupted"
  else
    pass "E1: hardlink did not follow dest dir symlink"
  fi
}

echo "=== hunt-fix summary: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
