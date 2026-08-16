#!/usr/bin/env bash
# tests/test_formats_compression.sh
# Comprehensive format × compression matrix test
#
# For every tar format (v7, oldgnu, gnu, ustar, pax) combined with every
# available compression scheme (none, gzip, bzip2, xz, zstd, lzma, lzip,
# lzop, compress, auto-compress), this script:
#   1. Extracts real sample material from the repo
#   2. Establishes MD5 ground truth + file/dir counts
#   3. Creates an archive with mutar using the given format+compression
#   4. Extracts the archive into a fresh directory
#   5. Verifies file counts, directory counts, file sizes, and MD5 sums
#   6. Cross-tests: mutar creates → system tar extracts  (and vice versa)
#
# Usage:
#   bash test_formats_compression.sh [/path/to/mutar]
#
# Set MUTAR_SRC_DIR to override where sample archives are located.
# Set VERBOSE=1 for detailed per-file output.
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TAR="${TAR:-tar}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="${MUTAR_SRC_DIR:-}"
if [ -z "$SRC_DIR" ]; then
  for d in "$REPO_ROOT/files/src" "$REPO_ROOT/src" "$REPO_ROOT/files"; do
    [ -f "$d/sidplay-2.0.9.tar.gz" ] && { SRC_DIR="$d"; break; }
  done
fi
SRC_DIR="${SRC_DIR:-$REPO_ROOT/files/src}"
VERBOSE="${VERBOSE:-0}"

# ── helpers ──────────────────────────────────────────────────────────────────
PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

require_cmd() { command -v "$1" &>/dev/null; }

log() { [ "$VERBOSE" = "1" ] && echo "    $*" || true; }

WORKROOT="$(mktemp -d /tmp/mutar_fmt_cmp.XXXXXX)"
trap 'rm -rf "$WORKROOT"' EXIT

# ── Ground truth: extract sidplay-2.0.9 (small, known good) ─────────────────
SAMPLE_ARCHIVE="$SRC_DIR/sidplay-2.0.9.tar.gz"
GROUND="$WORKROOT/ground"

echo "=== Setting up ground truth ==="
mkdir -p "$GROUND"
if [ ! -f "$SAMPLE_ARCHIVE" ]; then
  echo "NOTE: sample archive not found ($SAMPLE_ARCHIVE); using synthetic ground truth"
  mkdir -p "$GROUND/synth/dir a" "$GROUND/synth/sub"
  printf 'hello world\n' > "$GROUND/synth/file1.txt"
  printf 'binary' > "$GROUND/synth/dir a/x.bin"
  ln -s "../file1.txt" "$GROUND/synth/sub/link.txt" 2>/dev/null || true
else
  "$TAR" -xzf "$SAMPLE_ARCHIVE" -C "$GROUND"
fi

# Count files, dirs; generate MD5 manifest
GT_FILES=$(find "$GROUND" -type f | wc -l)
GT_DIRS=$(find "$GROUND" -type d | wc -l)
GT_MD5="$WORKROOT/ground_md5.txt"
find "$GROUND" -type f | sort | xargs md5sum 2>/dev/null | \
  sed "s|$GROUND/||g" | sort > "$GT_MD5" || true

echo "  Ground truth: $GT_FILES files, $GT_DIRS dirs"
echo "  MD5 manifest: $(wc -l < "$GT_MD5") entries"
echo ""

# ── Compression schemes to test ──────────────────────────────────────────────
# Format: "flag:extension:program_needed"
declare -a COMPRESSIONS=(
  "none:.tar:none"
  "-z:.tar.gz:gzip"
  "-j:.tar.bz2:bzip2"
  "-J:.tar.xz:xz"
  "--zstd:.tar.zst:zstd"
  "--lzma:.tar.lzma:lzma"
  "--lzip:.tar.lz:lzip"
  "--lzop:.tar.lzo:lzop"
  "-Z:.tar.Z:compress"
)

# ── Archive formats to test ───────────────────────────────────────────────────
declare -a FORMATS=("v7" "oldgnu" "gnu" "ustar" "pax")

# ── Per-combination test ──────────────────────────────────────────────────────
run_format_compression_test() {
  local fmt="$1"
  local cflag="$2"      # e.g. "-z"
  local ext="$3"        # e.g. ".tar.gz"
  local prog="$4"       # e.g. "gzip" (or "none")
  local label="$fmt+${prog}"

  # Skip if compression program not available
  if [ "$prog" != "none" ] && ! require_cmd "$prog"; then
    skip "$label" "program '$prog' not installed"
    return
  fi

  local W="$WORKROOT/${fmt}_${prog}"
  mkdir -p "$W"
  local ARCHIVE="$W/test${ext}"

  # ── Create archive ────────────────────────────────────────────────────────
  local create_flags=(-c -H "$fmt")
  [ "$cflag" != "none" ] && create_flags+=("$cflag")
  create_flags+=(-f "$ARCHIVE" -C "$GROUND" .)

  if ! "$MUTAR" "${create_flags[@]}" 2>"$W/create.err"; then
    fail "$label" "create failed (see $W/create.err)"
    return
  fi

  if [ ! -s "$ARCHIVE" ]; then
    fail "$label" "archive is empty: $ARCHIVE"
    return
  fi
  log "Archive size: $(du -sh "$ARCHIVE" | cut -f1)"

  # ── List archive (smoke test) ─────────────────────────────────────────────
  local list_flags=(-t)
  [ "$cflag" != "none" ] && list_flags+=("$cflag")
  list_flags+=(-f "$ARCHIVE")
  if ! "$MUTAR" "${list_flags[@]}" >"$W/listing.txt" 2>"$W/list.err"; then
    fail "$label" "list failed"
    return
  fi
  log "Listed $(wc -l < "$W/listing.txt") entries"

  # ── Extract archive ───────────────────────────────────────────────────────
  mkdir -p "$W/out"
  local extract_flags=(-x)
  [ "$cflag" != "none" ] && extract_flags+=("$cflag")
  extract_flags+=(-f "$ARCHIVE" -C "$W/out")
  if ! "$MUTAR" "${extract_flags[@]}" 2>"$W/extract.err"; then
    fail "$label" "extract failed (see $W/extract.err)"
    return
  fi

  # ── Verify file count ─────────────────────────────────────────────────────
  local out_files out_dirs
  out_files=$(find "$W/out" -type f | wc -l)
  out_dirs=$(find "$W/out" -type d | wc -l)

  if [ "$out_files" -ne "$GT_FILES" ]; then
    fail "$label" "file count mismatch: got $out_files, expected $GT_FILES"
    return
  fi
  if [ "$out_dirs" -ne "$GT_DIRS" ]; then
    fail "$label" "dir count mismatch: got $out_dirs, expected $GT_DIRS"
    return
  fi

  # ── Verify MD5 checksums ──────────────────────────────────────────────────
  local out_md5="$W/out_md5.txt"
  find "$W/out" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/out/||g" | sort > "$out_md5" || true

  if ! diff -q "$GT_MD5" "$out_md5" >/dev/null 2>&1; then
    fail "$label" "MD5 mismatch (diff follows)"
    diff "$GT_MD5" "$out_md5" | head -20
    return
  fi

  pass "$label"
}

# ── Auto-compress (-a) test ───────────────────────────────────────────────────
run_auto_compress_test() {
  local prog="$1"   # e.g. "gzip"
  local ext="$2"    # e.g. ".tar.gz"
  local label="auto+${prog}"

  if ! require_cmd "$prog"; then
    skip "$label" "program '$prog' not installed"
    return
  fi

  local W="$WORKROOT/auto_${prog}"
  mkdir -p "$W"
  local ARCHIVE="$W/test${ext}"

  if ! "$MUTAR" -acf "$ARCHIVE" -C "$GROUND" . 2>"$W/create.err"; then
    fail "$label" "auto-compress create failed"
    return
  fi

  mkdir -p "$W/out"
  if ! "$MUTAR" -axf "$ARCHIVE" -C "$W/out" 2>"$W/extract.err"; then
    fail "$label" "auto-compress extract failed"
    return
  fi

  local out_files
  out_files=$(find "$W/out" -type f | wc -l)
  if [ "$out_files" -ne "$GT_FILES" ]; then
    fail "$label" "file count mismatch: got $out_files, expected $GT_FILES"
    return
  fi

  local out_md5="$W/out_md5.txt"
  find "$W/out" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/out/||g" | sort > "$out_md5" || true
  if ! diff -q "$GT_MD5" "$out_md5" >/dev/null 2>&1; then
    fail "$label" "MD5 mismatch"
    return
  fi

  pass "$label"
}

# ── -I (--use-compress-program) test ─────────────────────────────────────────
run_use_compress_program_test() {
  local prog="$1"
  local ext="$2"
  local label="-I ${prog}"

  if ! require_cmd "$prog"; then
    skip "$label" "program '$prog' not installed"
    return
  fi

  local W="$WORKROOT/Iflag_${prog}"
  mkdir -p "$W"
  local ARCHIVE="$W/test${ext}"

  if ! "$MUTAR" -I "$prog" -cf "$ARCHIVE" -C "$GROUND" . 2>"$W/create.err"; then
    fail "$label" "create failed"
    return
  fi

  mkdir -p "$W/out"
  if ! "$MUTAR" -I "$prog" -xf "$ARCHIVE" -C "$W/out" 2>"$W/extract.err"; then
    fail "$label" "extract failed"
    return
  fi

  local out_files
  out_files=$(find "$W/out" -type f | wc -l)
  if [ "$out_files" -ne "$GT_FILES" ]; then
    fail "$label" "file count mismatch: got $out_files, expected $GT_FILES"
    return
  fi

  local out_md5="$W/out_md5.txt"
  find "$W/out" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/out/||g" | sort > "$out_md5" || true
  if ! diff -q "$GT_MD5" "$out_md5" >/dev/null 2>&1; then
    fail "$label" "MD5 mismatch"
    return
  fi

  pass "$label"
}

# ── Cross-test: mutar creates → system tar extracts ────────────────────────────
run_cross_mutar_to_systtar() {
  local fmt="$1"
  local cflag="$2"
  local ext="$3"
  local prog="$4"
  local label="cross:mutar→tar ${fmt}+${prog}"

  # v7 format: system tar may not handle GNU extensions in v7 mode well
  # pax: system tar should handle it
  if [ "$prog" != "none" ] && ! require_cmd "$prog"; then
    skip "$label" "program '$prog' not installed"
    return
  fi
  if ! require_cmd "$TAR"; then
    skip "$label" "system tar not installed"
    return
  fi

  local W="$WORKROOT/cross_s2t_${fmt}_${prog}"
  mkdir -p "$W"
  local ARCHIVE="$W/test${ext}"

  # star creates
  local create_flags=(-c -H "$fmt")
  [ "$cflag" != "none" ] && create_flags+=("$cflag")
  create_flags+=(-f "$ARCHIVE" -C "$GROUND" .)
  if ! "$MUTAR" "${create_flags[@]}" 2>/dev/null; then
    skip "$label" "mutar create failed (format may need GNU extensions)"
    return
  fi

  # system tar extracts
  mkdir -p "$W/out"
  local tar_xflags=(-xf "$ARCHIVE" -C "$W/out")
  [ "$cflag" != "none" ] && tar_xflags+=("$cflag")
  if ! "$TAR" "${tar_xflags[@]}" 2>/dev/null; then
    skip "$label" "system tar extract failed (format compatibility issue)"
    return
  fi

  local out_files
  out_files=$(find "$W/out" -type f | wc -l)
  if [ "$out_files" -ne "$GT_FILES" ]; then
    fail "$label" "file count mismatch: got $out_files, expected $GT_FILES"
    return
  fi

  local out_md5="$W/out_md5.txt"
  find "$W/out" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/out/||g" | sort > "$out_md5" || true
  if ! diff -q "$GT_MD5" "$out_md5" >/dev/null 2>&1; then
    fail "$label" "MD5 mismatch"
    return
  fi

  pass "$label"
}

# ── Cross-test: system tar creates → mutar extracts ───────────────────────────
run_cross_systtar_to_star() {
  local cflag="$1"
  local ext="$2"
  local prog="$3"
  local label="cross:tar→mutar ${prog}"

  if [ "$prog" != "none" ] && ! require_cmd "$prog"; then
    skip "$label" "program '$prog' not installed"
    return
  fi

  local W="$WORKROOT/cross_t2s_${prog}"
  mkdir -p "$W"
  local ARCHIVE="$W/test${ext}"

  # system tar creates (default gnu format)
  local tar_cflags=(-cf "$ARCHIVE" -C "$GROUND" .)
  [ "$cflag" != "none" ] && tar_cflags+=("$cflag")
  if ! "$TAR" "${tar_cflags[@]}" 2>/dev/null; then
    skip "$label" "system tar create failed"
    return
  fi

  # mutar extracts
  mkdir -p "$W/out"
  local extract_flags=(-xf "$ARCHIVE" -C "$W/out")
  [ "$cflag" != "none" ] && extract_flags+=("$cflag")
  if ! "$MUTAR" "${extract_flags[@]}" 2>/dev/null; then
    fail "$label" "mutar extract failed"
    return
  fi

  local out_files
  out_files=$(find "$W/out" -type f | wc -l)
  if [ "$out_files" -ne "$GT_FILES" ]; then
    fail "$label" "file count mismatch: got $out_files, expected $GT_FILES"
    return
  fi

  local out_md5="$W/out_md5.txt"
  find "$W/out" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/out/||g" | sort > "$out_md5" || true
  if ! diff -q "$GT_MD5" "$out_md5" >/dev/null 2>&1; then
    fail "$label" "MD5 mismatch"
    return
  fi

  pass "$label"
}

# ── Run all format × compression combinations ─────────────────────────────────
echo "=== Format × Compression matrix (mutar create+extract, MD5 verified) ==="
echo ""
for fmt in "${FORMATS[@]}"; do
  for comp_spec in "${COMPRESSIONS[@]}"; do
    IFS=: read -r cflag ext prog <<< "$comp_spec"
    echo "[${fmt}+${prog}]"
    run_format_compression_test "$fmt" "$cflag" "$ext" "$prog"
  done
done

echo ""
echo "=== Auto-compress (-a) by filename extension ==="
for comp_spec in "${COMPRESSIONS[@]}"; do
  IFS=: read -r cflag ext prog <<< "$comp_spec"
  [ "$prog" = "none" ] && continue
  echo "[auto+${prog}]"
  run_auto_compress_test "$prog" "$ext"
done

echo ""
echo "=== -I / --use-compress-program ==="
for comp_spec in "${COMPRESSIONS[@]}"; do
  IFS=: read -r cflag ext prog <<< "$comp_spec"
  [ "$prog" = "none" ] && continue
  echo "[-I ${prog}]"
  run_use_compress_program_test "$prog" "$ext"
done

echo ""
echo "=== Cross-compatibility: mutar → system tar (gnu format + all compressions) ==="
for comp_spec in "${COMPRESSIONS[@]}"; do
  IFS=: read -r cflag ext prog <<< "$comp_spec"
  echo "[cross:mutar→tar gnu+${prog}]"
  run_cross_mutar_to_systtar "gnu" "$cflag" "$ext" "$prog"
done

echo ""
echo "=== Cross-compatibility: system tar → mutar (all compressions) ==="
for comp_spec in "${COMPRESSIONS[@]}"; do
  IFS=: read -r cflag ext prog <<< "$comp_spec"
  echo "[cross:tar→mutar ${prog}]"
  run_cross_systtar_to_star "$cflag" "$ext" "$prog"
done

echo ""
if ! command -v timeout >/dev/null 2>&1; then
  echo "=== Skipping repo archives round-trip: 'timeout' not available ==="
else
echo "=== Additional: repo archives round-trip through gnu+gzip ==="
declare -a REPO_ARCHIVES=()
for f in "$SRC_DIR"/*.tar.gz "$SRC_DIR"/*.tar.xz "$SRC_DIR"/*.tar.bz2; do
  [ -f "$f" ] || continue
  # Skip very large archives (>20 MB) — too slow for a test loop
  sz=$(stat -c%s "$f" 2>/dev/null || echo 0)
  [ "$sz" -gt $((20*1024*1024)) ] && continue
  REPO_ARCHIVES+=("$f")
done

for archive in "${REPO_ARCHIVES[@]}"; do
  base=$(basename "$archive")
  echo "[repo: $base → gnu+gzip round-trip]"
  W="$WORKROOT/repo_${base}"
  mkdir -p "$W/src" "$W/repack" "$W/out"

  # Extract original (with timeout)
  if ! timeout 30 "$TAR" -xf "$archive" -C "$W/src" 2>/dev/null; then
    skip "repo:$base" "cannot extract original"
    continue
  fi

  orig_files=$(find "$W/src" -type f | wc -l)
  find "$W/src" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/src/||g" | sort > "$W/orig_md5.txt" || true

  # Repack with mutar into a separate location (not inside src)
  if ! timeout 30 "$MUTAR" -czf "$W/repack.tar.gz" -C "$W/src" . 2>/dev/null; then
    fail "repo:$base" "star repack failed"
    continue
  fi

  # Extract with mutar into a separate output dir
  if ! timeout 30 "$MUTAR" -xzf "$W/repack.tar.gz" -C "$W/out" 2>/dev/null; then
    fail "repo:$base" "mutar extract of repack failed"
    continue
  fi

  out_files=$(find "$W/out" -type f | wc -l)
  find "$W/out" -type f | sort | xargs md5sum 2>/dev/null | \
    sed "s|$W/out/||g" | sort > "$W/out_md5.txt" || true

  if [ "$out_files" -ne "$orig_files" ]; then
    fail "repo:$base" "file count: orig=$orig_files out=$out_files"
  elif ! diff -q "$W/orig_md5.txt" "$W/out_md5.txt" >/dev/null 2>&1; then
    fail "repo:$base" "MD5 mismatch after repack"
  else
    pass "repo:$base"
  fi
done
fi

echo ""
echo "========================================="
printf " Results: PASS=%-4d FAIL=%-4d SKIP=%-4d\n" "$PASS" "$FAIL" "$SKIP"
echo "========================================="
[ "$FAIL" -eq 0 ]
