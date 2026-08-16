#!/usr/bin/env bash
# tests/test_phase2_3_parity.sh — GOAL_GNU_PARITY Phases 2–3
#
# Covers:
#   G1.12 --exclude-ignore / --exclude-ignore-recursive
#   G1.5  -g / --listed-incremental skip filter (files, symlinks, specials)
#   G1.4  -G / --incremental dumpdir create + extract purge
#
# Usage: ./test_phase2_3_parity.sh [/path/to/mutar]
set -uo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
TMPBASE="$(mktemp -d /tmp/mutar_phase23.XXXXXX)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP+1)); }

if [ ! -x "$MUTAR" ]; then
    echo "mutar binary not found: $MUTAR"
    exit 1
fi

echo "========================================="
echo " mutar Phase 2–3 parity tests"
echo " binary: $MUTAR"
echo "========================================="

# ── G1.12 --exclude-ignore (non-recursive) ────────────────────────────────────
echo "[G1.12 exclude-ignore]"
{
    D="$TMPBASE/ex_nr"
    mkdir -p "$D/tree/sub" "$D/tree/keep"
    echo a > "$D/tree/a.txt"
    echo b > "$D/tree/skip.me"
    echo c > "$D/tree/sub/skip.me"
    echo d > "$D/tree/sub/ok.txt"
    echo e > "$D/tree/keep/skip.me"
    echo 'skip.me' > "$D/tree/.mutarignore"

    if ! "$MUTAR" -c -f "$D/nr.tar" --exclude-ignore=.mutarignore -C "$D" tree 2>"$D/err.txt"; then
        fail "G1.12-nr-create" "create failed: $(cat "$D/err.txt")"
    else
        LIST=$("$MUTAR" -tf "$D/nr.tar" 2>/dev/null | sort)
        # Top-level skip.me excluded
        if echo "$LIST" | grep -qx 'tree/skip.me'; then
            fail "G1.12-nr" "top-level skip.me should be excluded"
        # Nested skip.me kept (non-recursive)
        elif ! echo "$LIST" | grep -qx 'tree/sub/skip.me'; then
            fail "G1.12-nr" "nested skip.me should remain: $LIST"
        elif ! echo "$LIST" | grep -qx 'tree/keep/skip.me'; then
            fail "G1.12-nr" "keep/skip.me should remain: $LIST"
        elif ! echo "$LIST" | grep -qx 'tree/a.txt'; then
            fail "G1.12-nr" "a.txt missing: $LIST"
        else
            pass "G1.12: --exclude-ignore applies only to that directory's children"
        fi
    fi
}

# ── G1.12 --exclude-ignore-recursive ──────────────────────────────────────────
echo "[G1.12 exclude-ignore-recursive]"
{
    D="$TMPBASE/ex_r"
    mkdir -p "$D/tree/sub/deep" "$D/tree/keep"
    echo a > "$D/tree/a.txt"
    echo b > "$D/tree/skip.me"
    echo c > "$D/tree/sub/skip.me"
    echo d > "$D/tree/sub/ok.txt"
    echo e > "$D/tree/sub/deep/skip.me"
    echo f > "$D/tree/keep/skip.me"
    echo 'skip.me' > "$D/tree/.mutarignore"

    if ! "$MUTAR" -c -f "$D/r.tar" --exclude-ignore-recursive=.mutarignore -C "$D" tree 2>"$D/err.txt"; then
        fail "G1.12-r-create" "create failed: $(cat "$D/err.txt")"
    else
        LIST=$("$MUTAR" -tf "$D/r.tar" 2>/dev/null | sort)
        BAD=0
        for p in tree/skip.me tree/sub/skip.me tree/sub/deep/skip.me tree/keep/skip.me; do
            if echo "$LIST" | grep -qx "$p"; then
                echo "  unexpected member: $p"
                BAD=1
            fi
        done
        if [ "$BAD" -ne 0 ]; then
            fail "G1.12-r" "skip.me still present under subtree: $LIST"
        elif ! echo "$LIST" | grep -qx 'tree/sub/ok.txt'; then
            fail "G1.12-r" "ok.txt missing: $LIST"
        else
            pass "G1.12: --exclude-ignore-recursive applies to whole subtree"
        fi
    fi
}

# Nested ignore file only in subdirectory
echo "[G1.12 nested ignore file]"
{
    D="$TMPBASE/ex_nest"
    mkdir -p "$D/tree/sub"
    echo a > "$D/tree/a.txt"
    echo b > "$D/tree/sub/hide.dat"
    echo c > "$D/tree/sub/keep.dat"
    echo 'hide.dat' > "$D/tree/sub/.mutarignore"

    if ! "$MUTAR" -c -f "$D/n.tar" --exclude-ignore=.mutarignore -C "$D" tree 2>/dev/null; then
        fail "G1.12-nest-create" "create failed"
    else
        LIST=$("$MUTAR" -tf "$D/n.tar" 2>/dev/null | sort)
        if echo "$LIST" | grep -qx 'tree/sub/hide.dat'; then
            fail "G1.12-nest" "hide.dat should be excluded by sub/.mutarignore"
        elif ! echo "$LIST" | grep -qx 'tree/sub/keep.dat'; then
            fail "G1.12-nest" "keep.dat missing: $LIST"
        elif ! echo "$LIST" | grep -qx 'tree/a.txt'; then
            fail "G1.12-nest" "a.txt missing: $LIST"
        else
            pass "G1.12: ignore file in nested dir applies to that dir only"
        fi
    fi
}

# ── G1.5 listed-incremental skip filter ───────────────────────────────────────
echo "[G1.5 listed-incremental skip]"
{
    D="$TMPBASE/listed"
    mkdir -p "$D/tree/sub"
    echo a > "$D/tree/a.txt"
    echo b > "$D/tree/sub/b.txt"
    ln -s a.txt "$D/tree/link"
    # fifo if possible
    if mkfifo "$D/tree/fifo" 2>/dev/null; then
        HAVE_FIFO=1
    else
        HAVE_FIFO=0
    fi

    rm -f "$D/s.snar"
    if ! "$MUTAR" -c --level=0 -g "$D/s.snar" -f "$D/l0.tar" -C "$D" tree 2>"$D/err0.txt"; then
        fail "G1.5-l0" "level0 create failed: $(cat "$D/err0.txt")"
    else
        L0=$("$MUTAR" -tf "$D/l0.tar" 2>/dev/null | sort)
        if ! echo "$L0" | grep -q 'tree/a.txt' || ! echo "$L0" | grep -q 'tree/link'; then
            fail "G1.5-l0" "level0 missing members: $L0"
        else
            pass "G1.5: level-0 archives files and symlink"
        fi
    fi

    # No changes → only directory headers
    sleep 1
    cp "$D/s.snar" "$D/s_none.snar"
    if ! "$MUTAR" -c --level=1 -g "$D/s_none.snar" -f "$D/l_none.tar" -C "$D" tree 2>"$D/errn.txt"; then
        fail "G1.5-none" "level1 create failed: $(cat "$D/errn.txt")"
    else
        LN=$("$MUTAR" -tf "$D/l_none.tar" 2>/dev/null | sort)
        if echo "$LN" | grep -qE 'tree/a\.txt|tree/link|tree/sub/b\.txt|tree/fifo'; then
            fail "G1.5-none" "unchanged non-dirs should be skipped: $LN"
        elif ! echo "$LN" | grep -q 'tree/'; then
            fail "G1.5-none" "directory headers should still be present: $LN"
        else
            pass "G1.5: level≥1 skips unchanged files/symlinks/specials; keeps dirs"
        fi
    fi

    # Change only symlink
    ln -sfn sub/b.txt "$D/tree/link"
    cp "$D/s.snar" "$D/s_link.snar"
    if ! "$MUTAR" -c --level=1 -g "$D/s_link.snar" -f "$D/l_link.tar" -C "$D" tree 2>/dev/null; then
        fail "G1.5-link" "create failed"
    else
        LL=$("$MUTAR" -tf "$D/l_link.tar" 2>/dev/null | sort)
        if ! echo "$LL" | grep -qx 'tree/link'; then
            fail "G1.5-link" "changed symlink missing: $LL"
        elif echo "$LL" | grep -qx 'tree/a.txt'; then
            fail "G1.5-link" "unchanged a.txt should not appear: $LL"
        else
            pass "G1.5: changed symlink is re-archived"
        fi
    fi

    # Change only regular file
    echo changed > "$D/tree/a.txt"
    cp "$D/s.snar" "$D/s_file.snar"
    if ! "$MUTAR" -c --level=1 -g "$D/s_file.snar" -f "$D/l_file.tar" -C "$D" tree 2>/dev/null; then
        fail "G1.5-file" "create failed"
    else
        LF=$("$MUTAR" -tf "$D/l_file.tar" 2>/dev/null | sort)
        if ! echo "$LF" | grep -qx 'tree/a.txt'; then
            fail "G1.5-file" "changed file missing: $LF"
        else
            pass "G1.5: changed regular file is re-archived"
        fi
    fi

    # Snapshot still records dirs (fields separated by TAB)
    if head -1 "$D/s.snar" | grep -q 'MUTAR_SNAPSHOT_V2' \
       && grep -E $'^tree(/sub)?\t' "$D/s.snar" >/dev/null; then
        pass "G1.5: snapshot V2 still records directories"
    else
        fail "G1.5-snap" "snapshot missing dirs: $(cat -A "$D/s.snar")"
    fi
}

# ── G1.4 -G dumpdir create ────────────────────────────────────────────────────
echo "[G1.4 incremental dumpdir create]"
{
    D="$TMPBASE/dumpdir"
    mkdir -p "$D/d/sub"
    echo a > "$D/d/a.txt"
    echo b > "$D/d/sub/b.txt"
    echo c > "$D/d/c.txt"

    if ! "$MUTAR" -c -G -f "$D/g0.tar" -C "$D" d 2>"$D/err.txt"; then
        fail "G1.4-create" "create failed: $(cat "$D/err.txt")"
    else
        # Parse typeflags with python
        PYOUT=$(python3 - <<'PY' "$D/g0.tar"
import sys
data=open(sys.argv[1],'rb').read()
off=0
dirs=[]
files=[]
while off+512<=len(data):
    b=data[off:off+512]
    if b==b'\0'*512: break
    name=b[0:100].split(b'\0')[0].decode('latin1','replace')
    tf=chr(b[156]) if b[156] else '?'
    size=int(b[124:136].split(b'\0')[0].strip() or b'0', 8)
    if tf=='D':
        body=data[off+512:off+512+size]
        dirs.append((name, body))
    elif tf in ('0','\0'):
        files.append(name)
    nb=(size+511)//512 if size else 0
    off += 512 + nb*512
# Expect d/ and d/sub/ dumpdirs
names=[n for n,_ in dirs]
ok = any(n.rstrip('/')=='d' for n in names) and any('sub' in n for n in names)
# Body of d/ should contain Ya.txt, Yc.txt, Dsub
body0 = next((bd for n,bd in dirs if n.rstrip('/')=='d'), b'')
has_a = b'Ya.txt\0' in body0
has_c = b'Yc.txt\0' in body0
has_sub = b'Dsub\0' in body0
print('OK' if ok and has_a and has_c and has_sub else 'BAD')
print('dirs', names)
print('body', repr(body0[:60]))
PY
)
        if echo "$PYOUT" | head -1 | grep -q OK; then
            pass "G1.4: -G create emits GNUTYPE_DUMPDIR with Y/D entries"
        else
            fail "G1.4-create-fmt" "$PYOUT"
        fi

        # mutar can list it
        if "$MUTAR" -tf "$D/g0.tar" 2>/dev/null | grep -q 'd/a.txt'; then
            pass "G1.4: mutar lists dumpdir archive"
        else
            fail "G1.4-list" "list failed"
        fi

        # GNU tar interop (list)
        if command -v tar >/dev/null 2>&1; then
            if tar -tf "$D/g0.tar" 2>/dev/null | grep -q 'd/a.txt'; then
                pass "G1.4: GNU tar lists mutar -G archive"
            else
                fail "G1.4-gnu-list" "GNU tar could not list mutar dumpdir archive"
            fi
        else
            skip "G1.4-gnu-list" "system tar not available"
        fi
    fi
}

# ── G1.4 -G extract purge ─────────────────────────────────────────────────────
echo "[G1.4 incremental dumpdir extract/purge]"
{
    D="$TMPBASE/purge"
    mkdir -p "$D/src/d"
    echo a > "$D/src/d/a.txt"
    echo b > "$D/src/d/b.txt"
    "$MUTAR" -c -G -f "$D/g0.tar" -C "$D/src" d >/dev/null 2>&1

    mkdir -p "$D/out/d"
    echo a > "$D/out/d/a.txt"
    echo b > "$D/out/d/b.txt"
    echo extra > "$D/out/d/extra.txt"

    if ! "$MUTAR" -x -G -f "$D/g0.tar" -C "$D/out" 2>"$D/err.txt"; then
        fail "G1.4-extract" "extract failed: $(cat "$D/err.txt")"
    else
        if [ -f "$D/out/d/extra.txt" ]; then
            fail "G1.4-purge" "extra.txt should have been purged"
        elif [ ! -f "$D/out/d/a.txt" ] || [ ! -f "$D/out/d/b.txt" ]; then
            fail "G1.4-purge" "expected members missing"
        else
            pass "G1.4: -G extract purges files not in dumpdir"
        fi
    fi

    # Second archive without b.txt — purge b on extract
    rm -rf "$D/src2" && mkdir -p "$D/src2/d"
    echo a2 > "$D/src2/d/a.txt"
    echo c > "$D/src2/d/c.txt"
    "$MUTAR" -c -G -f "$D/g1.tar" -C "$D/src2" d >/dev/null 2>&1
    if ! "$MUTAR" -x -G -f "$D/g1.tar" -C "$D/out" 2>/dev/null; then
        fail "G1.4-extract2" "second extract failed"
    else
        if [ -f "$D/out/d/b.txt" ]; then
            fail "G1.4-purge2" "b.txt should be purged by second dumpdir"
        elif [ ! -f "$D/out/d/c.txt" ]; then
            fail "G1.4-purge2" "c.txt should be extracted"
        else
            pass "G1.4: successive -G extract updates directory via dumpdir"
        fi
    fi

    # Round-trip mutar-mutar without purge side effects on clean tree
    rm -rf "$D/rt" && mkdir -p "$D/rt"
    "$MUTAR" -x -f "$D/g0.tar" -C "$D/rt" >/dev/null 2>&1
    if [ -f "$D/rt/d/a.txt" ] && [ -f "$D/rt/d/b.txt" ]; then
        pass "G1.4: extract without -G still restores dumpdir archive files"
    else
        fail "G1.4-rt" "plain extract of dumpdir archive failed"
    fi
}

# ── Help text ─────────────────────────────────────────────────────────────────
echo "[help]"
{
    HELP=$("$MUTAR" --help 2>&1)
    ok=1
    echo "$HELP" | grep -q 'exclude-ignore' || ok=0
    echo "$HELP" | grep -q 'exclude-ignore-recursive' || ok=0
    echo "$HELP" | grep -q 'listed-incremental' || ok=0
    echo "$HELP" | grep -qE -- '--incremental' || ok=0
    if [ "$ok" -eq 1 ]; then
        pass "help mentions exclude-ignore and incremental options"
    else
        fail "help" "missing option text"
    fi
}

echo ""
echo "========================================="
echo " Results: PASS=$PASS   FAIL=$FAIL   SKIP=$SKIP"
echo "========================================="
[ "$FAIL" -eq 0 ]
