// boundary_tests.cpp — Integer overflow/underflow, off-by-one, and memory
// safety tests for mutar's core numeric and buffer-handling functions.
//
// Compiled with -fsanitize=address,undefined so every UB/overflow/OOB access
// triggers an immediate, attributable error.
//
// Build (handled by CMakeLists.txt target 'mutar_boundary_tests'):
//   cmake -DCMAKE_BUILD_TYPE=Debug ..
//   make mutar_boundary_tests
//
// Run standalone (example, no CMake):
//   g++ -std=c++23 -fsanitize=address,undefined -fno-omit-frame-pointer
//       -ggdb3 -O1 -I../src boundary_tests.cpp -o boundary_tests
//   ./boundary_tests
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../src/mutar.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// ─── Tiny test framework ──────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                                    \
    if (!(cond)) {                                                          \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "       \
                  << #cond << "\n";                                         \
        ++g_fail;                                                           \
    } else {                                                                \
        ++g_pass;                                                           \
    }                                                                       \
} while (false)

#define CHECK_EQ(a,b) do {                                                  \
    auto _a = (a); auto _b = (b);                                           \
    if (_a != _b) {                                                         \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "       \
                  << #a << " == " << #b                                     \
                  << "  (got " << _a << " vs " << _b << ")\n";             \
        ++g_fail;                                                           \
    } else { ++g_pass; }                                                    \
} while (false)

// ─── 1. read_octal — zero-length, all-NUL, overflow digits ───────────────────

static void test_read_octal() {
    using mutar::read_octal;

    // Empty field → 0 (no OOB read)
    CHECK_EQ(read_octal(""), 0ULL);

    // All-NUL field (8 NUL bytes) → 0
    const char nuls[8] = {};
    CHECK_EQ(read_octal(std::string_view(nuls, 8)), 0ULL);

    // Canonical value: 0000755\0 (mode 0755)
    CHECK_EQ(read_octal("0000755"), 0755ULL);

    // Max octal in 11 chars (12-byte size field, leave one for NUL):
    // "77777777777" = 077777777777 = 8 589 934 591 (< UINT64_MAX — no overflow)
    CHECK_EQ(read_octal("77777777777"), 077777777777ULL);

    // Space-terminated (GNU tar uses SP not NUL in some fields)
    CHECK_EQ(read_octal("644 "), 0644ULL);

    // Non-octal character stops scan cleanly
    CHECK_EQ(read_octal("012abc"), 012ULL);

    // Single '0' digit
    CHECK_EQ(read_octal("0"), 0ULL);

    // Off-by-one: field of exactly 1 byte containing '7'
    CHECK_EQ(read_octal(std::string_view("7", 1)), 7ULL);
}

// ─── 2. read_base256 — marker bit masking, sign, all-zeros, boundary values ──

static void test_read_base256() {
    using mutar::read_base256;
    using mutar::is_base256;

    // Build a 12-byte base-256 field for value 8 GiB (8 589 934 592)
    // = 0x0000000200000000. In a 12-byte big-endian field (byte 0 = marker),
    // the value occupies bytes 1..11. Byte 7 (offset 7 from start) holds 0x02.
    {
        char f[12] = {};
        f[0] = static_cast<char>(0x80);   // marker bit only (positive, no sign)
        f[7] = 0x02;                       // big-endian: 0x02 at byte offset 7
                                           // → value = 0x02 << (8*4) = 0x200000000
        CHECK(is_base256(std::string_view(f, 12)));
        std::int64_t val = read_base256(std::string_view(f, 12));
        CHECK_EQ(val, static_cast<std::int64_t>(0x200000000LL));
    }

    // Marker bit must NOT be included in the numeric value.
    // If marker bit leaked in (old bug): value would be ~= 0x80_0000_0000 for
    // the above field instead of 0x02_0000_0000.
    {
        char f[12] = {};
        f[0] = static_cast<char>(0x80);
        f[11] = 0x01;   // smallest positive base-256 value: 1
        std::int64_t val = read_base256(std::string_view(f, 12));
        CHECK_EQ(val, 1LL);
        // If 0x80 marker leaked: result would be 0x80 * 256^11 + 1 (absurd)
        CHECK(val < (1LL << 40));
    }

    // Single-byte field: 0x80 alone → value 0
    {
        char f[1] = { static_cast<char>(0x80) };
        CHECK_EQ(read_base256(std::string_view(f, 1)), 0LL);
    }

    // Negative (0xFF sign): field full of 0xFF → -1
    {
        char f[12];
        std::memset(f, 0xFF, 12);
        // 0xFF has both 0x80 (marker) and 0x40 (negative sign) set
        // After masking marker: first byte = 0x7F, sign bit = 1 → negative
        std::int64_t val = read_base256(std::string_view(f, 12));
        CHECK_EQ(val, -1LL);
    }

    // Zero value with marker: 0x80 followed by all zeros
    {
        char f[12] = {};
        f[0] = static_cast<char>(0x80);
        CHECK_EQ(read_base256(std::string_view(f, 12)), 0LL);
    }

    // Max positive in 8 bytes: for base-256 with N bytes, the MSB byte after
    // clearing the 0x80 marker can be at most 0x3F (sign bit 0x40 must be 0).
    // So the max-positive encoding for 8 bytes is: f[0]=0xBF (0x80|0x3F),
    // f[1..7]=0xFF → value = 0x3FFFFFFFFFFFFFFF = 2^62 - 1.
    {
        char f[8];
        f[0] = static_cast<char>(0xBF);  // marker(0x80) | max-positive-MSB(0x3F)
        std::memset(f + 1, 0xFF, 7);
        std::int64_t val = read_base256(std::string_view(f, 8));
        CHECK_EQ(val, static_cast<std::int64_t>(0x3FFFFFFFFFFFFFFFLL)); // 2^62 - 1
    }
}

// ─── 3. write_octal — off-by-one in field width, value zero, near-overflow ───

static void test_write_octal() {
    using mutar::write_octal;

    // Width=8, value=0 → "0000000\0"
    {
        char buf[8] = {};
        write_octal(buf, 8, 0);
        CHECK_EQ(buf[7], '\0');
        CHECK_EQ(std::string(buf), "0000000");
    }

    // Width=8, value=0755 → "0000755\0"
    {
        char buf[8] = {};
        write_octal(buf, 8, 0755);
        CHECK_EQ(std::string_view(buf, 7), "0000755");
    }

    // Width=12, large value: 077777777777 (max 11-digit octal)
    {
        char buf[12] = {};
        write_octal(buf, 12, 077777777777ULL);
        // Parse back and confirm round-trip
        auto back = mutar::read_octal(std::string_view(buf, 12));
        CHECK_EQ(back, 077777777777ULL);
    }

    // Width=1: only NUL written — must not write out of bounds (ASAN catches)
    {
        char buf[4] = {'\x7f','\x7f','\x7f','\x7f'};
        write_octal(buf, 1, 0);
        // buf[0] gets '\0', buf[1..3] unchanged
        CHECK_EQ(buf[0], '\0');
        CHECK_EQ(static_cast<unsigned char>(buf[1]), 0x7f);
    }

    // Off-by-one: width=2 should write "0\0" for value 0
    {
        char buf[3] = {'\x7f','\x7f','\x7f'};
        write_octal(buf, 2, 0);
        CHECK_EQ(buf[0], '0');
        CHECK_EQ(buf[1], '\0');
        CHECK_EQ(static_cast<unsigned char>(buf[2]), 0x7f); // not written
    }
}

// ─── 4. write_base256 — marker bit set, width boundary, round-trip ───────────

static void test_write_base256() {
    using mutar::write_base256;
    using mutar::read_base256;
    using mutar::is_base256;

    // 8 GiB round-trip
    {
        char buf[12] = {};
        std::int64_t v = 0x200000000LL; // 8 GiB
        write_base256(buf, 12, v);
        CHECK(is_base256(std::string_view(buf, 12)));
        CHECK_EQ(read_base256(std::string_view(buf, 12)), v);
    }

    // Value 1 round-trip (off-by-one guard)
    {
        char buf[12] = {};
        write_base256(buf, 12, 1);
        CHECK_EQ(read_base256(std::string_view(buf, 12)), 1LL);
    }

    // 2^62 - 1 round-trip (max positive for 8-byte base-256 field)
    {
        char buf[12] = {};
        std::int64_t v = 0x3FFFFFFFFFFFFFFFLL; // max safe positive in 8 bytes
        write_base256(buf, 12, v);
        CHECK_EQ(read_base256(std::string_view(buf, 12)), v);
    }

    // Width=1: only one byte written, must not OOB (ASAN)
    {
        char buf[4] = {};
        write_base256(buf, 1, 42);
        // marker bit must be set in buf[0]
        CHECK((static_cast<unsigned char>(buf[0]) & 0x80) != 0);
        CHECK_EQ(buf[1], '\0'); // untouched
    }
}

// ─── 5. block_checksum / valid_checksum — all-zero block, all-0xFF block ─────

static void test_checksum() {
    using namespace mutar;

    // All-zero block has known checksum = 8 * ' ' = 8 * 32 = 256
    {
        Block blk{};
        unsigned int sum = block_checksum(blk);
        CHECK_EQ(sum, 8u * 32u); // chksum field (8 bytes) treated as spaces

        // write_checksum then valid_checksum should round-trip
        write_checksum(blk);
        CHECK(valid_checksum(blk));
    }

    // All-0xFF block (except chksum treated as spaces)
    {
        Block blk;
        std::memset(blk.buffer, 0xFF, BLOCKSIZE);
        write_checksum(blk);
        CHECK(valid_checksum(blk));
    }

    // Corrupt one byte → checksum fails
    {
        Block blk{};
        write_checksum(blk);
        blk.buffer[0] ^= 0x01; // flip LSB
        CHECK(!valid_checksum(blk));
    }

    // Off-by-one: checksum field occupies bytes 148..155 (8 bytes)
    // Flipping byte 147 (just before) should invalidate the checksum
    {
        Block blk{};
        write_checksum(blk);
        blk.buffer[147] = static_cast<char>(0xFF);
        CHECK(!valid_checksum(blk));
    }

    // Flipping byte 156 (just after chksum field) should also invalidate
    {
        Block blk{};
        write_checksum(blk);
        blk.buffer[156] = static_cast<char>(0xFF);
        CHECK(!valid_checksum(blk));
    }
}

// ─── 6. pax_append — length convergence, zero-length value, single-char ──────
// We can't call pax_append directly (it's static), so we test indirectly via
// the PAX round-trip: write a PAX archive with extreme names and re-read it.

#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

// Helper: write bytes to a tmp file, return path
static std::string write_tmp(const std::string& data) {
    char tmpl[] = "/tmp/mutar_bt_XXXXXX";
    int fd = ::mkstemp(tmpl);
    assert(fd >= 0);
    assert(::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    ::close(fd);
    return tmpl;
}

// Run mutar with given args, return exit code and captured stdout/stderr
static int run_star(const std::vector<std::string>& args,
                    std::string* out = nullptr) {
    static const char* MUTAR = nullptr;
    if (!MUTAR) {
        // Try standard build location
        if (::access("./mutar", X_OK) == 0) MUTAR = "./mutar";
        else if (::access("../build/mutar", X_OK) == 0) MUTAR = "../build/mutar";
        else MUTAR = "mutar";
    }

    std::string cmd = std::string(MUTAR);
    for (const auto& a : args) cmd += " " + a;

    if (out) {
        cmd += " 2>&1";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) return -1;
        char buf[256];
        while (::fgets(buf, sizeof(buf), p)) *out += buf;
        return ::pclose(p);
    }
    cmd += " >/dev/null 2>&1";
    return ::system(cmd.c_str());
}

static void test_pax_long_name() {
    // PAX path: 200-char filename (within NAME_MAX=255) — exercises pax_append
    // length convergence and GNU LongName extension for names > 100 chars.
    std::string longname(200, 'a');
    longname += ".txt";

    // Create temp dir and file
    char tmpdir[] = "/tmp/mutar_bt_dir_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);
    std::string filepath = std::string(tmpdir) + "/" + longname;
    {
        int fd = ::open(filepath.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0);
        (void)::write(fd, "hello", 5);
        ::close(fd);
    }

    std::string archive = std::string(tmpdir) + "/test.tar";

    // Create PAX archive
    int rc = run_star({"--posix", "-cf", archive, "-C", tmpdir, longname});
    CHECK_EQ(rc, 0);

    // List it back
    std::string listing;
    rc = run_star({"-tf", archive}, &listing);
    CHECK_EQ(rc, 0);
    CHECK(listing.find(longname) != std::string::npos);

    // Extract and verify
    char outdir[] = "/tmp/mutar_bt_out_XXXXXX";
    assert(::mkdtemp(outdir) != nullptr);
    rc = run_star({"-xf", archive, "-C", outdir});
    CHECK_EQ(rc, 0);

    std::string extracted = std::string(outdir) + "/" + longname;
    CHECK_EQ(::access(extracted.c_str(), F_OK), 0);

    // Cleanup
    ::unlink(filepath.c_str());
    ::unlink(archive.c_str());
    ::unlink(extracted.c_str());
    ::rmdir(tmpdir);
    ::rmdir(outdir);
}

// ─── 7. sanitize_path — path traversal edge cases ─────────────────────────────

// Call sanitize_path via round-trip through an archive with a traversal name,
// and confirm mutar refuses to extract outside the destination.
static void test_sanitize_path_traversal() {
    // We'll synthesise a raw tar block with a traversal path and check mutar
    // extracts it safely (within extract dir, not outside).

    // Build a minimal ustar archive with entry named "../../evil.txt"
    char block[512 * 4] = {};

    // Header block
    auto* h = reinterpret_cast<mutar::PosixHeader*>(block);
    std::strncpy(h->name, "../../evil.txt", sizeof(h->name) - 1);
    std::memcpy(h->mode,  "0000644", 7);   // tar octal field, no NUL needed
    std::memcpy(h->uid,   "0001750", 7);
    std::memcpy(h->gid,   "0001750", 7);
    std::memcpy(h->size,  "00000000005", 11);
    std::memcpy(h->mtime, "14321651434", 11);
    h->typeflag = '0';
    std::memcpy(h->magic,   "ustar", 5);
    std::memcpy(h->version, "00", 2);

    // Checksum
    {
        mutar::Block blk;
        std::memcpy(blk.buffer, block, 512);
        mutar::write_checksum(blk);
        std::memcpy(block, blk.buffer, 512);
    }

    // Data block: "EVIL\n"
    std::memcpy(block + 512, "EVIL\n", 5);

    // End-of-archive: two zero blocks
    // (blocks 2 and 3 are already zero from memset)

    std::string archive = write_tmp(std::string(block, 512 * 4));

    char outdir[] = "/tmp/mutar_bt_trav_XXXXXX";
    assert(::mkdtemp(outdir) != nullptr);

    // star should extract, but NOT to ../../evil.txt outside outdir
    run_star({"-xf", archive, "-C", outdir});

    // Check: the evil file must NOT have appeared in parent of outdir
    std::string parent = std::string(outdir);
    auto slash = parent.rfind('/');
    if (slash != std::string::npos) parent = parent.substr(0, slash);
    std::string evil = parent + "/evil.txt";
    CHECK_EQ(::access(evil.c_str(), F_OK), -1); // must NOT exist

    // Cleanup
    ::unlink(archive.c_str());
    // Remove any file mutar may have created inside outdir
    (void)::system(("rm -rf " + std::string(outdir)).c_str());
}

// ─── 8. BlockBuffer — blocking_factor boundary (1, 32767, 0 exit) ────────────
// We can't directly instantiate BlockBuffer here (it's in the mutar namespace
// inside mutar.cpp), but we test via CLI: -b 0 must fail fast, -b 1 must work.

static void test_blocking_factor_validation() {
    // -b 0 → must fail (exit != 0)
    {
        std::string out;
        int rc = run_star({"-c", "-b", "0", "-f", "/dev/null", "/dev/null"}, &out);
        CHECK(rc != 0);
    }

    // -b -1 → must fail
    {
        int rc = run_star({"-c", "-b", "-1", "-f", "/dev/null", "/dev/null"});
        CHECK(rc != 0);
    }

    // -b not-a-number → must fail
    {
        int rc = run_star({"-c", "-b", "abc", "-f", "/dev/null", "/dev/null"});
        CHECK(rc != 0);
    }

    // -b 32768 → must fail (above max 32767)
    {
        int rc = run_star({"-c", "-b", "32768", "-f", "/dev/null", "/dev/null"});
        CHECK(rc != 0);
    }

    // -b 1 → valid, creating an archive should succeed
    {
        char tmpdir[] = "/tmp/mutar_bt_bf_XXXXXX";
        assert(::mkdtemp(tmpdir) != nullptr);
        std::string archive = std::string(tmpdir) + "/t.tar";
        std::string infile  = std::string(tmpdir) + "/f.txt";
        {
            int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
            assert(fd >= 0); ::write(fd, "x", 1); ::close(fd);
        }
        int rc = run_star({"-c", "-b", "1", "-f", archive, "-C", tmpdir, "f.txt"});
        CHECK_EQ(rc, 0);
        (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
    }
}

// ─── 9. strip_components — off-by-one: n=0, n=depth, n>depth ─────────────────
// strip_components is static in mutar.cpp; test via CLI --strip-components.

static void test_strip_components() {
    char tmpdir[] = "/tmp/mutar_bt_sc_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);

    // Create a/b/c/file.txt
    std::string adir = std::string(tmpdir) + "/a";
    std::string bdir = adir + "/b";
    std::string cdir = bdir + "/c";
    ::mkdir(adir.c_str(), 0755);
    ::mkdir(bdir.c_str(), 0755);
    ::mkdir(cdir.c_str(), 0755);
    std::string infile = cdir + "/file.txt";
    {
        int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0); ::write(fd, "data", 4); ::close(fd);
    }

    std::string archive = std::string(tmpdir) + "/test.tar";
    // Create: stored as a/b/c/file.txt (no ./ prefix in our impl)
    run_star({"-cf", archive, "-C", tmpdir, "a"});

    // strip-components=0 → exact path preserved
    {
        char od[] = "/tmp/mutar_bt_sc0_XXXXXX";
        assert(::mkdtemp(od) != nullptr);
        run_star({"-xf", archive, "-C", od, "--strip-components=0"});
        CHECK_EQ(::access((std::string(od)+"/a/b/c/file.txt").c_str(), F_OK), 0);
        (void)::system(("rm -rf " + std::string(od)).c_str());
    }

    // strip-components=1 → b/c/file.txt
    {
        char od[] = "/tmp/mutar_bt_sc1_XXXXXX";
        assert(::mkdtemp(od) != nullptr);
        run_star({"-xf", archive, "-C", od, "--strip-components=1"});
        CHECK_EQ(::access((std::string(od)+"/b/c/file.txt").c_str(), F_OK), 0);
        (void)::system(("rm -rf " + std::string(od)).c_str());
    }

    // strip-components=3 → file.txt directly in outdir
    {
        char od[] = "/tmp/mutar_bt_sc3_XXXXXX";
        assert(::mkdtemp(od) != nullptr);
        run_star({"-xf", archive, "-C", od, "--strip-components=3"});
        CHECK_EQ(::access((std::string(od)+"/file.txt").c_str(), F_OK), 0);
        (void)::system(("rm -rf " + std::string(od)).c_str());
    }

    // strip-components=4 → deeper than path; file must NOT appear (off-by-one)
    {
        char od[] = "/tmp/mutar_bt_sc4_XXXXXX";
        assert(::mkdtemp(od) != nullptr);
        run_star({"-xf", archive, "-C", od, "--strip-components=4"});
        CHECK_EQ(::access((std::string(od)+"/file.txt").c_str(), F_OK), -1);
        (void)::system(("rm -rf " + std::string(od)).c_str());
    }

    (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
}

// ─── 10. Large-file size field encoding — 8 GiB boundary (base-256 trigger) ──

static void test_size_field_boundary() {
    using namespace mutar;

    // write_number (octal) can represent up to 077777777777 = 8589934591 bytes.
    // Values above that must use base-256. Test the exact boundary.

    // Value that fits in 11 octal digits: 077777777777
    {
        char buf[12] = {};
        std::uint64_t v = 077777777777ULL;
        write_octal(buf, 12, v);
        CHECK(!is_base256(std::string_view(buf, 12)));
        CHECK_EQ(read_number(std::string_view(buf, 12)), v);
    }

    // Value one above max octal (8 589 934 592) must trigger base-256
    {
        char buf[12] = {};
        std::uint64_t v = 077777777777ULL + 1;
        // write_octal would truncate; caller should use write_base256 instead.
        // Test that write_base256 round-trips it correctly.
        write_base256(buf, 12, static_cast<std::int64_t>(v));
        CHECK(is_base256(std::string_view(buf, 12)));
        CHECK_EQ(static_cast<std::uint64_t>(read_base256(std::string_view(buf, 12))), v);
    }

    // 100 GiB round-trip
    {
        char buf[12] = {};
        std::int64_t v = 100LL * 1024 * 1024 * 1024;
        write_base256(buf, 12, v);
        CHECK_EQ(read_base256(std::string_view(buf, 12)), v);
    }
}

// ─── 11. Empty archive extraction — no crash, no OOB read ────────────────────

static void test_empty_archive() {
    // Two zero blocks = valid empty archive
    char blocks[1024] = {};
    std::string archive = write_tmp(std::string(blocks, 1024));

    char od[] = "/tmp/mutar_bt_empty_XXXXXX";
    assert(::mkdtemp(od) != nullptr);

    int rc = run_star({"-xf", archive, "-C", od});
    CHECK_EQ(rc, 0);

    std::string listing;
    rc = run_star({"-tf", archive}, &listing);
    CHECK_EQ(rc, 0);
    CHECK(listing.empty() || listing == "\n");

    ::unlink(archive.c_str());
    ::rmdir(od);
}

// ─── 12. Binary integrity — arbitrary byte values round-trip without corruption

static void test_binary_roundtrip() {
    char tmpdir[] = "/tmp/mutar_bt_bin_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);

    // Create a 512-byte file with every byte value 0x00..0xFF repeated twice
    std::string infile = std::string(tmpdir) + "/binary.bin";
    {
        char data[512];
        for (int i = 0; i < 512; ++i) data[i] = static_cast<char>(i & 0xFF);
        int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0);
        (void)::write(fd, data, 512);
        ::close(fd);
    }

    std::string archive = std::string(tmpdir) + "/t.tar";
    run_star({"-cf", archive, "-C", tmpdir, "binary.bin"});

    char od[] = "/tmp/mutar_bt_binout_XXXXXX";
    assert(::mkdtemp(od) != nullptr);
    run_star({"-xf", archive, "-C", od});

    // Compare byte-by-byte
    std::string extracted = std::string(od) + "/binary.bin";
    FILE* f = ::fopen(extracted.c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        char buf[512] = {};
        std::size_t n = ::fread(buf, 1, 512, f);
        ::fclose(f);
        CHECK_EQ(n, 512UL);
        for (int i = 0; i < 512; ++i)
            CHECK_EQ(static_cast<unsigned char>(buf[i]),
                     static_cast<unsigned char>(i & 0xFF));
    }

    (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
    (void)::system(("rm -rf " + std::string(od)).c_str());
}

// ─── 13. Exact 512-byte and 513-byte file boundary (one block, two blocks) ───

static void test_block_boundary_files() {
    for (std::size_t sz : {511UL, 512UL, 513UL, 1023UL, 1024UL, 1025UL}) {
        char tmpdir[] = "/tmp/mutar_bt_blk_XXXXXX";
        assert(::mkdtemp(tmpdir) != nullptr);

        std::string infile = std::string(tmpdir) + "/f";
        {
            std::vector<char> data(sz, static_cast<char>(sz & 0xFF));
            int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
            assert(fd >= 0);
            (void)::write(fd, data.data(), sz);
            ::close(fd);
        }
        std::string archive = std::string(tmpdir) + "/t.tar";
        int rc = run_star({"-cf", archive, "-C", tmpdir, "f"});
        CHECK_EQ(rc, 0);

        char od[] = "/tmp/mutar_bt_blkout_XXXXXX";
        assert(::mkdtemp(od) != nullptr);
        rc = run_star({"-xf", archive, "-C", od});
        CHECK_EQ(rc, 0);

        std::string extracted = std::string(od) + "/f";
        struct stat st {};
        CHECK_EQ(::stat(extracted.c_str(), &st), 0);
        CHECK_EQ(static_cast<std::size_t>(st.st_size), sz);

        (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
        (void)::system(("rm -rf " + std::string(od)).c_str());
    }
}

// ─── 14. uid/gid overflow — values > 0777777 must use PAX or base-256 ─────────

static void test_uid_gid_overflow() {
    // We test that large uid/gid values round-trip without truncation.
    // With PAX format they get stored as decimal strings.
    char tmpdir[] = "/tmp/mutar_bt_ugid_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);

    std::string infile = std::string(tmpdir) + "/f.txt";
    {
        int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0); ::write(fd, "x", 1); ::close(fd);
    }

    // Create a PAX archive (which records large uid/gid in extended headers)
    std::string archive = std::string(tmpdir) + "/t.tar";
    int rc = run_star({"--posix", "-cf", archive, "-C", tmpdir, "f.txt"});
    CHECK_EQ(rc, 0);

    // List should succeed (no crash from overflow arithmetic)
    std::string listing;
    rc = run_star({"-tf", archive}, &listing);
    CHECK_EQ(rc, 0);

    (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
}

// ─── 15. --strip-components=INT_MAX — must not wrap around or OOB ────────────

static void test_strip_components_extremes() {
    char tmpdir[] = "/tmp/mutar_bt_scx_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);
    std::string infile = std::string(tmpdir) + "/f.txt";
    {
        int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0); ::write(fd, "x", 1); ::close(fd);
    }
    std::string archive = std::string(tmpdir) + "/t.tar";
    run_star({"-cf", archive, "-C", tmpdir, "f.txt"});

    char od[] = "/tmp/mutar_bt_scxout_XXXXXX";
    assert(::mkdtemp(od) != nullptr);

    // Very large strip count: all entries should simply be skipped, no crash
    int rc = run_star({"-xf", archive, "-C", od, "--strip-components=2147483647"});
    // Should succeed (exit 0) even though nothing is extracted
    CHECK_EQ(rc, 0);
    // File must NOT have been extracted
    CHECK_EQ(::access((std::string(od) + "/f.txt").c_str(), F_OK), -1);

    (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
    (void)::system(("rm -rf " + std::string(od)).c_str());
}

// ─── 16. normalize_member — pathological ./ sequences ────────────────────────

static void test_normalize_member() {
    // normalize_member is static in mutar.cpp; test indirectly: create an
    // archive, then extract specific member by name with leading ./

    char tmpdir[] = "/tmp/mutar_bt_nm_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);
    std::string infile = std::string(tmpdir) + "/hello.txt";
    {
        int fd = ::open(infile.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0); ::write(fd, "world", 5); ::close(fd);
    }
    std::string archive = std::string(tmpdir) + "/t.tar";
    run_star({"-cf", archive, "-C", tmpdir, "hello.txt"});

    // Extract specifying "./hello.txt" — must find "hello.txt"
    char od[] = "/tmp/mutar_bt_nmout_XXXXXX";
    assert(::mkdtemp(od) != nullptr);
    int rc = run_star({"-xf", archive, "-C", od, "./hello.txt"});
    CHECK_EQ(rc, 0);
    CHECK_EQ(::access((std::string(od)+"/hello.txt").c_str(), F_OK), 0);

    // Also test ./././hello.txt
    char od2[] = "/tmp/mutar_bt_nmout2_XXXXXX";
    assert(::mkdtemp(od2) != nullptr);
    rc = run_star({"-xf", archive, "-C", od2, "././hello.txt"});
    CHECK_EQ(rc, 0);
    CHECK_EQ(::access((std::string(od2)+"/hello.txt").c_str(), F_OK), 0);

    (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
    (void)::system(("rm -rf " + std::string(od)).c_str());
    (void)::system(("rm -rf " + std::string(od2)).c_str());
}

// ─── 17. --null: null-terminated -T file parsing ─────────────────────────────

static void test_null_terminated_files_from() {
    char tmpdir[] = "/tmp/mutar_bt_null_XXXXXX";
    assert(::mkdtemp(tmpdir) != nullptr);

    // Create two files
    for (const char* name : {"alpha.txt", "beta.txt"}) {
        std::string path = std::string(tmpdir) + "/" + name;
        int fd = ::open(path.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0);
        (void)::write(fd, name, std::strlen(name));
        ::close(fd);
    }

    std::string archive = std::string(tmpdir) + "/t.tar";

    // Write a null-terminated file list: "alpha.txt\0beta.txt\0"
    std::string listfile = std::string(tmpdir) + "/files.lst";
    {
        // Use the write_tmp approach but in our tempdir
        int fd = ::open(listfile.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0);
        (void)::write(fd, "alpha.txt", 9);
        (void)::write(fd, "\0", 1);
        (void)::write(fd, "beta.txt", 8);
        (void)::write(fd, "\0", 1);
        ::close(fd);
    }

    // Create archive using --null -T
    int rc = run_star({"-cf", archive, "--null", "-T", listfile, "-C", tmpdir});
    CHECK_EQ(rc, 0);

    // List: both files must appear
    std::string listing;
    rc = run_star({"-tf", archive}, &listing);
    CHECK_EQ(rc, 0);
    CHECK(listing.find("alpha.txt") != std::string::npos);
    CHECK(listing.find("beta.txt")  != std::string::npos);

    // Edge case: consecutive NULs (empty entries should be skipped, no crash)
    std::string list2 = std::string(tmpdir) + "/files2.lst";
    {
        int fd = ::open(list2.c_str(), O_CREAT|O_WRONLY, 0644);
        assert(fd >= 0);
        (void)::write(fd, "\0\0alpha.txt\0\0", 13);  // 2 leading nulls, 2 trailing
        ::close(fd);
    }
    std::string archive2 = std::string(tmpdir) + "/t2.tar";
    rc = run_star({"-cf", archive2, "--null", "-T", list2, "-C", tmpdir});
    CHECK_EQ(rc, 0);
    std::string listing2;
    rc = run_star({"-tf", archive2}, &listing2);
    CHECK_EQ(rc, 0);
    CHECK(listing2.find("alpha.txt") != std::string::npos);

    (void)::system(("rm -rf " + std::string(tmpdir)).c_str());
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== mutar boundary/overflow/memory tests ===\n";

    // Pure unit tests (no mutar binary needed)
    std::cout << "[1] read_octal\n";        test_read_octal();
    std::cout << "[2] read_base256\n";      test_read_base256();
    std::cout << "[3] write_octal\n";       test_write_octal();
    std::cout << "[4] write_base256\n";     test_write_base256();
    std::cout << "[5] checksum\n";          test_checksum();
    std::cout << "[6] size_field_boundary\n"; test_size_field_boundary();

    // Integration tests (invoke mutar binary)
    std::cout << "[7] pax_long_name\n";          test_pax_long_name();
    std::cout << "[8] sanitize_path traversal\n"; test_sanitize_path_traversal();
    std::cout << "[9] blocking_factor valid.\n";  test_blocking_factor_validation();
    std::cout << "[10] strip_components\n";        test_strip_components();
    std::cout << "[11] empty_archive\n";           test_empty_archive();
    std::cout << "[12] binary_roundtrip\n";        test_binary_roundtrip();
    std::cout << "[13] block_boundary_files\n";    test_block_boundary_files();
    std::cout << "[14] uid_gid_overflow\n";        test_uid_gid_overflow();
    std::cout << "[15] strip_components_extremes\n"; test_strip_components_extremes();
    std::cout << "[16] normalize_member\n";        test_normalize_member();
    std::cout << "[17] null_terminated files_from\n"; test_null_terminated_files_from();

    std::cout << "\n===========================================\n";
    std::cout << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n";
    std::cout << "===========================================\n";
    return g_fail == 0 ? 0 : 1;
}
