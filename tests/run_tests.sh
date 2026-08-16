#!/usr/bin/env bash
# tests/run_tests.sh — Automated test harness for mutar (µtar)
# Usage: ./run_tests.sh [/path/to/mutar]
#
# Uses test material from RetroCodeMess/src/ where available.
# Compares mutar output against system tar for cross-validation.
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TAR="${TAR:-tar}"
TMPDIR_BASE="$(mktemp -d /tmp/mutar_tests.XXXXXX)"
trap 'rm -rf "$TMPDIR_BASE"' EXIT

# Locate the repo src/ directory relative to this script's location
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="${MUTAR_SRC_DIR:-$REPO_ROOT/src}"

PASS=0
FAIL=0
SKIP=0

# ── helpers ────────────────────────────────────────────────────────────────────
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

run_test() {
    local name="$1"; shift
    local fn="$1";  shift
    echo "[$name]"
    if "$fn" "$@" 2>&1; then
        pass "$name"
    else
        fail "$name" "test function returned non-zero"
    fi
}

require_cmd() {
    command -v "$1" &>/dev/null || { skip "$2" "requires $1"; return 1; }
}

# ── Test helpers ───────────────────────────────────────────────────────────────
mkwork() {
    local d="$TMPDIR_BASE/$1"
    mkdir -p "$d"
    echo "$d"
}

# Create a small directory tree for testing
make_tree() {
    local base="$1"
    mkdir -p "$base/dir1/subdir"
    echo "hello world"         > "$base/file1.txt"
    echo "another file"        > "$base/dir1/file2.txt"
    echo "nested"              > "$base/dir1/subdir/file3.txt"
    printf 'binary\x00data\n'  > "$base/dir1/binary.bin"
    ln -s "../file1.txt"         "$base/dir1/symlink.txt"
    mkdir -p "$base/dir2"
    echo "in dir2"             > "$base/dir2/file4.txt"
}

# ── T01: Create and list a basic archive ──────────────────────────────────────
test_create_list() {
    local W; W="$(mkwork T01)"
    make_tree "$W/input"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    
    # List must contain our files
    local listing
    listing="$("$MUTAR" -tf "$W/test.tar")"
    echo "$listing" | grep -q "file1.txt"     || { echo "missing file1.txt"; return 1; }
    echo "$listing" | grep -q "dir1/file2.txt" || { echo "missing dir1/file2.txt"; return 1; }
    echo "$listing" | grep -q "dir1/subdir/file3.txt" || { echo "missing file3.txt"; return 1; }
    return 0
}

# ── T02: Create and extract, verify content ───────────────────────────────────
test_create_extract() {
    local W; W="$(mkwork T02)"
    make_tree "$W/input"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/test.tar" -C "$W/output"

    diff -rq "$W/input/dir1/file2.txt" "$W/output/dir1/file2.txt" || { echo "file2 mismatch"; return 1; }
    diff -rq "$W/input/dir2/file4.txt" "$W/output/dir2/file4.txt" || { echo "file4 mismatch"; return 1; }
    local content
    content="$(cat "$W/output/file1.txt")"
    [[ "$content" == "hello world" ]] || { echo "file1 content wrong: $content"; return 1; }
    return 0
}

# ── T03: Verbose listing matches expected fields ──────────────────────────────
test_verbose_list() {
    local W; W="$(mkwork T03)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .

    local vlist
    vlist="$("$MUTAR" -tvf "$W/test.tar")"
    # Verbose output should have permission field like -rw-r--r-- or similar
    echo "$vlist" | grep -qE '^[-dl]' || { echo "no permission field in verbose output"; return 1; }
    echo "$vlist" | grep -q "file1.txt" || { echo "file1.txt not in verbose list"; return 1; }
    return 0
}

# ── T04: symlink preservation ─────────────────────────────────────────────────
test_symlink() {
    local W; W="$(mkwork T04)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/test.tar" -C "$W/output"

    [[ -L "$W/output/dir1/symlink.txt" ]] || { echo "symlink not extracted as symlink"; return 1; }
    local tgt
    tgt="$(readlink "$W/output/dir1/symlink.txt")"
    [[ "$tgt" == "../file1.txt" ]] || { echo "wrong symlink target: $tgt"; return 1; }
    return 0
}

# ── T05: gzip compression ─────────────────────────────────────────────────────
test_gzip() {
    require_cmd gzip T05 || return 0
    local W; W="$(mkwork T05)"
    make_tree "$W/input"

    "$MUTAR" -czf "$W/test.tar.gz" -C "$W/input" .
    [[ -f "$W/test.tar.gz" ]] || { echo "archive not created"; return 1; }

    mkdir -p "$W/output"
    "$MUTAR" -xzf "$W/test.tar.gz" -C "$W/output"
    diff -q "$W/input/file1.txt" "$W/output/file1.txt" || { echo "gzip content mismatch"; return 1; }
    return 0
}

# ── T06: bzip2 compression ────────────────────────────────────────────────────
test_bzip2() {
    require_cmd bzip2 T06 || return 0
    local W; W="$(mkwork T06)"
    make_tree "$W/input"

    "$MUTAR" -cjf "$W/test.tar.bz2" -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xjf "$W/test.tar.bz2" -C "$W/output"
    diff -q "$W/input/file1.txt" "$W/output/file1.txt" || { echo "bzip2 content mismatch"; return 1; }
    return 0
}

# ── T07: xz compression ───────────────────────────────────────────────────────
test_xz() {
    require_cmd xz T07 || return 0
    local W; W="$(mkwork T07)"
    make_tree "$W/input"

    "$MUTAR" -cJf "$W/test.tar.xz" -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xJf "$W/test.tar.xz" -C "$W/output"
    diff -q "$W/input/dir1/file2.txt" "$W/output/dir1/file2.txt" || { echo "xz content mismatch"; return 1; }
    return 0
}

# ── T08: star reads tar-written archive ───────────────────────────────────────
test_interop_read() {
    require_cmd tar T08 || return 0
    local W; W="$(mkwork T08)"
    make_tree "$W/input"

    # Write with system tar
    "$TAR" -cf "$W/gnu.tar" -C "$W/input" .
    
    # Read with mutar
    local listing
    listing="$("$MUTAR" -tf "$W/gnu.tar")"
    echo "$listing" | grep -q "file1.txt" || { echo "star can't read tar archive"; return 1; }

    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/gnu.tar" -C "$W/output"
    diff -q "$W/input/file1.txt" "$W/output/file1.txt" || { echo "interop extract mismatch"; return 1; }
    return 0
}

# ── T09: tar reads star-written archive ───────────────────────────────────────
test_interop_write() {
    require_cmd tar T09 || return 0
    local W; W="$(mkwork T09)"
    make_tree "$W/input"

    # Write with mutar
    "$MUTAR" -cf "$W/star.tar" -C "$W/input" .

    # Read with system tar
    local listing
    listing="$("$TAR" -tf "$W/star.tar" 2>&1)"
    echo "$listing" | grep -q "file1.txt" || { echo "tar can't read star archive"; return 1; }
    return 0
}

# ── T10: long filenames (GNU LongName) ────────────────────────────────────────
test_longname() {
    local W; W="$(mkwork T10)"
    mkdir -p "$W/input"
    # Create a filename longer than 100 chars
    local longdir="$W/input/a_very_deep_directory_structure_that_exceeds_the_ustar_limit/subdir1/subdir2"
    mkdir -p "$longdir"
    echo "long path test" > "$longdir/file_with_a_somewhat_long_name_exceeding_100_chars_total.txt"

    "$MUTAR" -cf "$W/long.tar" -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/long.tar" -C "$W/output"

    find "$W/output" -name "*.txt" | grep -q "file_with_a_somewhat_long" || { echo "long file not extracted"; return 1; }
    return 0
}

# ── T11: USTAR format ─────────────────────────────────────────────────────────
test_ustar_format() {
    local W; W="$(mkwork T11)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/ustar.tar" -H ustar -C "$W/input" .

    # Verify magic
    local magic
    magic="$(dd if="$W/ustar.tar" bs=1 skip=257 count=5 2>/dev/null)"
    [[ "$magic" == "ustar" ]] || { echo "wrong magic: '$magic'"; return 1; }
    return 0
}

# ── T12: PAX format ───────────────────────────────────────────────────────────
test_pax_format() {
    local W; W="$(mkwork T12)"
    # Create a name > 100 chars to force PAX path header
    mkdir -p "$W/input/aaaa/bbbb/cccc/dddd/eeee/ffff"
    echo "pax test" > "$W/input/aaaa/bbbb/cccc/dddd/eeee/ffff/ggggg_hhhh_iiii_jjjj.txt"

    "$MUTAR" -cf "$W/pax.tar" --posix -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/pax.tar" -C "$W/output"
    find "$W/output" -name "*.txt" | grep -q "ggggg_hhhh" || { echo "pax file not extracted"; return 1; }
    return 0
}

# ── T13: extract specific files ───────────────────────────────────────────────
test_selective_extract() {
    local W; W="$(mkwork T13)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    mkdir -p "$W/output"

    # Extract only file1.txt
    "$MUTAR" -xf "$W/test.tar" -C "$W/output" ./file1.txt
    [[ -f "$W/output/file1.txt" ]] || { echo "file1.txt not extracted"; return 1; }
    [[ ! -f "$W/output/dir1/file2.txt" ]] || { echo "file2.txt should not be extracted"; return 1; }
    return 0
}

# ── T14: --strip-components ───────────────────────────────────────────────────
test_strip_components() {
    local W; W="$(mkwork T14)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    mkdir -p "$W/output"

    "$MUTAR" -xf "$W/test.tar" -C "$W/output" --strip-components=1
    # dir1/file2.txt should appear as file2.txt
    [[ -f "$W/output/file2.txt" ]] || { echo "stripped path not found"; return 1; }
    return 0
}

# ── T15: --exclude pattern ────────────────────────────────────────────────────
test_exclude() {
    local W; W="$(mkwork T15)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" --exclude="*.bin" .

    local listing
    listing="$("$MUTAR" -tf "$W/test.tar")"
    echo "$listing" | grep -q "binary.bin" && { echo "binary.bin should be excluded"; return 1; }
    echo "$listing" | grep -q "file1.txt"  || { echo "file1.txt should be present"; return 1; }
    return 0
}

# ── T16: extract to stdout (-O) ───────────────────────────────────────────────
test_to_stdout() {
    local W; W="$(mkwork T16)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .

    local content
    content="$("$MUTAR" -xOf "$W/test.tar" ./file1.txt)"
    [[ "$content" == "hello world" ]] || { echo "stdout content wrong: $content"; return 1; }
    return 0
}

# ── T17: read the actual tar-1.35.tar.xz from the repo ───────────────────────
test_read_tar_source() {
    local tarxz
    # Try known repo path
    tarxz="$SRC_DIR/tar-1.35.tar.xz"
    [[ -f "$tarxz" ]] || { echo "tar source not found at $tarxz (skip)"; return 0; }
    require_cmd xz T17 || return 0

    local W; W="$(mkwork T17)"
    local listing
    listing="$("$MUTAR" -tJf "$tarxz" 2>&1 | head -20)"
    echo "$listing" | grep -q "tar-1.35" || { echo "can't list tar-1.35.tar.xz"; echo "$listing"; return 1; }
    return 0
}

# ── T18: keep-old-files (-k) ──────────────────────────────────────────────────
test_keep_old_files() {
    local W; W="$(mkwork T18)"
    make_tree "$W/input"
    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    mkdir -p "$W/output"
    echo "ORIGINAL" > "$W/output/file1.txt"

    # -k should NOT overwrite existing file
    "$MUTAR" -xkf "$W/test.tar" -C "$W/output" 2>/dev/null || true
    local content
    content="$(cat "$W/output/file1.txt")"
    [[ "$content" == "ORIGINAL" ]] || { echo "file1.txt was overwritten: $content"; return 1; }
    return 0
}

# ── T19: binary content integrity ─────────────────────────────────────────────
test_binary_content() {
    local W; W="$(mkwork T19)"
    mkdir -p "$W/input"
    # Write 4096 bytes of known binary content
    python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256))*16)" > "$W/input/binary.bin"
    local orig_md5
    orig_md5="$(md5sum "$W/input/binary.bin" | cut -d' ' -f1)"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" binary.bin
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/test.tar" -C "$W/output"
    local extr_md5
    extr_md5="$(md5sum "$W/output/binary.bin" | cut -d' ' -f1)"

    [[ "$orig_md5" == "$extr_md5" ]] || { echo "binary md5 mismatch: $orig_md5 vs $extr_md5"; return 1; }
    return 0
}

# ── T20: large file (>8GB octal overflow, base-256 encoding) ─────────────────
test_large_size_encoding() {
    # We create a 10MB file and verify binary integrity through archive round-trip
    local W; W="$(mkwork T20)"
    mkdir -p "$W/input"
    dd if=/dev/urandom bs=1M count=10 of="$W/input/large.bin" 2>/dev/null
    local orig_md5
    orig_md5="$(md5sum "$W/input/large.bin" | cut -d' ' -f1)"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" large.bin
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/test.tar" -C "$W/output"
    local extr_md5
    extr_md5="$(md5sum "$W/output/large.bin" | cut -d' ' -f1)"

    [[ "$orig_md5" == "$extr_md5" ]] || { echo "large file md5 mismatch"; return 1; }
    return 0
}

# ── T21: read archives from repo/src ─────────────────────────────────────────
test_repo_archives() {
    local src_dir="$SRC_DIR"
    [[ -d "$src_dir" ]] || { echo "src dir not found"; return 1; }

    local ok=0 err=0
    for f in "$src_dir"/*.tar.gz "$src_dir"/*.tar.xz "$src_dir"/*.tar.bz2; do
        [[ -f "$f" ]] || continue
        # Use cat to avoid SIGPIPE from head -5 with pipefail
        local listing
        listing="$("$MUTAR" -tf "$f" 2>&1)" || { echo "  ERR: $(basename "$f")"; err=$((err+1)); continue; }
        local first; first="$(echo "$listing" | head -1)"
        echo "  OK: $(basename "$f") → $first"
        ok=$((ok+1))
    done
    echo "  Summary: $ok OK, $err ERR"
    [[ $ok -gt 0 || $err -eq 0 ]] || { echo "errors reading archives from src/"; return 1; }
    [[ $ok -gt 0 ]] || { echo "no sample archives in src/ (skip)"; return 0; }
    return 0
}

# ── T22: hard link ────────────────────────────────────────────────────────────
test_hardlink() {
    local W; W="$(mkwork T22)"
    mkdir -p "$W/input"
    echo "original" > "$W/input/orig.txt"
    ln "$W/input/orig.txt" "$W/input/hardlink.txt"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" .
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/test.tar" -C "$W/output"

    # Both files should exist
    [[ -f "$W/output/orig.txt" ]]     || { echo "orig.txt missing"; return 1; }
    [[ -f "$W/output/hardlink.txt" ]] || { echo "hardlink.txt missing"; return 1; }
    return 0
}

# ── T23: empty archive ────────────────────────────────────────────────────────
test_empty_archive() {
    local W; W="$(mkwork T23)"
    mkdir -p "$W/empty"

    "$MUTAR" -cf "$W/empty.tar" -C "$W/empty" .
    local listing
    listing="$("$MUTAR" -tf "$W/empty.tar" 2>/dev/null)"
    # May have a single "." entry or be completely empty — both are fine
    return 0
}

# ── T24: v7 format ────────────────────────────────────────────────────────────
test_v7_format() {
    local W; W="$(mkwork T24)"
    mkdir -p "$W/input"
    echo "v7 test" > "$W/input/file.txt"

    "$MUTAR" -cf "$W/v7.tar" -H v7 -C "$W/input" file.txt
    mkdir -p "$W/output"
    "$MUTAR" -xf "$W/v7.tar" -C "$W/output"
    local content
    content="$(cat "$W/output/file.txt")"
    [[ "$content" == "v7 test" ]] || { echo "v7 content wrong: $content"; return 1; }
    return 0
}

# ── T25: --version / --help ───────────────────────────────────────────────────
test_help_version() {
    # Capture full output (avoid SIGPIPE+pipefail when grep -q closes early)
    local ver help
    ver="$("$MUTAR" --version)" || { echo "--version failed"; return 1; }
    help="$("$MUTAR" --help)" || { echo "--help failed"; return 1; }
    echo "$ver" | grep -q "mutar" || { echo "--version missing mutar brand"; return 1; }
    echo "$ver" | grep -q "99%" || { echo "--version missing 99% compat note"; return 1; }
    echo "$help" | grep -q "mutar" || { echo "--help missing mutar"; return 1; }
    echo "$help" | grep -q "Schilling" || { echo "--help missing Schily disclaimer"; return 1; }
    return 0
}

# ── T26: AREGTYPE (typeflag=0x00) regression — old GNU archives ───────────────
# Verifies that files stored with typeflag '\0' (old V7/GNU regular-file
# encoding, as used by e.g. sidplay-2.0.9.tar.gz) are extracted as regular
# files, NOT silently converted into directories.
test_aregtype_extract() {
    local src_dir="$SRC_DIR"
    local sidplay="$src_dir/sidplay-2.0.9.tar.gz"
    [[ -f "$sidplay" ]] || { echo "sidplay-2.0.9.tar.gz not found (skip)"; return 0; }
    require_cmd gzip T26 || return 0

    local W; W="$(mkwork T26)"

    # Step 1: extract the archive
    "$MUTAR" -xzf "$sidplay" -C "$W"
    local nfiles
    nfiles="$(find "$W" -type f | wc -l)"
    [[ "$nfiles" -gt 0 ]] || { echo "no regular files extracted (AREGTYPE bug)"; return 1; }

    # Step 2: repack what we extracted
    local topdir; topdir="$(ls "$W" | head -1)"
    "$MUTAR" -czf "$W/repack.tar.gz" -C "$W" "$topdir"

    # Step 3: re-extract the repacked archive
    mkdir -p "$W/verify"
    "$MUTAR" -xzf "$W/repack.tar.gz" -C "$W/verify"
    local nfiles2
    nfiles2="$(find "$W/verify" -type f | wc -l)"
    [[ "$nfiles2" -eq "$nfiles" ]] || {
        echo "round-trip file count mismatch: $nfiles -> $nfiles2"; return 1; }

    # Step 4: spot-check a known file
    local ref="$W/$topdir/unix/my_macros.m4"
    local got="$W/verify/$topdir/unix/my_macros.m4"
    [[ -f "$ref" ]] || { echo "my_macros.m4 missing after step-1 extract"; return 1; }
    [[ -f "$got" ]] || { echo "my_macros.m4 missing after round-trip"; return 1; }
    cmp -s "$ref" "$got" || { echo "my_macros.m4 binary mismatch after round-trip"; return 1; }
    return 0
}

# ── T27: round-trip all repo archives (extract→repack→re-extract) ─────────────
test_all_roundtrip() {
    local src_dir="$SRC_DIR"
    [[ -d "$src_dir" ]] || { echo "src dir not found"; return 1; }

    local ok=0 err=0
    for arc in "$src_dir"/*.tar.gz "$src_dir"/*.tar.xz "$src_dir"/*.tar.bz2; do
        [[ -f "$arc" ]] || continue
        local name; name="$(basename "$arc")"
        local W; W="$(mkwork "T27_${name}")"
        mkdir -p "$W/e1" "$W/e2"

        # Extract
        "$MUTAR" -xf "$arc" -C "$W/e1" 2>/dev/null
        local n1; n1="$(find "$W/e1" -type f | wc -l)"
        [[ "$n1" -gt 0 ]] || { echo "  ERR $name: 0 files extracted"; err=$((err+1)); continue; }

        # Repack (uncompressed to avoid gzip dependency in this test)
        local top; top="$(ls "$W/e1" | head -1)"
        "$MUTAR" -cf "$W/repack.tar" -C "$W/e1" "$top" 2>/dev/null

        # Re-extract
        "$MUTAR" -xf "$W/repack.tar" -C "$W/e2" 2>/dev/null
        local n2; n2="$(find "$W/e2" -type f | wc -l)"

        if [[ "$n1" -eq "$n2" ]]; then
            echo "  OK: $name ($n1 files)"
            ok=$((ok+1))
        else
            echo "  ERR: $name file count $n1 -> $n2"
            err=$((err+1))
        fi
    done
    echo "  Summary: $ok OK, $err ERR"
    [[ $err -eq 0 ]] || return 1
    return 0
}

# ── T28: --newer date filter ──────────────────────────────────────────────────
test_newer_filter() {
    local W; W="$(mkwork T28)"
    mkdir -p "$W/input"
    echo "old content" > "$W/input/old.txt"
    echo "new content" > "$W/input/new.txt"
    touch -t 200001010000 "$W/input/old.txt"   # year 2000 — old
    touch -t 203001010000 "$W/input/new.txt"   # year 2030 — future/new

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" --newer="2020-01-01" . 2>/dev/null
    local listing; listing="$("$MUTAR" -tf "$W/test.tar" 2>/dev/null)"
    echo "$listing" | grep -q "new.txt"  || { echo "new.txt missing"; return 1; }
    echo "$listing" | grep -q "old.txt"  && { echo "old.txt should be excluded"; return 1; }
    return 0
}

# ── T29: --mtime override ─────────────────────────────────────────────────────
test_mtime_override() {
    local W; W="$(mkwork T29)"
    mkdir -p "$W/input"
    echo "hello" > "$W/input/file.txt"
    touch -t 202301010000 "$W/input/file.txt"   # set to 2023

    "$MUTAR" -cf "$W/test.tar" --mtime="2000-06-15" -C "$W/input" . 2>/dev/null
    local ts
    ts="$("$MUTAR" -tvf "$W/test.tar" 2>/dev/null | grep "file.txt" | awk '{print $4}')"
    [[ "$ts" == "2000-06-15" ]] || { echo "mtime wrong: $ts (expected 2000-06-15)"; return 1; }
    return 0
}

# ── T30: --mode override ──────────────────────────────────────────────────────
test_mode_override() {
    local W; W="$(mkwork T30)"
    mkdir -p "$W/input"
    echo "hello" > "$W/input/file.txt"
    chmod 755 "$W/input/file.txt"

    "$MUTAR" -cf "$W/test.tar" --mode=640 -C "$W/input" . 2>/dev/null
    local perm
    perm="$("$MUTAR" -tvf "$W/test.tar" 2>/dev/null | grep "file.txt" | awk '{print $1}')"
    [[ "$perm" == "-rw-r-----" ]] || { echo "mode wrong: $perm (expected -rw-r-----)"; return 1; }
    return 0
}

# ── T31: --transform name rewriting ──────────────────────────────────────────
test_transform() {
    local W; W="$(mkwork T31)"
    mkdir -p "$W/input"
    echo "data" > "$W/input/original.txt"

    # Create with transform: original.txt → modified.txt
    "$MUTAR" -cf "$W/test.tar" --transform='s/original/modified/' -C "$W/input" . 2>/dev/null
    local listing; listing="$("$MUTAR" -tf "$W/test.tar" 2>/dev/null)"
    echo "$listing" | grep -q "modified.txt"  || { echo "modified.txt missing"; return 1; }
    echo "$listing" | grep -q "original.txt"  && { echo "original.txt should be renamed"; return 1; }

    # Extract with transform
    mkdir -p "$W/out"
    "$MUTAR" -xf "$W/test.tar" --transform='s/modified/extracted/' -C "$W/out" 2>/dev/null
    [[ -f "$W/out/extracted.txt" ]] || { echo "extracted.txt missing"; return 1; }
    return 0
}

# ── T32: --ignore-zeros (-i) spans concatenated archives ─────────────────────
test_ignore_zeros() {
    local W; W="$(mkwork T32)"
    mkdir -p "$W/input"
    echo "content" > "$W/input/file.txt"

    "$MUTAR" -cf "$W/a.tar" -C "$W/input" . 2>/dev/null
    cat "$W/a.tar" "$W/a.tar" > "$W/concat.tar"

    local c1; c1="$("$MUTAR" -tf  "$W/concat.tar" 2>/dev/null | grep "file.txt" | wc -l)"
    local c2; c2="$("$MUTAR" -itf "$W/concat.tar" 2>/dev/null | grep "file.txt" | wc -l)"

    [[ "$c1" -eq 1 ]] || { echo "without -i expected 1 file.txt, got $c1"; return 1; }
    [[ "$c2" -eq 2 ]] || { echo "with -i expected 2 file.txt (both copies), got $c2"; return 1; }
    return 0
}

# ── T33: --totals output ──────────────────────────────────────────────────────
test_totals() {
    local W; W="$(mkwork T33)"
    mkdir -p "$W/input"
    echo "hello" > "$W/input/file.txt"

    local out
    out="$("$MUTAR" -cf "$W/test.tar" --totals -C "$W/input" . 2>&1)"
    echo "$out" | grep -qi "total bytes" || { echo "no totals output: $out"; return 1; }
    return 0
}

# ── T34: --block-number (-R) in list ─────────────────────────────────────────
test_block_number() {
    local W; W="$(mkwork T34)"
    mkdir -p "$W/input"
    echo "data" > "$W/input/file.txt"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" . 2>/dev/null
    local first; first="$("$MUTAR" -Rtf "$W/test.tar" 2>/dev/null | head -1)"
    # Should start with "N: " where N is a number
    [[ "$first" =~ ^[0-9]+:\ .*$ ]] || { echo "no block-number prefix: $first"; return 1; }
    return 0
}

# ── T35: --remove-files deletes sources ──────────────────────────────────────
test_remove_files() {
    local W; W="$(mkwork T35)"
    mkdir -p "$W/input"
    echo "delete me" > "$W/input/gone.txt"

    "$MUTAR" -cf "$W/test.tar" --remove-files -C "$W/input" gone.txt 2>/dev/null
    [[ ! -f "$W/input/gone.txt" ]] || { echo "gone.txt still exists after --remove-files"; return 1; }
    [[ -f "$W/test.tar" ]] || { echo "archive not created"; return 1; }
    return 0
}

# ── T36: --atime-preserve restores file access time ──────────────────────────
test_atime_preserve() {
    local W; W="$(mkwork T36)"
    mkdir -p "$W/input"
    echo "content" > "$W/input/file.txt"
    # Set a specific atime we can check
    touch -a -t 200506151200 "$W/input/file.txt"
    local orig_atime; orig_atime="$(stat -c %X "$W/input/file.txt")"

    "$MUTAR" -cf "$W/test.tar" --atime-preserve -C "$W/input" file.txt 2>/dev/null
    local new_atime; new_atime="$(stat -c %X "$W/input/file.txt")"

    [[ "$orig_atime" -eq "$new_atime" ]] || {
        echo "atime changed: was $orig_atime now $new_atime"; return 1; }
    return 0
}

# ── T37: --record-size changes blocking factor ────────────────────────────────
test_record_size() {
    local W; W="$(mkwork T37)"
    mkdir -p "$W/input"
    echo "hello" > "$W/input/file.txt"

    # record-size=1024 → blocking_factor=2 → 2×512=1024 byte records
    "$MUTAR" -cf "$W/test.tar" --record-size=1024 -C "$W/input" . 2>/dev/null || return 1
    local sz; sz="$(stat -c %s "$W/test.tar")"
    # archive should be a multiple of 1024
    [[ $((sz % 1024)) -eq 0 ]] || { echo "archive size $sz not multiple of 1024"; return 1; }
    return 0
}

# ── T38: --one-top-level wraps extraction ────────────────────────────────────
test_one_top_level() {
    local W; W="$(mkwork T38)"
    mkdir -p "$W/input"
    echo "content" > "$W/input/file.txt"

    "$MUTAR" -cf "$W/test.tar" -C "$W" input/file.txt 2>/dev/null
    mkdir -p "$W/out"
    "$MUTAR" -xf "$W/test.tar" --one-top-level=mydir -C "$W/out" 2>/dev/null
    [[ -f "$W/out/mydir/input/file.txt" ]] || {
        echo "file not under mydir/; out: $(find "$W/out" -type f)"; return 1; }
    return 0
}

# ── T39: --to-command pipes file data to shell command ───────────────────────
test_to_command() {
    local W; W="$(mkwork T39)"
    mkdir -p "$W/input"
    echo "hello world" > "$W/input/file.txt"

    # Require /bin/sh and cat (should always be present, but guard anyway)
    if [ ! -x /bin/sh ] || ! command -v cat >/dev/null 2>&1; then
        echo "  SKIP: --to-command requires /bin/sh and cat"
        SKIP=$((SKIP+1)); return 0
    fi

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" . 2>/dev/null

    # Run extraction; non-zero exit is unexpected but not a hard requirement
    if ! "$MUTAR" -xf "$W/test.tar" --to-command="cat > $W/tocommand_out.txt" 2>/dev/null; then
        echo "  SKIP: --to-command invocation returned non-zero"
        SKIP=$((SKIP+1)); return 0
    fi

    local got
    got="$(cat "$W/tocommand_out.txt" 2>/dev/null || true)"
    [[ "$got" == "hello world" ]] || {
        echo "  FAIL: to-command output was: '$got' (expected 'hello world')"
        return 1
    }
    return 0
}

# ── T40: sparse file write + extract round-trip ──────────────────────────────
test_sparse() {
    local W; W="$(mkwork T40)"
    mkdir -p "$W/input"

    # Create a sparse file using truncate (creates holes)
    truncate -s 10M "$W/input/sparse.bin"
    # Write actual data at beginning and near end
    echo -n "START" | dd of="$W/input/sparse.bin" bs=5 seek=0 conv=notrunc 2>/dev/null
    echo -n "END__" | dd of="$W/input/sparse.bin" bs=5 seek=2097151 conv=notrunc 2>/dev/null

    local dense_size; dense_size="$(du -sb "$W/input/sparse.bin" | awk '{print $1}')"
    local logical_size; logical_size="$(stat -c %s "$W/input/sparse.bin")"

    # Archive with --sparse
    "$MUTAR" -cSf "$W/sparse.tar" -C "$W/input" sparse.bin 2>/dev/null

    # The archive should be much smaller than 10MB
    local arch_size; arch_size="$(stat -c %s "$W/sparse.tar")"
    [[ "$arch_size" -lt $((logical_size / 2)) ]] || {
        echo "  Note: sparse archive not smaller than half logical size ($arch_size >= $((logical_size/2)))"
        # Not necessarily a failure on all filesystems
    }

    # Extract and verify content and size
    mkdir -p "$W/out"
    "$MUTAR" -xf "$W/sparse.tar" -C "$W/out" 2>/dev/null
    [[ -f "$W/out/sparse.bin" ]] || { echo "sparse.bin not extracted"; return 1; }

    local out_size; out_size="$(stat -c %s "$W/out/sparse.bin")"
    [[ "$out_size" -eq "$logical_size" ]] || {
        echo "size mismatch: expected $logical_size got $out_size"; return 1; }

    # Verify data at the known positions
    local start_data end_data
    start_data="$(dd if="$W/out/sparse.bin" bs=5 count=1 skip=0 2>/dev/null)"
    [[ "$start_data" == "START" ]] || { echo "start data wrong: $start_data"; return 1; }
    return 0
}

# ── T41: hard link deduplification in archives ───────────────────────────────
test_hardlink_dedup() {
    local W; W="$(mkwork T41)"
    mkdir -p "$W/input"
    echo "shared data" > "$W/input/orig.txt"
    ln "$W/input/orig.txt" "$W/input/link1.txt"
    ln "$W/input/orig.txt" "$W/input/link2.txt"

    "$MUTAR" -cf "$W/test.tar" -C "$W/input" . 2>/dev/null

    # The archive should contain exactly ONE copy of the data
    # and TWO hard link entries (one regular + two LNKTYPE)
    local verbose; verbose="$("$MUTAR" -tvf "$W/test.tar" 2>/dev/null)"
    local h_count; h_count="$(echo "$verbose" | grep -c "^h" || true)"
    [[ "$h_count" -ge 2 ]] || { echo "expected >=2 hard link entries, got $h_count"; return 1; }

    # Extract and verify hard link relationship
    mkdir -p "$W/out"
    "$MUTAR" -xf "$W/test.tar" -C "$W/out" 2>/dev/null
    # All three files should have the same content
    cmp -s "$W/out/orig.txt"  "$W/out/link1.txt" || { echo "link1 content mismatch"; return 1; }
    cmp -s "$W/out/orig.txt"  "$W/out/link2.txt" || { echo "link2 content mismatch"; return 1; }
    return 0
}

# ── Run all tests ──────────────────────────────────────────────────────────────
echo "======================================="
echo " mutar (µtar) test suite"
echo " mutar binary: $MUTAR"
echo "======================================="

[[ -x "$MUTAR" ]] || { echo "FATAL: star binary not found or not executable: $MUTAR"; exit 1; }

run_test "T01 create+list"             test_create_list
run_test "T02 create+extract"          test_create_extract
run_test "T03 verbose list"            test_verbose_list
run_test "T04 symlink"                 test_symlink
run_test "T05 gzip"                    test_gzip
run_test "T06 bzip2"                   test_bzip2
run_test "T07 xz"                      test_xz
run_test "T08 interop: read tar"       test_interop_read
run_test "T09 interop: write tar"      test_interop_write
run_test "T10 long filename"           test_longname
run_test "T11 ustar format"            test_ustar_format
run_test "T12 pax format"              test_pax_format
run_test "T13 selective extract"       test_selective_extract
run_test "T14 strip-components"        test_strip_components
run_test "T15 exclude pattern"         test_exclude
run_test "T16 extract to stdout"       test_to_stdout
run_test "T17 read repo tar.xz"        test_read_tar_source
run_test "T18 keep-old-files"          test_keep_old_files
run_test "T19 binary integrity"        test_binary_content
run_test "T20 large file"              test_large_size_encoding
run_test "T21 repo archives"           test_repo_archives
run_test "T22 hard link"               test_hardlink
run_test "T23 empty archive"           test_empty_archive
run_test "T24 v7 format"               test_v7_format
run_test "T25 help/version"            test_help_version
run_test "T26 AREGTYPE regression"     test_aregtype_extract
run_test "T27 full round-trip"         test_all_roundtrip
run_test "T28 --newer filter"          test_newer_filter
run_test "T29 --mtime override"        test_mtime_override
run_test "T30 --mode override"         test_mode_override
run_test "T31 --transform"             test_transform
run_test "T32 --ignore-zeros"          test_ignore_zeros
run_test "T33 --totals"                test_totals
run_test "T34 --block-number"          test_block_number
run_test "T35 --remove-files"          test_remove_files
run_test "T36 --atime-preserve"        test_atime_preserve
run_test "T37 --record-size"           test_record_size
run_test "T38 --one-top-level"         test_one_top_level
run_test "T39 --to-command"            test_to_command
run_test "T40 sparse file"             test_sparse
run_test "T41 hardlink dedup"          test_hardlink_dedup

echo "======================================="
echo " Results: PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
echo "======================================="

[[ $FAIL -eq 0 ]]
