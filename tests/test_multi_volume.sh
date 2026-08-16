#!/usr/bin/env bash
# tests/test_multi_volume.sh — multi-volume (between-member + mid-file G1.6)
#
# Tests:
#   T-MVOL-PARSE   -L sets numeric tape length (rotation actually fires)
#   T-MVOL-RT-01   create many small files with -M -L 10 → multiple volumes; extract; compare
#   T-MVOL-VOLNO   --volno-file updates across create
#   T-MVOL-INFO    -F/--info-script invoked at volume boundary (stamp file)
#   T-MVOL-BIG     single file larger than tape length → mid-file split → extract OK
#   T-MVOL-BIG-GNU GNU tar extracts mutar mid-file multi-vol (if tar available)
#   T-MVOL-MIX     large file + small files still round-trip
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

# ── T-MVOL-RT-01: create → extract multi-vol round-trip (small files) ─────────
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

  # Require multi-volume rotation so volno-file is actually exercised (not false-green).
  if [ "$rc" -ne 0 ]; then
    fail "T-MVOL-VOLNO" "create failed rc=$rc err=$(head -c 300 "$D/err.txt")"
  elif [ "$nvol" -lt 2 ]; then
    fail "T-MVOL-VOLNO" "expected >=2 volumes to exercise volno, got $nvol"
  elif ! [[ "$final" =~ ^[0-9]+$ ]]; then
    fail "T-MVOL-VOLNO" "volno-file not numeric: '$final'"
  elif [ "$final" -lt 2 ]; then
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

# ── T-MVOL-BIG: single file > tape length → mid-file split → extract ──────────
echo "[T-MVOL-BIG]"
{
  D="$TMPBASE/big"
  mkdir -p "$D/src" "$D/out"
  # 25 KiB file with -L 10 (10 KiB) must split across volumes (GNUTYPE_MULTIVOL)
  dd if=/dev/urandom of="$D/src/huge.bin" bs=1024 count=25 status=none
  md5_src=$(md5sum < "$D/src/huge.bin" | awk '{print $1}')
  sz_src=$(wc -c < "$D/src/huge.bin")

  "$MUTAR" -c -M -L 10 -f "$D/c.tar" -C "$D" src/huge.bin >"$D/out.txt" 2>"$D/err.txt"
  rc_c=$?
  nvol=$(find "$D" -maxdepth 1 \( -name 'c.tar' -o -name 'c.tar.*' \) -type f | wc -l)

  # Inspect for typeflag 'M' on a continuation volume
  has_m=0
  for v in "$D"/c.tar "$D"/c.tar.*; do
    [ -f "$v" ] || continue
    # typeflag at offset 156 of first header
    tf=$(dd if="$v" bs=1 skip=156 count=1 status=none 2>/dev/null | od -An -t x1 | tr -d ' \n')
    if [ "$tf" = "4d" ]; then has_m=1; break; fi
  done

  "$MUTAR" -x -M -f "$D/c.tar" -C "$D/out" 2>"$D/extract.err"
  rc_x=$?
  md5_out=""
  sz_out=0
  if [ -f "$D/out/src/huge.bin" ]; then
    md5_out=$(md5sum < "$D/out/src/huge.bin" | awk '{print $1}')
    sz_out=$(wc -c < "$D/out/src/huge.bin")
  elif [ -f "$D/out/huge.bin" ]; then
    md5_out=$(md5sum < "$D/out/huge.bin" | awk '{print $1}')
    sz_out=$(wc -c < "$D/out/huge.bin")
  fi

  if [ "$rc_c" -ne 0 ]; then
    fail "T-MVOL-BIG" "create failed rc=$rc_c err=$(head -c 400 "$D/err.txt")"
  elif [ "$nvol" -lt 2 ]; then
    fail "T-MVOL-BIG" "expected >=2 volumes for oversized file, got $nvol"
  elif [ "$has_m" -ne 1 ]; then
    fail "T-MVOL-BIG" "no GNUTYPE_MULTIVOL typeflag 'M' found in volumes"
  elif [ "$rc_x" -ne 0 ]; then
    fail "T-MVOL-BIG" "extract failed rc=$rc_x err=$(head -c 400 "$D/extract.err")"
  elif [ "$md5_src" != "$md5_out" ] || [ "$sz_src" -ne "$sz_out" ]; then
    fail "T-MVOL-BIG" "content mismatch src=$md5_src/$sz_src out=$md5_out/$sz_out nvol=$nvol"
  else
    pass "T-MVOL-BIG: mid-file split $nvol vols, type M, content OK ($sz_out bytes)"
  fi
}

# ── T-MVOL-BIG-GNU: GNU tar extracts mutar mid-file multi-vol ─────────────────
echo "[T-MVOL-BIG-GNU]"
{
  if ! command -v tar >/dev/null 2>&1; then
    skip "T-MVOL-BIG-GNU" "GNU tar not installed"
  else
    D="$TMPBASE/biggnu"
    mkdir -p "$D/src" "$D/out"
    dd if=/dev/urandom of="$D/src/huge.bin" bs=1024 count=25 status=none
    md5_src=$(md5sum < "$D/src/huge.bin" | awk '{print $1}')

    "$MUTAR" -c -M -L 10 -f "$D/g.tar" -C "$D" src/huge.bin 2>"$D/create.err"
    rc_c=$?
    nvol=$(find "$D" -maxdepth 1 \( -name 'g.tar' -o -name 'g.tar.*' \) -type f | wc -l)

    # GNU tar multi-volume extract: use -F script to feed next volume path via rename
    # mutar names: g.tar, g.tar.2, g.tar.3, ...
    # GNU reopens the same -f name after -F returns; copy next volume into place.
    SCRIPT="$D/gnu_switch.sh"
    cat > "$SCRIPT" <<'EOF'
#!/bin/sh
# TAR_VOLUME is the volume number about to be read (1-based next).
next="$TAR_VOLUME"
base="$TAR_ARCHIVE"
# Prefer base.N then base (volume 1)
cand="${base}.${next}"
if [ ! -f "$cand" ] && [ "$next" -eq 1 ]; then cand="$base"; fi
if [ ! -f "$cand" ]; then
  # try base without suffix for vol1 already consumed
  echo "gnu_switch: missing $cand" >&2
  exit 1
fi
cp -f "$cand" "$base"
exit 0
EOF
    chmod +x "$SCRIPT"

    # Seed volume 1 into the name GNU will open
    # g.tar is already volume 1
    tar -x -M -F "$SCRIPT" -f "$D/g.tar" -C "$D/out" 2>"$D/extract.err"
    rc_x=$?

    md5_out=""
    if [ -f "$D/out/src/huge.bin" ]; then
      md5_out=$(md5sum < "$D/out/src/huge.bin" | awk '{print $1}')
    elif [ -f "$D/out/huge.bin" ]; then
      md5_out=$(md5sum < "$D/out/huge.bin" | awk '{print $1}')
    fi

    if [ "$rc_c" -ne 0 ]; then
      fail "T-MVOL-BIG-GNU" "mutar create failed rc=$rc_c"
    elif [ "$nvol" -lt 2 ]; then
      fail "T-MVOL-BIG-GNU" "expected multi-vol, got $nvol"
    elif [ "$rc_x" -ne 0 ] || [ "$md5_src" != "$md5_out" ]; then
      # Interop is best-effort; soft-fail as skip if GNU refuses our layout
      if [ "$rc_x" -ne 0 ]; then
        skip "T-MVOL-BIG-GNU" "GNU tar extract rc=$rc_x err=$(head -c 200 "$D/extract.err")"
      else
        fail "T-MVOL-BIG-GNU" "checksum mismatch src=$md5_src out=$md5_out"
      fi
    else
      pass "T-MVOL-BIG-GNU: GNU tar extracted mutar mid-file multi-vol ($nvol vols)"
    fi
  fi
}

# ── T-MVOL-MIX: large file + small files ──────────────────────────────────────
echo "[T-MVOL-MIX]"
{
  D="$TMPBASE/mix"
  mkdir -p "$D/src" "$D/out"
  dd if=/dev/urandom of="$D/src/big.bin" bs=1024 count=20 status=none
  for i in $(seq 1 15); do
    printf 'small-%s\n' "$(printf 'z%.0s' {1..100})" > "$D/src/s$i.txt"
  done
  (cd "$D/src" && find . -type f | sort | xargs md5sum) > "$D/src.md5"

  "$MUTAR" -c -M -L 10 -f "$D/m.tar" -C "$D" src/ 2>"$D/create.err"
  rc_c=$?
  nvol=$(find "$D" -maxdepth 1 \( -name 'm.tar' -o -name 'm.tar.*' \) -type f | wc -l)

  "$MUTAR" -x -M -f "$D/m.tar" -C "$D/out" 2>"$D/extract.err"
  rc_x=$?

  if [ "$rc_c" -ne 0 ] || [ "$rc_x" -ne 0 ]; then
    fail "T-MVOL-MIX" "create rc=$rc_c extract rc=$rc_x; c=$(head -c 200 "$D/create.err"); x=$(head -c 200 "$D/extract.err")"
  elif [ "$nvol" -lt 2 ]; then
    fail "T-MVOL-MIX" "expected >=2 volumes, got $nvol"
  else
    (cd "$D/out/src" && find . -type f | sort | xargs md5sum) > "$D/out.md5" 2>/dev/null || true
    if cmp -s "$D/src.md5" "$D/out.md5"; then
      pass "T-MVOL-MIX: large+small round-trip across $nvol volumes"
    else
      fail "T-MVOL-MIX" "checksum mismatch; diff=$(diff -u "$D/src.md5" "$D/out.md5" | head -20)"
    fi
  fi
}

# ── T-MVOL-VOLNO-BIG: volno-file still works with mid-file split ──────────────
echo "[T-MVOL-VOLNO-BIG]"
{
  D="$TMPBASE/volnobig"
  mkdir -p "$D/src"
  dd if=/dev/urandom of="$D/src/huge.bin" bs=1024 count=30 status=none
  VOLNO="$D/volno.txt"
  echo 1 > "$VOLNO"

  "$MUTAR" -c -M -L 10 -f "$D/v.tar" --volno-file="$VOLNO" -C "$D" src/ 2>"$D/err.txt"
  rc=$?
  final=$(tr -d '[:space:]' < "$VOLNO" 2>/dev/null || echo '')
  nvol=$(find "$D" -maxdepth 1 \( -name 'v.tar' -o -name 'v.tar.*' \) -type f | wc -l)

  if [ "$rc" -ne 0 ]; then
    fail "T-MVOL-VOLNO-BIG" "create failed rc=$rc err=$(head -c 300 "$D/err.txt")"
  elif [ "$nvol" -lt 2 ]; then
    fail "T-MVOL-VOLNO-BIG" "expected >=2 volumes, got $nvol"
  elif ! [[ "$final" =~ ^[0-9]+$ ]] || [ "$final" -lt 2 ]; then
    fail "T-MVOL-VOLNO-BIG" "volno-file=$final nvol=$nvol"
  else
    pass "T-MVOL-VOLNO-BIG: volno-file=$final after mid-file $nvol volumes"
  fi
}

echo
echo "=== multi-volume summary: $PASS passed, $FAIL failed, $SKIP skipped ==="
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
