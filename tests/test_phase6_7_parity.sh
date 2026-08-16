#!/usr/bin/env bash
# tests/test_phase6_7_parity.sh — GOAL_GNU_PARITY Phases 6–7
#
# Phase 6 (G1.7): rmt L (lseek) + remote create/read/append via mock rsh/rmt
# Phase 7 (G1.3): -s / --preserve-order / --same-order extract semantics
#
# Usage: ./test_phase6_7_parity.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
MOCK_RSH="$TESTDIR/mock_rsh.sh"
MOCK_RMT="$TESTDIR/mock_rmt.sh"
TMPBASE="$(mktemp -d /tmp/mutar_phase67.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

if [ ! -x "$MUTAR" ]; then
    echo "mutar binary not found: $MUTAR"
    exit 1
fi
chmod +x "$MOCK_RSH" "$MOCK_RMT" 2>/dev/null || true

RMT_FLAGS=(--rsh-command="$MOCK_RSH" --rmt-command="$MOCK_RMT")

# ── Phase 6: mock rmt L (lseek) protocol unit test ────────────────────────────
echo "[P6-01 mock rmt L lseek protocol]"
{
    F="$TMPBASE/proto.dat"
    printf 'ABCDEFGHIJ' > "$F"
    OUT="$( {
        printf 'O%s\n0\n' "$F"
        printf 'L0\n3\n'          # SEEK_SET 3
        printf 'R4\n'             # read 4 → DEFG
        printf 'L2\n0\n'          # SEEK_END
        printf 'L1\n-2\n'         # SEEK_CUR -2
        printf 'R2\n'             # last 2 → IJ
        printf 'C\n'
    } | python3 "$TESTDIR/mock_rmt.py" 2>/dev/null | tr -d '\0' )"
    # Expect A0, A3, A4+DEFG, A10, A8, A2+IJ, A0
    if echo "$OUT" | grep -q 'A3' && echo "$OUT" | grep -q 'DEFG' \
       && echo "$OUT" | grep -q 'IJ'; then
        pass "P6-01: mock rmt L (lseek) + R works"
    else
        fail "P6-01" "protocol output unexpected: $OUT"
    fi
}

# ── Phase 6: remote create + list via mock rsh/rmt ────────────────────────────
echo "[P6-02 remote create/list]"
{
    D="$TMPBASE/remote_cl"
    mkdir -p "$D/src"
    echo "alpha" > "$D/src/a.txt"
    echo "beta"  > "$D/src/b.txt"
    ARCH_PATH="$D/archive.tar"
    REMOTE="localhost:${ARCH_PATH}"

    if ! "$MUTAR" -c -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D" src/a.txt src/b.txt 2>"$D/err.txt"; then
        fail "P6-02" "remote create failed: $(cat "$D/err.txt")"
    elif [ ! -s "$ARCH_PATH" ]; then
        fail "P6-02" "remote archive not created on disk"
    else
        LIST="$("$MUTAR" -tf "$REMOTE" "${RMT_FLAGS[@]}" 2>/dev/null || true)"
        if echo "$LIST" | grep -q 'a.txt' && echo "$LIST" | grep -q 'b.txt'; then
            pass "P6-02: remote create + list via rmt"
        else
            fail "P6-02" "list missing members: $LIST"
        fi
    fi
}

# ── Phase 6: remote extract ───────────────────────────────────────────────────
echo "[P6-03 remote extract]"
{
    D="$TMPBASE/remote_x"
    mkdir -p "$D/src" "$D/out"
    echo "payload" > "$D/src/file.txt"
    ARCH_PATH="$D/archive.tar"
    REMOTE="localhost:${ARCH_PATH}"

    "$MUTAR" -c -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D" src/file.txt 2>/dev/null || true
    if "$MUTAR" -x -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D/out" 2>"$D/err.txt"; then
        if [ -f "$D/out/src/file.txt" ] && grep -q payload "$D/out/src/file.txt"; then
            pass "P6-03: remote extract via rmt"
        else
            fail "P6-03" "extracted content missing"
        fi
    else
        fail "P6-03" "remote extract failed: $(cat "$D/err.txt")"
    fi
}

# ── Phase 6: remote append (-r) uses L seek ───────────────────────────────────
echo "[P6-04 remote append -r]"
{
    D="$TMPBASE/remote_r"
    mkdir -p "$D"
    echo "one" > "$D/one.txt"
    echo "two" > "$D/two.txt"
    ARCH_PATH="$D/archive.tar"
    REMOTE="localhost:${ARCH_PATH}"

    "$MUTAR" -c -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D" one.txt 2>"$D/err_c.txt" || true
    if ! "$MUTAR" -r -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D" two.txt 2>"$D/err_r.txt"; then
        fail "P6-04" "remote append failed: $(cat "$D/err_r.txt")"
    else
        LIST="$("$MUTAR" -tf "$REMOTE" "${RMT_FLAGS[@]}" 2>/dev/null || true)"
        if echo "$LIST" | grep -q 'one.txt' && echo "$LIST" | grep -q 'two.txt'; then
            # Local list should match
            LISTL="$("$MUTAR" -tf "$ARCH_PATH" 2>/dev/null || true)"
            if echo "$LISTL" | grep -q 'two.txt'; then
                pass "P6-04: remote append (-r) with rmt L"
            else
                fail "P6-04" "local archive missing two.txt after append"
            fi
        else
            fail "P6-04" "list after append: $LIST"
        fi
    fi
}

# ── Phase 6: remote update (-u) ───────────────────────────────────────────────
echo "[P6-05 remote update -u]"
{
    D="$TMPBASE/remote_u"
    mkdir -p "$D"
    echo "old" > "$D/x.txt"
    ARCH_PATH="$D/archive.tar"
    REMOTE="localhost:${ARCH_PATH}"

    "$MUTAR" -c -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D" x.txt 2>/dev/null || true
    sleep 1
    echo "new content" > "$D/x.txt"
    if ! "$MUTAR" -u -f "$REMOTE" "${RMT_FLAGS[@]}" -C "$D" x.txt 2>"$D/err.txt"; then
        fail "P6-05" "remote update failed: $(cat "$D/err.txt")"
    else
        # Extract and check last occurrence / content presence
        mkdir -p "$D/out"
        "$MUTAR" -x -f "$ARCH_PATH" -C "$D/out" 2>/dev/null || true
        # After update, archive has two x.txt members; last extract wins if overwrite
        if [ -f "$D/out/x.txt" ]; then
            pass "P6-05: remote update (-u) accepted and wrote"
        else
            fail "P6-05" "no x.txt after update extract"
        fi
    fi
}

# ── Phase 6: help mentions L/lseek support ────────────────────────────────────
echo "[P6-06 help rmt L]"
{
    HELP="$("$MUTAR" --help 2>&1)"
    if echo "$HELP" | grep -qi 'lseek\|L=lseek\|O/R/W/L'; then
        if echo "$HELP" | grep -qi 'not supported'; then
            # Still OK if phrase is about something else; fail only if rmt line says not supported
            RMTLINE="$(echo "$HELP" | grep -i rmt-command | head -2)"
            if echo "$RMTLINE" | grep -qi 'not supported'; then
                fail "P6-06" "help still claims rmt lseek not supported: $RMTLINE"
            else
                pass "P6-06: help documents rmt L/lseek"
            fi
        else
            pass "P6-06: help documents rmt L/lseek"
        fi
    else
        fail "P6-06" "help missing rmt lseek mention"
    fi
}

# ── Phase 7: preserve-order extracts in archive order ─────────────────────────
echo "[P7-01 preserve-order in-order extract]"
{
    D="$TMPBASE/po1"
    mkdir -p "$D/src" "$D/out"
    echo a > "$D/src/a.txt"
    echo b > "$D/src/b.txt"
    echo c > "$D/src/c.txt"
    "$MUTAR" -c -f "$D/a.tar" -C "$D/src" a.txt b.txt c.txt 2>/dev/null

    # Request b then c (archive order) with -s
    if "$MUTAR" -x -s -f "$D/a.tar" -C "$D/out" b.txt c.txt 2>"$D/err.txt"; then
        if [ -f "$D/out/b.txt" ] && [ -f "$D/out/c.txt" ] && [ ! -f "$D/out/a.txt" ]; then
            pass "P7-01: -s extracts listed members in archive order"
        else
            fail "P7-01" "unexpected files: $(ls -la "$D/out")"
        fi
    else
        fail "P7-01" "extract failed: $(cat "$D/err.txt")"
    fi
}

# ── Phase 7: out-of-order want list misses earlier members ────────────────────
echo "[P7-02 preserve-order out-of-order misses]"
{
    D="$TMPBASE/po2"
    mkdir -p "$D/src" "$D/out"
    echo a > "$D/src/a.txt"
    echo b > "$D/src/b.txt"
    echo c > "$D/src/c.txt"
    "$MUTAR" -c -f "$D/a.tar" -C "$D/src" a.txt b.txt c.txt 2>/dev/null

    # Want c then b: archive order is a,b,c so c is found; b already passed → not found
    RC=0
    ERR="$("$MUTAR" -x -s -f "$D/a.tar" -C "$D/out" c.txt b.txt 2>&1)" || RC=$?
    if [ -f "$D/out/c.txt" ] && [ ! -f "$D/out/b.txt" ]; then
        if echo "$ERR" | grep -qi 'Not found'; then
            pass "P7-02: -s out-of-order reports Not found for missed member"
        else
            # Still pass if behavior is correct even without message
            pass "P7-02: -s out-of-order skips already-passed member (rc=$RC)"
        fi
    else
        fail "P7-02" "expected only c.txt; files=$(ls "$D/out" 2>/dev/null) err=$ERR"
    fi
}

# ── Phase 7: without -s, out-of-order list still extracts both ────────────────
echo "[P7-03 without -s extracts any order]"
{
    D="$TMPBASE/po3"
    mkdir -p "$D/src" "$D/out"
    echo a > "$D/src/a.txt"
    echo b > "$D/src/b.txt"
    "$MUTAR" -c -f "$D/a.tar" -C "$D/src" a.txt b.txt 2>/dev/null

    "$MUTAR" -x -f "$D/a.tar" -C "$D/out" b.txt a.txt 2>/dev/null || true
    if [ -f "$D/out/a.txt" ] && [ -f "$D/out/b.txt" ]; then
        pass "P7-03: without -s both members extracted regardless of list order"
    else
        fail "P7-03" "missing files: $(ls "$D/out" 2>/dev/null)"
    fi
}

# ── Phase 7: -s no longer emits not-implemented warning ───────────────────────
echo "[P7-04 -s no not-implemented warning]"
{
    D="$TMPBASE/po4"
    mkdir -p "$D"
    echo z > "$D/z.txt"
    "$MUTAR" -c -f "$D/a.tar" -C "$D" z.txt 2>/dev/null
    ERR="$("$MUTAR" -t -s -f "$D/a.tar" 2>&1)"
    if echo "$ERR" | grep -qi 'not yet implemented'; then
        fail "P7-04" "still warns not implemented: $ERR"
    else
        pass "P7-04: -s accepted without not-implemented warning"
    fi
}

# ── Phase 7: --preserve sets -s behaviour ─────────────────────────────────────
echo "[P7-05 --preserve implies order constraint]"
{
    D="$TMPBASE/po5"
    mkdir -p "$D/src" "$D/out"
    echo a > "$D/src/a.txt"
    echo b > "$D/src/b.txt"
    "$MUTAR" -c -f "$D/a.tar" -C "$D/src" a.txt b.txt 2>/dev/null
    "$MUTAR" -x --preserve -f "$D/a.tar" -C "$D/out" b.txt a.txt 2>/dev/null || true
    # With preserve-order, want b then a: b found, a already passed → only b
    if [ -f "$D/out/b.txt" ] && [ ! -f "$D/out/a.txt" ]; then
        pass "P7-05: --preserve applies same-order extract constraint"
    else
        # If both extracted, --preserve may not wire -s into extract yet
        fail "P7-05" "expected only b.txt; got $(ls "$D/out" 2>/dev/null)"
    fi
}

# ── Phase 7: --same-order alias ───────────────────────────────────────────────
echo "[P7-06 --same-order alias]"
{
    D="$TMPBASE/po6"
    mkdir -p "$D/src" "$D/out"
    echo a > "$D/src/a.txt"
    echo b > "$D/src/b.txt"
    "$MUTAR" -c -f "$D/a.tar" -C "$D/src" a.txt b.txt 2>/dev/null
    "$MUTAR" -x --same-order -f "$D/a.tar" -C "$D/out" a.txt b.txt 2>/dev/null || true
    if [ -f "$D/out/a.txt" ] && [ -f "$D/out/b.txt" ]; then
        pass "P7-06: --same-order extracts in-order list"
    else
        fail "P7-06" "missing files"
    fi
}

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "========================================="
echo " Results: PASS=$PASS   FAIL=$FAIL   SKIP=$SKIP"
echo "========================================="
[ "$FAIL" -eq 0 ]
