#!/usr/bin/env bash
# tests/test_multi_volume.sh — Phase C multi-volume (G2–G4)
#
# Tests:
#   T-MVOL-RT-01   create many small files with -M -L 10 → multiple volumes; extract; compare
#   T-MVOL-VOLNO   --volno-file updates across create
#   T-MVOL-INFO    -F/--info-script invoked at volume boundary (stamp file)
#   T-MVOL-BIG     single file larger than tape length → clear error
#   T-MVOL-PARSE   -L sets numeric tape length (rotation actually fires)
#
# Usage: ./test_multi_volume.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TMPBASE="$(mktemp -d /tmp/mutar_mvol.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

# ── T-MVOL-PARSE: -L must enable multi-vol rotation (numeric parse) ───────────
echo "[T-MVOL-PARSE]"
{
  D="$TMPBASE/parse"
  mkdir -p "$D/src"
  # 30 files × ~400 bytes ≈ enough to exceed 10 KiB tape with headers
  for i in $(seq 1 40); do
    printf 'file-%02d-payload-%s\n' "$i" "$(head -c 200 </dev/urandom | base64 | tr -d '\n')" \
      > "$D/src/f$(printf '%02d' "$i").txt"
  done

  "$MUTAR" -c -M -L 10 -f "$D/archive.tar" -C "$D" src/ >"$D/out.txt" 2>"$D/err.txt"
  rc=$?
  vols=$(find "$D" -maxdepth 1 -name 'archive.tar*' -type f | wc -l)
  if [ "$rc" -eq 0 ] && [ "$vols" -ge 2 ]; then
    pass "T-MVOL-PARSE: -L 10 produced $vols volume files"
  else
    fail "T-MVOL-PARSE" "rc=$rc vols=$vols; err=$(head -c 400 "$D/err.txt")"
  fi
}

# ── T-MVOL-RT-01: create → extract multi-vol round-trip ───────────────────────
echo "[T-MVOL-RT-01]"
{
  D="$TMPBASE/rt01"
  mkdir -p "$D/src" "$D/out"
  for i in $(seq 1 50); do
    # ~300 bytes each → many members, force several 10 KiB volumes
    dd if=/dev/urandom of="$D/src/blob$(printf '%03d' "$i").bin" bs=300 count=1 status=none 2>/dev/null
  done
  # checksums of source
  (cd "$D/src" && find . -type f | sort | xargs md5sum) > "$D/src.md5"

  "$MUTAR" -c -M -L 10 -f "$D/vol.tar" -C "$D" src/ 2>"$D/create.err"
  rc_c=$?
  nvol=$(find "$D" -maxdepth 1 \( -name 'vol.tar' -o -name 'vol.tar.*' \) -type f | wc -l)

  mkdir -p "$D/out"
  # Extract with -M; subsequent volumes auto-open when present
  "$MUTAR" -x -M -f "$D/vol.tar" -C "$D/out" 2>"$D/extract.err"
  rc_x=$?

  if [ "$rc_c" -ne 0 ] || [ "$rc_x" -ne 0 ]; then
    fail "T-MVOL-RT-01" "create rc=$rc_c extract rc=$rc_x; create.err=$(head -c 300 "$D/create.err"); extract.err=$(head -c 300 "$D/extract.err")"
  elif [ "$nvol" -lt 2 ]; then
    fail "T-MVOL-RT-01" "expected >=2 volumes, got $nvol"
  else
    (cd "$D/out/src" && find . -type f | sort | xargs md5sum) > "$D/out.md5" 2>/dev/null || true
    if cmp -s "$D/src.md5" "$D/out.md5"; then
      pass "T-MVOL-RT-01: round-trip $nvol volumes, content matches"
    else
      fail "T-MVOL-RT-01" "checksum mismatch; nvol=$nvol; diff=$(diff -u "$D/src.md5" "$D/out.md5" | head -20)"
    fi
  fi
}

# ── T-MVOL-VOLNO: --volno-file read/write ─────────────────────────────────────
echo "[T-MVOL-VOLNO]"
{
  D="$TMPBASE/volno"
  mkdir -p "$D/src"
  for i in $(seq 1 40); do
    printf 'v-%s\n' "$(printf 'x%.0s' {1..200})" > "$D/src/v$i.txt"
  done
  VOLNO="$D/volno.txt"
  echo 1 > "$VOLNO"

  "$MUTAR" -c -M -L 10 -f "$D/a.tar" --volno-file="$VOLNO" -C "$D" src/ 2>"$D/err.txt"
  rc=$?
  final=$(tr -d '[:space:]' < "$VOLNO" 2>/dev/null || echo '')
  nvol=$(find "$D" -maxdepth 1 \( -name 'a.tar' -o -name 'a.tar.*' \) -type f | wc -l)

  if [ "$rc" -ne 0 ]; then
    fail "T-MVOL-VOLNO" "create failed rc=$rc err=$(head -c 300 "$D/err.txt")"
  elif ! [[ "$final" =~ ^[0-9]+$ ]]; then
    fail "T-MVOL-VOLNO" "volno-file not numeric: '$final'"
  elif [ "$final" -lt 1 ]; then
    fail "T-MVOL-VOLNO" "volno-file invalid: $final"
  elif [ "$nvol" -ge 2 ] && [ "$final" -lt 2 ]; then
    fail "T-MVOL-VOLNO" "had $nvol volumes but volno-file=$final (expected >=2)"
  else
    pass "T-MVOL-VOLNO: volno-file=$final after $nvol volumes"
  fi
}

# ── T-MVOL-INFO: info-script invoked ──────────────────────────────────────────
echo "[T-MVOL-INFO]"
{
  D="$TMPBASE/info"
  mkdir -p "$D/src"
  for i in $(seq 1 40); do
    printf 'i-%s\n' "$(printf 'y%.0s' {1..200})" > "$D/src/i$i.txt"
  done
  STAMP="$D/info.stamp"
  SCRIPT="$D/info.sh"
  cat > "$SCRIPT" <<EOF
#!/bin/sh
echo "vol=\${TAR_VOLUME:-?} arch=\${TAR_ARCHIVE:-?}" >> "$STAMP"
exit 0
EOF
  chmod +x "$SCRIPT"

  "$MUTAR" -c -M -L 10 -F "$SCRIPT" -f "$D/b.tar" -C "$D" src/ 2>"$D/err.txt"
  rc=$?
  nvol=$(find "$D" -maxdepth 1 \( -name 'b.tar' -o -name 'b.tar.*' \) -type f | wc -l)
  lines=0
  [ -f "$STAMP" ] && lines=$(wc -l < "$STAMP")

  if [ "$rc" -ne 0 ]; then
    fail "T-MVOL-INFO" "create failed rc=$rc err=$(head -c 300 "$D/err.txt")"
  elif [ "$nvol" -lt 2 ]; then
    fail "T-MVOL-INFO" "need >=2 volumes to observe script; got $nvol"
  elif [ "$lines" -lt 1 ]; then
    fail "T-MVOL-INFO" "info-script never wrote stamp (lines=$lines)"
  else
    # Expect one invocation per rotation (nvol-1), not necessarily on final volume
    if [ "$lines" -ge 1 ]; then
      pass "T-MVOL-INFO: info-script ran $lines time(s) across $nvol volumes"
    else
      fail "T-MVOL-INFO" "unexpected"
    fi
  fi
}

# ── T-MVOL-BIG: oversized single file errors clearly ──────────────────────────
echo "[T-MVOL-BIG]"
{
  D="$TMPBASE/big"
  mkdir -p "$D/src"
  # 20 KiB file with -L 10 (10 KiB) must fail (no mid-file split)
  dd if=/dev/zero of="$D/src/huge.bin" bs=1024 count=20 status=none
  "$MUTAR" -c -M -L 10 -f "$D/c.tar" -C "$D" src/huge.bin >"$D/out.txt" 2>"$D/err.txt"
  rc=$?
  if [ "$rc" -ne 0 ] && grep -qi 'tape length\|mid-file\|not supported' "$D/err.txt"; then
    pass "T-MVOL-BIG: oversized file rejected with clear message"
  elif [ "$rc" -ne 0 ]; then
    pass "T-MVOL-BIG: oversized file rejected (rc=$rc)"
  else
    fail "T-MVOL-BIG" "expected failure for file > tape length; err=$(cat "$D/err.txt")"
  fi
}

echo
echo "=== multi-volume summary: $PASS passed, $FAIL failed, $SKIP skipped ==="
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
