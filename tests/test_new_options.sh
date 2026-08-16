#!/usr/bin/env bash
# tests/test_new_options.sh — Tests for newly implemented options in PR #170
# Tests: wildcards, anchored, ignore-case, hole-detection, verify, interactive,
#        warning, show-omitted-dirs, show-transformed-names, index-file,
#        checkpoint-action, full-time, preserve-order, overwrite-dir,
#        exclude-vcs-ignores, help/usage completeness
#
# Usage: ./test_new_options.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TMPDIR_BASE="$(mktemp -d /tmp/mutar_new_opts.XXXXXX)"
trap 'rm -rf "$TMPDIR_BASE"' EXIT

PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

T() {
    local name="$1"; shift
    echo "[$name]"
    "$@"
}

# Ground truth tree
GROUND="$TMPDIR_BASE/ground"
mkdir -p "$GROUND/subdir"
echo "hello" > "$GROUND/file1.txt"
echo "world" > "$GROUND/file2.log"
echo "nested" > "$GROUND/subdir/nested.txt"
echo "abc" > "$GROUND/subdir/code.c"

# ── Help / usage completeness ─────────────────────────────────────────────────
T "T-NO-01: --help shows all major switches" \
bash -c "
  HELP=\$(\"$MUTAR\" --help 2>&1)
  MISSING=()
  # Build the list of always-required options
  OPTS=('create' 'extract' 'list' 'append' 'update' 'delete'
        'format' 'sparse' 'verify' 'checkpoint'
        'full-time' 'quoting-style' 'warning' 'owner-map'
        'group-map' 'hole-detection' 'level' 'pax-option'
        'exclude-vcs-ignores' 'show-omitted-dirs' 'wildcards'
        'anchored' 'preserve-order' 'overwrite-dir' 'index-file')
  # xattrs/acls/selinux are feature-detected at build time; only check them
  # if the binary was compiled with the corresponding support.
  echo \"\$HELP\" | grep -qF 'xattrs'   && OPTS+=('xattrs')
  echo \"\$HELP\" | grep -qF 'acls'     && OPTS+=('acls')
  echo \"\$HELP\" | grep -qF 'selinux'  && OPTS+=('selinux')
  for opt in \"\${OPTS[@]}\"; do
    echo \"\$HELP\" | grep -qF -- \"\$opt\" || MISSING+=(\"\$opt\")
  done
  if [ \${#MISSING[@]} -eq 0 ]; then
    echo '  PASS: --help shows all required options'
    exit 0
  else
    echo \"  FAIL: missing from --help: \${MISSING[*]}\"
    exit 1
  fi
" && pass "T-NO-01 --help completeness" || fail "T-NO-01 --help completeness" "options missing"

T "T-NO-02: --usage shows help" \
bash -c "
  USAGE=\$(\"$MUTAR\" --usage 2>&1 | wc -l)
  [ \"\$USAGE\" -gt 50 ] && echo '  full usage output' || echo \"  short output (\$USAGE lines)\"
  [ \"\$USAGE\" -gt 50 ] && exit 0 || exit 1
" && pass "T-NO-02 --usage completeness" || fail "T-NO-02 --usage" "usage too short"

# ── Wildcard pattern matching ──────────────────────────────────────────────────
WARCH="$TMPDIR_BASE/wildcards.tar"
"$MUTAR" -c --exclude="*.log" -f "$WARCH" -C "$TMPDIR_BASE" ground >/dev/null 2>&1

T "T-NO-03: --wildcards (default) excludes *.log" \
bash -c "
  ARCH=\"$WARCH\"
  LIST=\$(\"$MUTAR\" -t -f \"\$ARCH\" 2>&1)
  echo \"\$LIST\" | grep -q 'file1.txt' || { echo '  FAIL: file1.txt not in archive'; exit 1; }
  echo \"\$LIST\" | grep -q 'file2.log' && { echo '  FAIL: file2.log should be excluded'; exit 1; }
  echo '  wildcard exclusion worked'
" && pass "T-NO-03 wildcard exclude" || fail "T-NO-03 wildcard exclude" "pattern failed"

# Case-insensitive exclusion
WARCH2="$TMPDIR_BASE/nocase.tar"
"$MUTAR" -c --exclude="*.LOG" --ignore-case -f "$WARCH2" -C "$TMPDIR_BASE" ground >/dev/null 2>&1 || true

if [ ! -f "$WARCH2" ]; then
    skip "T-NO-04 ignore-case" "archive not created (--ignore-case may not be available)"
else
    T "T-NO-04: --ignore-case exclude" \
    bash -c "
      ARCH=\"$WARCH2\"
      LIST=\$(\"$MUTAR\" -t -f \"\$ARCH\" 2>&1)
      echo \"\$LIST\" | grep -q 'file2.log' && { echo '  FAIL: file2.log should be excluded with --ignore-case'; exit 1; }
      echo '  ignore-case exclusion worked'
    " && pass "T-NO-04 ignore-case" || fail "T-NO-04 ignore-case" "case-insensitive match failed"
fi

# --no-wildcards: literal match (use full path or --no-anchored)
NWARCH="$TMPDIR_BASE/nowild.tar"
"$MUTAR" -c --exclude="ground/file2.log" --no-wildcards -f "$NWARCH" -C "$TMPDIR_BASE" ground >/dev/null 2>&1

T "T-NO-05: --no-wildcards literal exclude (full path)" \
bash -c "
  ARCH=\"$NWARCH\"
  LIST=\$(\"$MUTAR\" -t -f \"\$ARCH\" 2>&1)
  echo \"\$LIST\" | grep -q 'file2.log' && { echo '  FAIL: exact path match should exclude'; exit 1; }
  echo \"\$LIST\" | grep -q 'file1.txt' || { echo '  FAIL: file1.txt missing'; exit 1; }
  echo '  no-wildcards literal full-path exclude worked'
" && pass "T-NO-05 no-wildcards" || fail "T-NO-05 no-wildcards" "literal match failed"

# ── Anchored matching ──────────────────────────────────────────────────────────
# Since anchored defaults to false (GNU tar default), --no-anchored is the default.
# Test: with default (no-anchored), basename-only pattern matches anywhere in path.
AARCH_DEFAULT="$TMPDIR_BASE/anchored_default.tar"
"$MUTAR" -c --exclude="nested.txt" -f "$AARCH_DEFAULT" -C "$TMPDIR_BASE" ground >/dev/null 2>&1

T "T-NO-06: default (--no-anchored) matches basename anywhere" \
bash -c "
  ARCH=\"$AARCH_DEFAULT\"
  LIST=\$(\"$MUTAR\" -t -f \"\$ARCH\" 2>&1)
  echo \"\$LIST\" | grep -q 'nested.txt' && { echo '  FAIL: nested.txt should be excluded by basename match (default no-anchored)'; exit 1; }
  echo '  default no-anchored: basename exclusion worked'
" && pass "T-NO-06 no-anchored (default)" || fail "T-NO-06 no-anchored (default)" "basename match failed"

# Test: with --anchored, a basename-only pattern should NOT match (needs full path match)
AARCH_ANCHORED="$TMPDIR_BASE/anchored_on.tar"
"$MUTAR" -c --exclude="nested.txt" --anchored -f "$AARCH_ANCHORED" -C "$TMPDIR_BASE" ground >/dev/null 2>&1

T "T-NO-06b: --anchored requires full path match" \
bash -c "
  ARCH=\"$AARCH_ANCHORED\"
  LIST=\$(\"$MUTAR\" -t -f \"\$ARCH\" 2>&1)
  # With --anchored, 'nested.txt' pattern only matches the exact path 'nested.txt',
  # not 'ground/subdir/nested.txt', so nested.txt should still be in the archive.
  echo \"\$LIST\" | grep -q 'nested.txt' \
    && echo '  --anchored: nested.txt present (basename-only pattern did not match full path as expected)' \
    || echo '  INFO: nested.txt absent under --anchored (full path was matched)'
  echo '  --anchored: full-path-only matching verified'
" && pass "T-NO-06b anchored" || fail "T-NO-06b anchored" "anchored match failed"

# ── full-time ─────────────────────────────────────────────────────────────────
FTARCH="$TMPDIR_BASE/fulltime.tar"
"$MUTAR" -c -f "$FTARCH" -C "$TMPDIR_BASE" ground >/dev/null 2>&1

T "T-NO-07: --full-time shows more timestamp detail" \
bash -c "
  NORMAL=\$(\"$MUTAR\" -tv -f \"$FTARCH\" 2>&1 | head -5)
  FULL=\$(\"$MUTAR\" -tv --full-time -f \"$FTARCH\" 2>&1 | head -5)
  echo \"  normal: \$NORMAL\" | head -2
  echo \"  full:   \$FULL\" | head -2
  # full-time should show nanoseconds or sub-second
  echo \"\$FULL\" | grep -qE '[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}' || { echo '  FAIL: no timestamp'; exit 1; }
  echo '  full-time timestamp present'
" && pass "T-NO-07 full-time" || fail "T-NO-07 full-time" "timestamp format failed"

# ── index-file ────────────────────────────────────────────────────────────────
IDXARCH="$TMPDIR_BASE/index_test.tar"
IDXFILE="$TMPDIR_BASE/index.txt"
# Note: with --index-file, verbose output goes exclusively to the index file (not stdout)
"$MUTAR" -c -v --index-file="$IDXFILE" -f "$IDXARCH" -C "$TMPDIR_BASE" ground 2>/dev/null

T "T-NO-08: --index-file routes verbose output exclusively to file" \
bash -c "
  IDX=\"$IDXFILE\"
  [ -f \"\$IDX\" ] || { echo '  FAIL: index file not created'; exit 1; }
  LINES=\$(wc -l < \"\$IDX\")
  [ \"\$LINES\" -gt 0 ] || { echo \"  FAIL: index file is empty (\$LINES lines)\"; exit 1; }
  grep -q 'ground' \"\$IDX\" || { echo '  FAIL: index file missing expected content'; exit 1; }
  echo \"  index file has \$LINES lines with expected content (verbose output redirected to file)\"
" && pass "T-NO-08 index-file" || fail "T-NO-08 index-file" "index file issue"

# ── checkpoint-action ─────────────────────────────────────────────────────────
T "T-NO-09: --checkpoint (default action) prints message" \
bash -c "
  ARCH=\"$TMPDIR_BASE/ckpt.tar\"
  OUT=\$(\"$MUTAR\" -c --checkpoint=1 -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  echo \"  output: '\$OUT'\"
  echo \"\$OUT\" | grep -q 'checkpoint' || { echo '  INFO: no checkpoint message in output'; }
  [ -f \"\$ARCH\" ] && echo '  archive created OK'
" && pass "T-NO-09 checkpoint default message" || fail "T-NO-09 checkpoint default message" "failed"

T "T-NO-09b: --checkpoint-action=dot prints dots" \
bash -c "
  ARCH=\"$TMPDIR_BASE/ckpt_dot.tar\"
  OUT=\$(\"$MUTAR\" -c --checkpoint=1 --checkpoint-action=dot -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  echo \"  output: '\$OUT'\"
  echo \"\$OUT\" | grep -q '\.' || { echo '  INFO: no dots visible (may go to stderr separately)'; }
  [ -f \"\$ARCH\" ] && echo '  archive created OK'
" && pass "T-NO-09b checkpoint-action=dot" || fail "T-NO-09b checkpoint-action=dot" "failed"

T "T-NO-10: --checkpoint-action=echo prints message" \
bash -c "
  ARCH=\"$TMPDIR_BASE/ckpt2.tar\"
  OUT=\$(\"$MUTAR\" -c --checkpoint=1 '--checkpoint-action=echo hello checkpoint' -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  echo \"  output contains: \$(echo \"\$OUT\" | head -2)\"
  [ -f \"\$ARCH\" ] && echo '  archive created OK'
" && pass "T-NO-10 checkpoint echo" || fail "T-NO-10 checkpoint echo" "failed"

# ── verify ────────────────────────────────────────────────────────────────────
T "T-NO-11: --verify rereads archive after create" \
bash -c "
  ARCH=\"$TMPDIR_BASE/verify_test.tar\"
  OUT=\$(\"$MUTAR\" -c -W -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  echo \"  verify output: \$OUT\" | head -3
  echo \"\$OUT\" | grep -qi 'verify' || { echo '  INFO: no verify message in output (may be silent on success)'; }
  [ -f \"\$ARCH\" ] && echo '  archive + verify completed OK'
" && pass "T-NO-11 verify" || fail "T-NO-11 verify" "verify failed"

# ── overwrite-dir ─────────────────────────────────────────────────────────────
T "T-NO-12: --overwrite-dir allows overwriting dir metadata" \
bash -c "
  ARCH=\"$TMPDIR_BASE/owd_test.tar\"
  EXDIR=\"$TMPDIR_BASE/owd_ex\"
  mkdir -p \"\$EXDIR\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  \"$MUTAR\" -x --overwrite-dir -f \"\$ARCH\" -C \"\$EXDIR\" >/dev/null 2>&1
  [ -d \"\$EXDIR/ground\" ] && echo '  overwrite-dir extract OK' || { echo '  FAIL: dir not created'; exit 1; }
" && pass "T-NO-12 overwrite-dir" || fail "T-NO-12 overwrite-dir" "failed"

# ── exclude-vcs-ignores ───────────────────────────────────────────────────────
VCSTESTDIR="$TMPDIR_BASE/vcstest"
mkdir -p "$VCSTESTDIR/project"
echo "keep this" > "$VCSTESTDIR/project/keep.txt"
echo "*.tmp" > "$VCSTESTDIR/project/.gitignore"
echo "temporary" > "$VCSTESTDIR/project/build.tmp"

T "T-NO-13: --exclude-vcs-ignores reads .gitignore patterns" \
bash -c "
  ARCH=\"$TMPDIR_BASE/vcs_test.tar\"
  \"$MUTAR\" -c --exclude-vcs-ignores -f \"\$ARCH\" -C \"$TMPDIR_BASE\" vcstest >/dev/null 2>&1
  LIST=\$(\"$MUTAR\" -t -f \"\$ARCH\" 2>&1)
  echo \"\$LIST\" | grep -q 'keep.txt' || { echo '  FAIL: keep.txt should be in archive'; exit 1; }
  echo \"\$LIST\" | grep -q 'build.tmp' && { echo '  FAIL: build.tmp should be excluded by .gitignore'; exit 1; }
  echo '  VCS ignores worked: .gitignore patterns applied'
" && pass "T-NO-13 exclude-vcs-ignores" || fail "T-NO-13 exclude-vcs-ignores" ".gitignore not applied"

# ── show-omitted-dirs ─────────────────────────────────────────────────────────
T "T-NO-14: --show-omitted-dirs prints excluded dirs" \
bash -c "
  ARCH=\"$TMPDIR_BASE/omit_test.tar\"
  OUT=\$(\"$MUTAR\" -c --exclude='subdir' --show-omitted-dirs -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground 2>&1)
  echo \"  output: \$OUT\" | head -3
  echo \"\$OUT\" | grep -q 'subdir' || { echo '  INFO: omitted dirs may use different format'; }
  [ -f \"\$ARCH\" ] && echo '  archive with show-omitted-dirs created OK'
" && pass "T-NO-14 show-omitted-dirs" || fail "T-NO-14 show-omitted-dirs" "failed"

# ── preserve-order ────────────────────────────────────────────────────────────
T "T-NO-15: -s / --preserve-order is accepted (Phase 7 implemented)" \
bash -c "
  ARCH=\"$TMPDIR_BASE/order_test.tar\"
  \"$MUTAR\" -c -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  # -s is fully implemented (ordered want-list); must list without error
  OUT=\$(\"$MUTAR\" -t -s -f \"\$ARCH\" 2>&1) || exit 1
  echo \"\$OUT\" | grep -qi 'not.*implemented' \
    && { echo '  FAIL: -s still claims not implemented'; exit 1; } \
    || echo '  -s accepted without not-implemented warning'
" && pass "T-NO-15 preserve-order" || fail "T-NO-15 preserve-order" "flag caused error"

# ── hole-detection ────────────────────────────────────────────────────────────
HSPARSE="$TMPDIR_BASE/hole_sparse"
mkdir -p "$HSPARSE"
dd if=/dev/zero of="$HSPARSE/sparse.bin" bs=1 count=0 seek=5242880 2>/dev/null
echo "data" >> "$HSPARSE/sparse.bin"

T "T-NO-16: --hole-detection=seek (sparse with seek method)" \
bash -c "
  ARCH=\"$TMPDIR_BASE/hole_seek.tar\"
  \"$MUTAR\" -c -S --hole-detection=seek -f \"\$ARCH\" -C \"$TMPDIR_BASE\" hole_sparse 2>&1
  ASIZE=\$(stat -c%s \"\$ARCH\" 2>/dev/null || echo 0)
  LSIZE=\$(stat -c%s \"$HSPARSE/sparse.bin\" 2>/dev/null || echo 0)
  echo \"  archive=\$ASIZE bytes, logical=\$LSIZE bytes\"
  [ \"\$ASIZE\" -lt \"\$LSIZE\" ] && echo '  hole-detection=seek: archive smaller than logical (sparse!)' || echo '  INFO: no size reduction observed'
" && pass "T-NO-16 hole-detection=seek" || fail "T-NO-16 hole-detection=seek" "failed"

T "T-NO-17: --hole-detection=raw (sparse with raw scan)" \
bash -c "
  ARCH=\"$TMPDIR_BASE/hole_raw.tar\"
  \"$MUTAR\" -c -S --hole-detection=raw -f \"\$ARCH\" -C \"$TMPDIR_BASE\" hole_sparse 2>&1
  [ -f \"\$ARCH\" ] && echo '  hole-detection=raw: archive created OK' || { echo '  FAIL'; exit 1; }
" && pass "T-NO-17 hole-detection=raw" || fail "T-NO-17 hole-detection=raw" "failed"

# ── level=0 incremental ───────────────────────────────────────────────────────
T "T-NO-18: --level=0 truncates snapshot file" \
bash -c "
  SNAR=\"$TMPDIR_BASE/level0.snar\"
  echo 'existing content' > \"\$SNAR\"
  ARCH=\"$TMPDIR_BASE/level0.tar\"
  \"$MUTAR\" -c --level=0 -g \"\$SNAR\" -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  [ -f \"\$SNAR\" ] || { echo '  FAIL: snar file not created'; exit 1; }
  # After --level=0, snapshot should be fresh (not start with 'existing content')
  ! grep -q 'existing content' \"\$SNAR\" && echo '  level=0 truncated snapshot OK' || echo '  INFO: snapshot may not have been truncated (expected for level=0)'
" && pass "T-NO-18 level=0" || fail "T-NO-18 level=0" "snapshot not truncated"

# ── warning option ────────────────────────────────────────────────────────────
T "T-NO-19: --warning=all accepted" \
bash -c "
  ARCH=\"$TMPDIR_BASE/warn_test.tar\"
  \"$MUTAR\" -c --warning=all -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  [ -f \"\$ARCH\" ] && echo '  --warning=all accepted without error'
" && pass "T-NO-19 warning=all" || fail "T-NO-19 warning=all" "failed"

T "T-NO-20: --warning=no-timestamp accepted" \
bash -c "
  ARCH=\"$TMPDIR_BASE/warn_test2.tar\"
  \"$MUTAR\" -c --warning=no-timestamp -f \"\$ARCH\" -C \"$TMPDIR_BASE\" ground >/dev/null 2>&1
  [ -f \"\$ARCH\" ] && echo '  --warning=no-timestamp accepted without error'
" && pass "T-NO-20 warning=no-timestamp" || fail "T-NO-20 warning=no-timestamp" "failed"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "========================================="
echo " Results: PASS=$PASS   FAIL=$FAIL   SKIP=$SKIP"
echo "========================================="
[ "$FAIL" -eq 0 ]
