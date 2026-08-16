// mutar.hpp — µtar (mutar) C++23 GNU-tar-compatible archiver — header
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Block/header layout mirrors GNU tar's tar.h (src/tar.h in tar-1.35).
// Real code lives only in lib/, rmt/, scripts/, src/ — never in gnu/.
#pragma once

#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Optional feature headers — included only when the corresponding library was
// detected at cmake configure time. These conditional includes make the
// optional APIs available to mutar.cpp when the features are enabled.
#ifdef MUTAR_HAVE_XATTR
#  include <sys/xattr.h>
#endif

#ifdef MUTAR_HAVE_ACL
#  include <sys/acl.h>
#endif

namespace mutar {

// ── Block layout ──────────────────────────────────────────────────────────────
// 512 bytes per block; default blocking factor 20 → 10 240 bytes per record.

inline constexpr std::size_t BLOCKSIZE     = 512;
inline constexpr std::size_t DEFAULT_BLOCK = 20;   // 20 × 512 = 10 KiB record

// POSIX / ustar magic
inline constexpr std::string_view TMAGIC   = "ustar";  // + NUL = 6 bytes
inline constexpr std::string_view TVERSION = "00";

// GNU oldgnu magic occupies magic+version as one 8-byte field
inline constexpr std::string_view OLDGNU_MAGIC = "ustar  ";  // 7 chars + NUL

// Type flags (typeflag field)
inline constexpr char REGTYPE  = '0';
inline constexpr char AREGTYPE = '\0';
inline constexpr char LNKTYPE  = '1';
inline constexpr char SYMTYPE  = '2';
inline constexpr char CHRTYPE  = '3';
inline constexpr char BLKTYPE  = '4';
inline constexpr char DIRTYPE  = '5';
inline constexpr char FIFOTYPE = '6';
inline constexpr char CONTTYPE = '7';

// PAX extended headers
inline constexpr char XHDTYPE = 'x';   // per-file PAX header
inline constexpr char XGLTYPE = 'g';   // global PAX header

// GNU extensions
inline constexpr char GNUTYPE_DUMPDIR  = 'D';
inline constexpr char GNUTYPE_LONGLINK = 'K';
inline constexpr char GNUTYPE_LONGNAME = 'L';
inline constexpr char GNUTYPE_MULTIVOL = 'M';
inline constexpr char GNUTYPE_SPARSE   = 'S';
inline constexpr char GNUTYPE_VOLHDR   = 'V';

// Sparse descriptor: offset + numbytes (12 bytes each, octal ASCII)
struct [[nodiscard]] RawSparse {
    char offset[12];
    char numbytes[12];
};
static_assert(sizeof(RawSparse) == 24);

// POSIX ustar header (bytes 0–499 of a 512-byte block)
struct [[nodiscard]] PosixHeader {
    char name[100];      //   0
    char mode[8];        // 100
    char uid[8];         // 108
    char gid[8];         // 116
    char size[12];       // 124
    char mtime[12];      // 136
    char chksum[8];      // 148
    char typeflag;       // 156
    char linkname[100];  // 157
    char magic[6];       // 257
    char version[2];     // 263
    char uname[32];      // 265
    char gname[32];      // 297
    char devmajor[8];    // 329
    char devminor[8];    // 337
    char prefix[155];    // 345
    // 500-511: padding
};
static_assert(sizeof(PosixHeader) == 500);

// GNU oldgnu extension overlay at bytes 345–494
struct [[nodiscard]] OldgnuHeader {
    char unused_pad1[345];
    char atime[12];           // 345
    char ctime[12];           // 357
    char offset[12];          // 369
    char longnames[4];        // 381
    char unused_pad2;         // 385
    RawSparse sp[4];          // 386 (4 × 24 = 96 bytes)
    char isextended;          // 482
    char realsize[12];        // 483
    // 495–511: padding
};
static_assert(sizeof(OldgnuHeader) == 495);

// Sparse extension header (type 'S' continuation)
struct [[nodiscard]] SparseHeader {
    RawSparse sp[21];    //   0 (21 × 24 = 504 bytes)
    char isextended;     // 504
    // 505–511: padding
};
static_assert(sizeof(SparseHeader) == 505);

// The one 512-byte union block
union Block {
    char          buffer[BLOCKSIZE];
    PosixHeader   header;
    OldgnuHeader  oldgnu;
    SparseHeader  sparse;
};
static_assert(sizeof(Block) == BLOCKSIZE);

// ── Formats ───────────────────────────────────────────────────────────────────

enum class Format {
    Default,   // auto-detect on read; GNU on write
    V7,        // original Unix V7
    OldGNU,    // GNU tar ≤ 1.12
    USTAR,     // POSIX.1-1988
    GNU,       // GNU tar 1.13+ (OLDGNU + extensions)
    PAX,       // POSIX.1-2001 / pax
};

// ── Operations ────────────────────────────────────────────────────────────────

enum class Operation {
    None,
    Create,      // -c
    Extract,     // -x
    List,        // -t
    Append,      // -r
    Update,      // -u
    Delete,      // --delete
    Diff,        // -d / --compare
    Cat,         // -A / --catenate / --concatenate
    TestLabel,   // --test-label
};

// ── Compression ───────────────────────────────────────────────────────────────

enum class Compress {
    None,
    Auto,   // detect from filename / magic
    Gzip,   // -z / --gzip
    Bzip2,  // -j / --bzip2
    Xz,     // -J / --xz
    Zstd,   // --zstd
    Lzma,   // --lzma
    Lzip,   // --lzip
    Lzop,       // --lzop
    CompressZ,  // -Z / --compress (legacy compress program)
    Custom,     // --use-compress-program
};

// ── Config: parsed CLI options ────────────────────────────────────────────────

/// Parsed --pax-option rules applied when writing PAX extended headers.
/// MVP: only `delete=KEYWORD` (suppress emission of that keyword).
struct PaxOptionRules {
    std::set<std::string> delete_keywords;  // delete=keyword
};

struct Config {
    Operation   op            = Operation::None;
    Format      fmt           = Format::Default;
    Compress    compress      = Compress::Auto;
    std::string compress_prog;           // --use-compress-program
    std::string archive_file;            // -f
    std::vector<std::string> files;      // positional arguments
    std::string label;                   // -V / --label
    std::string listed_incremental;      // -g
    std::string newer_than;              // -N / --newer
    std::string mtime;                   // --mtime
    std::string owner;                   // --owner
    std::string group;                   // --group
    std::string owner_map_file;          // --owner-map
    std::string group_map_file;          // --group-map
    std::string to_command;              // --to-command
    std::string rsh_command;             // --rsh-command
    std::string rmt_command;             // --rmt-command
    std::string info_script;             // -F
    std::string volno_file;              // --volno-file
    std::string directory;               // -C / --directory
    std::string one_top_level;           // --one-top-level[=DIR]
    std::vector<std::string> exclude_patterns;
    std::vector<std::string> files_from; // -T / --files-from
    std::string transform_expr;          // --transform / --xform
    std::string sort_order;              // --sort
    std::string strip_components_str;    // --strip-components
    std::string occurrence_str;          // --occurrence
    std::string tape_length_str;         // -L
    std::string sparse_version;          // --sparse-version
    std::string hole_detection;          // --hole-detection
    std::string mode_str;                // --mode
    std::string record_size_str;         // --record-size
    std::vector<std::string> pax_options; // raw --pax-option=... (repeatable)
    PaxOptionRules pax_option_rules;     // parsed delete=KEYWORD set

    // Booleans — defaults match GNU tar behaviour
    bool verbose             = false;
    bool very_verbose        = false;   // -vv
    bool dereference         = false;   // -h
    bool hard_dereference    = false;   // -H (--hard-dereference)
    bool absolute_names      = false;   // -P
    bool one_file_system     = false;   // -l / --one-file-system
    bool ignore_zeros        = false;   // -i
    bool read_full_records   = false;   // -B
    bool sparse              = false;   // -S
    bool multi_volume        = false;   // -M
    bool interactive         = false;   // -w / --interactive
    bool verify              = false;   // -W
    bool same_owner          = false;   // --same-owner
    bool no_same_owner       = false;   // --no-same-owner
    bool same_permissions    = false;   // -p / --same-permissions
    bool no_same_permissions = false;   // --no-same-permissions
    bool numeric_owner       = false;   // --numeric-owner
    bool keep_old_files      = false;   // -k
    bool skip_old_files      = false;   // --skip-old-files
    bool keep_newer_files    = false;   // --keep-newer-files
    bool overwrite           = false;   // --overwrite
    bool unlink_first        = false;   // -U
    bool recursive_unlink    = false;   // --recursive-unlink
    bool no_overwrite_dir    = false;   // --no-overwrite-dir
    bool touch               = false;   // -m / --touch
    bool to_stdout           = false;   // -O
    bool remove_files        = false;   // --remove-files
    bool incremental         = false;   // -G (old incremental)
    bool ignore_failed_read  = false;   // --ignore-failed-read
    bool check_links         = false;   // --check-links
    bool delay_dir_restore   = false;   // --delay-directory-restore
    bool block_number        = false;   // -R
    bool totals              = false;   // --totals
    bool utc                 = false;   // --utc
    bool full_time           = false;   // --full-time
    bool show_omitted_dirs   = false;   // --show-omitted-dirs
    bool show_transformed    = false;   // --show-transformed-names
    bool null_terminated     = false;   // --null / -0
    bool seek                = true;    // -n / --seek (default auto)
    bool force_local         = false;   // --force-local
    bool xattrs              = false;   // --xattrs (only available when MUTAR_HAVE_XATTR)
#ifdef MUTAR_HAVE_XATTR
    std::vector<std::string> xattrs_include; // --xattrs-include=MASK
    std::vector<std::string> xattrs_exclude; // --xattrs-exclude=MASK
#endif
    bool acls                = false;   // --acls   (only available when MUTAR_HAVE_ACL)
    bool selinux             = false;   // --selinux accepted as no-op (unsupported)
    bool posix               = false;   // --posix (=pax format)
    bool old_archive         = false;   // --old-archive (=v7)
    bool utc_time            = false;   // --utc
    bool restrict_opt        = false;   // --restrict
    bool atime_preserve      = false;   // --atime-preserve

    bool   no_recursion        = false;  // --no-recursion
    bool   no_auto_compress    = false;  // --no-auto-compress
    bool   verbatim_files_from = false;  // --verbatim-files-from
    bool   exclude_backups     = false;  // --exclude-backups
    bool   exclude_vcs         = false;  // --exclude-vcs
    bool   compat_o            = false;  // -o compat
    int    checkpoint          = 0;      // --checkpoint[=N]
    std::string checkpoint_action;       // --checkpoint-action
    std::string index_file;              // --index-file (GNU: verbose routing)
    std::string mutar_index;             // --mutar-index=PATH (sidecar index R/W)
    bool        write_index = false;     // --write-index (create sidecar *.mutaridx)
    bool        seekable    = false;     // --seekable (index + seek-friendly compress)
    std::string starting_file;           // -K / --starting-file
    std::vector<std::string> exclude_from; // -X / --exclude-from (filenames)
    bool   anchored            = false;  // --anchored/--no-anchored (GNU tar default: --no-anchored)
    bool   ignore_case         = false;  // --ignore-case
    bool wildcards             = true;   // --wildcards / --no-wildcards
    bool wildcards_match_slash = true;   // --wildcards-match-slash / --no-wildcards-match-slash
    bool unquote               = true;   // --unquote / --no-unquote
    std::string quoting_style;           // --quoting-style
    bool preserve_order        = false;  // -s / --preserve-order / --same-order
    bool overwrite_dir         = true;   // --overwrite-dir (default: true)
    bool exclude_vcs_ignores   = false;  // --exclude-vcs-ignores
    int  level                 = -1;     // --level=NUMBER (-1 = unset)
    bool warn_all              = false;  // --warning=all
    bool warn_none             = false;  // --warning=none
    std::set<std::string> warnings_enabled;   // --warning=KEYWORD
    std::set<std::string> warnings_disabled;  // --warning=no-KEYWORD

    int  blocking_factor     = DEFAULT_BLOCK;
    int  strip_components    = 0;
    int  occurrence          = 0;
    long tape_length         = 0;
    unsigned sparse_major    = 1;
    unsigned sparse_minor    = 0;

    // Extended features
    bool clamp_mtime          = false;   // --clamp-mtime
    bool keep_dir_symlink     = false;   // --keep-directory-symlink
    bool exclude_caches       = false;   // --exclude-caches
    bool exclude_caches_all   = false;   // --exclude-caches-all
    bool exclude_caches_under = false;   // --exclude-caches-under
    bool backup               = false;   // --backup
    bool no_delay_dir_restore = false;   // --no-delay-directory-restore
    std::vector<std::string> exclude_tags;        // --exclude-tag=FILE
    std::vector<std::string> exclude_tags_all;    // --exclude-tag-all=FILE
    std::vector<std::string> exclude_tags_under;  // --exclude-tag-under=FILE
    std::string backup_control;                   // --backup[=CONTROL]
    std::string backup_suffix = "~";              // --suffix=STRING
};

// ── Sidecar archive index (MUTAR.INDEX.V1) ─────────────────────────────────────
// Optional; zero cost when unused. Offsets are byte positions in the
// *uncompressed* tar stream (seek only works on seekable uncompressed archives).

struct IndexEntry {
    std::string  name;
    std::uint64_t offset = 0;   // byte offset of first header block for this member
    std::int64_t size    = 0;   // logical / archived size from header
    char         typeflag = REGTYPE;
    std::int64_t mtime   = 0;
    unsigned     mode    = 0;
    unsigned     uid     = 0;
    unsigned     gid     = 0;
};

// ── In-memory tar entry ───────────────────────────────────────────────────────

struct SparseMap {
    std::int64_t offset;
    std::int64_t numbytes;
};

struct Entry {
    // From header
    std::string  name;
    std::string  linkname;
    std::string  uname;
    std::string  gname;
    char         typeflag   = REGTYPE;
    std::int64_t size       = 0;   // logical size
    std::int64_t asize      = 0;   // archived size (sparse: may differ)
    std::int64_t mtime      = 0;   // seconds since epoch
    long         mtime_nsec = 0;
    std::int64_t atime      = 0;
    std::int64_t ctime      = 0;
    unsigned int mode       = 0644;
    unsigned int uid        = 0;
    unsigned int gid        = 0;
    unsigned int devmajor   = 0;
    unsigned int devminor   = 0;
    Format       fmt        = Format::GNU;

    // Sparse support
    bool is_sparse               = false;
    std::int64_t real_size       = 0;
    std::vector<SparseMap> sparse_map;

    // PAX extended attributes
    std::map<std::string, std::string> pax_attrs;

    // xattrs, ACLs (stored as PAX attrs)
    // Block offset in archive (for diagnostics)
    std::int64_t block_offset = 0;
};

// ── Error type ────────────────────────────────────────────────────────────────

struct Error {
    std::string message;
    int         code = 0;   // errno or custom code
};

template<typename T>
using Result = std::expected<T, Error>;

inline Error sys_error(std::string_view ctx) {
    return Error{ std::format("{}: {}", ctx, std::strerror(errno)), errno };
}

inline Error msg_error(std::string msg) {
    return Error{ std::move(msg), 0 };
}

// ── Octal I/O helpers (used throughout) ───────────────────────────────────────

// Read NUL-or-space-terminated octal from fixed-width field.
inline std::uint64_t read_octal(std::string_view field) noexcept {
    std::uint64_t v = 0;
    for (char c : field) {
        if (c == '\0' || c == ' ') break;
        if (c >= '0' && c <= '7') v = (v << 3) | static_cast<unsigned>(c - '0');
        else break;
    }
    return v;
}

// GNU/PAX base-256 encoding: first byte has 0x80 or 0xFF set.
inline bool is_base256(std::string_view field) noexcept {
    return !field.empty() && (static_cast<unsigned char>(field[0]) & 0x80) != 0;
}

inline std::int64_t read_base256(std::string_view field) noexcept {
    if (field.empty()) return 0;
    // The first byte carries the 0x80 marker and the 0x40 sign bit.
    // Clear the marker bit before accumulating so it isn't part of the value.
    unsigned char first = static_cast<unsigned char>(field[0]) & 0x7f;
    bool negative = (first & 0x40) != 0;
    std::int64_t v = negative ? -1 : 0;
    v = (v << 8) | static_cast<std::int64_t>(first);
    for (std::size_t i = 1; i < field.size(); ++i)
        v = (v << 8) | static_cast<unsigned char>(field[i]);
    return v;
}

inline std::uint64_t read_number(std::string_view field) noexcept {
    if (is_base256(field)) return static_cast<std::uint64_t>(read_base256(field));
    return read_octal(field);
}

// Write octal into a fixed-width field (width includes NUL if space allows).
// IMPORTANT: uses while-loop (not do-while) to avoid decrementing width past
// zero — when width reaches 0 the old do-while would wrap std::size_t to
// SIZE_MAX and write one byte before the start of the buffer (stack OOB).
inline void write_octal(char* field, std::size_t width, std::uint64_t v) noexcept {
    if (width == 0) return;
    field[--width] = '\0';
    while (width > 0 && v) {
        field[--width] = static_cast<char>('0' + (v & 7));
        v >>= 3;
    }
    while (width > 0) field[--width] = '0';
}

// Write base-256 when value overflows octal field
inline void write_base256(char* field, std::size_t width, std::int64_t v) noexcept {
    for (std::size_t i = width; i-- > 0;) {
        field[i] = static_cast<char>(v & 0xff);
        v >>= 8;
    }
    field[0] = static_cast<char>(static_cast<unsigned char>(field[0]) | 0x80);
}

// Compute tar checksum (sum of all bytes treating chksum as spaces)
inline unsigned int block_checksum(const Block& blk) noexcept {
    unsigned int sum = 0;
    for (std::size_t i = 0; i < BLOCKSIZE; ++i) {
        // Treat the 8-byte chksum field (offset 148..155) as spaces
        if (i >= 148 && i < 156) sum += ' ';
        else                     sum += static_cast<unsigned char>(blk.buffer[i]);
    }
    return sum;
}

inline void write_checksum(Block& blk) noexcept {
    unsigned int sum = block_checksum(blk);
    // Store as 6 octal digits + NUL + space (traditional format)
    std::snprintf(blk.header.chksum, 7, "%06o", sum);
    blk.header.chksum[6] = '\0';
    blk.header.chksum[7] = ' ';
}

inline bool valid_checksum(const Block& blk) noexcept {
    unsigned int stored = static_cast<unsigned int>(read_octal(
        std::string_view(blk.header.chksum, 8)));
    return stored == block_checksum(blk);
}

// ── Format detection ──────────────────────────────────────────────────────────

inline Format detect_format(const Block& blk) noexcept {
    std::string_view magic(blk.header.magic, 8); // overlaps version too
    if (magic.starts_with("ustar  "))  return Format::OldGNU;
    if (magic.starts_with("ustar\000""00")) return Format::USTAR;
    if (magic.starts_with("ustar"))    return Format::USTAR; // may be PAX
    return Format::V7;
}

} // namespace mutar
