#!/usr/bin/env bash
# tests/test_index_seek.sh — MUTAR.INDEX.V1 sidecar index + seek extract
#
# Usage: bash test_index_seek.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TAR="${TAR:-tar}"
WORK="$(mktemp -d /tmp/mutar_idx.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }

echo "=== mutar index/seek tests ==="
echo "binary: $MUTAR"
echo "work:   $WORK"
echo

# Ground tree with many members so middle-file extract is meaningful
mkdir -p "$WORK/in/sub/deep"
for i in $(seq 1 50); do
  printf 'payload-%03d\n' "$i" > "$WORK/in/file_$(printf '%03d' "$i").txt"
done
echo 'middle-marker' > "$WORK/in/sub/deep/target.txt"
echo 'link-target' > "$WORK/in/sub/linktgt"
ln -s linktgt "$WORK/in/sub/thelink"
mkdir -p "$WORK/in/emptydir"

# ── T-IDX-01: create with --write-index produces sidecar ─────────────────────
echo "[T-IDX-01: --write-index creates ARCHIVE.mutaridx]"
if "$MUTAR" -cf "$WORK/a.tar" --write-index -C "$WORK/in" .; then
  if [[ -f "$WORK/a.tar.mutaridx" ]]; then
    head -1 "$WORK/a.tar.mutaridx" | grep -q 'MUTAR.INDEX.V1' \
      && pass "T-IDX-01 magic" \
      || fail "T-IDX-01 magic" "bad header: $(head -1 "$WORK/a.tar.mutaridx")"
    n=$(grep -c . "$WORK/a.tar.mutaridx" || true)
    # header + members
    [[ "$n" -gt 10 ]] && pass "T-IDX-01 member count ($n lines)" \
      || fail "T-IDX-01 member count" "only $n lines"
  else
    fail "T-IDX-01 sidecar" "missing $WORK/a.tar.mutaridx"
  fi
else
  fail "T-IDX-01 create" "mutar -c --write-index failed"
fi

# ── T-IDX-02: --mutar-index=PATH explicit path ───────────────────────────────
echo "[T-IDX-02: --mutar-index=PATH]"
if "$MUTAR" -cf "$WORK/b.tar" --mutar-index="$WORK/custom.idx" -C "$WORK/in" .; then
  [[ -f "$WORK/custom.idx" ]] && pass "T-IDX-02 explicit path" \
    || fail "T-IDX-02 explicit path" "custom.idx missing"
else
  fail "T-IDX-02 create" "failed"
fi

# ── T-IDX-03: fast list from index (no archive scan required for names) ──────
echo "[T-IDX-03: list via index]"
list_out="$("$MUTAR" -tf "$WORK/a.tar" --mutar-index="$WORK/a.tar.mutaridx" 2>/dev/null || true)"
echo "$list_out" | grep -q 'file_001.txt' \
  && pass "T-IDX-03 lists file_001" \
  || fail "T-IDX-03 lists file_001" "not in list"
echo "$list_out" | grep -q 'sub/deep/target.txt' \
  && pass "T-IDX-03 lists target" \
  || fail "T-IDX-03 lists target" "not in list"
# Auto-detect sidecar next to archive
list_auto="$("$MUTAR" -tf "$WORK/a.tar" 2>/dev/null || true)"
echo "$list_auto" | grep -q 'file_050.txt' \
  && pass "T-IDX-03 auto-detect sidecar" \
  || fail "T-IDX-03 auto-detect sidecar" "missing file_050"

# ── T-IDX-04: GNU tar still reads the archive ────────────────────────────────
echo "[T-IDX-04: GNU tar interop]"
if command -v "$TAR" >/dev/null; then
  if "$TAR" -tf "$WORK/a.tar" >/dev/null 2>&1; then
    pass "T-IDX-04 system tar -tf"
  else
    fail "T-IDX-04 system tar -tf" "tar cannot list mutar archive"
  fi
else
  echo "  SKIP: T-IDX-04 (no system tar)"
fi

# ── T-IDX-05: selective extract uses seek (MUTAR_DEBUG_SEEK) ─────────────────
echo "[T-IDX-05: seek extract middle file]"
mkdir -p "$WORK/out5"
seek_log="$WORK/seek.log"
if MUTAR_DEBUG_SEEK=1 "$MUTAR" -xf "$WORK/a.tar" \
     --mutar-index="$WORK/a.tar.mutaridx" \
     -C "$WORK/out5" sub/deep/target.txt 2>"$seek_log"; then
  if [[ -f "$WORK/out5/sub/deep/target.txt" ]]; then
    content=$(cat "$WORK/out5/sub/deep/target.txt")
    [[ "$content" == "middle-marker" ]] \
      && pass "T-IDX-05 content" \
      || fail "T-IDX-05 content" "got: $content"
  else
    fail "T-IDX-05 extract" "file missing"
  fi
  if grep -q 'seek_to_byte' "$seek_log"; then
    pass "T-IDX-05 seek instrumentation"
  else
    # May still pass if index path not taken; show log for diagnosis
    fail "T-IDX-05 seek instrumentation" "no seek_to_byte in log: $(cat "$seek_log")"
  fi
  # Should not have extracted unrelated files
  if [[ ! -f "$WORK/out5/file_001.txt" ]]; then
    pass "T-IDX-05 only requested member"
  else
    fail "T-IDX-05 only requested member" "file_001 also extracted"
  fi
else
  fail "T-IDX-05 extract" "command failed"
fi

# ── T-IDX-06: without index, sequential still works ──────────────────────────
echo "[T-IDX-06: sequential without index]"
"$MUTAR" -cf "$WORK/c.tar" -C "$WORK/in" .  # no --write-index
mkdir -p "$WORK/out6"
"$MUTAR" -xf "$WORK/c.tar" -C "$WORK/out6" file_010.txt
[[ -f "$WORK/out6/file_010.txt" ]] \
  && pass "T-IDX-06 sequential selective extract" \
  || fail "T-IDX-06 sequential selective extract" "missing"

# ── T-IDX-07: index list matches sequential list (same members) ──────────────
echo "[T-IDX-07: index list == sequential list]"
# Force sequential by removing sidecar temporarily
mv "$WORK/a.tar.mutaridx" "$WORK/a.tar.mutaridx.bak"
seq_list="$("$MUTAR" -tf "$WORK/a.tar" | sed 's|^\./||' | sort)"
mv "$WORK/a.tar.mutaridx.bak" "$WORK/a.tar.mutaridx"
idx_list="$("$MUTAR" -tf "$WORK/a.tar" --mutar-index="$WORK/a.tar.mutaridx" | sed 's|^\./||' | sort)"
if [[ "$seq_list" == "$idx_list" ]]; then
  pass "T-IDX-07 member sets equal"
else
  fail "T-IDX-07 member sets equal" "differs"
  diff -u <(echo "$seq_list") <(echo "$idx_list") | head -40 || true
fi

# ── T-IDX-08: no-args / help mentions new options ────────────────────────────
echo "[T-IDX-08: help documents index options]"
help="$("$MUTAR" --help)"
echo "$help" | grep -q -- '--write-index' \
  && pass "T-IDX-08 --write-index in help" \
  || fail "T-IDX-08 --write-index in help" "missing"
echo "$help" | grep -q -- '--mutar-index' \
  && pass "T-IDX-08 --mutar-index in help" \
  || fail "T-IDX-08 --mutar-index in help" "missing"

echo
echo "========================================="
echo " Results: PASS=$PASS  FAIL=$FAIL"
echo "========================================="
[[ $FAIL -eq 0 ]]
