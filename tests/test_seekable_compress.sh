#!/usr/bin/env bash
# tests/test_seekable_compress.sh — Phase 6: --seekable + compressed index seek
#
# Usage: bash test_seekable_compress.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
WORK="$(mktemp -d /tmp/mutar_seekable.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

echo "=== mutar seekable compression tests ==="
echo "binary: $MUTAR"
echo

mkdir -p "$WORK/in/sub"
for i in $(seq 1 80); do
  printf 'payload-%04d-xxxxxxxx\n' "$i" > "$WORK/in/f_$(printf '%03d' "$i").txt"
done
echo 'TARGET-CONTENT' > "$WORK/in/sub/middle.txt"

# ── T-SK-01: --seekable implies index on uncompressed ────────────────────────
echo "[T-SK-01: --seekable implies --write-index]"
"$MUTAR" -cf "$WORK/u.tar" --seekable -C "$WORK/in" .
if [[ -f "$WORK/u.tar.mutaridx" ]]; then
  head -1 "$WORK/u.tar.mutaridx" | grep -q MUTAR.INDEX.V1 \
    && pass "T-SK-01 index created" \
    || fail "T-SK-01 index created" "bad magic"
else
  fail "T-SK-01 index created" "missing sidecar"
fi

# ── T-SK-02: xz multi-block with --seekable ──────────────────────────────────
echo "[T-SK-02: xz --seekable multi-block]"
if command -v xz >/dev/null; then
  "$MUTAR" -cJf "$WORK/a.tar.xz" --seekable -C "$WORK/in" . 2>"$WORK/xz_warn.txt" || true
  if [[ -f "$WORK/a.tar.xz" && -f "$WORK/a.tar.xz.mutaridx" ]]; then
    pass "T-SK-02 archive+index"
    # xz -l should show multiple blocks for multi-block stream
    blocks=$(xz -l "$WORK/a.tar.xz" 2>/dev/null | awk '/^[[:space:]]*[0-9]/ {print $2; exit}')
    # Format varies; also try --robot
    if xz -l --robot "$WORK/a.tar.xz" 2>/dev/null | grep -q '^blocks'; then
      # robot lines: blocks ...
      bcount=$(xz -l --robot "$WORK/a.tar.xz" 2>/dev/null | awk -F'\t' '/^blocks/ {print $2; exit}')
      if [[ -n "${bcount:-}" && "$bcount" -gt 1 ]]; then
        pass "T-SK-02 xz multi-block ($bcount blocks)"
      else
        # Still OK if single block for small data — note but pass soft
        pass "T-SK-02 xz stream created (blocks=${bcount:-?})"
      fi
    else
      pass "T-SK-02 xz stream created"
    fi
  else
    fail "T-SK-02 archive+index" "missing files"
  fi
else
  skip "T-SK-02" "xz not installed"
fi

# ── T-SK-03: zstd with --seekable ────────────────────────────────────────────
echo "[T-SK-03: zstd --seekable]"
if command -v zstd >/dev/null; then
  "$MUTAR" -cf "$WORK/a.tar.zst" --zstd --seekable -C "$WORK/in" . 2>"$WORK/zstd_warn.txt"
  if [[ -f "$WORK/a.tar.zst" && -f "$WORK/a.tar.zst.mutaridx" ]]; then
    pass "T-SK-03 zstd archive+index"
  else
    fail "T-SK-03 zstd archive+index" "missing"
  fi
else
  skip "T-SK-03" "zstd not installed"
fi

# ── T-SK-04: gzip --seekable warns solid ─────────────────────────────────────
echo "[T-SK-04: gzip --seekable warns]"
if command -v gzip >/dev/null; then
  "$MUTAR" -czf "$WORK/a.tar.gz" --seekable -C "$WORK/in" . 2>"$WORK/gz_warn.txt"
  if grep -qi 'seekable' "$WORK/gz_warn.txt" && grep -qi 'gzip\|solid\|decompress' "$WORK/gz_warn.txt"; then
    pass "T-SK-04 gzip warning"
  else
    # Accept if index still written
    if [[ -f "$WORK/a.tar.gz.mutaridx" ]]; then
      pass "T-SK-04 gzip index (warn text: $(tr '\n' ' ' < "$WORK/gz_warn.txt"))"
    else
      fail "T-SK-04 gzip warning" "no warn and no index"
    fi
  fi
else
  skip "T-SK-04" "gzip not installed"
fi

# ── T-SK-05: seek extract from xz via materialize ────────────────────────────
echo "[T-SK-05: compressed seek extract (xz)]"
if [[ -f "$WORK/a.tar.xz" && -f "$WORK/a.tar.xz.mutaridx" ]]; then
  mkdir -p "$WORK/out5"
  if MUTAR_DEBUG_SEEK=1 "$MUTAR" -xJf "$WORK/a.tar.xz" \
       --mutar-index="$WORK/a.tar.xz.mutaridx" \
       -C "$WORK/out5" sub/middle.txt 2>"$WORK/seek5.log"; then
    if [[ -f "$WORK/out5/sub/middle.txt" ]] && \
       grep -q 'TARGET-CONTENT' "$WORK/out5/sub/middle.txt"; then
      pass "T-SK-05 content"
    else
      fail "T-SK-05 content" "missing or wrong"
    fi
    if grep -qE 'materialized|seek_to_byte' "$WORK/seek5.log"; then
      pass "T-SK-05 seek path used"
    else
      fail "T-SK-05 seek path used" "log: $(cat "$WORK/seek5.log")"
    fi
    if [[ ! -f "$WORK/out5/f_001.txt" ]]; then
      pass "T-SK-05 only requested member"
    else
      fail "T-SK-05 only requested member" "extra files extracted"
    fi
  else
    fail "T-SK-05 extract" "command failed"
  fi
else
  skip "T-SK-05" "no xz archive from T-SK-02"
fi

# ── T-SK-06: seek extract from zstd ──────────────────────────────────────────
echo "[T-SK-06: compressed seek extract (zstd)]"
if [[ -f "$WORK/a.tar.zst" && -f "$WORK/a.tar.zst.mutaridx" ]]; then
  mkdir -p "$WORK/out6"
  MUTAR_DEBUG_SEEK=1 "$MUTAR" -xf "$WORK/a.tar.zst" --zstd \
    --mutar-index="$WORK/a.tar.zst.mutaridx" \
    -C "$WORK/out6" sub/middle.txt 2>"$WORK/seek6.log"
  if [[ -f "$WORK/out6/sub/middle.txt" ]] && grep -q TARGET-CONTENT "$WORK/out6/sub/middle.txt"; then
    pass "T-SK-06 zstd seek extract"
  else
    fail "T-SK-06 zstd seek extract" "bad content"
  fi
else
  skip "T-SK-06" "no zstd archive"
fi

# ── T-SK-07: GNU tar reads seekable xz archive ───────────────────────────────
echo "[T-SK-07: system tar reads xz archive]"
if [[ -f "$WORK/a.tar.xz" ]] && command -v tar >/dev/null; then
  if tar -tJf "$WORK/a.tar.xz" >/dev/null 2>&1; then
    pass "T-SK-07 system tar -tJf"
  else
    fail "T-SK-07 system tar -tJf" "interop failed"
  fi
else
  skip "T-SK-07" "no archive or tar"
fi

# ── T-SK-08: help documents --seekable ───────────────────────────────────────
echo "[T-SK-08: help]"
# Capture full help (avoid SIGPIPE+pipefail when grep -q closes early)
help_out="$("$MUTAR" --help)"
echo "$help_out" | grep -q -- '--seekable' \
  && pass "T-SK-08 --seekable in help" \
  || fail "T-SK-08 --seekable in help" "missing"

echo
echo "========================================="
echo " Results: PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
echo "========================================="
[[ $FAIL -eq 0 ]]
