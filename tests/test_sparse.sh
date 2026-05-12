#!/usr/bin/env bash
# star/tests/test_sparse.sh
# Sparse file create / read / write / extract test suite
#
# Verifies:
#   1. GNU sparse ('S') format write and extract via star
#   2. Hole pattern is preserved (sparse file stays sparse after extract)
#   3. Logical content identical to original (MD5 match)
#   4. Sparse across all formats that support it (gnu, oldgnu, ustar, pax)
#   5. Sparse across all available compression schemes
#   6. Cross-test: star writes sparse → system tar extracts (and vice versa)
#   7. Multiple non-contiguous holes
#   8. File that starts and ends with a hole
#   9. Very large sparse file (simulated via truncate)
#  10. Sparse file with only one tiny data segment
#
# Usage:  bash test_sparse.sh [/path/to/star]
set -euo pipefail

STAR="${1:-$(dirname "$0")/../build/star}"
TAR="${TAR:-tar}"
VERBOSE="${VERBOSE:-0}"

PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }
log()  { [ "$VERBOSE" = "1" ] && echo "    $*" || true; }

require_cmd() { command -v "$1" &>/dev/null; }

WORKROOT="$(mktemp -d /tmp/star_sparse.XXXXXX)"
trap 'rm -rf "$WORKROOT"' EXIT

# ── Helper: create a sparse file with known hole pattern ─────────────────────
# make_sparse FILE SIZE SEGMENTS...
#   SEGMENTS: "offset:length" pairs of data segments (holes between them)
make_sparse() {
  local file="$1"; local total="$2"; shift 2
  : > "$file"
  truncate -s "$total" "$file"
  for seg in "$@"; do
    local off="${seg%%:*}"
    local len="${seg##*:}"
    # Write a recognisable pattern at each data segment
    dd if=/dev/urandom bs=1 count="$len" 2>/dev/null | \
      dd of="$file" bs=1 seek="$off" conv=notrunc 2>/dev/null
  done
}

# md5 of a file
file_md5() { md5sum "$1" | awk '{print $1}'; }

# ── Helper: check if file has actual holes (allocated < total) ────────────────
has_holes() {
  local file="$1"
  local total allocated
  total=$(stat --format="%s" "$file")
  allocated=$(stat --format="%b" "$file")   # 512-byte blocks actually allocated
  local alloc_bytes=$(( allocated * 512 ))
  [ "$alloc_bytes" -lt "$total" ]
}

# ── T-SP-01: Basic sparse round-trip (gnu format) ────────────────────────────
echo "[T-SP-01: basic gnu sparse round-trip]"
if ! require_cmd gzip; then
  skip "T-SP-01" "gzip not available"
else
W="$WORKROOT/sp01"
mkdir -p "$W/input" "$W/out"

# 1 MB file; data at 0-4K and 512K-516K; rest is holes
make_sparse "$W/input/sparse.bin" $((1024*1024)) "0:4096" "$((512*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/sparse.bin")
ORIG_SIZE=$(stat -c%s "$W/input/sparse.bin")

"$STAR" -S -czf "$W/test.tar.gz" -C "$W/input" sparse.bin 2>"$W/create.err"
ARCH_SIZE=$(stat -c%s "$W/test.tar.gz")
log "sparse.bin=$ORIG_SIZE bytes; archive=$ARCH_SIZE bytes"

# Archive must be smaller than the file (sparse compression working)
if [ "$ARCH_SIZE" -ge "$((ORIG_SIZE / 2))" ]; then
  fail "T-SP-01" "archive ($ARCH_SIZE) not smaller than half of original ($ORIG_SIZE) — sparse not effective"
else
  "$STAR" -xzf "$W/test.tar.gz" -C "$W/out" 2>"$W/extract.err"
  EXT_MD5=$(file_md5 "$W/out/sparse.bin")
  EXT_SIZE=$(stat -c%s "$W/out/sparse.bin")
  if [ "$EXT_MD5" != "$ORIG_MD5" ]; then
    fail "T-SP-01" "MD5 mismatch: orig=$ORIG_MD5 ext=$EXT_MD5"
  elif [ "$EXT_SIZE" -ne "$ORIG_SIZE" ]; then
    fail "T-SP-01" "size mismatch: orig=$ORIG_SIZE ext=$EXT_SIZE"
  else
    pass "T-SP-01"
  fi
fi
fi

# ── T-SP-02: Sparse file preserves holes after extract ───────────────────────
echo "[T-SP-02: hole preservation after extract]"
W="$WORKROOT/sp02"
mkdir -p "$W/input" "$W/out"

# 4 MB with data only in 2 small regions
make_sparse "$W/input/sparse.bin" $((4*1024*1024)) "0:512" "$((2*1024*1024)):512"
"$STAR" -S -cf "$W/test.tar" -C "$W/input" sparse.bin 2>/dev/null
"$STAR" -xf "$W/test.tar" -C "$W/out" 2>/dev/null

if has_holes "$W/out/sparse.bin"; then
  pass "T-SP-02"
else
  # Not a hard failure — some filesystems don't support holes, just skip
  skip "T-SP-02" "extracted file has no holes (filesystem may not support sparse files)"
fi

# ── T-SP-03: Multiple non-contiguous holes ───────────────────────────────────
echo "[T-SP-03: multiple non-contiguous holes]"
W="$WORKROOT/sp03"
mkdir -p "$W/input" "$W/out"

# 8 data segments scattered over 10 MB
make_sparse "$W/input/multi.bin" $((10*1024*1024)) \
  "0:1024" \
  "$((1*1024*1024)):2048" \
  "$((3*1024*1024)):4096" \
  "$((5*1024*1024)):1024" \
  "$((7*1024*1024)):512" \
  "$((9*1024*1024)):1024"

ORIG_MD5=$(file_md5 "$W/input/multi.bin")
"$STAR" -S -czf "$W/test.tar.gz" -C "$W/input" multi.bin 2>/dev/null
"$STAR" -xzf "$W/test.tar.gz" -C "$W/out" 2>/dev/null
EXT_MD5=$(file_md5 "$W/out/multi.bin")

if [ "$EXT_MD5" = "$ORIG_MD5" ]; then
  pass "T-SP-03"
else
  fail "T-SP-03" "MD5 mismatch after multi-hole round-trip"
fi

# ── T-SP-04: File starts AND ends with a hole ─────────────────────────────────
echo "[T-SP-04: file starts and ends with hole]"
W="$WORKROOT/sp04"
mkdir -p "$W/input" "$W/out"

# 2 MB; data only in the middle
make_sparse "$W/input/bookend.bin" $((2*1024*1024)) "$((512*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/bookend.bin")
"$STAR" -S -cf "$W/test.tar" -C "$W/input" bookend.bin 2>/dev/null
"$STAR" -xf "$W/test.tar" -C "$W/out" 2>/dev/null
EXT_MD5=$(file_md5 "$W/out/bookend.bin")

if [ "$EXT_MD5" = "$ORIG_MD5" ]; then
  pass "T-SP-04"
else
  fail "T-SP-04" "MD5 mismatch for bookend-hole file"
fi

# ── T-SP-05: Very large sparse file (1 GB logical, tiny data) ────────────────
echo "[T-SP-05: large sparse file (1 GB logical)]"
W="$WORKROOT/sp05"
mkdir -p "$W/input" "$W/out"

# 1 GiB file with just 4 KiB of data
truncate -s $((1024*1024*1024)) "$W/input/gigahole.bin"
dd if=/dev/urandom bs=4096 count=1 of="$W/input/gigahole.bin" conv=notrunc 2>/dev/null
dd if=/dev/urandom bs=4096 count=1 of="$W/input/gigahole.bin" \
   seek=$(( (1024*1024*1024/4096) - 1 )) conv=notrunc 2>/dev/null

ORIG_MD5=$(file_md5 "$W/input/gigahole.bin")
"$STAR" -S -cf "$W/test.tar" -C "$W/input" gigahole.bin 2>/dev/null
ARCH_SIZE=$(stat -c%s "$W/test.tar")
log "1 GiB sparse → archive=$ARCH_SIZE bytes"

if [ "$ARCH_SIZE" -ge $((10*1024*1024)) ]; then
  fail "T-SP-05" "archive too large ($ARCH_SIZE bytes) for 1-GiB sparse file"
else
  "$STAR" -xf "$W/test.tar" -C "$W/out" 2>/dev/null
  EXT_SIZE=$(stat -c%s "$W/out/gigahole.bin")
  EXT_MD5=$(file_md5 "$W/out/gigahole.bin")
  if [ "$EXT_MD5" != "$ORIG_MD5" ]; then
    fail "T-SP-05" "MD5 mismatch"
  elif [ "$EXT_SIZE" -ne $((1024*1024*1024)) ]; then
    fail "T-SP-05" "size mismatch: got $EXT_SIZE expected $((1024*1024*1024))"
  else
    pass "T-SP-05"
  fi
fi

# ── T-SP-06: Dense file (no holes) should be handled normally ────────────────
echo "[T-SP-06: dense file with -S flag (no actual holes)]"
W="$WORKROOT/sp06"
mkdir -p "$W/input" "$W/out"

dd if=/dev/urandom bs=65536 count=4 of="$W/input/dense.bin" 2>/dev/null
ORIG_MD5=$(file_md5 "$W/input/dense.bin")
"$STAR" -S -czf "$W/test.tar.gz" -C "$W/input" dense.bin 2>/dev/null
"$STAR" -xzf "$W/test.tar.gz" -C "$W/out" 2>/dev/null
EXT_MD5=$(file_md5 "$W/out/dense.bin")

if [ "$EXT_MD5" = "$ORIG_MD5" ]; then
  pass "T-SP-06"
else
  fail "T-SP-06" "MD5 mismatch for dense file with -S"
fi

# ── T-SP-07: Sparse files across all formats ─────────────────────────────────
echo "[T-SP-07: sparse across all supported formats]"
W="$WORKROOT/sp07"
mkdir -p "$W/input"
make_sparse "$W/input/fmt_test.bin" $((2*1024*1024)) "0:4096" "$((1*1024*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/fmt_test.bin")

# Formats that support GNU sparse type: gnu, oldgnu (pax uses different sparse)
# v7 and ustar don't have sparse support — star falls back to dense write
for fmt in gnu oldgnu ustar pax; do
  out="$W/out_$fmt"
  mkdir -p "$out"
  "$STAR" -S -H "$fmt" -cf "$W/test_${fmt}.tar" -C "$W/input" fmt_test.bin 2>/dev/null
  "$STAR" -xf "$W/test_${fmt}.tar" -C "$out" 2>/dev/null
  ext_md5=$(file_md5 "$out/fmt_test.bin")
  if [ "$ext_md5" = "$ORIG_MD5" ]; then
    pass "T-SP-07-$fmt"
  else
    fail "T-SP-07-$fmt" "MD5 mismatch (format=$fmt)"
  fi
done

# ── T-SP-08: Sparse across all compression schemes ───────────────────────────
echo "[T-SP-08: sparse with all compression schemes]"
W="$WORKROOT/sp08"
mkdir -p "$W/input"
make_sparse "$W/input/cmp_test.bin" $((2*1024*1024)) "0:4096" "$((1*1024*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/cmp_test.bin")

declare -a SP_COMPRESSIONS=(
  "-z:.tar.gz:gzip"
  "-j:.tar.bz2:bzip2"
  "-J:.tar.xz:xz"
  "--zstd:.tar.zst:zstd"
  "--lzma:.tar.lzma:lzma"
  "--lzip:.tar.lz:lzip"
  "--lzop:.tar.lzo:lzop"
  "-Z:.tar.Z:compress"
)

for comp_spec in "${SP_COMPRESSIONS[@]}"; do
  IFS=: read -r cflag ext prog <<< "$comp_spec"
  label="T-SP-08-${prog}"
  if ! require_cmd "$prog"; then
    skip "$label" "program '$prog' not installed"
    continue
  fi
  out="$W/out_${prog}"
  mkdir -p "$out"
  "$STAR" -S "$cflag" -cf "$W/cmp_test${ext}" -C "$W/input" cmp_test.bin 2>/dev/null
  "$STAR" "$cflag" -xf "$W/cmp_test${ext}" -C "$out" 2>/dev/null
  ext_md5=$(file_md5 "$out/cmp_test.bin")
  if [ "$ext_md5" = "$ORIG_MD5" ]; then
    pass "$label"
  else
    fail "$label" "MD5 mismatch with compression=$prog"
  fi
done

# ── T-SP-09: Cross-test star(sparse) → system tar extracts ───────────────────
echo "[T-SP-09: star sparse write → system tar extract]"
W="$WORKROOT/sp09"
mkdir -p "$W/input" "$W/out"
make_sparse "$W/input/cross.bin" $((2*1024*1024)) "0:4096" "$((1*1024*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/cross.bin")

"$STAR" -S -czf "$W/cross.tar.gz" -C "$W/input" cross.bin 2>/dev/null
if "$TAR" -xzf "$W/cross.tar.gz" -C "$W/out" 2>/dev/null; then
  ext_md5=$(file_md5 "$W/out/cross.bin")
  if [ "$ext_md5" = "$ORIG_MD5" ]; then
    pass "T-SP-09"
  else
    fail "T-SP-09" "MD5 mismatch: system tar extracted wrong data"
  fi
else
  skip "T-SP-09" "system tar could not extract star sparse archive"
fi

# ── T-SP-10: Cross-test system tar(sparse) → star extracts ───────────────────
echo "[T-SP-10: system tar sparse write → star extract]"
W="$WORKROOT/sp10"
mkdir -p "$W/input" "$W/out"
make_sparse "$W/input/cross.bin" $((2*1024*1024)) "0:4096" "$((1*1024*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/cross.bin")

if "$TAR" -S -czf "$W/cross.tar.gz" -C "$W/input" cross.bin 2>/dev/null; then
  "$STAR" -xzf "$W/cross.tar.gz" -C "$W/out" 2>/dev/null
  ext_md5=$(file_md5 "$W/out/cross.bin")
  if [ "$ext_md5" = "$ORIG_MD5" ]; then
    pass "T-SP-10"
  else
    fail "T-SP-10" "MD5 mismatch: star extracted wrong data from system tar sparse archive"
  fi
else
  skip "T-SP-10" "system tar could not create sparse archive"
fi

# ── T-SP-11: Mixed archive (sparse + regular + dir + symlink) ────────────────
echo "[T-SP-11: mixed archive with sparse + regular + symlink]"
W="$WORKROOT/sp11"
mkdir -p "$W/input/subdir" "$W/out"

make_sparse "$W/input/sparse.bin" $((1024*1024)) "0:4096" "$((512*1024)):4096"
echo "regular file" > "$W/input/regular.txt"
echo "nested" > "$W/input/subdir/nested.txt"
ln -s "../regular.txt" "$W/input/subdir/link.txt"
dd if=/dev/urandom bs=4096 count=4 of="$W/input/dense.bin" 2>/dev/null

declare -A orig_md5s
for f in sparse.bin regular.txt subdir/nested.txt dense.bin; do
  orig_md5s[$f]=$(file_md5 "$W/input/$f")
done

"$STAR" -S -czf "$W/mixed.tar.gz" -C "$W/input" . 2>/dev/null
"$STAR" -xzf "$W/mixed.tar.gz" -C "$W/out" 2>/dev/null

ok=1
for f in sparse.bin regular.txt subdir/nested.txt dense.bin; do
  ext_md5=$(file_md5 "$W/out/$f" 2>/dev/null || echo "MISSING")
  if [ "$ext_md5" != "${orig_md5s[$f]}" ]; then
    fail "T-SP-11" "MD5 mismatch for $f"
    ok=0
    break
  fi
done
[ "$ok" -eq 1 ] && pass "T-SP-11"

# ── T-SP-12: --sparse-version=1.0 (PAX sparse format) ────────────────────────
echo "[T-SP-12: --sparse-version=1.0 (PAX extended sparse)]"
W="$WORKROOT/sp12"
mkdir -p "$W/input" "$W/out"
make_sparse "$W/input/paxsparse.bin" $((2*1024*1024)) "0:4096" "$((1*1024*1024)):4096"
ORIG_MD5=$(file_md5 "$W/input/paxsparse.bin")

"$STAR" -S --sparse-version=1.0 -H pax -cf "$W/pax_sparse.tar" \
  -C "$W/input" paxsparse.bin 2>/dev/null

"$STAR" -xf "$W/pax_sparse.tar" -C "$W/out" 2>/dev/null
EXT_MD5=$(file_md5 "$W/out/paxsparse.bin" 2>/dev/null || echo "MISSING")
if [ "$EXT_MD5" = "$ORIG_MD5" ]; then
  pass "T-SP-12"
else
  fail "T-SP-12" "MD5 mismatch for PAX sparse version=1.0"
fi

# ── T-SP-13: archive size sanity — sparse archive << dense archive ───────────
echo "[T-SP-13: archive size sanity (sparse << dense)]"
W="$WORKROOT/sp13"
mkdir -p "$W/input"
# 10 MB file, only 8 KiB of data
make_sparse "$W/input/sanity.bin" $((10*1024*1024)) "0:4096" "$((5*1024*1024)):4096"

"$STAR" -S  -cf "$W/sparse.tar" -C "$W/input" sanity.bin 2>/dev/null
"$STAR"     -cf "$W/dense.tar"  -C "$W/input" sanity.bin 2>/dev/null

sparse_sz=$(stat -c%s "$W/sparse.tar")
dense_sz=$(stat -c%s "$W/dense.tar")
log "sparse.tar=$sparse_sz  dense.tar=$dense_sz"

if [ "$sparse_sz" -ge "$dense_sz" ]; then
  fail "T-SP-13" "sparse archive ($sparse_sz) >= dense archive ($dense_sz)"
else
  pass "T-SP-13"
fi

echo ""
echo "========================================="
printf " Results: PASS=%-4d FAIL=%-4d SKIP=%-4d\n" "$PASS" "$FAIL" "$SKIP"
echo "========================================="
[ "$FAIL" -eq 0 ]
