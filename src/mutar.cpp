// mutar.cpp — µtar (mutar) C++23 GNU-tar-compatible archiver — implementation
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Implements: create (-c), extract (-x), list (-t), append (-r),
//             update (-u), delete (--delete), diff (-d), cat (-A),
//             test-label (--test-label).
// Formats:    v7, oldgnu, ustar, gnu, pax/posix.
// Compression: gzip, bzip2, xz, zstd, lzma, lzip, lzop (via child process).
//
// Source reference: tar-1.35 src/, lib/, rmt/, scripts/

#include "mutar.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <strings.h>
#include <unistd.h>
#include <utime.h>

#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

// ── Compatibility: print helpers for GCC < 14 ────────────────────────────────
// GCC 13 has <format> but not <print>. We define helpers in mutar_compat to
// avoid injecting into namespace std (which is undefined behavior).
namespace mutar_compat {
template<typename... Args>
void print(std::FILE* f, std::format_string<Args...> fmt, Args&&... args) {
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), f);
}
template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), stdout);
}
} // namespace mutar_compat

// Bring into mutar namespace via using so all existing call-sites work unchanged.
namespace mutar {
using mutar_compat::print;
}

namespace mutar {

// Index file for --index-file (verbose output redirection)
static FILE* g_index_fp = nullptr;

// ── Compression helpers ───────────────────────────────────────────────────────

static const char* compress_prog_for(Compress c, std::string_view custom) {
    switch (c) {
    case Compress::Gzip:   return "gzip";
    case Compress::Bzip2:  return "bzip2";
    case Compress::Xz:     return "xz";
    case Compress::Zstd:   return "zstd";
    case Compress::Lzma:   return "lzma";
    case Compress::Lzip:   return "lzip";
    case Compress::Lzop:      return "lzop";
    case Compress::CompressZ: return "compress";
    case Compress::Custom:    return custom.data();
    default:               return nullptr;
    }
}

// Detect compression from filename extension
static Compress detect_compress(std::string_view filename) {
    auto ends = [&](std::string_view suf) {
        return filename.size() >= suf.size() &&
               filename.substr(filename.size() - suf.size()) == suf;
    };
    if (ends(".tar.gz")  || ends(".tgz"))   return Compress::Gzip;
    if (ends(".tar.bz2") || ends(".tbz2") || ends(".tbz")) return Compress::Bzip2;
    if (ends(".tar.xz")  || ends(".txz"))   return Compress::Xz;
    if (ends(".tar.zst") || ends(".tzst"))  return Compress::Zstd;
    if (ends(".tar.lzma"))                  return Compress::Lzma;
    if (ends(".tar.lz"))                    return Compress::Lzip;
    if (ends(".tar.lzo"))                   return Compress::Lzop;
    if (ends(".tar.Z")   || ends(".taz"))   return Compress::CompressZ;
    return Compress::None;
}

// Detect compression from magic bytes (first 6 bytes of file)
static Compress detect_compress_magic(int fd) {
    unsigned char magic[6]{};
    auto n = ::pread(fd, magic, sizeof(magic), 0);
    if (n < 2) return Compress::None;
    // gzip: 1f 8b
    if (magic[0] == 0x1f && magic[1] == 0x8b) return Compress::Gzip;
    // bzip2: BZ
    if (magic[0] == 'B' && magic[1] == 'Z')   return Compress::Bzip2;
    // xz: fd 37 7a 58 5a 00
    if (n >= 6 && magic[0] == 0xfd && magic[1] == '7' &&
        magic[2] == 'z' && magic[3] == 'X' && magic[4] == 'Z') return Compress::Xz;
    // zstd: fd 2f b5 28
    if (n >= 4 && magic[0] == 0xfd && magic[1] == 0x2f &&
        magic[2] == 0xb5 && magic[3] == 0x28) return Compress::Zstd;
    // lzip: LZIP
    if (n >= 4 && magic[0] == 'L' && magic[1] == 'Z' &&
        magic[2] == 'I' && magic[3] == 'P') return Compress::Lzip;
    // lzma: classic .lzma magic (5d 00 00)
    if (n >= 3 && magic[0] == 0x5d && magic[1] == 0x00 && magic[2] == 0x00)
        return Compress::Lzma;
    // lzop: 89 4c 5a 4f 00 0d
    if (n >= 4 && magic[0] == 0x89 && magic[1] == 0x4c &&
        magic[2] == 0x5a && magic[3] == 0x4f) return Compress::Lzop;
    // compress (.Z): 1f 9d
    if (magic[0] == 0x1f && magic[1] == 0x9d) return Compress::CompressZ;
    return Compress::None;
}

// ── ArchiveStream: wraps a raw fd, optionally piped through decompressor ──────
// Forward-declare remote helpers (defined after the ArchiveStream section)
static bool is_remote_archive(const std::string& path, bool force_local);
static int  open_remote_stream(const std::string& archive_file,
                                const std::string& rsh_cmd,
                                const std::string& rmt_cmd,
                                bool for_write);

class ArchiveStream {
public:
    // Read: open archive for reading (decompress if needed)
    static Result<ArchiveStream> open_read(const Config& cfg) {
        ArchiveStream s;
        const bool use_stdin = cfg.archive_file.empty() || cfg.archive_file == "-";

        if (use_stdin) {
            s.fd_ = STDIN_FILENO;
        } else if (is_remote_archive(cfg.archive_file, cfg.force_local)) {
            // Remote archive via rmt protocol
            int fd = open_remote_stream(cfg.archive_file, cfg.rsh_command, cfg.rmt_command, false);
            if (fd < 0) return std::unexpected(msg_error(
                std::format("cannot open remote archive '{}' for reading", cfg.archive_file)));
            s.fd_ = fd;
            s.owns_fd_ = true;
            s.child_pid_ = -2; // sentinel: remote bridge (no waitpid needed here)
        } else {
            s.fd_ = ::open(cfg.archive_file.c_str(), O_RDONLY);
            if (s.fd_ < 0) return std::unexpected(sys_error(cfg.archive_file));
            s.owns_fd_ = true;
        }

        // Determine compression
        Compress comp = cfg.compress;
        if (comp == Compress::Auto) {
            comp = detect_compress(cfg.archive_file);
            if (comp == Compress::None && s.owns_fd_)
                comp = detect_compress_magic(s.fd_);
        } else if (comp != Compress::None && s.owns_fd_) {
            // When a specific compressor is requested, still verify the magic
            // bytes of the actual file and prefer them on a mismatch.  This
            // handles the common case where modern tools (e.g. system tar
            // --lzma) produce XZ-format data even though the filename / flag
            // says "lzma".
            Compress magic_comp = detect_compress_magic(s.fd_);
            if (magic_comp != Compress::None && magic_comp != comp)
                comp = magic_comp;
        }

        if (comp != Compress::None && comp != Compress::Auto) {
            const char* prog = compress_prog_for(comp, cfg.compress_prog);
            if (!prog || !prog[0])
                return std::unexpected(msg_error("unknown compression program"));

            // Pipe: fd_  → prog -d → child_pipe[read-end]
            int pipefd[2];
            if (::pipe(pipefd) < 0) return std::unexpected(sys_error("pipe"));

            pid_t pid = ::fork();
            if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]);
                           return std::unexpected(sys_error("fork")); }
            if (pid == 0) {
                // Child: prog -d < s.fd_ > pipefd[1]
                ::dup2(s.fd_, STDIN_FILENO);
                ::dup2(pipefd[1], STDOUT_FILENO);
                ::close(pipefd[0]);
                if (s.owns_fd_) ::close(s.fd_);
                ::execlp(prog, prog, "-d", nullptr);
                ::_exit(127);
            }
            ::close(pipefd[1]);
            if (s.owns_fd_) ::close(s.fd_);
            s.fd_ = pipefd[0];
            s.owns_fd_ = true;
            s.child_pid_ = pid;
        }
        return s;
    }

    // Write: open archive for writing (compress if needed)
    static Result<ArchiveStream> open_write(const Config& cfg) {
        ArchiveStream s;
        const bool use_stdout = cfg.archive_file.empty() || cfg.archive_file == "-";

        // Determine compression before opening
        Compress comp = cfg.compress;
        if (comp == Compress::Auto) {
            if (cfg.no_auto_compress)
                comp = Compress::None;
            else
                comp = detect_compress(cfg.archive_file);
        }

        if (use_stdout) {
            s.fd_ = STDOUT_FILENO;
        } else if (is_remote_archive(cfg.archive_file, cfg.force_local)) {
            // Remote archive via rmt protocol
            int fd = open_remote_stream(cfg.archive_file, cfg.rsh_command, cfg.rmt_command, true);
            if (fd < 0) return std::unexpected(msg_error(
                std::format("cannot open remote archive '{}' for writing", cfg.archive_file)));
            s.fd_ = fd;
            s.owns_fd_ = true;
            s.child_pid_ = -2; // sentinel: remote bridge
        } else {
            int flags = O_WRONLY | O_CREAT | O_TRUNC;
            s.fd_ = ::open(cfg.archive_file.c_str(), flags, 0666);
            if (s.fd_ < 0) return std::unexpected(sys_error(cfg.archive_file));
            s.owns_fd_ = true;
        }

        if (comp != Compress::None && comp != Compress::Auto) {
            const char* prog = compress_prog_for(comp, cfg.compress_prog);
            if (!prog || !prog[0])
                return std::unexpected(msg_error("unknown compression program"));

            // --seekable: prefer multi-block xz / chunked zstd; warn for solid gzip/bzip2.
            if (cfg.seekable) {
                if (comp == Compress::Gzip || comp == Compress::Bzip2 ||
                    comp == Compress::CompressZ || comp == Compress::Lzop) {
                    print(stderr,
                          "mutar: warning: --seekable with {} still needs full "
                          "decompress for random access (solid stream)\n",
                          prog);
                }
            }

            int pipefd[2];
            if (::pipe(pipefd) < 0) return std::unexpected(sys_error("pipe"));

            // For gzip writing to a seekable file, dup the output fd now so we
            // can patch the mtime field (bytes 4-7) in the gzip header after the
            // compressor finishes.  This makes `file foo.tar.gz` report a proper
            // "last modified" timestamp matching the archive creation time.
            if (comp == Compress::Gzip && !use_stdout) {
                int dup_fd = ::dup(s.fd_);
                if (dup_fd >= 0) {
                    // Only use the dup'd fd if the output is seekable
                    if (::lseek(dup_fd, 0, SEEK_CUR) == -1 && errno == ESPIPE) {
                        ::close(dup_fd);
                    } else {
                        s.patch_fd_    = dup_fd;
                        s.patch_mtime_ = std::time(nullptr);
                    }
                }
            }

            pid_t pid = ::fork();
            if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]);
                           return std::unexpected(sys_error("fork")); }
            if (pid == 0) {
                // Child: prog [seekable-args] < pipefd[0] > s.fd_
                ::dup2(pipefd[0], STDIN_FILENO);
                ::dup2(s.fd_, STDOUT_FILENO);
                ::close(pipefd[1]);
                if (s.owns_fd_) ::close(s.fd_);
                if (cfg.seekable && comp == Compress::Xz) {
                    // Multi-block stream with index (xz -l shows Blocks > 1).
                    ::execlp(prog, prog, "--block-size=1MiB", nullptr);
                } else if (cfg.seekable && comp == Compress::Zstd) {
                    // Independent multi-thread chunks (~1 MiB) for better locality.
                    ::execlp(prog, prog, "-T0", "-B1M", nullptr);
                } else {
                    ::execlp(prog, prog, nullptr);
                }
                ::_exit(127);
            }
            ::close(pipefd[0]);
            if (s.owns_fd_) ::close(s.fd_);
            s.fd_ = pipefd[1];
            s.owns_fd_ = true;
            s.child_pid_ = pid;
        }
        return s;
    }

    /// Drain a (possibly decompressed) stream into a temp file and return a
    /// seekable ArchiveStream on that file. Used so index-based seek works for
    /// compressed archives (full decompress once, then random access).
    static Result<ArchiveStream> materialize_seekable(ArchiveStream& src,
                                                      std::string& temp_path_out) {
        char tmpl[] = "/tmp/mutar_seek_XXXXXX";
        int tfd = ::mkstemp(tmpl);
        if (tfd < 0)
            return std::unexpected(sys_error("mkstemp for seek materialize"));
        temp_path_out = tmpl;

        char buf[1 << 16];
        for (;;) {
            ssize_t n = ::read(src.fd(), buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                int e = errno;
                ::close(tfd);
                ::unlink(tmpl);
                return std::unexpected(Error{
                    std::format("materialize read: {}", std::strerror(e)), e});
            }
            if (n == 0) break;
            std::size_t off = 0;
            while (off < static_cast<std::size_t>(n)) {
                ssize_t w = ::write(tfd, buf + off, static_cast<std::size_t>(n) - off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    int e = errno;
                    ::close(tfd);
                    ::unlink(tmpl);
                    return std::unexpected(Error{
                        std::format("materialize write: {}", std::strerror(e)), e});
                }
                off += static_cast<std::size_t>(w);
            }
        }
        // Close source (waits for decompressor child if any)
        src.close();
        if (::lseek(tfd, 0, SEEK_SET) < 0) {
            int e = errno;
            ::close(tfd);
            ::unlink(tmpl);
            return std::unexpected(Error{
                std::format("materialize lseek: {}", std::strerror(e)), e});
        }
        return adopt_fd(tfd);
    }

    // Open for read-write (append/update/delete — needs seekable archive)
    static Result<ArchiveStream> open_rdwr(const Config& cfg) {
        ArchiveStream s;
        s.fd_ = ::open(cfg.archive_file.c_str(), O_RDWR);
        if (s.fd_ < 0) return std::unexpected(sys_error(cfg.archive_file));
        s.owns_fd_ = true;
        // No compression for in-place operations
        return s;
    }

    ArchiveStream() = default;
    ArchiveStream(const ArchiveStream&) = delete;
    ArchiveStream& operator=(const ArchiveStream&) = delete;
    ArchiveStream(ArchiveStream&& o) noexcept
        : fd_(o.fd_), owns_fd_(o.owns_fd_), child_pid_(o.child_pid_),
          patch_fd_(o.patch_fd_), patch_mtime_(o.patch_mtime_) {
        o.fd_ = -1; o.owns_fd_ = false; o.child_pid_ = -1;
        o.patch_fd_ = -1; o.patch_mtime_ = 0;
    }
    ArchiveStream& operator=(ArchiveStream&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_; owns_fd_ = o.owns_fd_; child_pid_ = o.child_pid_;
            patch_fd_ = o.patch_fd_; patch_mtime_ = o.patch_mtime_;
            o.fd_ = -1; o.owns_fd_ = false; o.child_pid_ = -1;
            o.patch_fd_ = -1; o.patch_mtime_ = 0;
        }
        return *this;
    }

    /// Adopt an already-open fd (e.g. materialised temp archive).
    static ArchiveStream adopt_fd(int fd) {
        ArchiveStream s;
        s.fd_ = fd;
        s.owns_fd_ = true;
        return s;
    }

    ~ArchiveStream() { close(); }

    void close() {
        if (owns_fd_ && fd_ >= 0) { ::close(fd_); fd_ = -1; owns_fd_ = false; }
        if (child_pid_ > 0) {
            int st = 0;
            ::waitpid(child_pid_, &st, 0);
            child_pid_ = -1;
            // After compressor finishes, patch gzip mtime header (bytes 4-7, LE uint32).
            // Only patch when the compressor exited successfully to avoid writing
            // into a corrupt/partial stream.
            if (patch_fd_ >= 0) {
                bool compressor_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                if (compressor_ok && patch_mtime_ != 0) {
                    auto t = static_cast<std::uint32_t>(patch_mtime_);
                    unsigned char mb[4] = {
                        static_cast<unsigned char>( t        & 0xff),
                        static_cast<unsigned char>((t >>  8) & 0xff),
                        static_cast<unsigned char>((t >> 16) & 0xff),
                        static_cast<unsigned char>((t >> 24) & 0xff)
                    };
                    ssize_t written = ::pwrite(patch_fd_, mb, 4, 4);
                    (void)written; // best-effort; non-fatal if it fails
                }
                ::close(patch_fd_);
                patch_fd_ = -1;
            }
        }
    }

    int fd() const noexcept { return fd_; }

    // Full-buffer reads (for block-aligned I/O)
    ssize_t read_buf(void* buf, std::size_t n) {
        auto* p = static_cast<char*>(buf);
        std::size_t total = 0;
        while (total < n) {
            ssize_t r = ::read(fd_, p + total, n - total);
            if (r < 0) { if (errno == EINTR) continue; return -1; }
            if (r == 0) break;
            total += static_cast<std::size_t>(r);
        }
        return static_cast<ssize_t>(total);
    }

    bool write_buf(const void* buf, std::size_t n) {
        const auto* p = static_cast<const char*>(buf);
        std::size_t total = 0;
        while (total < n) {
            ssize_t w = ::write(fd_, p + total, n - total);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            total += static_cast<std::size_t>(w);
        }
        return true;
    }

    off_t seek(off_t pos, int whence = SEEK_SET) {
        return ::lseek(fd_, pos, whence);
    }

    /// True when the underlying fd supports lseek and is not a compression pipe.
    [[nodiscard]] bool is_seekable() const noexcept {
        if (fd_ < 0) return false;
        if (child_pid_ > 0) return false; // decompressor/compressor pipe
        errno = 0;
        off_t cur = ::lseek(fd_, 0, SEEK_CUR);
        return cur != static_cast<off_t>(-1);
    }

    bool truncate_at(off_t pos) {
        return ::ftruncate(fd_, pos) == 0;
    }

private:
    int   fd_        = -1;
    bool  owns_fd_   = false;
    pid_t child_pid_ = -1;
    int   patch_fd_  = -1;       // dup of output fd for gzip mtime patching
    time_t patch_mtime_ = 0;     // archive creation time to embed in gzip header
};

// ── Block buffer (blocking-factor–aligned I/O) ───────────────────────────────

class BlockBuffer {
public:
    explicit BlockBuffer(int blocking = DEFAULT_BLOCK) {
        // Validate before any use — a zero or negative blocking factor would
        // cause record_size_ to be 0 or wrap, leading to UB in buf_.resize().
        if (blocking < 1) {
            std::fprintf(stderr,
                         "mutar: invalid blocking factor %d (must be >= 1)\n",
                         blocking);
            std::exit(EXIT_FAILURE);
        }
        blocking_    = blocking;
        record_size_ = static_cast<std::size_t>(blocking) * BLOCKSIZE;
        buf_.resize(record_size_);
    }

    // Read one block; returns false on clean EOF.
    // Guards against short fills: only returns a block when at least BLOCKSIZE
    // bytes are available, so stale bytes never leak into a partial last block.
    bool read_block(ArchiveStream& s, Block& blk) {
        if (pos_ >= used_) {
            if (!fill(s)) { eof_ = true; return false; }
        }
        // Ensure a full block is available; treat sub-block tail as EOF.
        if (pos_ + BLOCKSIZE > used_) { eof_ = true; return false; }
        std::memcpy(blk.buffer, buf_.data() + pos_, BLOCKSIZE);
        pos_ += BLOCKSIZE;
        block_no_++;
        return true;
    }

    void write_block(ArchiveStream& s, const Block& blk) {
        if (pos_ >= record_size_) flush(s);
        std::memcpy(buf_.data() + pos_, blk.buffer, BLOCKSIZE);
        pos_ += BLOCKSIZE;
        block_no_++;
    }

    void flush(ArchiveStream& s) {
        if (pos_ == 0) return;
        // Pad record to full record size
        if (pos_ < record_size_)
            std::memset(buf_.data() + pos_, 0, record_size_ - pos_);
        if (!s.write_buf(buf_.data(), record_size_)) {
            std::perror("mutar: write");
            std::exit(EXIT_FAILURE);
        }
        pos_ = 0;
    }

    std::int64_t block_no() const noexcept { return block_no_; }
    bool         eof()      const noexcept { return eof_; }

    /// Reset buffer state after an lseek on the underlying stream.
    /// @param absolute_block  next block number that will be read (0-based).
    void reset_at_block(std::int64_t absolute_block) noexcept {
        pos_      = 0;
        used_     = 0;
        eof_      = false;
        block_no_ = absolute_block;
    }

private:
    bool fill(ArchiveStream& s) {
        ssize_t n = s.read_buf(buf_.data(), record_size_);
        if (n <= 0) { used_ = 0; return false; }
        used_ = static_cast<std::size_t>(n);
        pos_  = 0;
        return true;
    }

    int              blocking_;
    std::size_t      record_size_;
    std::vector<char> buf_;
    std::size_t      pos_  = 0;
    std::size_t      used_ = 0;
    std::int64_t     block_no_ = 0;
    bool             eof_  = false;
};

// ── Sidecar index (MUTAR.INDEX.V1) ─────────────────────────────────────────────

/// Default sidecar path: archive path + ".mutaridx".
static std::string default_mutaridx_path(const std::string& archive) {
    if (archive.empty() || archive == "-") return {};
    return archive + ".mutaridx";
}

/// Resolve index path for read/write from Config.
static std::string resolve_mutaridx_path(const Config& cfg, bool for_write) {
    if (!cfg.mutar_index.empty()) return cfg.mutar_index;
    if (for_write && cfg.write_index) return default_mutaridx_path(cfg.archive_file);
    if (!for_write) return default_mutaridx_path(cfg.archive_file);
    return {};
}

class ArchiveIndex {
public:
    Result<void> load(const std::filesystem::path& path) {
        entries_.clear();
        by_name_.clear();
        std::ifstream in(path);
        if (!in) return std::unexpected(sys_error(path.string()));
        std::string line;
        if (!std::getline(in, line))
            return std::unexpected(msg_error("empty index file"));
        if (line != "MUTAR.INDEX.V1")
            return std::unexpected(msg_error(
                std::format("unsupported index magic: {}", line)));
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            // Wire format (TAB-separated, name last):
            //   offset\tsize\ttypeflag\tmtime\tmode\tuid\tgid\tname
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (int i = 0; i < 7; ++i) {
                auto tab = line.find('\t', start);
                if (tab == std::string::npos) { fields.clear(); break; }
                fields.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }
            if (fields.size() != 7) continue;
            IndexEntry e;
            try {
                e.offset   = static_cast<std::uint64_t>(std::stoull(fields[0]));
                e.size     = static_cast<std::int64_t>(std::stoll(fields[1]));
                e.typeflag = fields[2].empty() ? REGTYPE : fields[2][0];
                e.mtime    = static_cast<std::int64_t>(std::stoll(fields[3]));
                e.mode     = static_cast<unsigned>(std::stoul(fields[4]));
                e.uid      = static_cast<unsigned>(std::stoul(fields[5]));
                e.gid      = static_cast<unsigned>(std::stoul(fields[6]));
            } catch (...) {
                continue;
            }
            e.name = line.substr(start);
            if (e.name.empty()) continue;
            by_name_[e.name] = entries_.size();
            entries_.push_back(std::move(e));
        }
        return {};
    }

    Result<void> save(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return std::unexpected(sys_error(path.string()));
        out << "MUTAR.INDEX.V1\n";
        for (const auto& e : entries_) {
            // typeflag as single char; name last
            char tf = e.typeflag ? e.typeflag : REGTYPE;
            if (tf == AREGTYPE) tf = REGTYPE; // printable
            out << e.offset << '\t' << e.size << '\t' << tf << '\t'
                << e.mtime << '\t' << e.mode << '\t' << e.uid << '\t' << e.gid
                << '\t' << e.name << '\n';
        }
        if (!out) return std::unexpected(msg_error("failed writing index"));
        return {};
    }

    void add(IndexEntry e) {
        by_name_[e.name] = entries_.size();
        entries_.push_back(std::move(e));
    }

    [[nodiscard]] std::optional<IndexEntry> find(std::string_view name) const {
        auto it = by_name_.find(std::string(name));
        if (it == by_name_.end()) {
            // try with/without trailing slash for directories
            std::string alt(name);
            if (!alt.empty() && alt.back() == '/') {
                alt.pop_back();
                it = by_name_.find(alt);
            } else {
                alt.push_back('/');
                it = by_name_.find(alt);
            }
            if (it == by_name_.end()) return std::nullopt;
        }
        return entries_[it->second];
    }

    [[nodiscard]] const std::vector<IndexEntry>& all() const noexcept { return entries_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    std::vector<IndexEntry> entries_;
    std::map<std::string, std::size_t> by_name_;
};

// ── Header decode/encode ──────────────────────────────────────────────────────

// Safely read a NUL-terminated (or field-width-limited) string from a char array
template<std::size_t N>
static std::string field_str(const char (&field)[N]) {
    return std::string(field, std::find(field, field + N, '\0'));
}

static Entry decode_header(const Block& blk) {
    Entry e;
    const auto& h = blk.header;
    // Preserve raw typeflag. NUL means "old V7 regular file"; the distinction
    // from REGTYPE ('0') matters when the name ends with '/' (implicit directory).
    e.typeflag = h.typeflag;

    // Detect format for extended fields
    std::string_view magic(h.magic, 8);
    if (magic.starts_with("ustar  ") || magic.starts_with("ustar\0 ")) {
        e.fmt = Format::OldGNU;
    } else if (magic.starts_with("ustar")) {
        e.fmt = Format::USTAR; // also PAX
    } else {
        e.fmt = Format::V7;
    }

    // name + prefix (ustar/gnu)
    std::string prefix = field_str(h.prefix);
    std::string name   = field_str(h.name);
    if (!prefix.empty() && e.fmt != Format::V7 && e.fmt != Format::OldGNU)
        e.name = prefix + "/" + name;
    else
        e.name = name;

    e.linkname = field_str(h.linkname);
    e.mode     = static_cast<unsigned int>(read_octal({h.mode, 8}));
    e.uid      = static_cast<unsigned int>(read_number({h.uid,  8}));
    e.gid      = static_cast<unsigned int>(read_number({h.gid,  8}));
    e.size     = static_cast<std::int64_t>(read_number({h.size, 12}));
    e.mtime    = static_cast<std::int64_t>(read_number({h.mtime, 12}));
    e.devmajor = static_cast<unsigned int>(read_octal({h.devmajor, 8}));
    e.devminor = static_cast<unsigned int>(read_octal({h.devminor, 8}));

    if (e.fmt != Format::V7) {
        e.uname = field_str(h.uname);
        e.gname = field_str(h.gname);
    }

    // GNU oldgnu: atime/ctime in overlay
    if (e.fmt == Format::OldGNU) {
        const auto& g = blk.oldgnu;
        e.atime = static_cast<std::int64_t>(read_octal({g.atime, 12}));
        e.ctime = static_cast<std::int64_t>(read_octal({g.ctime, 12}));
        // Sparse info is decoded separately
        if (h.typeflag == GNUTYPE_SPARSE) {
            e.is_sparse = true;
            e.real_size = static_cast<std::int64_t>(read_octal({g.realsize, 12}));
        }
    }

    e.asize = e.size;
    return e;
}

// Write a header block from Entry
static Block encode_header(const Entry& e, const Config& cfg) {
    Block blk{};
    auto& h = blk.header;

    // Choose effective format
    Format fmt = cfg.fmt;
    if (fmt == Format::Default) fmt = Format::GNU;

    // name / prefix split for ustar/pax (100+155=255 chars max)
    std::string name     = e.name;
    std::string prefix;

    auto needs_longname [[maybe_unused]] = [&] { return name.size() > 100; };

    if ((fmt == Format::USTAR || fmt == Format::PAX) && name.size() > 100) {
        // Try to split at a slash
        auto slash = name.rfind('/', 154);
        if (slash != std::string::npos && slash < 155 && name.size() - slash - 1 <= 100) {
            prefix = name.substr(0, slash);
            name   = name.substr(slash + 1);
        }
        // else: need a LongName extension — caller handles that
    }

    auto write_field = [](char* dst, std::size_t n, std::string_view s) {
        std::size_t len = std::min(s.size(), n - 1);
        std::memcpy(dst, s.data(), len);
        dst[len] = '\0';
    };

    write_field(h.name,     100, name);
    write_field(h.linkname, 100, e.linkname);
    write_field(h.prefix,   155, prefix);

    write_octal(h.mode,     8, e.mode & 07777);
    write_octal(h.uid,      8, e.uid);
    write_octal(h.gid,      8, e.gid);
    write_octal(h.size,    12, static_cast<std::uint64_t>(e.size));
    write_octal(h.mtime,   12, static_cast<std::uint64_t>(e.mtime));
    h.typeflag = e.typeflag;

    if (fmt != Format::V7) {
        // magic + version
        if (fmt == Format::OldGNU || fmt == Format::GNU) {
            std::memcpy(h.magic, "ustar  ", 8); // includes version field
        } else {
            std::memcpy(h.magic,   "ustar", 5); h.magic[5] = '\0';
            std::memcpy(h.version, "00", 2);
        }
        write_field(h.uname, 32, e.uname);
        write_field(h.gname, 32, e.gname);
        write_octal(h.devmajor, 8, e.devmajor);
        write_octal(h.devminor, 8, e.devminor);
    }

    // Handle large uid/gid/size via base-256 (GNU extension)
    if (e.uid > 0777777) write_base256(h.uid, 8, e.uid);
    if (e.gid > 0777777) write_base256(h.gid, 8, e.gid);
    if (e.size > 077777777777LL) write_base256(h.size, 12, e.size);

    write_checksum(blk);
    return blk;
}

// ── PAX extended header encode/decode ─────────────────────────────────────────

/// True if --pax-option did not request delete= for this keyword.
[[nodiscard]] static bool pax_allowed(const Config& cfg, std::string_view key)
{
    return !cfg.pax_option_rules.delete_keywords.contains(std::string(key));
}

// Encode a PAX record: "N keyword=value\n" where N = total length.
// Value may contain arbitrary bytes (GNU SCHILY.xattr raw binary); length is authoritative.
static void pax_append(std::string& out, std::string_view key, std::string_view val) {
    // Converge on the total record length: len = digits(len) + 1(space) + key + 1(=) + val + 1(\n)
    // e.g. "30 mtime=1234567890.123456789\n" has len=30, "30" is 2 digits, base=28
    std::size_t base = 1 + key.size() + 1 + val.size() + 1;  // space + key + = + val + \n
    std::size_t len = base;
    for (;;) {
        std::string s = std::to_string(len);
        if (s.size() + base == len) break;
        len = s.size() + base;
        // At most 2 iterations to converge (digit count can increase by 1 at most)
    }
    out += std::to_string(len);
    out += ' ';
    out += key;
    out += '=';
    out += val;
    out += '\n';
}

/// Append a PAX record only when the keyword is not deleted via --pax-option.
static void pax_append_if(std::string& out, const Config& cfg,
                          std::string_view key, std::string_view val)
{
    if (pax_allowed(cfg, key))
        pax_append(out, key, val);
}

static std::map<std::string, std::string> pax_parse(std::string_view data) {
    std::map<std::string, std::string> attrs;
    while (!data.empty()) {
        auto sp = data.find(' ');
        if (sp == std::string_view::npos) break;
        std::size_t total = 0;
        for (char c : data.substr(0, sp))
            if (c >= '0' && c <= '9') total = total * 10 + static_cast<unsigned>(c - '0');
        if (total == 0 || total > data.size()) break;
        // Keyword ends at first '='; value is the rest up to the trailing '\n' (may be binary).
        std::string_view rec = data.substr(sp + 1, total - sp - 2);
        auto eq = rec.find('=');
        if (eq != std::string_view::npos)
            attrs[std::string(rec.substr(0, eq))] = std::string(rec.substr(eq + 1));
        data = data.substr(total);
    }
    return attrs;
}

// ── xattr / ACL helpers (SCHILY PAX keywords; SELinux never stored) ───────────

static bool entry_has_schily_pax(const Entry& e) {
    for (const auto& [k, v] : e.pax_attrs) {
        (void)v;
        if (k.starts_with("SCHILY.")) return true;
    }
    return false;
}

static void pax_append_schily(std::string& out, const Config& cfg, const Entry& e) {
    for (const auto& [k, v] : e.pax_attrs) {
        if (k.starts_with("SCHILY."))
            pax_append_if(out, cfg, k, v);
    }
}

#ifdef MUTAR_HAVE_XATTR
// GNU tar percent-encodes only '=' and '%' in the xattr *name* (keyword); values are raw.
static std::string xattr_encode_keyword(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '=' || c == '%') {
            out += '%';
            static constexpr char hex[] = "0123456789ABCDEF";
            out += hex[c >> 4];
            out += hex[c & 0xf];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

static std::string xattr_decode_keyword(std::string_view enc) {
    std::string out;
    out.reserve(enc.size());
    for (std::size_t i = 0; i < enc.size(); ++i) {
        if (enc[i] == '%' && i + 2 < enc.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int hi = hex(enc[i + 1]), lo = hex(enc[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += enc[i];
    }
    return out;
}

static bool xattr_key_matches(std::string_view key, std::string_view pattern) {
    return ::fnmatch(std::string(pattern).c_str(), std::string(key).c_str(), 0) == 0;
}

/// Whether to store this xattr key under --xattrs (include/exclude + hard skips).
static bool xattr_key_wanted(const Config& cfg, std::string_view key) {
    // Never store SELinux context (unsupported by project policy).
    if (key == "security.selinux") return false;
    // POSIX ACL xattrs are handled by --acls → SCHILY.acl.*; skip raw forms.
    if (key == "system.posix_acl_access" || key == "system.posix_acl_default")
        return false;
    if (!cfg.xattrs_exclude.empty()) {
        for (const auto& pat : cfg.xattrs_exclude) {
            if (xattr_key_matches(key, pat)) return false;
        }
    }
    if (!cfg.xattrs_include.empty()) {
        for (const auto& pat : cfg.xattrs_include) {
            if (xattr_key_matches(key, pat)) return true;
        }
        return false;
    }
    return true;
}

static void collect_xattrs(Entry& e, const std::string& fspath, const Config& cfg) {
    if (!cfg.xattrs) return;
    ssize_t list_sz = ::llistxattr(fspath.c_str(), nullptr, 0);
    if (list_sz <= 0) return;
    std::string list(static_cast<std::size_t>(list_sz), '\0');
    list_sz = ::llistxattr(fspath.c_str(), list.data(), list.size());
    if (list_sz < 0) return;
    std::size_t i = 0;
    const std::size_t n = static_cast<std::size_t>(list_sz);
    while (i < n) {
        const char* key_c = list.data() + i;
        std::size_t key_len = std::strlen(key_c);
        if (key_len == 0) break;
        std::string_view key(key_c, key_len);
        i += key_len + 1;
        if (!xattr_key_wanted(cfg, key)) continue;
        ssize_t vsz = ::lgetxattr(fspath.c_str(), key_c, nullptr, 0);
        if (vsz < 0) continue;
        std::string val(static_cast<std::size_t>(vsz), '\0');
        if (vsz > 0) {
            if (::lgetxattr(fspath.c_str(), key_c, val.data(), val.size()) < 0)
                continue;
        }
        std::string pax_key = "SCHILY.xattr." + xattr_encode_keyword(key);
        e.pax_attrs[std::move(pax_key)] = std::move(val);
    }
}

static void restore_xattrs(const Entry& e, const std::string& path, const Config& cfg) {
    if (!cfg.xattrs) return;
    constexpr std::string_view prefix = "SCHILY.xattr.";
    for (const auto& [k, v] : e.pax_attrs) {
        if (!k.starts_with(prefix)) continue;
        std::string name = xattr_decode_keyword(std::string_view(k).substr(prefix.size()));
        if (name == "security.selinux") continue;
        if (name == "system.posix_acl_access" || name == "system.posix_acl_default")
            continue;
        if (::lsetxattr(path.c_str(), name.c_str(), v.data(), v.size(), 0) < 0) {
            print(stderr, "mutar: {}: setxattr({}): {}\n",
                  path, name, std::strerror(errno));
        }
    }
}
#else
static void collect_xattrs(Entry&, const std::string&, const Config&) {}
static void restore_xattrs(const Entry&, const std::string&, const Config&) {}
#endif // MUTAR_HAVE_XATTR

#ifdef MUTAR_HAVE_ACL
static void collect_acls(Entry& e, const std::string& fspath, const Config& cfg, bool is_dir) {
    if (!cfg.acls) return;
    acl_t access_acl = ::acl_get_file(fspath.c_str(), ACL_TYPE_ACCESS);
    if (access_acl) {
        mode_t equiv = 0;
        // Skip trivial ACLs that are fully represented by the mode bits.
        if (::acl_equiv_mode(access_acl, &equiv) != 0) {
            ssize_t len = 0;
            char* txt = ::acl_to_text(access_acl, &len);
            if (txt) {
                e.pax_attrs["SCHILY.acl.access"] = std::string(txt, static_cast<std::size_t>(
                    len > 0 ? len : static_cast<ssize_t>(std::strlen(txt))));
                ::acl_free(txt);
            }
        }
        ::acl_free(access_acl);
    }
    if (is_dir) {
        acl_t def_acl = ::acl_get_file(fspath.c_str(), ACL_TYPE_DEFAULT);
        if (def_acl) {
            // Default ACLs are never "trivial" in the mode sense; store if non-empty.
            ssize_t len = 0;
            char* txt = ::acl_to_text(def_acl, &len);
            if (txt && txt[0] != '\0') {
                // Empty default ACL text is typically just "" — skip pure empty.
                bool only_ws = true;
                for (const char* p = txt; *p; ++p) {
                    if (*p != ' ' && *p != '\t' && *p != '\n') { only_ws = false; break; }
                }
                if (!only_ws) {
                    e.pax_attrs["SCHILY.acl.default"] = std::string(txt, static_cast<std::size_t>(
                        len > 0 ? len : static_cast<ssize_t>(std::strlen(txt))));
                }
                ::acl_free(txt);
            } else if (txt) {
                ::acl_free(txt);
            }
            ::acl_free(def_acl);
        }
    }
}

static void restore_acls(const Entry& e, const std::string& path, const Config& cfg) {
    if (!cfg.acls) return;
    auto set_one = [&](const char* key, acl_type_t type) {
        auto it = e.pax_attrs.find(key);
        if (it == e.pax_attrs.end()) return;
        acl_t acl = ::acl_from_text(it->second.c_str());
        if (!acl) {
            print(stderr, "mutar: {}: acl_from_text({}): {}\n",
                  path, key, std::strerror(errno));
            return;
        }
        if (::acl_set_file(path.c_str(), type, acl) < 0) {
            print(stderr, "mutar: {}: acl_set_file({}): {}\n",
                  path, key, std::strerror(errno));
        }
        ::acl_free(acl);
    };
    set_one("SCHILY.acl.access", ACL_TYPE_ACCESS);
    set_one("SCHILY.acl.default", ACL_TYPE_DEFAULT);
}
#else
static void collect_acls(Entry&, const std::string&, const Config&, bool) {}
static void restore_acls(const Entry&, const std::string&, const Config&) {}
#endif // MUTAR_HAVE_ACL

static void restore_xattrs_acls(const Entry& e, const std::string& path, const Config& cfg) {
    restore_xattrs(e, path, cfg);
    restore_acls(e, path, cfg);
}

// ── Owner/group map helpers ───────────────────────────────────────────────────

// Load an owner/group map file: lines of "OLD NEW" (names or numeric IDs).
// Comment lines (starting with #) and blank lines are skipped.
static std::map<std::string,std::string> load_id_map(const std::string& path) {
    std::map<std::string,std::string> m;
    if (path.empty()) return m;
    std::ifstream f(path);
    if (!f) {
        print(stderr, "mutar: {}: cannot open map file: {}\n",
                     path, std::strerror(errno));
        return m;
    }
    std::string line;
    while (std::getline(f, line)) {
        auto cpos = line.find('#');
        if (cpos != std::string::npos) line.resize(cpos);
        std::istringstream iss(line);
        std::string from, to;
        if (!(iss >> from >> to)) continue;
        m[from] = to;
    }
    return m;
}

// Apply owner/group maps to an entry (mutates uname/gname/uid/gid in-place).
static void apply_owner_map(Entry& e,
                             const std::map<std::string,std::string>& omap,
                             const std::map<std::string,std::string>& gmap) {
    if (!omap.empty()) {
        auto it = omap.find(e.uname);
        if (it != omap.end()) {
            e.uname = it->second;
            char* endp = nullptr; errno = 0;
            long nuid = std::strtol(it->second.c_str(), &endp, 10);
            if (endp && *endp == '\0' && errno == 0 && nuid >= 0 &&
                static_cast<unsigned long>(nuid) <= UINT_MAX)
                e.uid = static_cast<unsigned>(nuid);
        }
        auto it2 = omap.find(std::to_string(e.uid));
        if (it2 != omap.end()) {
            char* endp = nullptr; errno = 0;
            long nuid = std::strtol(it2->second.c_str(), &endp, 10);
            if (endp && *endp == '\0' && errno == 0 && nuid >= 0 &&
                static_cast<unsigned long>(nuid) <= UINT_MAX)
                e.uid = static_cast<unsigned>(nuid);
        }
    }
    if (!gmap.empty()) {
        auto it = gmap.find(e.gname);
        if (it != gmap.end()) {
            e.gname = it->second;
            char* endp = nullptr; errno = 0;
            long ngid = std::strtol(it->second.c_str(), &endp, 10);
            if (endp && *endp == '\0' && errno == 0 && ngid >= 0 &&
                static_cast<unsigned long>(ngid) <= UINT_MAX)
                e.gid = static_cast<unsigned>(ngid);
        }
        auto it2 = gmap.find(std::to_string(e.gid));
        if (it2 != gmap.end()) {
            char* endp = nullptr; errno = 0;
            long ngid = std::strtol(it2->second.c_str(), &endp, 10);
            if (endp && *endp == '\0' && errno == 0 && ngid >= 0 &&
                static_cast<unsigned long>(ngid) <= UINT_MAX)
                e.gid = static_cast<unsigned>(ngid);
        }
    }
}

// ── Warning emission helper ───────────────────────────────────────────────────

// Emit a warning message, respecting --warning=none/all/KEYWORD/no-KEYWORD.
// category: short string like "failed-read", "newer", "missing-links", "xdev"
static void mutar_warn(const Config& cfg, std::string_view category, std::string_view msg) {
    if (cfg.warn_none) return;
    if (cfg.warn_all) { std::fprintf(stderr, "mutar: warning: %.*s\n",
                                     static_cast<int>(msg.size()), msg.data()); return; }
    std::string cat(category);
    if (cfg.warnings_disabled.count(cat)) return;
    if (cfg.warnings_enabled.empty() || cfg.warnings_enabled.count(cat))
        std::fprintf(stderr, "mutar: warning: %.*s\n",
                     static_cast<int>(msg.size()), msg.data());
}

// ── Multi-volume helpers ──────────────────────────────────────────────────────

// Generate the archive filename for volume N (1-based).
// If base contains "%d", substitute the volume number.
// Otherwise volume 1 = base, volume N = base.N
static std::string make_volume_name(const std::string& base, int vol_num) {
    if (base.find("%d") != std::string::npos) {
        char buf[PATH_MAX];
        // Intentionally using user-supplied format string (GNU tar %d convention)
        // NOLINTNEXTLINE(cert-err33-c)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        std::snprintf(buf, sizeof(buf), base.c_str(), vol_num);
#pragma GCC diagnostic pop
        return buf;
    }
    if (vol_num == 1) return base;
    return base + "." + std::to_string(vol_num);
}

// Read initial volume number from --volno-file (default 1).
static int read_volno_file(const std::string& path) {
    if (path.empty()) return 1;
    std::ifstream in(path);
    if (!in) return 1;
    int v = 0;
    in >> v;
    if (!in || v < 1) return 1;
    return v;
}

// Atomically write current volume number to --volno-file (mkstemp + rename).
static bool write_volno_file(const std::string& path, int vol_num) {
    if (path.empty()) return true;
    std::string dir = ".";
    auto slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0)
        dir = path.substr(0, slash);
    else if (slash == 0)
        dir = "/";

    std::string tmpl = dir + "/.mutar_volno_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        print(stderr, "mutar: cannot create temp for volno-file '{}': {}\n",
              path, std::strerror(errno));
        return false;
    }
    std::string content = std::to_string(vol_num) + "\n";
    const char* p = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            print(stderr, "mutar: cannot write volno-file temp: {}\n", std::strerror(errno));
            ::close(fd);
            ::unlink(buf.data());
            return false;
        }
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    ::close(fd);
    if (::rename(buf.data(), path.c_str()) < 0) {
        print(stderr, "mutar: cannot update volno-file '{}': {}\n",
              path, std::strerror(errno));
        ::unlink(buf.data());
        return false;
    }
    return true;
}

// Run -F/--info-script at a volume boundary. Non-zero exit → failure.
// Sets GNU-like TAR_ARCHIVE and TAR_VOLUME in the environment.
static bool run_info_script(const Config& cfg, const std::string& archive_path,
                            int vol_num, const char* subcommand) {
    if (cfg.info_script.empty()) return true;
    ::setenv("TAR_ARCHIVE", archive_path.c_str(), 1);
    ::setenv("TAR_VOLUME", std::to_string(vol_num).c_str(), 1);
    if (subcommand && subcommand[0])
        ::setenv("TAR_SUBCOMMAND", subcommand, 1);
    int rc = ::system(cfg.info_script.c_str());
    if (rc != 0) {
        int status = rc;
        if (WIFEXITED(rc)) status = WEXITSTATUS(rc);
        print(stderr, "mutar: info-script '{}' failed with status {}\n",
              cfg.info_script, status);
        return false;
    }
    return true;
}

// Max 512-byte blocks for --tape-length=N (N × 1024 bytes).
static std::int64_t tape_max_blocks(const Config& cfg) {
    if (cfg.tape_length <= 0) return 0;
    return (static_cast<std::int64_t>(cfg.tape_length) * 1024LL)
           / static_cast<std::int64_t>(BLOCKSIZE);
}

// ── Remote archive detection ──────────────────────────────────────────────────

// Returns true if path looks like [user@]host:path and --force-local is not set.
static bool is_remote_archive(const std::string& path, bool force_local) {
    if (force_local) return false;
    if (path.empty() || path == "-") return false;
    auto colon = path.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    // Make sure the part before ':' has no '/' (that would be a local path)
    auto slash = path.find('/');
    return slash == std::string::npos || slash > colon;
}

// Parse [user@]host:path → {user, host, path}  (user may be empty)
static std::tuple<std::string,std::string,std::string> parse_remote_path(const std::string& s) {
    auto colon = s.find(':');
    std::string hostpart = s.substr(0, colon);
    std::string path     = s.substr(colon + 1);
    std::string user, host;
    auto at = hostpart.find('@');
    if (at != std::string::npos) { user = hostpart.substr(0, at); host = hostpart.substr(at + 1); }
    else host = hostpart;
    return {user, host, path};
}

// Open a remote archive via rmt protocol over rsh.
// Returns a read or write fd (pipe end) on success, or -1 on failure.
// mode: O_RDONLY (0) for read, O_WRONLY|O_CREAT|O_TRUNC (577) for write.
static int open_remote_stream(const std::string& archive_file,
                               const std::string& rsh_cmd_arg,
                               const std::string& rmt_cmd_arg,
                               bool for_write) {
    auto [user, host, remote_path] = parse_remote_path(archive_file);
    const std::string rsh_bin = rsh_cmd_arg.empty() ? "rsh" : rsh_cmd_arg;
    const std::string rmt_bin = rmt_cmd_arg.empty() ? "rmt" : rmt_cmd_arg;

    // We create two pipes for the rmt sub-process: one for data (our pipe) and
    // one for the rsh/rmt communication.
    // Topology for WRITE:
    //   main-writes-to → data_pipe[1]
    //   bridge reads data_pipe[0], sends W commands to rmt_pipe → rmt process
    // Topology for READ:
    //   bridge reads R responses from rmt_pipe → writes to data_pipe[1]
    //   main reads from data_pipe[0]

    int data_pipe[2];
    if (::pipe(data_pipe) < 0) {
        print(stderr, "mutar: pipe: {}\n", std::strerror(errno));
        return -1;
    }

    pid_t bridge_pid = ::fork();
    if (bridge_pid < 0) {
        ::close(data_pipe[0]); ::close(data_pipe[1]);
        print(stderr, "mutar: fork: {}\n", std::strerror(errno));
        return -1;
    }

    if (bridge_pid == 0) {
        // Bridge child process
        int rmt_in[2], rmt_out[2];
        if (::pipe(rmt_in) < 0 || ::pipe(rmt_out) < 0) ::_exit(1);

        pid_t rsh_pid = ::fork();
        if (rsh_pid < 0) ::_exit(1);
        if (rsh_pid == 0) {
            // rsh child: exec rsh host rmt
            ::dup2(rmt_in[0],  STDIN_FILENO);
            ::dup2(rmt_out[1], STDOUT_FILENO);
            ::close(rmt_in[0]); ::close(rmt_in[1]);
            ::close(rmt_out[0]); ::close(rmt_out[1]);
            if (!user.empty())
                ::execlp(rsh_bin.c_str(), rsh_bin.c_str(),
                          "-l", user.c_str(), host.c_str(), rmt_bin.c_str(), nullptr);
            else
                ::execlp(rsh_bin.c_str(), rsh_bin.c_str(),
                          host.c_str(), rmt_bin.c_str(), nullptr);
            ::_exit(127);
        }
        ::close(rmt_in[0]); ::close(rmt_out[1]);
        // rmt_in[1] = write to rmt stdin; rmt_out[0] = read rmt stdout

        // Helper lambdas for rmt protocol I/O
        auto rmt_write_cmd = [&](const std::string& cmd) -> bool {
            const char* p = cmd.c_str();
            std::size_t n = cmd.size();
            while (n > 0) {
                ssize_t w = ::write(rmt_in[1], p, n);
                if (w <= 0) return false;
                p += w; n -= static_cast<std::size_t>(w);
            }
            return true;
        };
        auto rmt_read_response = [&]() -> std::string {
            std::string resp;
            char c;
            while (::read(rmt_out[0], &c, 1) == 1) {
                resp += c;
                if (c == '\n') break;
            }
            return resp;
        };
        auto rmt_read_bytes = [&](char* buf, std::size_t n) -> ssize_t {
            std::size_t got = 0;
            while (got < n) {
                ssize_t r = ::read(rmt_out[0], buf + got, n - got);
                if (r <= 0) return got == 0 ? r : static_cast<ssize_t>(got);
                got += static_cast<std::size_t>(r);
            }
            return static_cast<ssize_t>(got);
        };

        // Send O command: O path\nmode\n
        int open_mode = for_write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
        std::string open_cmd = std::format("O {}\n{}\n", remote_path, open_mode);
        if (!rmt_write_cmd(open_cmd)) {
            std::fprintf(stderr, "mutar: rmt: failed to send open command\n");
            ::close(rmt_in[1]); ::close(rmt_out[0]);
            ::close(data_pipe[0]); ::close(data_pipe[1]);
            ::_exit(1);
        }
        std::string resp = rmt_read_response();
        if (resp.empty() || resp[0] != 'A') {
            std::fprintf(stderr, "mutar: rmt: remote open failed: %s", resp.c_str());
            ::close(rmt_in[1]); ::close(rmt_out[0]);
            ::close(data_pipe[0]); ::close(data_pipe[1]);
            ::_exit(1);
        }

        constexpr std::size_t RMT_CHUNK = 10240; // 20 blocks × 512
        if (for_write) {
            // Write bridge: read raw data from data_pipe[0], forward via W commands
            ::close(data_pipe[1]);
            char buf[RMT_CHUNK];
            for (;;) {
                ssize_t got = ::read(data_pipe[0], buf, sizeof(buf));
                if (got <= 0) break;
                std::string wcmd = std::format("W{}\n", got);
                if (!rmt_write_cmd(wcmd)) break;
                const char* p = buf;
                std::size_t rem = static_cast<std::size_t>(got);
                while (rem > 0) {
                    ssize_t w = ::write(rmt_in[1], p, rem);
                    if (w <= 0) goto write_done;
                    p += w; rem -= static_cast<std::size_t>(w);
                }
                // Read A response
                rmt_read_response();
            }
write_done:
            ::close(data_pipe[0]);
        } else {
            // Read bridge: issue R commands, write data to data_pipe[1]
            ::close(data_pipe[0]);
            for (;;) {
                std::string rcmd = std::format("R{}\n", RMT_CHUNK);
                if (!rmt_write_cmd(rcmd)) break;
                std::string aresp = rmt_read_response();
                if (aresp.empty() || aresp[0] != 'A') break;
                std::size_t nbytes = 0;
                try {
                    nbytes = static_cast<std::size_t>(std::stoul(aresp.substr(1)));
                } catch (...) {
                    break; // malformed rmt response — stop reading
                }
                if (nbytes == 0) break;
                char rbuf[RMT_CHUNK];
                ssize_t got = rmt_read_bytes(rbuf, nbytes);
                if (got <= 0) break;
                const char* p = rbuf;
                std::size_t rem = static_cast<std::size_t>(got);
                while (rem > 0) {
                    ssize_t w = ::write(data_pipe[1], p, rem);
                    if (w <= 0) goto read_done;
                    p += w; rem -= static_cast<std::size_t>(w);
                }
            }
read_done:
            ::close(data_pipe[1]);
        }

        // Send C command to close remote file
        rmt_write_cmd("C\n");
        rmt_read_response();
        ::close(rmt_in[1]); ::close(rmt_out[0]);
        ::waitpid(rsh_pid, nullptr, 0);
        ::_exit(0);
    }

    // Parent: return the appropriate pipe end
    if (for_write) {
        ::close(data_pipe[0]);
        return data_pipe[1];  // parent writes here
    } else {
        ::close(data_pipe[1]);
        return data_pipe[0];  // parent reads here
    }
}

// Forward declarations of helpers used inside ArchiveWriter (defined later)
static std::time_t parse_date_string(const std::string& s);
static std::string apply_transform(const std::string& name, const std::string& expr);

struct SparseSegment { std::int64_t offset; std::int64_t length; };
static std::vector<SparseSegment> detect_sparse_segments(int fd, std::int64_t file_size,
                                                          std::string_view hole_detection = "seek");

// ── Archive reader ────────────────────────────────────────────────────────────

// Reads entries sequentially from an archive.
// Handles: GNU LongName/LongLink, PAX extended headers, sparse maps.
class ArchiveReader {
public:
    ArchiveReader(ArchiveStream& s, int blocking, bool ignore_zeros = false)
        : stream_(&s), buf_(blocking), ignore_zeros_(ignore_zeros) {}

    /// Switch to a new volume stream (multi-volume extract). Resets block counter.
    void swap_stream(ArchiveStream& s) {
        stream_ = &s;
        buf_.reset_at_block(0);
    }
    struct ReadResult {
        Entry entry;
        bool  ok;   // false = EOF or error
        bool  eof;
    };

    // Read next entry header; populates entry and positions stream at file data.
    // Returns {entry, true, false} on success, {_, false, true} on clean EOF.
    ReadResult next_entry() {
        // Long name/link accumulation
        std::string pending_longname;
        std::string pending_longlink;
        std::map<std::string, std::string> pending_pax;

        for (;;) {
            Block blk{};
            if (!buf_.read_block(*stream_, blk))
                return {{}, false, true};

            // EOF block: two consecutive zero blocks (or single if --ignore-zeros)
            if (is_zero_block(blk)) {
                if (ignore_zeros_) continue; // skip zero blocks, keep reading
                Block blk2{};
                buf_.read_block(*stream_, blk2); // consume second zero block
                return {{}, false, true};
            }

            if (!valid_checksum(blk)) {
                print(stderr, "mutar: invalid block checksum at block {}\n",
                           buf_.block_no());
                return {{}, false, false};
            }

            Entry e = decode_header(blk);
            e.block_offset = buf_.block_no() - 1;

            // Merge pending PAX attributes
            e.pax_attrs = std::move(pending_pax);
            pending_pax.clear();

            // GNU LongName / LongLink
            if (e.typeflag == GNUTYPE_LONGNAME || e.typeflag == GNUTYPE_LONGLINK) {
                std::string longstr = read_data_string(e.size);
                // Strip trailing NUL
                while (!longstr.empty() && longstr.back() == '\0') longstr.pop_back();
                if (e.typeflag == GNUTYPE_LONGNAME) pending_longname = std::move(longstr);
                else                                pending_longlink = std::move(longstr);
                continue;
            }

            // PAX extended header
            if (e.typeflag == XHDTYPE || e.typeflag == XGLTYPE) {
                std::string pax_data = read_data_string(e.size);
                pending_pax = pax_parse(pax_data);
                continue;
            }

            // Apply pending long names
            if (!pending_longname.empty()) { e.name     = pending_longname; pending_longname.clear(); }
            if (!pending_longlink.empty()) { e.linkname = pending_longlink; pending_longlink.clear(); }

            // Apply PAX attributes to entry
            apply_pax_attrs(e);

            // GNU sparse: read sparse map from header + extension blocks
            if (e.typeflag == GNUTYPE_SPARSE || e.is_sparse)
                read_gnu_sparse_map(e, blk);

            return {std::move(e), true, false};
        }
    }

    // Skip over the data blocks for the current entry
    void skip_entry(const Entry& e) {
        std::int64_t to_skip = e.asize;
        std::int64_t blocks  = (to_skip + BLOCKSIZE - 1) / BLOCKSIZE;
        Block dummy{};
        for (std::int64_t i = 0; i < blocks; ++i)
            buf_.read_block(*stream_, dummy);
    }

    // Read entry data into a string (used for LongName, PAX headers)
    std::string read_data_string(std::int64_t sz) {
        std::string result;
        result.resize(static_cast<std::size_t>(sz));
        std::int64_t blocks = (sz + BLOCKSIZE - 1) / BLOCKSIZE;
        std::string blockbuf(static_cast<std::size_t>(blocks * BLOCKSIZE), '\0');
        Block blk{};
        for (std::int64_t i = 0; i < blocks; ++i) {
            buf_.read_block(*stream_, blk);
            std::memcpy(blockbuf.data() + i * BLOCKSIZE, blk.buffer, BLOCKSIZE);
        }
        result = blockbuf.substr(0, static_cast<std::size_t>(sz));
        return result;
    }

    // Read entry data, writing to fd (or string if fd < 0)
    bool read_entry_data(const Entry& e, int out_fd,
                         std::function<void(const char*, std::size_t)> on_data = {}) {
        std::int64_t remaining = e.asize;
        Block blk{};
        while (remaining > 0) {
            if (!buf_.read_block(*stream_, blk)) return false;
            std::size_t chunk = static_cast<std::size_t>(
                std::min<std::int64_t>(BLOCKSIZE, remaining));
            if (out_fd >= 0) {
                const char* p = blk.buffer;
                std::size_t left = chunk;
                while (left > 0) {
                    ssize_t w = ::write(out_fd, p, left);
                    if (w < 0) { if (errno == EINTR) continue; return false; }
                    p += w; left -= static_cast<std::size_t>(w);
                }
            }
            if (on_data) on_data(blk.buffer, chunk);
            remaining -= BLOCKSIZE;
        }
        return true;
    }

    std::int64_t block_no() const { return buf_.block_no(); }

    /// Seek to an absolute byte offset in a seekable archive and reset the block buffer.
    /// @return false if the stream is not seekable or lseek fails.
    bool seek_to_byte(std::uint64_t offset) {
        if (!stream_->is_seekable()) return false;
        if (offset % BLOCKSIZE != 0) {
            print(stderr, "mutar: index offset {} is not block-aligned\n", offset);
            return false;
        }
        if (stream_->seek(static_cast<off_t>(offset), SEEK_SET) < 0) return false;
        buf_.reset_at_block(static_cast<std::int64_t>(offset / BLOCKSIZE));
        if (std::getenv("MUTAR_DEBUG_SEEK"))
            print(stderr, "mutar: seek_to_byte offset={}\n", offset);
        return true;
    }

    // Sparse extraction: distribute concatenated archive segments to correct file offsets.
    bool extract_sparse_data(const Entry& e, int out_fd) {
        if (e.sparse_map.empty()) { read_entry_data(e, out_fd); return true; }
        std::size_t seg_idx = 0;
        std::int64_t seg_rem = e.sparse_map[0].numbytes;
        if (out_fd >= 0)
            ::lseek(out_fd, static_cast<off_t>(e.sparse_map[0].offset), SEEK_SET);

        std::int64_t arch_rem = e.asize;
        Block blk{};
        while (arch_rem > 0) {
            if (!buf_.read_block(*stream_, blk)) return false;
            std::size_t chunk = static_cast<std::size_t>(
                std::min<std::int64_t>(BLOCKSIZE, arch_rem));

            std::size_t buf_pos = 0;
            while (buf_pos < chunk && seg_idx < e.sparse_map.size()) {
                std::size_t to_write = static_cast<std::size_t>(
                    std::min<std::int64_t>(seg_rem,
                        static_cast<std::int64_t>(chunk - buf_pos)));
                if (out_fd >= 0) {
                    const char* p = blk.buffer + buf_pos;
                    std::size_t left = to_write;
                    while (left > 0) {
                        ssize_t w = ::write(out_fd, p, left);
                        if (w < 0) { if (errno == EINTR) continue; return false; }
                        p += w; left -= static_cast<std::size_t>(w);
                    }
                }
                buf_pos  += to_write;
                seg_rem  -= static_cast<std::int64_t>(to_write);
                if (seg_rem == 0) {
                    ++seg_idx;
                    if (seg_idx < e.sparse_map.size()) {
                        seg_rem = e.sparse_map[seg_idx].numbytes;
                        if (out_fd >= 0)
                            ::lseek(out_fd,
                                    static_cast<off_t>(e.sparse_map[seg_idx].offset),
                                    SEEK_SET);
                    }
                }
            }
            arch_rem -= static_cast<std::int64_t>(BLOCKSIZE);
        }
        return true;
    }

    static bool is_zero_block(const Block& blk) noexcept {
        for (char c : blk.buffer) if (c) return false;
        return true;
    }

    void apply_pax_attrs(Entry& e) {
        for (auto& [k, v] : e.pax_attrs) {
            if (k == "path")           e.name = v;
            else if (k == "linkpath")  e.linkname = v;
            else if (k == "uname")     e.uname = v;
            else if (k == "gname")     e.gname = v;
            else if (k == "size")      e.size = std::stoll(v);
            else if (k == "mtime") {
                double t = std::stod(v);
                e.mtime = static_cast<std::int64_t>(t);
                e.mtime_nsec = static_cast<long>((t - e.mtime) * 1e9);
            }
            else if (k == "atime")     e.atime = static_cast<std::int64_t>(std::stod(v));
            else if (k == "uid")       e.uid   = static_cast<unsigned>(std::stoul(v));
            else if (k == "gid")       e.gid   = static_cast<unsigned>(std::stoul(v));
            else if (k == "GNU.sparse.realsize") {
                e.is_sparse = true;
                e.real_size = std::stoll(v);
            }
            else if (k == "GNU.sparse.name")     e.name = v;
            else if (k == "GNU.sparse.map") {
                // Parse "offset,length,offset,length,..." into sparse_map
                e.sparse_map.clear();
                e.is_sparse = true;
                std::istringstream ss(v);
                std::string tok;
                try {
                    while (std::getline(ss, tok, ',')) {
                        std::int64_t off = std::stoll(tok);
                        if (!std::getline(ss, tok, ',')) break;
                        std::int64_t nb  = std::stoll(tok);
                        e.sparse_map.push_back({off, nb});
                    }
                } catch (...) {
                    // Malformed sparse map — discard partial parse
                    e.sparse_map.clear();
                    e.is_sparse = false;
                }
                e.asize = 0;
                for (auto& sm : e.sparse_map) e.asize += sm.numbytes;
            }
        }
        e.asize = e.size; // may be overridden by sparse
    }

    void read_gnu_sparse_map(Entry& e, const Block& blk) {
        // Read sparse map from oldgnu overlay
        const auto& g = blk.oldgnu;
        for (int i = 0; i < 4; ++i) {
            auto off = static_cast<std::int64_t>(read_octal({g.sp[i].offset,  12}));
            auto nb  = static_cast<std::int64_t>(read_octal({g.sp[i].numbytes,12}));
            if (nb == 0) break;
            e.sparse_map.push_back({off, nb});
        }
        // Extension sparse blocks
        bool isext = g.isextended != 0;
        while (isext) {
            Block ext{};
            buf_.read_block(*stream_, ext);
            const auto& sp = ext.sparse;
            for (int i = 0; i < 21; ++i) {
                auto off = static_cast<std::int64_t>(read_octal({sp.sp[i].offset,   12}));
                auto nb  = static_cast<std::int64_t>(read_octal({sp.sp[i].numbytes, 12}));
                if (nb == 0) break;
                e.sparse_map.push_back({off, nb});
            }
            isext = sp.isextended != 0;
        }
        // asize = sum of numbytes
        e.asize = 0;
        for (auto& sm : e.sparse_map) e.asize += sm.numbytes;
        e.is_sparse = true;
    }

    ArchiveStream* stream_;
    BlockBuffer    buf_;
    bool           ignore_zeros_ = false;
};

// ── Archive writer ────────────────────────────────────────────────────────────

class ArchiveWriter {
public:
    ArchiveWriter(ArchiveStream& s, int blocking, Format fmt)
        : stream_(&s), buf_(blocking), fmt_(fmt) {}

    ~ArchiveWriter() { /* caller must call finish() */ }

    /// Switch to a new volume stream (multi-volume create). Resets block counter.
    void swap_stream(ArchiveStream& s) {
        stream_ = &s;
        buf_.reset_at_block(0);
    }

    // Set optional owner/group remapping maps (pointers may be null).
    void set_owner_map(const std::map<std::string,std::string>* om,
                       const std::map<std::string,std::string>* gm) {
        owner_map_ = om; group_map_ = gm;
    }

    /// Enable sidecar index collection (pointer owned by caller).
    void set_index(ArchiveIndex* idx) { index_ = idx; }

    // Write two EOF blocks + flush
    void finish() {
        Block zero{};
        buf_.write_block(*stream_, zero);
        buf_.write_block(*stream_, zero);
        buf_.flush(*stream_);
    }

    // Add a single file/dir/symlink/device/hardlink from the filesystem
    bool add_path(const std::string& archname, const std::string& fspath,
                  const Config& cfg) {
        struct stat st{};
        int rc = cfg.dereference ? ::stat(fspath.c_str(), &st)
                                 : ::lstat(fspath.c_str(), &st);
        if (rc < 0) {
            print(stderr, "mutar: {}: {}\n", fspath, std::strerror(errno));
            return false;
        }

        Entry e;
        e.name     = archname;
        e.mode     = st.st_mode & 07777;
        e.uid      = static_cast<unsigned>(st.st_uid);
        e.gid      = static_cast<unsigned>(st.st_gid);
        e.mtime    = st.st_mtim.tv_sec;
        e.mtime_nsec = static_cast<long>(st.st_mtim.tv_nsec);
        e.atime    = st.st_atim.tv_sec;
        e.ctime    = st.st_ctim.tv_sec;
        e.size     = 0;
        e.fmt      = fmt_;

        // Lookup uname/gname
        if (struct passwd* pw = ::getpwuid(st.st_uid)) e.uname = pw->pw_name;
        if (struct group*  gr = ::getgrgid(st.st_gid)) e.gname = gr->gr_name;
        if (cfg.numeric_owner) { e.uname.clear(); e.gname.clear(); }
        if (!cfg.owner.empty()) e.uname = cfg.owner;
        if (!cfg.group.empty()) e.gname = cfg.group;
        // Apply --owner-map / --group-map
        if (owner_map_ || group_map_) {
            static const std::map<std::string,std::string> empty_map{};
            apply_owner_map(e,
                owner_map_ ? *owner_map_ : empty_map,
                group_map_ ? *group_map_ : empty_map);
        }

        // --mtime: override mtime for all entry types
        if (!cfg.mtime.empty()) {
            std::time_t mt = parse_date_string(cfg.mtime);
            if (mt != (std::time_t)-1) {
                // --clamp-mtime: only override if file's mtime is NEWER than given value
                if (!cfg.clamp_mtime || e.mtime > mt) {
                    e.mtime = mt; e.mtime_nsec = 0;
                }
            }
        }
        // --mode: override permissions for all entry types (octal string)
        if (!cfg.mode_str.empty()) {
            char* endp = nullptr;
            long m = std::strtol(cfg.mode_str.c_str(), &endp, 8);
            if (endp && *endp == '\0' && m >= 0) e.mode = static_cast<unsigned>(m) & 07777;
        }

        // Collect xattrs / ACLs into e.pax_attrs as SCHILY.* (before any write path).
        const bool is_dir = S_ISDIR(st.st_mode);
        collect_xattrs(e, fspath, cfg);
        collect_acls(e, fspath, cfg, is_dir);

        if (S_ISREG(st.st_mode)) {
            e.typeflag = REGTYPE;
            e.size     = st.st_size;
            // --sparse: use SEEK_DATA/SEEK_HOLE to detect holes
            if (cfg.sparse && st.st_size > 0)
                return write_sparse(e, fspath, cfg, st);
            return write_regular(e, fspath, cfg, st);
        } else if (is_dir) {
            e.typeflag = DIRTYPE;
            if (!e.name.empty() && e.name.back() != '/') e.name += '/';
            e.size = 0;
            return write_header_only(e, cfg);
        } else if (S_ISLNK(st.st_mode)) {
            char buf[PATH_MAX + 1]{};
            ssize_t n = ::readlink(fspath.c_str(), buf, PATH_MAX);
            if (n < 0) { print(stderr, "mutar: readlink {}: {}\n", fspath, std::strerror(errno)); return false; }
            buf[n] = '\0';
            e.typeflag = SYMTYPE;
            e.linkname = buf;
            e.size     = 0;
            return write_header_only(e, cfg);
        } else if (S_ISCHR(st.st_mode)) {
            e.typeflag = CHRTYPE;
            e.devmajor = static_cast<unsigned>(::major(st.st_rdev));
            e.devminor = static_cast<unsigned>(::minor(st.st_rdev));
            e.size     = 0;
            return write_header_only(e, cfg);
        } else if (S_ISBLK(st.st_mode)) {
            e.typeflag = BLKTYPE;
            e.devmajor = static_cast<unsigned>(::major(st.st_rdev));
            e.devminor = static_cast<unsigned>(::minor(st.st_rdev));
            e.size     = 0;
            return write_header_only(e, cfg);
        } else if (S_ISFIFO(st.st_mode)) {
            e.typeflag = FIFOTYPE;
            e.size     = 0;
            return write_header_only(e, cfg);
        }
        // Ignore sockets, etc.
        return true;
    }

    std::int64_t block_no() const { return buf_.block_no(); }

private:
    /// Record a member in the sidecar index at the current write position
    /// (before any LongName/PAX extension headers for this member).
    void record_index_entry(const Entry& e) {
        if (!index_) return;
        IndexEntry ie;
        ie.name     = e.name;
        ie.offset   = static_cast<std::uint64_t>(buf_.block_no()) * BLOCKSIZE;
        ie.size     = e.is_sparse ? e.real_size : e.size;
        ie.typeflag = e.typeflag ? e.typeflag : REGTYPE;
        ie.mtime    = e.mtime;
        ie.mode     = e.mode;
        ie.uid      = e.uid;
        ie.gid      = e.gid;
        index_->add(std::move(ie));
    }

    // Write a GNU LongName or LongLink extension record
    void write_long_ext(char type, std::string_view longname) {
        Entry meta;
        meta.typeflag = type;
        meta.name     = "././@LongLink";
        meta.size     = static_cast<std::int64_t>(longname.size() + 1); // +1 for NUL
        meta.mode     = 0;
        meta.uid      = 0;
        meta.gid      = 0;
        meta.mtime    = 0;
        meta.fmt      = Format::GNU;
        Block hblk = encode_header(meta, Config{});  // V7-like meta header
        // Override magic to GNU
        std::memcpy(hblk.header.magic, "ustar  ", 8);
        write_checksum(hblk);
        buf_.write_block(*stream_, hblk);

        // Write data blocks
        std::string data(longname);
        data.push_back('\0');
        write_data_bytes(data.data(), data.size());
    }

    // Write PAX extended header (honours --pax-option delete=KEYWORD).
    // Also emits SCHILY.xattr.* / SCHILY.acl.* collected into e.pax_attrs.
    void write_pax_header(const Entry& e, bool need_path, bool need_link,
                          bool need_size, bool need_time, const Config& cfg) {
        std::string pax_data;
        if (need_path)
            pax_append_if(pax_data, cfg, "path", e.name);
        if (need_link)
            pax_append_if(pax_data, cfg, "linkpath", e.linkname);
        if (need_size)
            pax_append_if(pax_data, cfg, "size", std::to_string(e.size));
        if (e.uid > 0777777)
            pax_append_if(pax_data, cfg, "uid", std::to_string(e.uid));
        if (e.gid > 0777777)
            pax_append_if(pax_data, cfg, "gid", std::to_string(e.gid));
        if (need_time && e.mtime_nsec != 0) {
            pax_append_if(pax_data, cfg, "mtime",
                std::format("{}.{:09d}", e.mtime, e.mtime_nsec));
        }
        if (!e.uname.empty())
            pax_append_if(pax_data, cfg, "uname", e.uname);
        if (!e.gname.empty())
            pax_append_if(pax_data, cfg, "gname", e.gname);
        pax_append_schily(pax_data, cfg, e);

        if (pax_data.empty()) return;

        Entry meta;
        meta.typeflag = XHDTYPE;
        meta.name     = std::format("PaxHeaders/{}", e.name);
        if (meta.name.size() > 99) meta.name = meta.name.substr(0, 99);
        meta.size     = static_cast<std::int64_t>(pax_data.size());
        meta.mtime    = e.mtime;
        meta.mode     = 0600;
        meta.fmt      = Format::PAX;

        Config tmp_cfg;
        tmp_cfg.fmt = Format::USTAR;
        Block hblk = encode_header(meta, tmp_cfg);
        buf_.write_block(*stream_, hblk);
        write_data_bytes(pax_data.data(), pax_data.size());
    }

public:
    bool write_header_only(Entry& e, const Config& cfg) {
        record_index_entry(e);
        maybe_write_extensions(e, cfg);
        Block hblk = encode_header(e, cfg);
        buf_.write_block(*stream_, hblk);
        return true;
    }

    void write_data_bytes(const char* data, std::size_t sz) {
        std::size_t offset = 0;
        while (offset < sz) {
            Block blk{};
            std::size_t chunk = std::min(sz - offset, BLOCKSIZE);
            std::memcpy(blk.buffer, data + offset, chunk);
            buf_.write_block(*stream_, blk);
            offset += BLOCKSIZE; // advance full block even if partial
        }
    }

private:

    void maybe_write_extensions(const Entry& e, const Config& cfg) {
        bool long_name = e.name.size() > 100;
        bool long_link = e.linkname.size() > 100;
        bool need_schily = entry_has_schily_pax(e);
        bool need_pax = long_name || long_link || e.size > 077777777777LL
                        || e.mtime_nsec != 0 || need_schily;

        // SCHILY xattr/ACL records require a PAX 'x' header even under GNU format.
        if (fmt_ == Format::PAX || fmt_ == Format::USTAR || need_schily) {
            if (need_pax)
                write_pax_header(e, long_name, long_link,
                                 e.size > 077777777777LL, e.mtime_nsec != 0, cfg);
        } else {
            // GNU LongName/LongLink
            if (long_name) write_long_ext(GNUTYPE_LONGNAME, e.name);
            if (long_link) write_long_ext(GNUTYPE_LONGLINK, e.linkname);
        }
    }

    bool write_regular(Entry& e, const std::string& fspath,
                       const Config& cfg, const struct stat& st) {
        record_index_entry(e);
        maybe_write_extensions(e, cfg);
        Block hblk = encode_header(e, cfg);
        buf_.write_block(*stream_, hblk);

        int fd = ::open(fspath.c_str(), O_RDONLY);
        if (fd < 0) {
            print(stderr, "mutar: {}: {}\n", fspath, std::strerror(errno));
            write_data_zeros(e.size);
            return false;
        }

        std::int64_t remaining = e.size;
        char blkbuf[BLOCKSIZE];
        bool io_error = false;
        while (remaining > 0 && !io_error) {
            std::int64_t to_read = std::min(remaining, static_cast<std::int64_t>(BLOCKSIZE));
            std::memset(blkbuf, 0, BLOCKSIZE);
            ssize_t got = 0;
            while (got < to_read) {
                ssize_t n = ::read(fd, blkbuf + got, static_cast<std::size_t>(to_read - got));
                if (n < 0) { if (errno == EINTR) continue; io_error = true; break; }
                if (n == 0) { io_error = true; break; }  // EOF before expected size
                got += n;
            }
            if (!io_error || got > 0) {
                Block b{};
                std::memcpy(b.buffer, blkbuf, BLOCKSIZE);
                buf_.write_block(*stream_, b);
                remaining -= got;
            }
        }
        ::close(fd);
        // --atime-preserve: restore original access time after reading the file
        if (cfg.atime_preserve) {
            struct timespec ts[2];
            ts[0] = {.tv_sec = st.st_atim.tv_sec, .tv_nsec = st.st_atim.tv_nsec};
            ts[1] = {.tv_sec = st.st_mtim.tv_sec, .tv_nsec = st.st_mtim.tv_nsec};
            ::utimensat(AT_FDCWD, fspath.c_str(), ts, AT_SYMLINK_NOFOLLOW);
        }
        // Pad any remaining blocks (file shorter than stated size)
        write_data_zeros(remaining);
        return true;
    }

    // Write a GNU sparse ('S' type) entry using SEEK_DATA/SEEK_HOLE hole detection.
    bool write_sparse(Entry& e, const std::string& fspath,
                      const Config& cfg, const struct stat& st) {
        int fd = ::open(fspath.c_str(), O_RDONLY);
        if (fd < 0) { print(stderr, "mutar: {}: {}\n", fspath, std::strerror(errno)); return false; }

        auto segs = detect_sparse_segments(fd, e.size, cfg.hole_detection);

        // No actual holes → fall back to regular write
        if (segs.size() == 1 && segs[0].offset == 0 && segs[0].length == e.size) {
            ::close(fd);
            return write_regular(e, fspath, cfg, st);
        }

        // Compute archived size = sum of data segment lengths
        std::int64_t arch_size = 0;
        for (auto& seg : segs) arch_size += seg.length;

        std::int64_t logical_size = e.size;
        e.is_sparse  = true;
        e.real_size  = logical_size;
        e.asize      = arch_size;
        e.size       = arch_size;  // header 'size' = archived data size
        e.sparse_map.clear();
        for (auto& seg : segs)
            e.sparse_map.push_back(SparseMap{seg.offset, seg.length});

        // Index before any extension/header blocks for this member.
        {
            Entry idx_e = e;
            idx_e.size = logical_size;
            idx_e.typeflag = GNUTYPE_SPARSE;
            record_index_entry(idx_e);
        }

        // GNU sparse format using PAX extended header (GNU extensions within PAX 'x' header)
        if (fmt_ == Format::PAX) {
            e.typeflag = REGTYPE;

            // Build "offset,length[,offset,length...]" sparse map string
            std::string sparse_map_val;
            for (std::size_t si = 0; si < segs.size(); ++si) {
                if (si > 0) sparse_map_val += ',';
                sparse_map_val += std::to_string(segs[si].offset);
                sparse_map_val += ',';
                sparse_map_val += std::to_string(segs[si].length);
            }

            // Build PAX extended header data (honours --pax-option delete=KEYWORD)
            std::string pax_data;
            pax_append_if(pax_data, cfg, "GNU.sparse.major",    "1");
            pax_append_if(pax_data, cfg, "GNU.sparse.minor",    "0");
            pax_append_if(pax_data, cfg, "GNU.sparse.name",     e.name);
            pax_append_if(pax_data, cfg, "GNU.sparse.realsize",
                          std::to_string(logical_size));
            pax_append_if(pax_data, cfg, "GNU.sparse.map",      sparse_map_val);
            if (e.name.size() > 100)
                pax_append_if(pax_data, cfg, "path", e.name);
            if (e.mtime_nsec != 0)
                pax_append_if(pax_data, cfg, "mtime",
                    std::format("{}.{:09d}", e.mtime, e.mtime_nsec));
            if (!e.uname.empty())
                pax_append_if(pax_data, cfg, "uname", e.uname);
            if (!e.gname.empty())
                pax_append_if(pax_data, cfg, "gname", e.gname);
            pax_append_schily(pax_data, cfg, e);

            // Emit PAX 'x' extended header block
            Entry pax_meta;
            pax_meta.typeflag = XHDTYPE;
            pax_meta.name     = std::format("PaxHeaders/{}", e.name);
            if (pax_meta.name.size() > 99) pax_meta.name = pax_meta.name.substr(0, 99);
            pax_meta.size     = static_cast<std::int64_t>(pax_data.size());
            pax_meta.mtime    = e.mtime;
            pax_meta.mode     = 0600;
            pax_meta.fmt      = Format::PAX;
            {
                Config tmp_cfg;
                tmp_cfg.fmt = Format::USTAR;
                Block phblk = encode_header(pax_meta, tmp_cfg);
                buf_.write_block(*stream_, phblk);
                write_data_bytes(pax_data.data(), pax_data.size());
            }

            // Regular data header (size = archived bytes)
            Block hblk = encode_header(e, cfg);
            write_checksum(hblk);
            buf_.write_block(*stream_, hblk);

            // Write data: concatenate segment bytes into one stream, pad at end
            bool ok = true;
            Block b{};
            std::memset(b.buffer, 0, BLOCKSIZE);
            std::size_t buf_filled = 0;

            for (auto& seg : segs) {
                if (::lseek(fd, static_cast<off_t>(seg.offset), SEEK_SET) < 0) { ok = false; break; }
                std::int64_t remaining = seg.length;
                while (remaining > 0) {
                    std::size_t space = BLOCKSIZE - buf_filled;
                    if (space == 0) {
                        buf_.write_block(*stream_, b);
                        std::memset(b.buffer, 0, BLOCKSIZE);
                        buf_filled = 0;
                        space = BLOCKSIZE;
                    }
                    std::int64_t to_read = std::min<std::int64_t>(
                        remaining, static_cast<std::int64_t>(space));
                    ssize_t got = 0;
                    while (got < to_read) {
                        ssize_t n = ::read(fd, b.buffer + buf_filled + got,
                                           static_cast<std::size_t>(to_read - got));
                        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
                        if (n == 0) { ok = false; break; }
                        got += n;
                    }
                    if (!ok) break;
                    buf_filled += static_cast<std::size_t>(got);
                    remaining  -= got;
                }
                if (!ok) break;
            }
            if (ok && buf_filled > 0)
                buf_.write_block(*stream_, b);
            // Pad remaining blocks to fill the full logical block count
            std::int64_t full_blocks  = arch_size / static_cast<std::int64_t>(BLOCKSIZE);
            std::int64_t total_blocks = (arch_size + static_cast<std::int64_t>(BLOCKSIZE) - 1)
                                        / static_cast<std::int64_t>(BLOCKSIZE);
            std::int64_t written_blocks = full_blocks + (buf_filled > 0 ? 1 : 0);
            for (std::int64_t pi = written_blocks; pi < total_blocks; ++pi) {
                Block zb{}; buf_.write_block(*stream_, zb);
            }

            ::close(fd);
            if (cfg.atime_preserve) {
                struct timespec ts[2];
                ts[0] = {.tv_sec = st.st_atim.tv_sec, .tv_nsec = st.st_atim.tv_nsec};
                ts[1] = {.tv_sec = st.st_mtim.tv_sec, .tv_nsec = st.st_mtim.tv_nsec};
                ::utimensat(AT_FDCWD, fspath.c_str(), ts, AT_SYMLINK_NOFOLLOW);
            }
            return ok;
        }

        // GNU sparse ('S' type) format for non-PAX modes
        e.typeflag   = GNUTYPE_SPARSE;

        // Write GNU LongName extension if needed (before the sparse header)
        maybe_write_extensions(e, cfg);

        // Build the GNU sparse ('S') header block
        Block hblk = encode_header(e, cfg);
        // Encode realsize and sparse map into the OldGNU overlay
        write_octal(hblk.oldgnu.realsize, 12, static_cast<std::uint64_t>(logical_size));
        std::size_t seg_idx = 0;
        std::size_t n_inline = std::min(segs.size(), std::size_t{4});
        for (std::size_t i = 0; i < 4; ++i) {
            if (i < n_inline) {
                write_octal(hblk.oldgnu.sp[i].offset,   12, static_cast<std::uint64_t>(segs[i].offset));
                write_octal(hblk.oldgnu.sp[i].numbytes, 12, static_cast<std::uint64_t>(segs[i].length));
            }
        }
        seg_idx = n_inline;
        hblk.oldgnu.isextended = (segs.size() > 4) ? 1 : 0;
        // GNU tar compatibility: write a (realsize, 0) sentinel in the first
        // unused sparse slot of the main header.  Older tar extractors use this
        // sentinel to determine the logical file size rather than the realsize
        // field; without it they stop at the end of the last data segment.
        if (n_inline < 4) {
            write_octal(hblk.oldgnu.sp[n_inline].offset,   12, static_cast<std::uint64_t>(logical_size));
            write_octal(hblk.oldgnu.sp[n_inline].numbytes, 12, 0);
        }
        write_checksum(hblk);
        buf_.write_block(*stream_, hblk);

        // Write extension sparse header blocks (21 entries each)
        while (seg_idx < segs.size()) {
            Block ext{};
            std::size_t n_ext = std::min(segs.size() - seg_idx, std::size_t{21});
            for (std::size_t i = 0; i < n_ext; ++i) {
                write_octal(ext.sparse.sp[i].offset,   12, static_cast<std::uint64_t>(segs[seg_idx + i].offset));
                write_octal(ext.sparse.sp[i].numbytes, 12, static_cast<std::uint64_t>(segs[seg_idx + i].length));
            }
            seg_idx += n_ext;
            ext.sparse.isextended = (seg_idx < segs.size()) ? 1 : 0;
            // In the last extension block, write sentinel in first unused slot
            if (!ext.sparse.isextended && n_ext < 21) {
                write_octal(ext.sparse.sp[n_ext].offset,   12, static_cast<std::uint64_t>(logical_size));
                write_octal(ext.sparse.sp[n_ext].numbytes, 12, 0);
            }
            buf_.write_block(*stream_, ext);
        }

        // Write data: concatenate segment bytes into one logical stream;
        // carry partial blocks across segments and pad with zeros only once at the end.
        bool ok = true;
        Block b{};
        std::memset(b.buffer, 0, BLOCKSIZE);
        std::size_t buf_filled = 0;

        for (auto& seg : segs) {
            if (::lseek(fd, static_cast<off_t>(seg.offset), SEEK_SET) < 0) { ok = false; break; }
            std::int64_t remaining = seg.length;
            while (remaining > 0) {
                std::size_t space = BLOCKSIZE - buf_filled;
                if (space == 0) {
                    buf_.write_block(*stream_, b);
                    std::memset(b.buffer, 0, BLOCKSIZE);
                    buf_filled = 0;
                    space = BLOCKSIZE;
                }
                std::int64_t to_read = std::min<std::int64_t>(
                    remaining, static_cast<std::int64_t>(space));
                ssize_t got = 0;
                while (got < to_read) {
                    ssize_t n = ::read(fd, b.buffer + buf_filled + got,
                                       static_cast<std::size_t>(to_read - got));
                    if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
                    if (n == 0) { ok = false; break; } // unexpected EOF
                    got += n;
                }
                if (!ok) break;
                buf_filled += static_cast<std::size_t>(got);
                remaining  -= static_cast<std::int64_t>(got);
                if (buf_filled == BLOCKSIZE) {
                    buf_.write_block(*stream_, b);
                    std::memset(b.buffer, 0, BLOCKSIZE);
                    buf_filled = 0;
                }
            }
            if (!ok) break;
        }

        // Flush the last (potentially partial) block, padded with zeros, once.
        if (ok && buf_filled > 0) {
            buf_.write_block(*stream_, b);
        }
        ::close(fd);

        // --atime-preserve: restore original access time
        if (cfg.atime_preserve) {
            struct timespec ts[2];
            ts[0] = {.tv_sec = st.st_atim.tv_sec, .tv_nsec = st.st_atim.tv_nsec};
            ts[1] = {.tv_sec = st.st_mtim.tv_sec, .tv_nsec = st.st_mtim.tv_nsec};
            ::utimensat(AT_FDCWD, fspath.c_str(), ts, AT_SYMLINK_NOFOLLOW);
        }
        return ok;
    }

    void write_data_zeros(std::int64_t sz) {
        Block zero{};
        std::int64_t blocks = (sz + BLOCKSIZE - 1) / BLOCKSIZE;
        for (std::int64_t i = 0; i < blocks; ++i)
            buf_.write_block(*stream_, zero);
    }

    ArchiveStream* stream_;
    BlockBuffer    buf_;
    Format         fmt_;
    const std::map<std::string,std::string>* owner_map_ = nullptr;
    const std::map<std::string,std::string>* group_map_ = nullptr;
    ArchiveIndex*  index_ = nullptr;
};

// ── Path utilities ────────────────────────────────────────────────────────────

// Strip leading '/' and '../' components to prevent path traversal
static std::string sanitize_path(std::string_view path, bool absolute_names) {
    if (absolute_names) return std::string(path);
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);

    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        auto part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!components.empty()) components.pop_back();
            } else {
                components.emplace_back(part);
            }
        }
        if (end == path.size()) break;
        start = end + 1;
    }
    if (components.empty()) return ".";
    std::string result;
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (i) result += '/';
        result += components[i];
    }
    return result;
}

// Normalize an archive member name: strip leading ./ (mirrors walk_dir normalization).
// Used when building the 'want' set from user-supplied names so that
// "./file1.txt" and "file1.txt" both match the stored name "file1.txt".
static std::string normalize_member(std::string_view name) {
    while (name.size() > 1 && name.starts_with("./"))
        name.remove_prefix(2);
    return std::string(name);
}

// Strip N leading path components
static std::string strip_components(std::string_view path, int n) {
    for (int i = 0; i < n && !path.empty(); ++i) {
        auto slash = path.find('/');
        if (slash == std::string_view::npos) return {};
        path.remove_prefix(slash + 1);
        while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    }
    return std::string(path);
}

// Check if 'name' matches any exclude pattern, respecting cfg matching flags.
static bool is_excluded(const Config& cfg, std::string_view name) {
    for (const auto& pat : cfg.exclude_patterns) {
        if (!cfg.wildcards) {
            // Literal string matching
            std::string sname(name);
            if (cfg.anchored) {
                if (cfg.ignore_case ? ::strcasecmp(sname.c_str(), pat.c_str()) == 0
                                    : sname == pat)
                    return true;
            } else {
                // Match against the full path AND each path component
                if (cfg.ignore_case ? ::strcasecmp(sname.c_str(), pat.c_str()) == 0
                                    : sname == pat)
                    return true;
                std::string_view rem = name;
                while (!rem.empty()) {
                    auto slash = rem.find('/');
                    std::string_view comp = (slash == std::string_view::npos) ? rem : rem.substr(0, slash);
                    std::string scomp(comp);
                    if (!comp.empty() && (cfg.ignore_case
                            ? ::strcasecmp(scomp.c_str(), pat.c_str()) == 0
                            : scomp == pat))
                        return true;
                    if (slash == std::string_view::npos) break;
                    rem = rem.substr(slash + 1);
                }
            }
        } else {
            // Wildcard (fnmatch) matching
            int flags = FNM_LEADING_DIR;
            if (!cfg.wildcards_match_slash) flags |= FNM_PATHNAME;
            if (cfg.ignore_case)            flags |= FNM_CASEFOLD;

            // Full path match
            if (::fnmatch(pat.c_str(), std::string(name).c_str(), flags) == 0)
                return true;
            // If not anchored or pattern has no '/', also match against path components
            if (!cfg.anchored || pat.find('/') == std::string::npos) {
                int comp_flags = flags & ~FNM_PATHNAME;
                std::string_view rem = name;
                while (!rem.empty()) {
                    auto slash = rem.find('/');
                    std::string_view comp = (slash == std::string_view::npos) ? rem : rem.substr(0, slash);
                    if (!comp.empty() &&
                        ::fnmatch(pat.c_str(), std::string(comp).c_str(), comp_flags) == 0)
                        return true;
                    if (slash == std::string_view::npos) break;
                    rem = rem.substr(slash + 1);
                }
            }
        }
    }
    return false;
}

// ── Date parsing helper (--newer / --mtime) ──────────────────────────────────
// Accepts: file path (uses its mtime), ISO date, or seconds-since-epoch.
static std::time_t parse_date_string(const std::string& s) {
    if (s.empty()) return (std::time_t)-1;
    // Try as a file path first
    struct stat st{};
    if (::stat(s.c_str(), &st) == 0) return st.st_mtime;
    // Common ISO / human-readable date formats
    const char* fmts[] = {
        "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%d",          "%d %b %Y %H:%M:%S",
        "%d %b %Y",          nullptr
    };
    for (const char** f = fmts; *f; ++f) {
        struct tm tm{}; char* end = ::strptime(s.c_str(), *f, &tm);
        if (end && (*end == '\0' || *end == 'Z')) {
            tm.tm_isdst = -1;
            // 'Z' means UTC — use timegm() to avoid local-timezone offset
            return (*end == 'Z') ? ::timegm(&tm) : ::mktime(&tm);
        }
    }
    // Seconds since epoch
    char* endp = nullptr; errno = 0;
    long long v = std::strtoll(s.c_str(), &endp, 10);
    if (!errno && endp && *endp == '\0') return static_cast<std::time_t>(v);
    return (std::time_t)-1;
}

// ── Name transform helper (--transform / --xform) ────────────────────────────
// Supports semicolon-separated  s/pattern/replacement/[flags]  expressions.
static std::string apply_transform(const std::string& name, const std::string& expr) {
    if (expr.empty()) return name;
    std::string result = name;
    std::string_view rest(expr);
    while (!rest.empty()) {
        while (!rest.empty() && (rest[0] == ';' || rest[0] == ' ')) rest.remove_prefix(1);
        if (rest.size() < 4 || rest[0] != 's') break;
        char delim = rest[1]; rest.remove_prefix(2);
        auto e1 = rest.find(delim); if (e1 == std::string_view::npos) break;
        std::string pat(rest.substr(0, e1)); rest.remove_prefix(e1 + 1);
        auto e2 = rest.find(delim); if (e2 == std::string_view::npos) break;
        std::string rep(rest.substr(0, e2)); rest.remove_prefix(e2 + 1);
        std::string flags;
        while (!rest.empty() && rest[0] != ';' && rest[0] != ' ') { flags += rest[0]; rest.remove_prefix(1); }
        try {
            auto rf = std::regex::ECMAScript;
            if (flags.find('i') != std::string::npos) rf |= std::regex::icase;
            std::regex re(pat, rf);
            bool global = flags.find('g') != std::string::npos;
            result = std::regex_replace(result, re, rep,
                global ? std::regex_constants::match_default
                       : std::regex_constants::format_first_only);
        } catch (const std::regex_error&) { return name; }
    }
    return result;
}

// ── Sparse-segment detector (SEEK_DATA / SEEK_HOLE) ──────────────────────────

// Detect sparse segments in fd.
// hole_detection="seek" (default): use SEEK_DATA/SEEK_HOLE kernel interface.
// hole_detection="raw": scan the file byte-by-byte for zero regions (fallback).
static std::vector<SparseSegment> detect_sparse_segments(int fd, std::int64_t file_size,
                                                          std::string_view hole_detection) {
    std::vector<SparseSegment> segs;
    if (file_size <= 0) return segs;

    // "seek" method: use SEEK_DATA/SEEK_HOLE kernel interface (preferred)
    if (hole_detection != "raw") {
#ifdef SEEK_DATA
        std::int64_t pos = 0;
        while (pos < file_size) {
            off_t data_off = ::lseek(fd, static_cast<off_t>(pos), SEEK_DATA);
            if (data_off < 0) break; // ENXIO = no more data segments
            off_t hole_off = ::lseek(fd, data_off, SEEK_HOLE);
            if (hole_off < 0) hole_off = static_cast<off_t>(file_size);
            if (hole_off > static_cast<off_t>(file_size)) hole_off = static_cast<off_t>(file_size);
            if (hole_off > data_off)
                segs.push_back({static_cast<std::int64_t>(data_off),
                                static_cast<std::int64_t>(hole_off - data_off)});
            pos = static_cast<std::int64_t>(hole_off);
        }
        if (!segs.empty()) { ::lseek(fd, 0, SEEK_SET); return segs; }
#endif
    }

    // "raw" method (or SEEK_DATA not available): scan for zero pages as holes
    {
        constexpr std::int64_t PAGE = 4096;
        std::vector<char> page(static_cast<std::size_t>(PAGE));
        ::lseek(fd, 0, SEEK_SET);
        std::int64_t off = 0;
        std::int64_t seg_start = -1;
        while (off < file_size) {
            std::int64_t to_read = std::min(PAGE, file_size - off);
            ssize_t n = ::read(fd, page.data(), static_cast<std::size_t>(to_read));
            if (n <= 0) break;
            bool all_zero = true;
            for (ssize_t i = 0; i < n; ++i) {
                if (page[static_cast<std::size_t>(i)] != '\0') { all_zero = false; break; }
            }
            if (!all_zero) {
                if (seg_start < 0) seg_start = off; // start of data segment
            } else {
                if (seg_start >= 0) {
                    segs.push_back({seg_start, off - seg_start});
                    seg_start = -1;
                }
            }
            off += n;
        }
        if (seg_start >= 0)
            segs.push_back({seg_start, file_size - seg_start});
    }

    if (segs.empty()) segs.push_back({0, file_size});
    ::lseek(fd, 0, SEEK_SET);
    return segs;
}

// Recursive directory walker
static void walk_dir(const std::string& base_dir, const std::string& relpath,
                     const Config& cfg,
                     std::function<void(const std::string& archname, const std::string& fspath)> cb,
                     dev_t same_dev = static_cast<dev_t>(-1)) {
    namespace fs = std::filesystem;
    std::string full = base_dir.empty() ? relpath : base_dir + "/" + relpath;

    struct stat st{};
    if (::lstat(full.c_str(), &st) < 0) {
        mutar_warn(cfg, "failed-read", std::format("{}: {}", full, std::strerror(errno)));
        return;
    }

    std::string archname = relpath;
    if (!cfg.directory.empty() && archname.starts_with(cfg.directory))
        archname = archname.substr(cfg.directory.size());
    if (archname.starts_with("/")) archname = archname.substr(1);
    // Normalize: strip leading ./ (e.g. "./dir1/f" → "dir1/f")
    while (archname.size() > 1 && archname.starts_with("./"))
        archname = archname.substr(2);

    if (archname.empty()) archname = ".";

    if (is_excluded(cfg, archname)) {
        if (cfg.show_omitted_dirs && S_ISDIR(st.st_mode))
            print(stderr, "mutar: {}/\n", archname);
        return;
    }

    cb(archname, full);

    if (S_ISDIR(st.st_mode) && !cfg.no_recursion) {
        // Establish root device on first descent
        dev_t this_dev = (same_dev == static_cast<dev_t>(-1)) ? st.st_dev : same_dev;
        // Walk contents
        DIR* d = ::opendir(full.c_str());
        if (!d) { print(stderr, "mutar: opendir {}: {}\n", full, std::strerror(errno)); return; }
        std::vector<std::string> entries;
        while (struct dirent* de = ::readdir(d)) {
            std::string_view dname(de->d_name);
            if (dname == "." || dname == "..") continue;
            entries.emplace_back(dname);
        }
        ::closedir(d);
        if (cfg.sort_order == "name") std::ranges::sort(entries);

        // ── Per-directory exclusion: cache-tag directories ────────────────────
        // --exclude-caches-all / --exclude-caches-under: skip ALL contents
        //   (directory entry itself was already emitted by cb above).
        // --exclude-caches: skip all contents EXCEPT the CACHEDIR.TAG file.
        bool cache_tag_present = false;
        if (cfg.exclude_caches || cfg.exclude_caches_all || cfg.exclude_caches_under) {
            struct stat tag_st{};
            if (::stat((full + "/CACHEDIR.TAG").c_str(), &tag_st) == 0)
                cache_tag_present = true;
        }
        if (cache_tag_present && (cfg.exclude_caches_all || cfg.exclude_caches_under))
            return; // skip all contents entirely

        // ── Per-directory exclusion: user-specified tag files ─────────────────
        // --exclude-tag-all / --exclude-tag-under: skip ALL contents.
        // --exclude-tag: skip all contents EXCEPT the matching tag file(s).
        std::vector<std::string> simple_tags_present;
        bool skip_all_tag = false;
        auto scan_tags = [&](const std::vector<std::string>& tags,
                             std::vector<std::string>* keep_out) {
            for (const auto& tag : tags) {
                struct stat tag_st{};
                if (::stat((full + "/" + tag).c_str(), &tag_st) == 0) {
                    if (keep_out) keep_out->push_back(tag);
                    else skip_all_tag = true;
                }
            }
        };
        if (!cfg.exclude_tags.empty())
            scan_tags(cfg.exclude_tags, &simple_tags_present);
        if (!cfg.exclude_tags_all.empty())
            scan_tags(cfg.exclude_tags_all, nullptr);
        if (!cfg.exclude_tags_under.empty())
            scan_tags(cfg.exclude_tags_under, nullptr);
        if (skip_all_tag)
            return; // skip all contents (--exclude-tag-all / --exclude-tag-under)

        // --exclude-vcs-ignores: read .gitignore/.hgignore/.cvsignore/.bzrignore patterns
        std::vector<std::string> vcs_patterns;
        if (cfg.exclude_vcs_ignores) {
            for (const char* ignore_file : {".gitignore", ".hgignore", ".cvsignore", ".bzrignore"}) {
                std::ifstream ifs(full + "/" + ignore_file);
                if (!ifs) continue;
                std::string line;
                while (std::getline(ifs, line)) {
                    // Skip comments and empty lines
                    if (line.empty() || line[0] == '#') continue;
                    // Strip trailing whitespace
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                        line.pop_back();
                    if (!line.empty()) vcs_patterns.push_back(line);
                }
            }
        }

        for (const auto& ent : entries) {
            // --exclude-vcs-ignores: check vcs ignore patterns for this entry
            if (!vcs_patterns.empty()) {
                bool ignored = false;
                for (const auto& vpat : vcs_patterns) {
                    int vflags = 0;
                    if (cfg.ignore_case) vflags |= FNM_CASEFOLD;
                    if (!cfg.wildcards_match_slash) vflags |= FNM_PATHNAME;
                    if (::fnmatch(vpat.c_str(), ent.c_str(), vflags) == 0) { ignored = true; break; }
                }
                if (ignored) continue;
            }
            // --exclude-caches: keep only CACHEDIR.TAG, skip everything else
            if (cache_tag_present && cfg.exclude_caches && ent != "CACHEDIR.TAG")
                continue;
            // --exclude-tag: keep only the matching tag file(s), skip everything else
            if (!simple_tags_present.empty()) {
                bool is_kept_tag = std::find(simple_tags_present.begin(),
                                             simple_tags_present.end(),
                                             ent) != simple_tags_present.end();
                if (!is_kept_tag) continue;
            }
            std::string child = full + "/" + ent;
            // --one-file-system: skip entries on a different filesystem
            if (cfg.one_file_system) {
                struct stat cst{};
                if (::lstat(child.c_str(), &cst) == 0 && cst.st_dev != this_dev) {
                    if (cfg.verbose)
                        mutar_warn(cfg, "xdev",
                                  std::format("{}: file is on a different filesystem; not dumped", child));
                    continue;
                }
            }
            walk_dir("", child, cfg, cb, this_dev);
        }
    }
}

// ── Name quoting (--quoting-style) ────────────────────────────────────────────
// Styles: literal (raw), escape (backslash), c / c-maybe, shell / shell-always.
// Unknown styles fall back to literal. Default (empty) keeps historical raw output.

static bool name_needs_shell_quote(const std::string& name) {
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '.' || c == '/' || c == '_' || c == '-' ||
            c == '+' || c == ',' || c == ':' || c == '@' || c == '%')
            continue;
        return true;
    }
    return name.empty();
}

static bool name_needs_c_quote(const std::string& name) {
    for (unsigned char c : name) {
        if (c <= 0x1f || c == '"' || c == '\\' || c == ' ' || c == '\'' || c >= 0x7f)
            return true;
    }
    return false;
}

static std::string quote_name(const std::string& name, const std::string& style) {
    if (style.empty() || style == "literal")
        return name;

    if (style == "escape") {
        std::string out;
        out.reserve(name.size() + 8);
        for (unsigned char c : name) {
            if (c == ' ' || c == '\\' || c == '\t' || c == '\n' || c == '"' ||
                c == '\'' || c == '$' || c == '`' || c <= 0x1f || c >= 0x7f)
                out.push_back('\\');
            if (c == '\n') { out.push_back('n'); continue; }
            if (c == '\t') { out.push_back('t'); continue; }
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    if (style == "c" || style == "c-maybe") {
        if (style == "c-maybe" && !name_needs_c_quote(name))
            return name;
        std::string out;
        out.reserve(name.size() + 8);
        out.push_back('"');
        for (unsigned char c : name) {
            if (c == '"' || c == '\\') {
                out.push_back('\\');
                out.push_back(static_cast<char>(c));
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c < 0x20 || c >= 0x7f) {
                out += std::format("\\{:03o}", static_cast<unsigned>(c));
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
        return out;
    }

    if (style == "shell" || style == "shell-always") {
        if (style == "shell" && !name_needs_shell_quote(name))
            return name;
        std::string out;
        out.reserve(name.size() + 8);
        out.push_back('\'');
        for (char c : name) {
            if (c == '\'')
                out += "'\\''";
            else
                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    // locale / clocale / unknown → literal
    return name;
}

// ── Backup path (--backup[=CONTROL] / --suffix) ───────────────────────────────
// CONTROL: none/off (no backup), simple/never (suffix), numbered/t (file.~N~),
//          existing/nil (numbered if any .~N~ exists, else simple).

static std::string normalize_backup_control(std::string_view ctrl) {
    if (ctrl.empty()) return "simple";
    if (ctrl == "none" || ctrl == "off") return "none";
    if (ctrl == "t" || ctrl == "numbered") return "numbered";
    if (ctrl == "nil" || ctrl == "existing") return "existing";
    if (ctrl == "never" || ctrl == "simple") return "simple";
    // Unknown CONTROL: treat as simple (GNU falls back similarly for some values)
    return "simple";
}

static bool numbered_backup_exists(const std::string& path) {
    // GNU "existing": numbered if any path.~N~ is present; check .~1~ is enough
    // for the common case (numbered backups are dense from 1).
    std::string first = path + ".~1~";
    return ::access(first.c_str(), F_OK) == 0;
}

static std::string next_numbered_backup(const std::string& path) {
    for (int n = 1; n < 1000000; ++n) {
        std::string cand = path + ".~" + std::to_string(n) + "~";
        if (::access(cand.c_str(), F_OK) != 0)
            return cand;
    }
    // Fallback if absurd number of backups already exist
    return path + ".~999999~";
}

/// Compute backup destination path, or empty string if backups are disabled.
static std::string make_backup_path(const std::string& path, const Config& cfg) {
    if (!cfg.backup)
        return {};
    std::string ctrl = normalize_backup_control(cfg.backup_control);
    if (ctrl == "none")
        return {};
    if (ctrl == "numbered")
        return next_numbered_backup(path);
    if (ctrl == "existing") {
        if (numbered_backup_exists(path))
            return next_numbered_backup(path);
        return path + cfg.backup_suffix;
    }
    // simple
    return path + cfg.backup_suffix;
}

// ── --restrict: reject dangerous option combinations ──────────────────────────
// When --restrict is set, refuse absolute names, --to-command, and multi-volume
// (the multi-volume interactive shell escape is the classic GNU tar concern;
// absolute paths and to-command are additional hardening).

static bool enforce_restrict(const Config& cfg) {
    if (!cfg.restrict_opt)
        return true;
    if (cfg.absolute_names) {
        print(stderr,
              "mutar: --restrict forbids -P / --absolute-names\n");
        return false;
    }
    if (!cfg.to_command.empty()) {
        print(stderr,
              "mutar: --restrict forbids --to-command\n");
        return false;
    }
    if (cfg.multi_volume) {
        print(stderr,
              "mutar: --restrict forbids multi-volume (-M / -L / --info-script)\n");
        return false;
    }
    return true;
}

// Snapshot entry for listed-incremental (mtime + optional device)
struct SnapRec {
    std::int64_t  mtime   = 0;
    std::uint64_t dev     = 0;
    bool          has_dev = false;
};

// ── Verbose line output (matching GNU tar format) ─────────────────────────────

static std::string format_mode(char typeflag, unsigned mode) {
    char buf[11];
    switch (typeflag) {
    case DIRTYPE:  buf[0] = 'd'; break;
    case SYMTYPE:  buf[0] = 'l'; break;
    case LNKTYPE:  buf[0] = 'h'; break;
    case CHRTYPE:  buf[0] = 'c'; break;
    case BLKTYPE:  buf[0] = 'b'; break;
    case FIFOTYPE: buf[0] = 'p'; break;
    default:       buf[0] = '-'; break;
    }
    buf[1] = (mode & 0400) ? 'r' : '-';
    buf[2] = (mode & 0200) ? 'w' : '-';
    buf[3] = (mode & 04100) == 04100 ? 's' : (mode & 0100) ? 'x' : (mode & 04000) ? 'S' : '-';
    buf[4] = (mode & 0040) ? 'r' : '-';
    buf[5] = (mode & 0020) ? 'w' : '-';
    buf[6] = (mode & 02010) == 02010 ? 's' : (mode & 0010) ? 'x' : (mode & 02000) ? 'S' : '-';
    buf[7] = (mode & 0004) ? 'r' : '-';
    buf[8] = (mode & 0002) ? 'w' : '-';
    buf[9] = (mode & 01001) == 01001 ? 't' : (mode & 0001) ? 'x' : (mode & 01000) ? 'T' : '-';
    buf[10] = '\0';
    return buf;
}

static std::string format_time(std::int64_t t, bool utc = false) {
    time_t tt = static_cast<time_t>(t);
    struct tm tm_buf{};
    if (utc) ::gmtime_r(&tt, &tm_buf);
    else     ::localtime_r(&tt, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

// Execute checkpoint action
static void do_checkpoint(const Config& cfg, std::int64_t num) {
    if (cfg.checkpoint_action.empty()) {
        // Default behavior: print a standard checkpoint message
        print(stderr, "mutar: checkpoint {}\n", num);
    } else if (cfg.checkpoint_action == "dot" || cfg.checkpoint_action == ".") {
        std::fputc('.', stderr);
    } else if (cfg.checkpoint_action.starts_with("echo")) {
        std::string msg = cfg.checkpoint_action.size() > 4
            ? cfg.checkpoint_action.substr(5) // skip "echo "
            : std::format("checkpoint {}", num);
        print(stderr, "mutar: {}\n", msg);
    } else if (cfg.checkpoint_action.starts_with("ttyout=")) {
        std::string msg = cfg.checkpoint_action.substr(7);
        FILE* tty = std::fopen("/dev/tty", "w");
        if (tty) { std::fputs(msg.c_str(), tty); std::fclose(tty); }
        else print(stderr, "mutar: checkpoint-action: cannot open /dev/tty\n");
    } else {
        print(stderr, "mutar: checkpoint {}\n", num);
    }
}

static void print_verbose(const Entry& e, const Config& cfg,
                          std::int64_t block_no_val = -1) {
    std::string owner = e.uname.empty() ? std::to_string(e.uid) : e.uname;
    std::string grp   = e.gname.empty() ? std::to_string(e.gid) : e.gname;
    std::string og    = owner + "/" + grp;
    std::string tstr;
    if (cfg.full_time) {
        // Full-resolution timestamp with nanoseconds
        time_t tt = static_cast<time_t>(e.mtime);
        struct tm tm_buf{};
        if (cfg.utc) ::gmtime_r(&tt, &tm_buf);
        else         ::localtime_r(&tt, &tm_buf);
        char buf[48];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        tstr = std::format("{}.{:09}", buf,
                            std::min(static_cast<long>(e.mtime_nsec), 999999999L));
    } else {
        tstr = format_time(e.mtime, cfg.utc);
    }
    std::string line;
    if (cfg.block_number && block_no_val >= 0)
        line += std::format("{}: ", block_no_val);
    const std::string qname = quote_name(e.name, cfg.quoting_style);
    const std::string qlink = (e.typeflag == SYMTYPE)
                                  ? quote_name(e.linkname, cfg.quoting_style)
                                  : std::string{};
    line += std::format("{} {:<17} {:>8} {} {}{}\n",
               format_mode(e.typeflag, e.mode),
               og,
               e.size,
               tstr,
               qname,
               (e.typeflag == SYMTYPE ? " -> " + qlink : ""));
    std::FILE* out = g_index_fp ? g_index_fp : stdout;
    std::fputs(line.c_str(), out);
}

// ── Operations ────────────────────────────────────────────────────────────────

// ── list (-t) ─────────────────────────────────────────────────────────────────

static int op_list(const Config& cfg) {
    // Fast path: list from sidecar index without scanning the archive.
    // Used when --mutar-index is set or ARCHIVE.mutaridx exists.
    // Verbose mode still falls through to sequential scan for full metadata.
    if (!cfg.verbose) {
        std::string idx_path = resolve_mutaridx_path(cfg, false);
        if (!cfg.mutar_index.empty() ||
            (!idx_path.empty() && std::filesystem::exists(idx_path))) {
            if (!idx_path.empty() && std::filesystem::exists(idx_path)) {
                ArchiveIndex aidx;
                auto lr = aidx.load(idx_path);
                if (lr) {
                    std::set<std::string> want;
                    for (const auto& f : cfg.files) want.insert(normalize_member(f));
                    std::int64_t total_bytes = 0;
                    for (const auto& ie : aidx.all()) {
                        std::string display_name = ie.name;
                        if (!cfg.transform_expr.empty())
                            display_name = apply_transform(display_name, cfg.transform_expr);
                        if (cfg.strip_components > 0) {
                            display_name = strip_components(display_name, cfg.strip_components);
                            if (display_name.empty()) continue;
                        }
                        if (!want.empty() && !want.contains(ie.name) &&
                            !want.contains(display_name) &&
                            !want.contains(normalize_member(ie.name)))
                            continue;
                        if (cfg.block_number)
                            print("{}: ", ie.offset / BLOCKSIZE);
                        print("{}\n", quote_name(display_name, cfg.quoting_style));
                        total_bytes += ie.size;
                    }
                    if (cfg.totals)
                        print(stderr, "Total bytes listed: {}\n", total_bytes);
                    return EXIT_SUCCESS;
                }
                print(stderr, "mutar: warning: cannot load index '{}': {}; scanning archive\n",
                      idx_path, lr.error().message);
            }
        }
    }

    auto res = ArchiveStream::open_read(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& s = *res;
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros);

    if (!cfg.index_file.empty()) {
        g_index_fp = std::fopen(cfg.index_file.c_str(), "w");
        if (!g_index_fp)
            print(stderr, "mutar: {}: cannot open index file\n", cfg.index_file);
    }

    // Normalize user-supplied member names so "./file.txt" matches stored "file.txt"
    std::set<std::string> want;
    for (const auto& f : cfg.files) want.insert(normalize_member(f));

    std::int64_t total_bytes = 0;
    std::int64_t checkpoint_count = 0;
    std::map<std::string, int> occurrence_map;
    bool started = cfg.starting_file.empty();
    for (;;) {
        auto [e, ok, eof] = reader.next_entry();
        if (eof) break;
        if (!ok) return EXIT_FAILURE;
        if (cfg.checkpoint > 0 && ++checkpoint_count % cfg.checkpoint == 0)
            do_checkpoint(cfg, checkpoint_count);
        if (!started) {
            if (e.name == cfg.starting_file) started = true;
            else { reader.skip_entry(e); continue; }
        }

        std::string display_name = e.name;
        if (!cfg.transform_expr.empty())
            display_name = apply_transform(display_name, cfg.transform_expr);
        if (cfg.strip_components > 0) {
            display_name = strip_components(display_name, cfg.strip_components);
            if (display_name.empty()) { reader.skip_entry(e); continue; }
        }
        if (!want.empty() && !want.contains(e.name) && !want.contains(display_name)) {
            if (cfg.show_omitted_dirs &&
                (e.typeflag == DIRTYPE || (!e.name.empty() && e.name.back() == '/')))
                print(stderr, "mutar: {}\n", e.name);
            reader.skip_entry(e);
            continue;
        }
        // --occurrence: only process the Nth occurrence of a member name
        if (cfg.occurrence > 0) {
            int cnt = ++occurrence_map[e.name];
            if (cnt != cfg.occurrence) { reader.skip_entry(e); continue; }
        }

        // For display purposes temporarily use transformed name
        Entry disp_e = e; disp_e.name = display_name;
        if (cfg.verbose)
            print_verbose(disp_e, cfg, reader.block_no() - 1);
        else {
            if (cfg.block_number)
                print("{}: ", reader.block_no() - 1);
            print("{}\n", quote_name(display_name, cfg.quoting_style));
        }

        total_bytes += (e.asize + BLOCKSIZE - 1) / BLOCKSIZE * BLOCKSIZE;
        reader.skip_entry(e);
    }
    if (cfg.totals)
        print(stderr, "Total bytes listed: {}\n", total_bytes);
    if (g_index_fp) { std::fclose(g_index_fp); g_index_fp = nullptr; }
    return EXIT_SUCCESS;
}

// ── create (-c) ───────────────────────────────────────────────────────────────

static int op_create(const Config& cfg) {
    // Multi-volume: starting volume number from --volno-file (default 1)
    int vol_num = 1;
    if (cfg.multi_volume && !cfg.volno_file.empty())
        vol_num = read_volno_file(cfg.volno_file);

    Config vol_cfg = cfg;
    if (cfg.multi_volume)
        vol_cfg.archive_file = make_volume_name(cfg.archive_file, vol_num);

    auto res = ArchiveStream::open_write(vol_cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream s = std::move(*res);

    if (cfg.multi_volume && !cfg.volno_file.empty()) {
        if (!write_volno_file(cfg.volno_file, vol_num))
            return EXIT_FAILURE;
    }

    if (!cfg.index_file.empty()) {
        g_index_fp = std::fopen(cfg.index_file.c_str(), "w");
        if (!g_index_fp)
            print(stderr, "mutar: {}: cannot open index file\n", cfg.index_file);
    }

    // --level=0 with --listed-incremental: truncate the snapshot file (start fresh)
    if (cfg.level == 0 && !cfg.listed_incremental.empty()) {
        int snfd = ::open(cfg.listed_incremental.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (snfd >= 0) ::close(snfd);
    }

    Format fmt = cfg.fmt;
    if (fmt == Format::Default) fmt = Format::GNU;
    if (cfg.posix) fmt = Format::PAX;
    if (cfg.old_archive) fmt = Format::V7;

    ArchiveWriter writer(s, cfg.blocking_factor, fmt);

    // Optional sidecar index collection (--write-index / --mutar-index / --seekable)
    ArchiveIndex create_index;
    const bool collect_index =
        cfg.write_index || !cfg.mutar_index.empty() || cfg.seekable;
    if (collect_index)
        writer.set_index(&create_index);

    // Volume label
    if (!cfg.label.empty()) {
        Entry label_e;
        label_e.typeflag = GNUTYPE_VOLHDR;
        label_e.name     = cfg.label;
        label_e.size     = 0;
        label_e.mtime    = std::time(nullptr);
        label_e.fmt      = Format::GNU;
        writer.write_header_only(label_e, cfg);
    }

    // Pre-parse --newer cutoff once
    std::time_t newer_cutoff = (std::time_t)-1;
    if (!cfg.newer_than.empty())
        newer_cutoff = parse_date_string(cfg.newer_than);

    // Track visited inodes for hard links
    std::map<std::pair<dev_t, ino_t>, std::string> seen_inodes;
    // Track archive count per inode for --check-links
    std::map<std::pair<dev_t, ino_t>, int> inode_archive_count;

    // Load --owner-map / --group-map files
    std::map<std::string,std::string> owner_map = load_id_map(cfg.owner_map_file);
    std::map<std::string,std::string> group_map = load_id_map(cfg.group_map_file);
    if (!owner_map.empty() || !group_map.empty())
        writer.set_owner_map(owner_map.empty() ? nullptr : &owner_map,
                             group_map.empty() ? nullptr : &group_map);

    // Load incremental snapshot (for --listed-incremental with --level >= 1)
    // Format: MUTAR_SNAPSHOT_V1 → name\tmtime
    //         MUTAR_SNAPSHOT_V2 → name\tmtime\tdev  (device for --check-device)
    std::map<std::string, SnapRec> snapshot_map;
    bool do_incremental = !cfg.listed_incremental.empty() && cfg.level >= 1;
    if (do_incremental) {
        std::ifstream snap_in(cfg.listed_incremental);
        if (snap_in) {
            std::string header;
            std::getline(snap_in, header);  // consume MUTAR_SNAPSHOT_V* header
            std::string line;
            while (std::getline(snap_in, line)) {
                auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                std::string sname = line.substr(0, tab);
                std::string rest  = line.substr(tab + 1);
                SnapRec rec{};
                auto tab2 = rest.find('\t');
                std::string smts = (tab2 == std::string::npos) ? rest : rest.substr(0, tab2);
                char* endp = nullptr; errno = 0;
                rec.mtime = std::strtoll(smts.c_str(), &endp, 10);
                if (endp == smts.c_str() || (*endp != '\0' && *endp != '\t') || errno != 0) {
                    print(stderr, "mutar: snapshot: malformed mtime entry '{}'; skipping\n", line);
                    continue;
                }
                if (tab2 != std::string::npos) {
                    std::string sdev = rest.substr(tab2 + 1);
                    char* dend = nullptr; errno = 0;
                    unsigned long long dv = std::strtoull(sdev.c_str(), &dend, 10);
                    if (dend != sdev.c_str() && *dend == '\0' && errno == 0) {
                        rec.dev = static_cast<std::uint64_t>(dv);
                        rec.has_dev = true;
                    }
                }
                snapshot_map[sname] = rec;
            }
        } else {
            // Snapshot file missing → archive all (treat as level 0)
            do_incremental = false;
        }
    }
    // Collect entries for snapshot write/update (files + directories + specials)
    std::vector<std::pair<std::string, SnapRec>> snapshot_entries;

    // Change directory if requested
    if (!cfg.directory.empty()) {
        if (::chdir(cfg.directory.c_str()) < 0) {
            print(stderr, "mutar: -C {}: {}\n", cfg.directory, std::strerror(errno));
            return EXIT_FAILURE;
        }
    }

    int exit_code = EXIT_SUCCESS;
    std::int64_t total_bytes = 0;
    std::int64_t checkpoint_count = 0;
    std::vector<std::string> files_to_remove; // for --remove-files
    const std::int64_t max_blocks = tape_max_blocks(cfg);
    const bool do_multivol = cfg.multi_volume && max_blocks > 0;

    // Finish current volume, run info-script, open next volume, swap writer stream.
    auto rotate_volume = [&]() -> bool {
        writer.finish();
        total_bytes += writer.block_no() * static_cast<std::int64_t>(BLOCKSIZE);
        std::string finished_path = vol_cfg.archive_file;
        s.close();

        if (!run_info_script(cfg, finished_path, vol_num, "-c")) {
            exit_code = EXIT_FAILURE;
            return false;
        }

        ++vol_num;
        if (!cfg.volno_file.empty()) {
            if (!write_volno_file(cfg.volno_file, vol_num)) {
                exit_code = EXIT_FAILURE;
                return false;
            }
        }

        vol_cfg.archive_file = make_volume_name(cfg.archive_file, vol_num);
        print(stderr, "mutar: writing volume #{} to {}\n", vol_num, vol_cfg.archive_file);

        auto next = ArchiveStream::open_write(vol_cfg);
        if (!next) {
            print(stderr, "mutar: cannot open volume {}: {}\n",
                  vol_cfg.archive_file, next.error().message);
            exit_code = EXIT_FAILURE;
            return false;
        }
        s = std::move(*next);
        writer.swap_stream(s);
        return true;
    };

    auto add_file = [&](const std::string& raw_archname, const std::string& fspath) {
        // Apply --newer filter (skip files not newer than cutoff)
        if (newer_cutoff != (std::time_t)-1) {
            struct stat fst{};
            if (::lstat(fspath.c_str(), &fst) == 0 && S_ISREG(fst.st_mode)) {
                if (fst.st_mtime <= newer_cutoff) {
                    mutar_warn(cfg, "newer",
                              std::format("{}: file is not newer than cutoff; not dumped", fspath));
                    return;
                }
            }
        }
        // Incremental filter: skip regular files unchanged since last snapshot.
        // Directories/symlinks/specials are always archived (GNU-like dump of
        // metadata); they are still recorded in the snapshot for future use.
        // With --check-device (default), a changed st_dev forces re-archive.
        if (do_incremental) {
            struct stat inc_st{};
            if (::lstat(fspath.c_str(), &inc_st) == 0 && S_ISREG(inc_st.st_mode)) {
                auto sit = snapshot_map.find(raw_archname);
                if (sit != snapshot_map.end() &&
                    inc_st.st_mtime <= sit->second.mtime) {
                    bool dev_changed = false;
                    if (cfg.check_device && sit->second.has_dev &&
                        static_cast<std::uint64_t>(inc_st.st_dev) != sit->second.dev)
                        dev_changed = true;
                    if (!dev_changed)
                        return; // unchanged file, skip
                }
            }
        }

        // Apply --transform to the archive member name
        std::string archname = raw_archname;
        if (!cfg.transform_expr.empty()) {
            archname = apply_transform(archname, cfg.transform_expr);
            if (cfg.show_transformed && archname != raw_archname)
                print(stderr, "mutar: {} -> {}\n", raw_archname, archname);
        }

        // Hard link detection for regular files with nlink > 1
        {
            struct stat hst{};
            if (::lstat(fspath.c_str(), &hst) == 0 &&
                S_ISREG(hst.st_mode) && hst.st_nlink > 1) {
                auto key = std::make_pair(hst.st_dev, hst.st_ino);
                if (cfg.check_links) ++inode_archive_count[key];
                auto it = seen_inodes.find(key);
                if (!cfg.hard_dereference && it != seen_inodes.end()) {
                    // Emit as a hard link entry instead of duplicating data
                    Entry link_e;
                    link_e.typeflag   = LNKTYPE;
                    link_e.name       = archname;
                    link_e.linkname   = it->second;
                    link_e.mode       = hst.st_mode & 07777;
                    link_e.uid        = static_cast<unsigned>(hst.st_uid);
                    link_e.gid        = static_cast<unsigned>(hst.st_gid);
                    link_e.mtime      = hst.st_mtim.tv_sec;
                    link_e.mtime_nsec = static_cast<long>(hst.st_mtim.tv_nsec);
                    link_e.size       = 0;
                    link_e.fmt        = fmt;
                    if (struct passwd* pw = ::getpwuid(hst.st_uid)) link_e.uname = pw->pw_name;
                    if (struct group*  gr = ::getgrgid(hst.st_gid)) link_e.gname = gr->gr_name;
                    if (cfg.numeric_owner) { link_e.uname.clear(); link_e.gname.clear(); }
                    if (!cfg.owner.empty()) link_e.uname = cfg.owner;
                    if (!cfg.group.empty()) link_e.gname = cfg.group;
                    if (cfg.verbose) print("{}\n", archname);
                    writer.write_header_only(link_e, cfg);
                    return;
                }
                seen_inodes[key] = archname;
            }
        }

        if (cfg.verbose) {
            print("{}\n", archname);
            if (g_index_fp) std::fprintf(g_index_fp, "%s\n", archname.c_str());
        }

        // Track path for snapshot update (regular files, dirs, symlinks, specials)
        if (!cfg.listed_incremental.empty()) {
            struct stat snap_st{};
            if (::lstat(fspath.c_str(), &snap_st) == 0) {
                SnapRec rec;
                rec.mtime   = static_cast<std::int64_t>(snap_st.st_mtime);
                rec.dev     = static_cast<std::uint64_t>(snap_st.st_dev);
                rec.has_dev = true;
                snapshot_entries.emplace_back(raw_archname, rec);
            }
        }

        // Multi-volume: refuse members larger than tape length (no mid-file split).
        // Pre-rotate when the next member will not fit on the current volume.
        if (do_multivol) {
            struct stat mst{};
            if (::lstat(fspath.c_str(), &mst) == 0 && S_ISREG(mst.st_mode)) {
                const std::int64_t max_bytes =
                    static_cast<std::int64_t>(cfg.tape_length) * 1024LL;
                const std::int64_t data_blocks =
                    (mst.st_size + static_cast<std::int64_t>(BLOCKSIZE) - 1)
                    / static_cast<std::int64_t>(BLOCKSIZE);
                const std::int64_t need_blocks = 1 + data_blocks;
                const std::int64_t need_bytes =
                    need_blocks * static_cast<std::int64_t>(BLOCKSIZE);
                if (need_bytes > max_bytes) {
                    print(stderr,
                          "mutar: {}: file larger than tape length ({} KiB); "
                          "mid-file multi-volume split is not supported\n",
                          fspath, cfg.tape_length);
                    exit_code = EXIT_FAILURE;
                    return;
                }
                if (writer.block_no() > 0 &&
                    writer.block_no() + need_blocks + 2 > max_blocks) {
                    if (!rotate_volume()) return;
                }
            } else if (writer.block_no() > 0 &&
                       writer.block_no() + 3 > max_blocks) {
                if (!rotate_volume()) return;
            }
        }

        if (!writer.add_path(archname, fspath, cfg))
            exit_code = EXIT_FAILURE;
        else if (cfg.remove_files)
            files_to_remove.push_back(fspath);

        // --checkpoint progress
        if (cfg.checkpoint > 0) {
            ++checkpoint_count;
            if (checkpoint_count % cfg.checkpoint == 0)
                do_checkpoint(cfg, checkpoint_count);
        }
    };

    if (cfg.files.empty()) {
        walk_dir("", ".", cfg, add_file);
    } else {
        for (const auto& f : cfg.files)
            walk_dir("", f, cfg, add_file);
    }

    writer.finish();
    total_bytes += writer.block_no() * static_cast<std::int64_t>(BLOCKSIZE);
    s.close();

    if (cfg.multi_volume && !cfg.volno_file.empty()) {
        if (!write_volno_file(cfg.volno_file, vol_num))
            exit_code = EXIT_FAILURE;
    }

    // Write sidecar index (MUTAR.INDEX.V1) after archive is complete
    if (collect_index && exit_code == EXIT_SUCCESS) {
        std::string idx_path = resolve_mutaridx_path(cfg, true);
        if (idx_path.empty()) {
            print(stderr, "mutar: cannot determine index path (need -f ARCHIVE)\n");
            exit_code = EXIT_FAILURE;
        } else {
            auto sr = create_index.save(idx_path);
            if (!sr) {
                print(stderr, "mutar: cannot write index '{}': {}\n",
                      idx_path, sr.error().message);
                exit_code = EXIT_FAILURE;
            } else if (cfg.verbose) {
                print(stderr, "mutar: wrote index {} ({} members)\n",
                      idx_path, create_index.all().size());
            }
        }
    }

    // Write/update incremental snapshot file (V2: name\tmtime\tdev)
    if (!cfg.listed_incremental.empty() && exit_code == EXIT_SUCCESS) {
        // Merge archived entries into existing snapshot (for level >= 1 incremental)
        std::map<std::string, SnapRec> snap_out = snapshot_map;
        for (auto& [sname, srec] : snapshot_entries)
            snap_out[sname] = srec;

        // Atomic write via mkstemp
        std::string tmp_tmpl = cfg.listed_incremental + ".XXXXXX";
        std::vector<char> tmp_buf(tmp_tmpl.begin(), tmp_tmpl.end());
        tmp_buf.push_back('\0');
        int snap_fd = ::mkstemp(tmp_buf.data());
        std::string tmp_path(tmp_buf.data()); // actual path after mkstemp fills template
        if (snap_fd >= 0) {
            FILE* snap_fp = ::fdopen(snap_fd, "w");
            if (snap_fp) {
                std::fprintf(snap_fp, "MUTAR_SNAPSHOT_V2\n");
                for (auto& [sn, sm] : snap_out) {
                    if (sm.has_dev)
                        std::fprintf(snap_fp, "%s\t%lld\t%llu\n", sn.c_str(),
                                     static_cast<long long>(sm.mtime),
                                     static_cast<unsigned long long>(sm.dev));
                    else
                        std::fprintf(snap_fp, "%s\t%lld\n", sn.c_str(),
                                     static_cast<long long>(sm.mtime));
                }
                std::fclose(snap_fp);
                if (::rename(tmp_path.c_str(), cfg.listed_incremental.c_str()) < 0) {
                    print(stderr, "mutar: cannot rename snapshot '{}': {}\n",
                          tmp_path, std::strerror(errno));
                    ::unlink(tmp_path.c_str());
                }
            } else {
                ::close(snap_fd);
                ::unlink(tmp_path.c_str());
                print(stderr, "mutar: cannot write snapshot '{}': {}\n",
                      cfg.listed_incremental, std::strerror(errno));
            }
        } else {
            print(stderr, "mutar: cannot create snapshot temp file for '{}': {}\n",
                  cfg.listed_incremental, std::strerror(errno));
        }
    }

    // --verify: re-read the archive and check all entries are valid
    if (cfg.verify && exit_code == EXIT_SUCCESS) {
        print(stderr, "mutar: Verifying archive...\n");
        Config vcfg = cfg;
        auto vres = ArchiveStream::open_read(vcfg);
        bool verify_ok = true;
        if (!vres) {
            print(stderr, "mutar: verify: cannot reopen archive: {}\n", vres.error().message);
            verify_ok = false;
        } else {
            ArchiveStream& vs = *vres;
            ArchiveReader vreader(vs, cfg.blocking_factor, cfg.ignore_zeros);
            int verify_count = 0;
            for (;;) {
                auto [ve, vok, veof] = vreader.next_entry();
                if (veof) break;
                if (!vok) { verify_ok = false; break; }
                ++verify_count;
                vreader.skip_entry(ve);
            }
            vs.close();
            if (verify_ok)
                print(stderr, "mutar: Verify OK ({} entries)\n", verify_count);
            else
                print(stderr, "mutar: Verify FAILED\n");
        }
        if (!verify_ok) exit_code = EXIT_FAILURE;
    }

    // --check-links: warn if not all hard links to a file were archived
    if (cfg.check_links) {
        for (auto& [key, count] : inode_archive_count) {
            auto it = seen_inodes.find(key);
            if (it == seen_inodes.end()) continue;
            struct stat cst{};
            if (::stat(it->second.c_str(), &cst) == 0 &&
                cst.st_nlink > static_cast<nlink_t>(count)) {
                mutar_warn(cfg, "missing-links",
                          std::format("{}: Not all links archived", it->second));
            }
        }
    }

    // --totals: print total bytes written to stderr
    if (cfg.totals)
        print(stderr, "Total bytes written: {}\n", total_bytes);

    if (g_index_fp) { std::fclose(g_index_fp); g_index_fp = nullptr; }

    // --remove-files: delete source files after successful archive
    if (exit_code == EXIT_SUCCESS) {
        for (const auto& fp : files_to_remove)
            ::unlink(fp.c_str());
    }

    return exit_code;
}

// ── extract (-x) ──────────────────────────────────────────────────────────────

static int op_extract(const Config& cfg) {
    auto res = ArchiveStream::open_read(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream s = std::move(*res);
    std::string materialize_temp;
    bool unlink_materialize = false;

    if (!cfg.index_file.empty()) {
        g_index_fp = std::fopen(cfg.index_file.c_str(), "w");
        if (!g_index_fp)
            print(stderr, "mutar: {}: cannot open index file\n", cfg.index_file);
    }

    // Normalize user-supplied member names so "./file.txt" matches stored "file.txt"
    std::set<std::string> want;
    for (const auto& f : cfg.files) want.insert(normalize_member(f));

    // Optional sidecar index for selective extract via seek
    ArchiveIndex extract_index;
    bool have_index = false;
    {
        std::string idx_path = resolve_mutaridx_path(cfg, false);
        if (!idx_path.empty() && std::filesystem::exists(idx_path)) {
            auto lr = extract_index.load(idx_path);
            if (lr) {
                have_index = true;
            } else if (!cfg.mutar_index.empty()) {
                print(stderr, "mutar: warning: cannot load index '{}': {}\n",
                      idx_path, lr.error().message);
            }
        } else if (!cfg.mutar_index.empty()) {
            print(stderr, "mutar: warning: index '{}' not found; sequential extract\n",
                  cfg.mutar_index);
        }
    }

    // Compressed streams are pipes (not seekable). When we have an index and
    // named members, materialise decompressed bytes to a temp file so seek
    // extract works for .tar.xz / .tar.zst / etc. (full decompress once).
    const bool want_seek =
        have_index && !want.empty() && !s.is_seekable();
    if (want_seek) {
        auto mat = ArchiveStream::materialize_seekable(s, materialize_temp);
        if (!mat) {
            print(stderr, "mutar: cannot materialize seekable view: {}\n",
                  mat.error().message);
            return EXIT_FAILURE;
        }
        s = std::move(*mat);
        unlink_materialize = true;
        if (std::getenv("MUTAR_DEBUG_SEEK"))
            print(stderr, "mutar: materialized compressed archive to {} for seek\n",
                  materialize_temp);
    }

    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros);

    if (!cfg.directory.empty()) {
        if (::chdir(cfg.directory.c_str()) < 0) {
            print(stderr, "mutar: -C {}: {}\n", cfg.directory, std::strerror(errno));
            if (unlink_materialize && !materialize_temp.empty())
                ::unlink(materialize_temp.c_str());
            return EXIT_FAILURE;
        }
    }

    // --one-top-level: compute the wrapping directory name
    std::string one_top;
    if (!cfg.one_top_level.empty()) {
        if (cfg.one_top_level == "__auto__") {
            // Use archive basename minus compression+tar suffixes.
            // Compound suffixes must be checked before single-extension ones.
            namespace fs = std::filesystem;
            std::string base = fs::path(cfg.archive_file).filename().string();
            for (const char* suf : {".tar.gz",".tar.bz2",".tar.xz",".tar.zst",
                                     ".tar.lzma",".tar.lz",".tar.lzop",".tar.Z",
                                     ".tgz",".tbz2",".txz",".tar"}) {
                if (base.ends_with(suf)) { base.resize(base.size() - std::strlen(suf)); break; }
            }
            one_top = base.empty() ? "archive" : base;
        } else {
            one_top = cfg.one_top_level;
        }
        // Create the top-level directory
        std::error_code ec; std::filesystem::create_directories(one_top, ec);
    }

    // Collect directories to fix up timestamps/permissions at the end
    struct DirFix { std::string path; std::int64_t mtime; long nsec; unsigned mode; };
    std::vector<DirFix> dir_fixups;

    int exit_code = EXIT_SUCCESS;
    std::int64_t total_bytes = 0;
    std::int64_t checkpoint_count = 0;
    std::map<std::string, int> occurrence_map;
    int extract_vol_num = 1;
    if (cfg.multi_volume && !cfg.volno_file.empty())
        extract_vol_num = read_volno_file(cfg.volno_file);


    // Seek-based selective extract when index + seekable stream + named members.
    std::vector<IndexEntry> seek_queue;
    bool use_index_seek = false;
    std::size_t seek_qi = 0;
    if (have_index && !want.empty() && s.is_seekable()) {
        for (const auto& ie : extract_index.all()) {
            std::string n = normalize_member(ie.name);
            if (want.contains(ie.name) || want.contains(n))
                seek_queue.push_back(ie);
        }
        std::ranges::sort(seek_queue, [](const IndexEntry& a, const IndexEntry& b) {
            return a.offset < b.offset;
        });
        use_index_seek = !seek_queue.empty();
    }

    for (;;) {
        if (use_index_seek) {
            if (seek_qi >= seek_queue.size()) break;
            if (!reader.seek_to_byte(seek_queue[seek_qi].offset)) {
                print(stderr, "mutar: seek to offset {} failed; falling back to sequential\n",
                      seek_queue[seek_qi].offset);
                use_index_seek = false;
                // Restart sequential from beginning of archive
                if (!reader.seek_to_byte(0)) {
                    print(stderr, "mutar: cannot rewind archive after failed seek\n");
                    exit_code = EXIT_FAILURE;
                    break;
                }
                seek_qi = 0;
                // fall through to sequential next_entry
            } else {
                ++seek_qi;
            }
        }

        auto [e, ok, eof] = reader.next_entry();
        if (eof) {
            if (use_index_seek) break; // done with seek queue
            // Multi-volume: on EOF, open next volume and continue
            if (cfg.multi_volume) {
                std::string cur_vol = make_volume_name(cfg.archive_file, extract_vol_num);
                if (!run_info_script(cfg, cur_vol, extract_vol_num, "-x")) {
                    exit_code = EXIT_FAILURE;
                    break;
                }
                ++extract_vol_num;
                if (!cfg.volno_file.empty())
                    write_volno_file(cfg.volno_file, extract_vol_num);

                std::string next_vol = make_volume_name(cfg.archive_file, extract_vol_num);
                // Auto-open when the next volume file exists; otherwise prompt once.
                if (::access(next_vol.c_str(), R_OK) != 0) {
                    print(stderr, "mutar: Prepare volume #{} ({}) and press return: ",
                          extract_vol_num, next_vol);
                    std::fflush(stderr);
                    FILE* tty = std::fopen("/dev/tty", "r");
                    if (!tty) tty = stdin;
                    char dummy_vol[256];
                    if (!std::fgets(dummy_vol, sizeof(dummy_vol), tty)) {
                        print(stderr, "mutar: multi-volume: no more volumes (EOF)\n");
                        if (tty != stdin) std::fclose(tty);
                        break;
                    }
                    if (tty != stdin) std::fclose(tty);
                }

                Config next_cfg = cfg;
                next_cfg.archive_file = next_vol;
                auto next_res = ArchiveStream::open_read(next_cfg);
                if (!next_res) {
                    print(stderr, "mutar: cannot open volume {}: {}\n",
                          next_vol, next_res.error().message);
                    break;
                }
                print(stderr, "mutar: reading volume #{} from {}\n",
                      extract_vol_num, next_vol);
                s.close();
                s = std::move(*next_res);
                reader.swap_stream(s);
                continue;
            }
            break;
        }
        if (!ok) { exit_code = EXIT_FAILURE; continue; }
        if (cfg.checkpoint > 0 && ++checkpoint_count % cfg.checkpoint == 0)
            do_checkpoint(cfg, checkpoint_count);

        std::string outpath = sanitize_path(e.name, cfg.absolute_names);
        if (!cfg.transform_expr.empty())
            outpath = apply_transform(outpath, cfg.transform_expr);
        if (cfg.strip_components > 0) {
            outpath = strip_components(outpath, cfg.strip_components);
            if (outpath.empty()) { reader.skip_entry(e); continue; }
        }
        // Prepend --one-top-level directory
        if (!one_top.empty() && outpath != "." && outpath != "./")
            outpath = one_top + "/" + outpath;

        if (!want.empty() && !want.contains(e.name) && !want.contains(outpath)) {
            reader.skip_entry(e);
            continue;
        }
        // --occurrence: only process the Nth occurrence of a member name
        if (cfg.occurrence > 0) {
            int cnt = ++occurrence_map[e.name];
            if (cnt != cfg.occurrence) { reader.skip_entry(e); continue; }
        }

        if (is_excluded(cfg, outpath)) {
            reader.skip_entry(e);
            continue;
        }

        if (cfg.verbose) {
            Entry disp_e = e; disp_e.name = outpath;
            print_verbose(disp_e, cfg);
        }

        // --interactive: prompt user before extracting each file
        if (cfg.interactive) {
            std::fprintf(stderr, "mutar: extract `%s'? [y/N] ", outpath.c_str());
            std::fflush(stderr);
            std::string ans;
            // Prefer reading from the controlling terminal (/dev/tty) to avoid
            // consuming archive data from stdin when it is not a TTY.
            std::istream* in = nullptr;
            std::ifstream tty("/dev/tty");
            if (tty.is_open()) {
                in = &tty;
            } else if (::isatty(STDIN_FILENO)) {
                in = &std::cin;
            } else {
                // No safe interactive input available; default to "no".
                reader.skip_entry(e);
                continue;
            }
            if (!std::getline(*in, ans) || (ans != "y" && ans != "Y")) {
                reader.skip_entry(e);
                continue;
            }
        }

        // Create parent directories
        {
            namespace fs = std::filesystem;
            fs::path p(outpath);
            if (p.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
            }
        }

        switch (e.typeflag) {
        case DIRTYPE:
        {
            // typeflag '5' is always a directory
            std::string dpath = outpath;
            if (!dpath.empty() && dpath.back() == '/') dpath.pop_back();
            // --keep-directory-symlink: preserve existing symlink pointing to a dir
            if (cfg.keep_dir_symlink) {
                struct stat lnk_st{};
                if (::lstat(dpath.c_str(), &lnk_st) == 0 && S_ISLNK(lnk_st.st_mode)) {
                    struct stat tgt_st{};
                    if (::stat(dpath.c_str(), &tgt_st) == 0 && S_ISDIR(tgt_st.st_mode)) {
                        dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
                        reader.skip_entry(e);
                        break;
                    }
                }
            }

            // Check what already exists at dpath
            struct stat existing_st{};
            bool path_exists = (::lstat(dpath.c_str(), &existing_st) == 0);
            if (path_exists && !S_ISDIR(existing_st.st_mode)) {
                // Target path exists but is not a directory — cannot mkdir over it
                print(stderr, "mutar: {}: Cannot overwrite non-directory with directory\n", dpath);
                reader.skip_entry(e);
                break;
            }

            // Create the directory (no-op if already exists)
            ::mkdir(dpath.c_str(), 0777);

            // --no-overwrite-dir: when dir already existed, skip metadata update
            if (path_exists && cfg.no_overwrite_dir) {
                if (cfg.verbose)
                    print(stderr, "mutar: {}: directory already exists, skipping metadata update\n", dpath);
                reader.skip_entry(e);
                break;
            }

            if (cfg.no_delay_dir_restore) {
                // Apply timestamps/permissions immediately
                if (!cfg.touch) {
                    struct timespec ts[2];
                    ts[0].tv_sec = ts[1].tv_sec   = e.mtime;
                    ts[0].tv_nsec = ts[1].tv_nsec  = e.mtime_nsec;
                    ::utimensat(AT_FDCWD, dpath.c_str(), ts, 0);
                }
                bool set_perm = cfg.same_permissions ||
                                (::getuid() == 0 && !cfg.no_same_permissions);
                if (set_perm) ::chmod(dpath.c_str(), e.mode & 07777);
            } else {
                dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
            }
            restore_xattrs_acls(e, dpath, cfg);
            reader.skip_entry(e);
            break;
        }
        case SYMTYPE:
        {
            ::unlink(outpath.c_str());
            if (::symlink(e.linkname.c_str(), outpath.c_str()) < 0) {
                print(stderr, "mutar: symlink {}: {}\n", outpath, std::strerror(errno));
                exit_code = EXIT_FAILURE;
            } else {
                restore_xattrs_acls(e, outpath, cfg);
            }
            reader.skip_entry(e);
            break;
        }
        case LNKTYPE:
        {
            ::unlink(outpath.c_str());
            std::string link_target = sanitize_path(e.linkname, cfg.absolute_names);
            // Apply the same path rewrites as for outpath so the hard link target
            // remains consistent when --transform / --strip-components / --one-top-level
            // are in use.
            if (!cfg.transform_expr.empty())
                link_target = apply_transform(link_target, cfg.transform_expr);
            if (cfg.strip_components > 0)
                link_target = strip_components(link_target, cfg.strip_components);
            if (!one_top.empty() && link_target != "." && link_target != "./")
                link_target = one_top + "/" + link_target;
            if (::link(link_target.c_str(), outpath.c_str()) < 0) {
                print(stderr, "mutar: hardlink {} -> {}: {}\n",
                           outpath, link_target, std::strerror(errno));
                exit_code = EXIT_FAILURE;
            }
            reader.skip_entry(e);
            break;
        }
        case CHRTYPE:
        case BLKTYPE:
        {
            mode_t mtype = (e.typeflag == CHRTYPE) ? S_IFCHR : S_IFBLK;
            dev_t dev = ::makedev(e.devmajor, e.devminor);
            ::unlink(outpath.c_str());
            if (::mknod(outpath.c_str(), mtype | (e.mode & 07777), dev) < 0 && errno != EEXIST) {
                print(stderr, "mutar: mknod {}: {}\n", outpath, std::strerror(errno));
                exit_code = EXIT_FAILURE;
            }
            reader.skip_entry(e);
            break;
        }
        case FIFOTYPE:
        {
            ::unlink(outpath.c_str());
            if (::mkfifo(outpath.c_str(), e.mode & 07777) < 0) {
                print(stderr, "mutar: mkfifo {}: {}\n", outpath, std::strerror(errno));
                exit_code = EXIT_FAILURE;
            }
            reader.skip_entry(e);
            break;
        }
        case GNUTYPE_LONGNAME:
        case GNUTYPE_LONGLINK:
        case GNUTYPE_VOLHDR:
        case GNUTYPE_DUMPDIR:
            reader.skip_entry(e);
            break;
        default: // REGTYPE ('0'), AREGTYPE ('\0' old V7 regular), CONTTYPE ('7')
        {
            // AREGTYPE ('\0') with a name ending in '/' is an old implicit-directory
            // entry (used by some historical tar implementations).  Treat as a dir.
            if (e.typeflag == AREGTYPE && !outpath.empty() && outpath.back() == '/') {
                std::string dpath = outpath;
                dpath.pop_back();
                ::mkdir(dpath.c_str(), 0777);
                dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
                reader.skip_entry(e);
                break;
            }

            // Check keep/skip/overwrite policy
            struct stat existing{};
            bool exists = (::lstat(outpath.c_str(), &existing) == 0);
            if (exists) {
                if (cfg.keep_old_files) {
                    print(stderr, "mutar: {}: file exists\n", outpath);
                    reader.skip_entry(e);
                    exit_code = EXIT_FAILURE;
                    break;
                }
                if (cfg.skip_old_files) {
                    reader.skip_entry(e);
                    break;
                }
                if (cfg.keep_newer_files && existing.st_mtime > e.mtime) {
                    reader.skip_entry(e);
                    break;
                }
                if (cfg.unlink_first) ::unlink(outpath.c_str());
            }

            if (cfg.to_stdout) {
                reader.read_entry_data(e, STDOUT_FILENO);
                total_bytes += e.size;
                break;
            }

            // --to-command: pipe extracted data to a shell command
            if (!cfg.to_command.empty()) {
                int pipefd[2];
                if (::pipe(pipefd) < 0) {
                    print(stderr, "mutar: pipe: {}\n", std::strerror(errno));
                    reader.skip_entry(e);
                    exit_code = EXIT_FAILURE;
                    break;
                }
                pid_t pid = ::fork();
                if (pid < 0) {
                    print(stderr, "mutar: fork: {}\n", std::strerror(errno));
                    ::close(pipefd[0]); ::close(pipefd[1]);
                    reader.skip_entry(e);
                    exit_code = EXIT_FAILURE;
                    break;
                }
                if (pid == 0) {
                    ::close(pipefd[1]);
                    if (::dup2(pipefd[0], STDIN_FILENO) < 0) ::_exit(127);
                    ::close(pipefd[0]);
                    ::setenv("TAR_FILENAME", outpath.c_str(), 1);
                    ::setenv("TAR_SIZE", std::to_string(e.size).c_str(), 1);
                    ::setenv("TAR_REALNAME", e.name.c_str(), 1);
                    ::execlp("/bin/sh", "sh", "-c", cfg.to_command.c_str(), nullptr);
                    ::_exit(127);
                }
                ::close(pipefd[0]);
                bool data_ok = reader.read_entry_data(e, pipefd[1]);
                ::close(pipefd[1]);
                int status = 0;
                ::waitpid(pid, &status, 0);
                total_bytes += e.size;
                if (!data_ok || (WIFEXITED(status) && WEXITSTATUS(status) != 0)) {
                    if (!cfg.ignore_failed_read)
                        print(stderr, "mutar: --to-command '{}' exited {}\n",
                              cfg.to_command, WEXITSTATUS(status));
                    // Non-fatal by default (mirrors GNU tar behaviour)
                }
                break;
            }

            // --backup: rename existing file before overwriting
            if (cfg.backup && exists && !cfg.keep_old_files && !cfg.skip_old_files) {
                std::string bak = make_backup_path(outpath, cfg);
                if (!bak.empty())
                    ::rename(outpath.c_str(), bak.c_str());
            }

            int fd = ::open(outpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                print(stderr, "mutar: {}: {}\n", outpath, std::strerror(errno));
                reader.skip_entry(e);
                exit_code = EXIT_FAILURE;
                break;
            }

            // Sparse extraction: write each data segment at its correct file offset
            if (e.is_sparse && !e.sparse_map.empty()) {
                // Pre-allocate the full logical file size (creates hole-based sparse file)
                std::int64_t real_sz = e.real_size > 0 ? e.real_size : e.size;
                ::ftruncate(fd, static_cast<off_t>(real_sz));
                reader.extract_sparse_data(e, fd);
            } else {
                reader.read_entry_data(e, fd);
            }
            total_bytes += e.size;
            ::close(fd);

            // Permissions
            bool set_perm = cfg.same_permissions ||
                            (::getuid() == 0 && !cfg.no_same_permissions);
            if (set_perm)
                ::chmod(outpath.c_str(), e.mode & 07777);
            else {
                mode_t saved_umask = ::umask(0);
                ::umask(saved_umask);
                ::chmod(outpath.c_str(), (e.mode & 0777) & ~saved_umask);
            }

            // Timestamps
            if (!cfg.touch) {
                struct timespec ts[2];
                ts[0].tv_sec  = e.atime ? e.atime : e.mtime;
                ts[0].tv_nsec = 0;
                ts[1].tv_sec  = e.mtime;
                ts[1].tv_nsec = e.mtime_nsec;
                ::utimensat(AT_FDCWD, outpath.c_str(), ts, 0);
            }

            // Ownership (root only by default; -o/--no-same-owner disables even for root)
            bool effective_no_same_owner = cfg.no_same_owner || cfg.compat_o;
            if (!effective_no_same_owner && (cfg.same_owner || ::getuid() == 0)) {
                uid_t uid_val = e.uid;
                gid_t gid_val = e.gid;
                if (!e.uname.empty() && !cfg.numeric_owner) {
                    if (struct passwd* pw = ::getpwnam(e.uname.c_str())) uid_val = pw->pw_uid;
                }
                if (!e.gname.empty() && !cfg.numeric_owner) {
                    if (struct group* gr = ::getgrnam(e.gname.c_str())) gid_val = gr->gr_gid;
                }
                ::lchown(outpath.c_str(), uid_val, gid_val);
            }

            // Extended attributes and POSIX ACLs (SCHILY.* from PAX header)
            restore_xattrs_acls(e, outpath, cfg);
            break;
        }
        }
    }

    // Fix up directory timestamps (apply in reverse order)
    for (auto it = dir_fixups.rbegin(); it != dir_fixups.rend(); ++it) {
        if (!cfg.touch) {
            struct timespec ts[2];
            ts[0].tv_sec = ts[1].tv_sec  = it->mtime;
            ts[0].tv_nsec = ts[1].tv_nsec = it->nsec;
            ::utimensat(AT_FDCWD, it->path.c_str(), ts, 0);
        }
        bool set_perm = cfg.same_permissions || (::getuid() == 0 && !cfg.no_same_permissions);
        if (set_perm) ::chmod(it->path.c_str(), it->mode & 07777);
    }

    if (cfg.totals)
        print(stderr, "Total bytes extracted: {}\n", total_bytes);

    if (g_index_fp) { std::fclose(g_index_fp); g_index_fp = nullptr; }

    if (unlink_materialize && !materialize_temp.empty())
        ::unlink(materialize_temp.c_str());

    return exit_code;
}

// ── diff / compare (-d) ───────────────────────────────────────────────────────

static int op_diff(const Config& cfg) {
    auto res = ArchiveStream::open_read(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& s = *res;
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros);

    int exit_code = EXIT_SUCCESS;

    for (;;) {
        auto [e, ok, eof] = reader.next_entry();
        if (eof) break;
        if (!ok) { exit_code = EXIT_FAILURE; continue; }

        if (e.typeflag == GNUTYPE_LONGNAME || e.typeflag == GNUTYPE_LONGLINK ||
            e.typeflag == GNUTYPE_VOLHDR || e.typeflag == XHDTYPE || e.typeflag == XGLTYPE) {
            reader.skip_entry(e);
            continue;
        }

        std::string fspath = sanitize_path(e.name, cfg.absolute_names);
        if (!cfg.directory.empty()) fspath = cfg.directory + "/" + fspath;

        struct stat st{};
        if (::lstat(fspath.c_str(), &st) < 0) {
            print(stderr, "mutar: {}: {}\n", fspath, std::strerror(errno));
            exit_code = EXIT_FAILURE;
            reader.skip_entry(e);
            continue;
        }

        // Compare size
        if (e.typeflag == REGTYPE || e.typeflag == AREGTYPE || e.typeflag == CONTTYPE) {
            if (st.st_size != e.size) {
                print(stderr, "mutar: {}: size differs (archive={} fs={})\n",
                           fspath, e.size, st.st_size);
                exit_code = EXIT_FAILURE;
            }
            // Compare mtime
            if (st.st_mtim.tv_sec != e.mtime) {
                print(stderr, "mutar: {}: mtime differs\n", fspath);
                exit_code = EXIT_FAILURE;
            }
        }
        reader.skip_entry(e);
    }
    return exit_code;
}

// ── append (-r) ───────────────────────────────────────────────────────────────

static int op_append(const Config& cfg) {
    // Find end of archive, then write new entries
    // The archive must be seekable (no compression)
    auto res = ArchiveStream::open_rdwr(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& s = *res;
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros);

    // Skip to end
    for (;;) {
        auto [e, ok, eof] = reader.next_entry();
        if (eof || !ok) break;
        reader.skip_entry(e);
    }

    // Seek back two blocks (the two EOF zero blocks) and overwrite
    // The block_no after reading two zero blocks includes them.
    // We want to overwrite the first zero block.
    off_t write_pos = (reader.block_no() - 2) * static_cast<off_t>(BLOCKSIZE);
    if (write_pos < 0) write_pos = 0;
    s.seek(write_pos);

    Format fmt = cfg.fmt;
    if (fmt == Format::Default) fmt = Format::GNU;
    ArchiveWriter writer(s, cfg.blocking_factor, fmt);

    int exit_code = EXIT_SUCCESS;
    for (const auto& f : cfg.files) {
        if (cfg.verbose) print("{}\n", f);
        if (!writer.add_path(f, f, cfg)) exit_code = EXIT_FAILURE;
    }
    writer.finish();
    s.close();
    return exit_code;
}

// ── update (-u): like append but only for newer files ─────────────────────────

static int op_update(const Config& cfg) {
    // Read existing archive to collect entry mtimes
    std::map<std::string, std::int64_t> archive_mtimes;
    {
        Config rdcfg = cfg;
        auto res = ArchiveStream::open_read(rdcfg);
        if (!res) {
            print(stderr, "mutar: {}\n", res.error().message);
            return EXIT_FAILURE;
        }
        ArchiveStream& s = *res;
        ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros);
        for (;;) {
            auto [e, ok, eof] = reader.next_entry();
            if (eof || !ok) break;
            archive_mtimes[e.name] = e.mtime;
            reader.skip_entry(e);
        }
    }

    // Build list of files newer than archived version
    std::vector<std::string> to_append;
    for (const auto& f : cfg.files) {
        struct stat st{};
        if (::stat(f.c_str(), &st) < 0) continue;
        auto it = archive_mtimes.find(f);
        if (it == archive_mtimes.end() || st.st_mtime > it->second)
            to_append.push_back(f);
    }

    if (to_append.empty()) return EXIT_SUCCESS;

    Config append_cfg = cfg;
    append_cfg.files = to_append;
    return op_append(append_cfg);
}

// ── delete (--delete) ─────────────────────────────────────────────────────────

static int op_delete(const Config& cfg) {
    // Read whole archive, write matching entries to a temp file, then replace.
    std::set<std::string> to_delete(cfg.files.begin(), cfg.files.end());

    // Build temp file name
    std::string tmpfile = cfg.archive_file + ".mutar_tmp_XXXXXX";
    {
        std::vector<char> tmplate(tmpfile.begin(), tmpfile.end());
        tmplate.push_back('\0');
        int tmpfd = ::mkstemp(tmplate.data());
        if (tmpfd < 0) {
            print(stderr, "mutar: mkstemp: {}\n", std::strerror(errno));
            return EXIT_FAILURE;
        }
        tmpfile = tmplate.data();
        ::close(tmpfd);
    }

    // Read source
    {
        auto res = ArchiveStream::open_read(cfg);
        if (!res) {
            print(stderr, "mutar: {}\n", res.error().message);
            ::unlink(tmpfile.c_str());
            return EXIT_FAILURE;
        }
        ArchiveStream& src = *res;
        ArchiveReader reader(src, cfg.blocking_factor);

        Config out_cfg;
        out_cfg.archive_file = tmpfile;
        out_cfg.fmt          = cfg.fmt;
        out_cfg.blocking_factor = cfg.blocking_factor;

        auto dst_res = ArchiveStream::open_write(out_cfg);
        if (!dst_res) {
            print(stderr, "mutar: {}\n", dst_res.error().message);
            ::unlink(tmpfile.c_str());
            return EXIT_FAILURE;
        }
        ArchiveStream& dst = *dst_res;
        Format fmt = cfg.fmt == Format::Default ? Format::GNU : cfg.fmt;
        ArchiveWriter writer(dst, cfg.blocking_factor, fmt);

        for (;;) {
            auto [e, ok, eof] = reader.next_entry();
            if (eof) break;
            if (!ok) { ::unlink(tmpfile.c_str()); return EXIT_FAILURE; }

            if (to_delete.contains(e.name)) {
                reader.skip_entry(e);
                continue;
            }

            // Re-emit via ArchiveWriter so extensions are regenerated
            writer.write_header_only(e, cfg);
            if (e.size > 0) {
                std::string databuf;
                databuf.reserve(static_cast<std::size_t>(e.size));
                reader.read_entry_data(e, -1, [&](const char* p, std::size_t n) {
                    std::size_t space = static_cast<std::size_t>(e.size) - databuf.size();
                    if (space > 0) databuf.append(p, std::min(n, space));
                });
                writer.write_data_bytes(databuf.data(), databuf.size());
            } else {
                reader.skip_entry(e);
            }
        }
        writer.finish();
        dst.close();
        src.close();
    }

    // Replace original archive with temp file
    if (::rename(tmpfile.c_str(), cfg.archive_file.c_str()) < 0) {
        print(stderr, "mutar: rename: {}\n", std::strerror(errno));
        ::unlink(tmpfile.c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

// ── test-label (--test-label) ─────────────────────────────────────────────────

static int op_test_label(const Config& cfg) {
    auto res = ArchiveStream::open_read(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& s = *res;
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros);

    auto [e, ok, eof] = reader.next_entry();
    if (!ok || eof) return EXIT_FAILURE;

    if (e.typeflag == GNUTYPE_VOLHDR) {
        if (cfg.label.empty()) {
            print("{}\n", e.name);
            return EXIT_SUCCESS;
        }
        if (::fnmatch(cfg.label.c_str(), e.name.c_str(), 0) == 0) {
            if (cfg.verbose) print("{}\n", e.name);
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}

// ── cat (-A): concatenate archives ────────────────────────────────────────────

static int op_cat(const Config& cfg) {
    // Open destination (append mode, seekable)
    auto dst_res = ArchiveStream::open_rdwr(cfg);
    if (!dst_res) {
        print(stderr, "mutar: {}\n", dst_res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& dst = *dst_res;

    // Seek to just before EOF blocks
    {
        Config tmp = cfg;
        ArchiveReader dummy(dst, cfg.blocking_factor);
        for (;;) {
            auto [e, ok, eof] = dummy.next_entry();
            if (eof || !ok) break;
            dummy.skip_entry(e);
        }
        off_t pos = (dummy.block_no() - 2) * static_cast<off_t>(BLOCKSIZE);
        if (pos < 0) pos = 0;
        dst.seek(pos);
    }

    int exit_code = EXIT_SUCCESS;

    for (const auto& srcfile : cfg.files) {
        Config src_cfg;
        src_cfg.archive_file = srcfile;
        auto src_res = ArchiveStream::open_read(src_cfg);
        if (!src_res) {
            print(stderr, "mutar: {}: {}\n", srcfile, src_res.error().message);
            exit_code = EXIT_FAILURE;
            continue;
        }
        ArchiveStream& src = *src_res;

        // Copy blocks until EOF blocks
        BlockBuffer bb(cfg.blocking_factor);
        for (;;) {
            Block blk{};
            if (!bb.read_block(src, blk)) break;
            // Stop at zero block
            bool all_zero = true;
            for (char c : blk.buffer) if (c) { all_zero = false; break; }
            if (all_zero) break;
            dst.write_buf(blk.buffer, BLOCKSIZE);
        }
    }

    // Write two EOF blocks
    Block zero{};
    dst.write_buf(zero.buffer, BLOCKSIZE);
    dst.write_buf(zero.buffer, BLOCKSIZE);
    return exit_code;
}

// ── CLI parsing ───────────────────────────────────────────────────────────────

enum LongOptVal : int {
    OPT_DELETE        = 1000,
    OPT_TEST_LABEL,
    OPT_OVERWRITE,
    OPT_OVERWRITE_DIR,
    OPT_NO_OVERWRITE_DIR,
    OPT_SKIP_OLD_FILES,
    OPT_KEEP_NEWER,
    OPT_KEEP_DIR_SYMLINK,
    OPT_ONE_TOP_LEVEL,
    OPT_TO_COMMAND,
    OPT_IGNORE_CMD_ERR,
    OPT_NO_IGNORE_CMD_ERR,
    OPT_OWNER,
    OPT_GROUP,
    OPT_OWNER_MAP,
    OPT_GROUP_MAP,
    OPT_MTIME,
    OPT_CLAMP_MTIME,
    OPT_MODE,
    OPT_ATIME_PRESERVE,
    OPT_NUMERIC_OWNER,
    OPT_SAME_OWNER,
    OPT_NO_SAME_OWNER,
    OPT_SAME_PERMISSIONS,
    OPT_NO_SAME_PERMISSIONS,
    OPT_DELAY_DIR_RESTORE,
    OPT_NO_DELAY_DIR_RESTORE,
    OPT_SORT,
    OPT_FORMAT,
    OPT_OLD_ARCHIVE,
    OPT_POSIX,
    OPT_PAX_OPTION,
    OPT_LABEL,
    OPT_MULTI_VOLUME,
    OPT_TAPE_LENGTH,
    OPT_INFO_SCRIPT,
    OPT_VOLNO_FILE,
    OPT_BLOCKING,
    OPT_RECORD_SIZE,
    OPT_FORCE_LOCAL,
    OPT_RMT_COMMAND,
    OPT_RSH_COMMAND,
    OPT_REMOVE_FILES,
    OPT_RECURSIVE_UNLINK,
    OPT_IGNORE_FAILED_READ,
    OPT_OCCURRENCE,
    OPT_NO_SEEK,
    OPT_CHECK_DEVICE,
    OPT_NO_CHECK_DEVICE,
    OPT_SPARSE_VERSION,
    OPT_HOLE_DETECTION,
    OPT_INCREMENTAL,
    OPT_LISTED_INCREMENTAL,
    OPT_LEVEL,
    OPT_TRANSFORM,
    OPT_STRIP_COMPONENTS,
    OPT_SHOW_OMITTED_DIRS,
    OPT_SHOW_TRANSFORMED,
    OPT_BLOCK_NUMBER,
    OPT_TOTALS,
    OPT_UTC,
    OPT_FULL_TIME,
    OPT_ZSTD,
    OPT_LZMA,
    OPT_LZIP,
    OPT_LZOP,
    OPT_USE_COMPRESS_PROG,
    OPT_EXCLUDE,
    OPT_EXCLUDE_FROM,
    OPT_EXCLUDE_VCSIGNORE,
    OPT_NULL,
    OPT_NEWER_MTIME,
    OPT_HARD_DEREFERENCE,
    OPT_VERIFY,
    OPT_CHECK_LINKS,
    OPT_INDEX_FILE,
    OPT_WRITE_INDEX,
    OPT_MUTAR_INDEX,
    OPT_SEEKABLE,
    OPT_RESTRICT,
    OPT_SPARSE,
    OPT_INTERACTIVE,
    OPT_WARNING,
    OPT_NO_SAME_ATTR,
#ifdef MUTAR_HAVE_XATTR
    OPT_XATTRS,
    OPT_NO_XATTRS,
    OPT_XATTRS_INCLUDE,
    OPT_XATTRS_EXCLUDE,
#endif
    OPT_SELINUX,
    OPT_NO_SELINUX,
#ifdef MUTAR_HAVE_ACL
    OPT_ACLS,
    OPT_NO_ACLS,
#endif
    OPT_DEREFERENCE,
    OPT_REWIND,
    OPT_AFTER_DATE,
    OPT_COMPRESS,
    OPT_NO_RECURSION,
    OPT_RECURSION,
    OPT_NO_AUTO_COMPRESS,
    OPT_CHECKPOINT,
    OPT_CHECKPOINT_ACTION,
    OPT_STARTING_FILE,
    OPT_EXCLUDE_BACKUPS,
    OPT_EXCLUDE_CACHES,
    OPT_EXCLUDE_CACHES_ALL,
    OPT_EXCLUDE_CACHES_UNDER,
    OPT_EXCLUDE_VCS,
    OPT_SHOW_DEFAULTS,
    OPT_USAGE,
    OPT_ANCHORED,
    OPT_NO_ANCHORED,
    OPT_IGNORE_CASE,
    OPT_NO_IGNORE_CASE,
    OPT_WILDCARDS,
    OPT_NO_WILDCARDS,
    OPT_WILDCARDS_MATCH_SLASH,
    OPT_NO_WILDCARDS_MATCH_SLASH,
    OPT_VERBATIM_FILES_FROM,
    OPT_NO_VERBATIM_FILES_FROM,
    OPT_NO_NULL,
    OPT_UNQUOTE,
    OPT_NO_UNQUOTE,
    OPT_QUOTING_STYLE,
    OPT_ADD_FILE,
    OPT_ONE_FILE_SYSTEM,   // --one-file-system (was incorrectly mapped to OPT_NO_SAME_ATTR)
    OPT_BACKUP,
    OPT_SUFFIX,
    OPT_EXCLUDE_TAG,
    OPT_EXCLUDE_TAG_ALL,
    OPT_EXCLUDE_TAG_UNDER,
    OPT_BZIP,
    OPT_EXCLUDE_IGNORE,
    OPT_EXCLUDE_IGNORE_RECURSIVE,
    OPT_QUOTE_CHARS,
    OPT_NO_QUOTE_CHARS,
    OPT_HELP,
    OPT_VERSION,
};

static const struct option long_opts[] = {
    // Main operations
    {"list",             no_argument,       nullptr, 't'},
    {"extract",          no_argument,       nullptr, 'x'},
    {"get",              no_argument,       nullptr, 'x'},
    {"create",           no_argument,       nullptr, 'c'},
    {"diff",             no_argument,       nullptr, 'd'},
    {"compare",          no_argument,       nullptr, 'd'},
    {"append",           no_argument,       nullptr, 'r'},
    {"update",           no_argument,       nullptr, 'u'},
    {"catenate",         no_argument,       nullptr, 'A'},
    {"concatenate",      no_argument,       nullptr, 'A'},
    {"delete",           no_argument,       nullptr, OPT_DELETE},
    {"test-label",       no_argument,       nullptr, OPT_TEST_LABEL},
    // Operation modifiers
    {"sparse",           no_argument,       nullptr, 'S'},
    {"hole-detection",   required_argument, nullptr, OPT_HOLE_DETECTION},
    {"sparse-version",   required_argument, nullptr, OPT_SPARSE_VERSION},
    {"incremental",      no_argument,       nullptr, 'G'},
    {"listed-incremental",required_argument,nullptr, 'g'},
    {"level",            required_argument, nullptr, OPT_LEVEL},
    {"ignore-failed-read",no_argument,      nullptr, OPT_IGNORE_FAILED_READ},
    {"occurrence",       optional_argument, nullptr, OPT_OCCURRENCE},
    {"seek",             no_argument,       nullptr, 'n'},
    {"no-seek",          no_argument,       nullptr, OPT_NO_SEEK},
    {"no-check-device",  no_argument,       nullptr, OPT_NO_CHECK_DEVICE},
    {"check-device",     no_argument,       nullptr, OPT_CHECK_DEVICE},
    // Overwrite control
    {"verify",           no_argument,       nullptr, 'W'},
    {"remove-files",     no_argument,       nullptr, OPT_REMOVE_FILES},
    {"keep-old-files",   no_argument,       nullptr, 'k'},
    {"skip-old-files",   no_argument,       nullptr, OPT_SKIP_OLD_FILES},
    {"keep-newer-files", no_argument,       nullptr, OPT_KEEP_NEWER},
    {"overwrite",        no_argument,       nullptr, OPT_OVERWRITE},
    {"unlink-first",     no_argument,       nullptr, 'U'},
    {"recursive-unlink", no_argument,       nullptr, OPT_RECURSIVE_UNLINK},
    {"no-overwrite-dir", no_argument,       nullptr, OPT_NO_OVERWRITE_DIR},
    {"overwrite-dir",    no_argument,       nullptr, OPT_OVERWRITE_DIR},
    {"keep-directory-symlink", no_argument, nullptr, OPT_KEEP_DIR_SYMLINK},
    {"one-top-level",    optional_argument, nullptr, OPT_ONE_TOP_LEVEL},
    // Output stream
    {"to-stdout",        no_argument,       nullptr, 'O'},
    {"to-command",       required_argument, nullptr, OPT_TO_COMMAND},
    {"ignore-command-error",   no_argument, nullptr, OPT_IGNORE_CMD_ERR},
    {"no-ignore-command-error",no_argument, nullptr, OPT_NO_IGNORE_CMD_ERR},
    // File attributes
    {"owner",            required_argument, nullptr, OPT_OWNER},
    {"group",            required_argument, nullptr, OPT_GROUP},
    {"owner-map",        required_argument, nullptr, OPT_OWNER_MAP},
    {"group-map",        required_argument, nullptr, OPT_GROUP_MAP},
    {"mtime",            required_argument, nullptr, OPT_MTIME},
    {"clamp-mtime",      no_argument,       nullptr, OPT_CLAMP_MTIME},
    {"mode",             required_argument, nullptr, OPT_MODE},
    {"atime-preserve",   optional_argument, nullptr, OPT_ATIME_PRESERVE},
    {"touch",            no_argument,       nullptr, 'm'},
    {"same-owner",       no_argument,       nullptr, OPT_SAME_OWNER},
    {"no-same-owner",    no_argument,       nullptr, OPT_NO_SAME_OWNER},
    {"numeric-owner",    no_argument,       nullptr, OPT_NUMERIC_OWNER},
    {"preserve-permissions", no_argument,   nullptr, 'p'},
    {"same-permissions", no_argument,       nullptr, 'p'},
    {"no-same-permissions", no_argument,    nullptr, OPT_NO_SAME_PERMISSIONS},
    {"preserve-order",   no_argument,       nullptr, 's'},
    {"same-order",       no_argument,       nullptr, 's'},
    {"delay-directory-restore", no_argument,nullptr, OPT_DELAY_DIR_RESTORE},
    {"no-delay-directory-restore",no_argument,nullptr,OPT_NO_DELAY_DIR_RESTORE},
    {"sort",             required_argument, nullptr, OPT_SORT},
    // Extended attributes
#ifdef MUTAR_HAVE_XATTR
    {"xattrs",           no_argument,       nullptr, OPT_XATTRS},
    {"no-xattrs",        no_argument,       nullptr, OPT_NO_XATTRS},
    {"xattrs-include",   required_argument, nullptr, OPT_XATTRS_INCLUDE},
    {"xattrs-exclude",   required_argument, nullptr, OPT_XATTRS_EXCLUDE},
#endif
    {"selinux",          no_argument,       nullptr, OPT_SELINUX},
    {"no-selinux",       no_argument,       nullptr, OPT_NO_SELINUX},
#ifdef MUTAR_HAVE_ACL
    {"acls",             no_argument,       nullptr, OPT_ACLS},
    {"no-acls",          no_argument,       nullptr, OPT_NO_ACLS},
#endif
    // Device selection
    {"file",             required_argument, nullptr, 'f'},
    {"force-local",      no_argument,       nullptr, OPT_FORCE_LOCAL},
    {"rmt-command",      required_argument, nullptr, OPT_RMT_COMMAND},
    {"rsh-command",      required_argument, nullptr, OPT_RSH_COMMAND},
    {"multi-volume",     no_argument,       nullptr, 'M'},
    {"tape-length",      required_argument, nullptr, 'L'},
    {"info-script",      required_argument, nullptr, 'F'},
    {"new-volume-script",required_argument, nullptr, 'F'},
    {"volno-file",       required_argument, nullptr, OPT_VOLNO_FILE},
    // Blocking
    {"blocking-factor",  required_argument, nullptr, 'b'},
    {"record-size",      required_argument, nullptr, OPT_RECORD_SIZE},
    {"ignore-zeros",     no_argument,       nullptr, 'i'},
    {"read-full-records",no_argument,       nullptr, 'B'},
    // Format
    {"format",           required_argument, nullptr, 'H'},
    {"old-archive",      no_argument,       nullptr, OPT_OLD_ARCHIVE},
    {"portability",      no_argument,       nullptr, OPT_OLD_ARCHIVE},
    {"posix",            no_argument,       nullptr, OPT_POSIX},
    {"pax-option",       required_argument, nullptr, OPT_PAX_OPTION},
    {"label",            required_argument, nullptr, 'V'},
    // File selection
    {"directory",        required_argument, nullptr, 'C'},
    {"files-from",       required_argument, nullptr, 'T'},
    {"null",             no_argument,       nullptr, OPT_NULL},
    {"after-date",       required_argument, nullptr, 'N'},
    {"newer",            required_argument, nullptr, 'N'},
    {"newer-mtime",      required_argument, nullptr, OPT_NEWER_MTIME},
    {"backup",           optional_argument, nullptr, OPT_BACKUP},
    {"suffix",           required_argument, nullptr, OPT_SUFFIX},
    {"exclude",          required_argument, nullptr, OPT_EXCLUDE},
    {"exclude-from",     required_argument, nullptr, OPT_EXCLUDE_FROM},
    {"exclude-vcs-ignores",no_argument,     nullptr, OPT_EXCLUDE_VCSIGNORE},
    {"no-recursion",     no_argument,       nullptr, OPT_NO_RECURSION},
    {"recursion",        no_argument,       nullptr, OPT_RECURSION},
    {"dereference",      no_argument,       nullptr, 'h'},
    {"hard-dereference", no_argument,       nullptr, OPT_HARD_DEREFERENCE},
    {"one-file-system",  no_argument,       nullptr, OPT_ONE_FILE_SYSTEM},
    {"check-links",      no_argument,       nullptr, OPT_CHECK_LINKS},
    {"strip-components", required_argument, nullptr, OPT_STRIP_COMPONENTS},
    {"transform",        required_argument, nullptr, OPT_TRANSFORM},
    {"xform",            required_argument, nullptr, OPT_TRANSFORM},
    // Compression
    {"gzip",             no_argument,       nullptr, 'z'},
    {"gunzip",           no_argument,       nullptr, 'z'},
    {"ungzip",           no_argument,       nullptr, 'z'},
    {"bzip2",            no_argument,       nullptr, 'j'},
    {"xz",               no_argument,       nullptr, 'J'},
    {"zstd",             no_argument,       nullptr, OPT_ZSTD},
    {"lzma",             no_argument,       nullptr, OPT_LZMA},
    {"lzip",             no_argument,       nullptr, OPT_LZIP},
    {"lzop",             no_argument,       nullptr, OPT_LZOP},
    {"use-compress-program", required_argument, nullptr, OPT_USE_COMPRESS_PROG},
    {"auto-compress",    no_argument,       nullptr, 'a'},
    // Informative
    {"verbose",          no_argument,       nullptr, 'v'},
    {"block-number",     no_argument,       nullptr, 'R'},
    {"totals",           optional_argument, nullptr, OPT_TOTALS},
    {"utc",              no_argument,       nullptr, OPT_UTC},
    {"full-time",        no_argument,       nullptr, OPT_FULL_TIME},
    {"index-file",       required_argument, nullptr, OPT_INDEX_FILE},
    {"write-index",      no_argument,       nullptr, OPT_WRITE_INDEX},
    {"mutar-index",      required_argument, nullptr, OPT_MUTAR_INDEX},
    {"seekable",         no_argument,       nullptr, OPT_SEEKABLE},
    {"show-omitted-dirs",no_argument,       nullptr, OPT_SHOW_OMITTED_DIRS},
    {"show-transformed-names",no_argument,  nullptr, OPT_SHOW_TRANSFORMED},
    {"show-stored-names",no_argument,       nullptr, OPT_SHOW_TRANSFORMED},
    {"restrict",         no_argument,       nullptr, OPT_RESTRICT},
    {"warning",          required_argument, nullptr, OPT_WARNING},
    {"interactive",      no_argument,       nullptr, 'w'},
    {"confirmation",     no_argument,       nullptr, 'w'},
    {"absolute-names",   no_argument,       nullptr, 'P'},
    {"help",             no_argument,       nullptr, OPT_HELP},
    {"version",          no_argument,       nullptr, OPT_VERSION},
    {"compress",         no_argument,       nullptr, OPT_COMPRESS},
    {"uncompress",       no_argument,       nullptr, OPT_COMPRESS},
    {"no-auto-compress", no_argument,       nullptr, OPT_NO_AUTO_COMPRESS},
    {"checkpoint",       optional_argument, nullptr, OPT_CHECKPOINT},
    {"checkpoint-action",required_argument, nullptr, OPT_CHECKPOINT_ACTION},
    {"starting-file",    required_argument, nullptr, OPT_STARTING_FILE},
    {"exclude-backups",  no_argument,       nullptr, OPT_EXCLUDE_BACKUPS},
    {"exclude-caches",   no_argument,       nullptr, OPT_EXCLUDE_CACHES},
    {"exclude-caches-all",no_argument,      nullptr, OPT_EXCLUDE_CACHES_ALL},
    {"exclude-caches-under",no_argument,    nullptr, OPT_EXCLUDE_CACHES_UNDER},
    {"exclude-vcs",      no_argument,       nullptr, OPT_EXCLUDE_VCS},
    {"show-defaults",    no_argument,       nullptr, OPT_SHOW_DEFAULTS},
    {"usage",            no_argument,       nullptr, OPT_USAGE},
    {"anchored",         no_argument,       nullptr, OPT_ANCHORED},
    {"no-anchored",      no_argument,       nullptr, OPT_NO_ANCHORED},
    {"ignore-case",      no_argument,       nullptr, OPT_IGNORE_CASE},
    {"no-ignore-case",   no_argument,       nullptr, OPT_NO_IGNORE_CASE},
    {"wildcards",        no_argument,       nullptr, OPT_WILDCARDS},
    {"no-wildcards",     no_argument,       nullptr, OPT_NO_WILDCARDS},
    {"wildcards-match-slash",no_argument,   nullptr, OPT_WILDCARDS_MATCH_SLASH},
    {"no-wildcards-match-slash",no_argument,nullptr, OPT_NO_WILDCARDS_MATCH_SLASH},
    {"verbatim-files-from",no_argument,     nullptr, OPT_VERBATIM_FILES_FROM},
    {"no-verbatim-files-from",no_argument,  nullptr, OPT_NO_VERBATIM_FILES_FROM},
    {"no-null",          no_argument,       nullptr, OPT_NO_NULL},
    {"unquote",          no_argument,       nullptr, OPT_UNQUOTE},
    {"no-unquote",       no_argument,       nullptr, OPT_NO_UNQUOTE},
    {"quoting-style",    required_argument, nullptr, OPT_QUOTING_STYLE},
    {"add-file",         required_argument, nullptr, OPT_ADD_FILE},
    {"exclude-tag",      required_argument, nullptr, OPT_EXCLUDE_TAG},
    {"exclude-tag-all",  required_argument, nullptr, OPT_EXCLUDE_TAG_ALL},
    {"exclude-tag-under",required_argument, nullptr, OPT_EXCLUDE_TAG_UNDER},
    {"bzip",             no_argument,       nullptr, 'j'},
    {"exclude-ignore",   required_argument, nullptr, OPT_EXCLUDE_FROM},
    {"exclude-ignore-recursive",required_argument,nullptr,OPT_EXCLUDE_FROM},
    {"quote-chars",      required_argument, nullptr, OPT_QUOTING_STYLE},
    {"no-quote-chars",   required_argument, nullptr, OPT_QUOTING_STYLE},
    {"strip",            required_argument, nullptr, OPT_STRIP_COMPONENTS},
    {nullptr, 0, nullptr, 0}
};

static void set_op(Config& cfg, Operation op, char flag) {
    if (cfg.op != Operation::None && cfg.op != op) {
        print(stderr, "mutar: conflicting operations\n");
        std::exit(EXIT_FAILURE);
    }
    cfg.op = op;
}

static void set_format(Config& cfg, std::string_view name) {
    if      (name == "v7")                    cfg.fmt = Format::V7;
    else if (name == "oldgnu")                cfg.fmt = Format::OldGNU;
    else if (name == "gnu")                   cfg.fmt = Format::GNU;
    else if (name == "ustar")                 cfg.fmt = Format::USTAR;
    else if (name == "pax" || name == "posix")cfg.fmt = Format::PAX;
    else {
        print(stderr, "mutar: unknown format: {}\n", name);
        std::exit(EXIT_FAILURE);
    }
}

static void print_usage(const char* prog);   // forward declaration
static void print_version();                  // forward declaration

static Config parse_args(int argc, char* argv[]) {
    Config cfg;

    // Short option string — every short option used above
    // c=create x=extract t=list r=append u=update A=cat d=diff
    // f=file v=verbose z=gzip j=bzip2 J=xz C=dir k=keep O=stdout
    // p=same-perms s=order m=touch S=sparse G=incr g=listed-incr
    // b=blocking i=ignore-zeros B=read-full M=multi-volume L=tape-len
    // F=info-script h=dereference l=check-links n=seek P=absolute-names
    // R=block-number V=label H=format T=files-from N=newer w=interactive
    // W=verify U=unlink-first a=auto-compress 0=null K=starting-file
    // X=exclude-from I=use-compress-program Z=compress o=compat
    const char* short_opts = "cxtruAdvzjJf:C:kOpsmSGg:b:iBML:F:hlnPRV:H:T:N:wWUa0K:X:I:Zo";

    // Handle old-style leading positional mode chars (e.g. "tar czf foo.tar .")
    // GNU tar supports: first arg can be mode flags without '-'
    // We do this by inserting '-' if argv[1] looks like flags without leading '-'
    if (argc > 1 && argv[1][0] != '-' && argv[1][0] != '\0') {
        // Check if it contains a known operation char
        std::string_view first(argv[1]);
        if (first.find_first_of("cxtruda") != std::string_view::npos) {
            // Insert '-' by shifting: create modified argv
            static std::string modified;
            modified = "-";
            modified += argv[1];
            argv[1] = modified.data();
        }
    }

    int opt;
    int long_idx = 0;
    while ((opt = ::getopt_long(argc, argv, short_opts, long_opts, &long_idx)) != -1) {
        switch (opt) {
        case 'c': set_op(cfg, Operation::Create,    'c'); break;
        case 'x': set_op(cfg, Operation::Extract,   'x'); break;
        case 't': set_op(cfg, Operation::List,       't'); break;
        case 'r': set_op(cfg, Operation::Append,     'r'); break;
        case 'u': set_op(cfg, Operation::Update,     'u'); break;
        case 'A': set_op(cfg, Operation::Cat,        'A'); break;
        case 'd': set_op(cfg, Operation::Diff,       'd'); break;
        case OPT_DELETE:     set_op(cfg, Operation::Delete,    0); break;
        case OPT_TEST_LABEL: set_op(cfg, Operation::TestLabel, 0); break;

        case 'f': cfg.archive_file = ::optarg; break;
        case 'v': cfg.verbose = true; break;
        case 'z': cfg.compress = Compress::Gzip;  break;
        case 'j': cfg.compress = Compress::Bzip2; break;
        case 'J': cfg.compress = Compress::Xz;    break;
        case 'a': cfg.compress = Compress::Auto;  break;
        case OPT_ZSTD: cfg.compress = Compress::Zstd; break;
        case OPT_LZMA: cfg.compress = Compress::Lzma; break;
        case OPT_LZIP: cfg.compress = Compress::Lzip; break;
        case OPT_LZOP: cfg.compress = Compress::Lzop; break;
        case OPT_USE_COMPRESS_PROG:
            cfg.compress = Compress::Custom;
            cfg.compress_prog = ::optarg;
            break;

        case 'C': cfg.directory = ::optarg; break;
        case 'k': cfg.keep_old_files    = true; break;
        case 'O': cfg.to_stdout         = true; break;
        case 'p': cfg.same_permissions  = true; break;
        case 's':
            cfg.preserve_order = true;
            print(stderr, "mutar: warning: --preserve-order/-s is not yet implemented; option accepted but ignored\n");
            break;
        case 'm': cfg.touch             = true; break;
        case 'S': cfg.sparse            = true; break;
        case 'G': cfg.incremental       = true; break;
        case 'g': cfg.listed_incremental = ::optarg; cfg.incremental = true; break;
        case 'b': {
            char* end = nullptr; errno = 0;
            long val = std::strtol(::optarg, &end, 10);
            if (errno != 0 || end == ::optarg || *end != '\0' || val <= 0 || val > 32767) {
                print(stderr, "mutar: invalid blocking factor '{}': must be 1..32767\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.blocking_factor = static_cast<int>(val);
            break;
        }
        case 'i': cfg.ignore_zeros      = true; break;
        case 'B': cfg.read_full_records  = true; break;
        case 'M': cfg.multi_volume      = true; break;
        case 'L': {
            // -L / --tape-length=N : N × 1024 bytes per volume (implies -M)
            char* endp = nullptr; errno = 0;
            long long val = std::strtoll(::optarg, &endp, 10);
            if (errno != 0 || endp == ::optarg || *endp != '\0' || val <= 0 ||
                val > static_cast<long long>(std::numeric_limits<std::int64_t>::max() / 1024)) {
                print(stderr, "mutar: invalid tape-length '{}'\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.tape_length = static_cast<long>(val);
            cfg.tape_length_str = ::optarg;
            cfg.multi_volume = true;
            break;
        }
        case 'F':
            cfg.info_script = ::optarg;
            cfg.multi_volume = true; // -F implies -M
            break;
        case 'h': cfg.dereference       = true; break;
        case 'l': cfg.check_links = true; break;
        case 'n': cfg.seek              = true; break;
        case 'P': cfg.absolute_names    = true; break;
        case 'R': cfg.block_number      = true; break;
        case 'V': cfg.label             = ::optarg; break;
        case 'H': set_format(cfg, ::optarg); break;
        case 'T': cfg.files_from.push_back(::optarg); break;
        case 'N': cfg.newer_than        = ::optarg; break;
        case 'w': cfg.interactive       = true; break;
        case 'W': cfg.verify            = true; break;
        case 'U': cfg.unlink_first      = true; break;
        case '0': cfg.null_terminated   = true; break;

        case OPT_OLD_ARCHIVE:        cfg.fmt = Format::V7; break;
        case OPT_POSIX:              cfg.fmt = Format::PAX; cfg.posix = true; break;
        case OPT_OWNER:              cfg.owner = ::optarg; break;
        case OPT_GROUP:              cfg.group = ::optarg; break;
        case OPT_MTIME:              cfg.mtime = ::optarg; break;
        case OPT_MODE:               cfg.mode_str = ::optarg; break;
        case OPT_NUMERIC_OWNER:      cfg.numeric_owner = true; break;
        case OPT_SAME_OWNER:         cfg.same_owner = true; break;
        case OPT_NO_SAME_OWNER:      cfg.no_same_owner = true; break;
        case OPT_SAME_PERMISSIONS:   cfg.same_permissions = true; break;
        case OPT_NO_SAME_PERMISSIONS:cfg.no_same_permissions = true; break;
        case OPT_SKIP_OLD_FILES:     cfg.skip_old_files = true; break;
        case OPT_KEEP_NEWER:         cfg.keep_newer_files = true; break;
        case OPT_OVERWRITE:          cfg.overwrite = true; break;
        case OPT_NO_OVERWRITE_DIR:   cfg.no_overwrite_dir = true; cfg.overwrite_dir = false; break;
        case OPT_RECURSIVE_UNLINK:   cfg.recursive_unlink = true; break;
        case OPT_REMOVE_FILES:       cfg.remove_files = true; break;
        case OPT_IGNORE_FAILED_READ: cfg.ignore_failed_read = true; break;
        case OPT_HARD_DEREFERENCE:   cfg.hard_dereference = true; break;
        case OPT_STRIP_COMPONENTS: {
            char* end = nullptr; errno = 0;
            long val = std::strtol(::optarg, &end, 10);
            if (errno != 0 || end == ::optarg || *end != '\0' || val < 0) {
                print(stderr, "mutar: invalid strip-components '{}'\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.strip_components = static_cast<int>(val);
            break;
        }
        case OPT_TRANSFORM:          cfg.transform_expr = ::optarg; break;
        case OPT_SORT:               cfg.sort_order = ::optarg; break;
        case OPT_FORCE_LOCAL:        cfg.force_local = true; break;
        case OPT_RMT_COMMAND:        cfg.rmt_command = ::optarg; break;
        case OPT_RSH_COMMAND:        cfg.rsh_command = ::optarg; break;
        case OPT_RECORD_SIZE: {
            // Convert record-size (bytes) to blocking factor (blocks of 512)
            char* endp = nullptr; errno = 0;
            long long sz = std::strtoll(::optarg, &endp, 10);
            if (errno || endp == ::optarg || *endp != '\0' || sz <= 0 || sz % BLOCKSIZE != 0) {
                print(stderr, "mutar: invalid record-size '{}': must be a positive multiple of 512\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.blocking_factor  = static_cast<int>(sz / BLOCKSIZE);
            cfg.record_size_str  = ::optarg;
            break;
        }
        case OPT_EXCLUDE:            cfg.exclude_patterns.emplace_back(::optarg); break;
        case OPT_DELAY_DIR_RESTORE:  cfg.delay_dir_restore = true; break;
        case OPT_TOTALS:             cfg.totals = true; break;
        case OPT_UTC:                cfg.utc = true; break;
        case OPT_FULL_TIME:          cfg.full_time = true; break;
        case OPT_SHOW_OMITTED_DIRS:  cfg.show_omitted_dirs = true; break;
        case OPT_SHOW_TRANSFORMED:   cfg.show_transformed = true; break;
        case OPT_RESTRICT:           cfg.restrict_opt = true; break;
#ifdef MUTAR_HAVE_XATTR
        case OPT_XATTRS:             cfg.xattrs = true; break;
        case OPT_NO_XATTRS:          cfg.xattrs = false; break;
        case OPT_XATTRS_INCLUDE:     cfg.xattrs_include.emplace_back(::optarg); break;
        case OPT_XATTRS_EXCLUDE:     cfg.xattrs_exclude.emplace_back(::optarg); break;
#endif
        case OPT_SELINUX:
            print(stderr, "mutar: warning: SELinux support is not available (unsupported)\n");
            cfg.selinux = false;
            break;
        case OPT_NO_SELINUX:
            cfg.selinux = false;
            break;
#ifdef MUTAR_HAVE_ACL
        case OPT_ACLS:               cfg.acls = true; break;
        case OPT_NO_ACLS:            cfg.acls = false; break;
#endif
        case OPT_CHECK_LINKS:        cfg.check_links = true; break;
        case OPT_ATIME_PRESERVE:     cfg.atime_preserve = true; break;
        case OPT_SPARSE_VERSION:     cfg.sparse_version = ::optarg; break;
        case OPT_ONE_FILE_SYSTEM:    cfg.one_file_system = true; break;
        case OPT_TO_COMMAND:         cfg.to_command = ::optarg; break;
        case OPT_NEWER_MTIME:        cfg.newer_than = ::optarg; break;
        case OPT_ONE_TOP_LEVEL:
            cfg.one_top_level = (::optarg && ::optarg[0]) ? ::optarg : "__auto__";
            break;
        case OPT_OCCURRENCE: {
            if (::optarg) {
                char* end = nullptr; errno = 0;
                long val = std::strtol(::optarg, &end, 10);
                if (errno != 0 || end == ::optarg || *end != '\0' || val <= 0) {
                    print(stderr, "mutar: invalid occurrence '{}'\n", ::optarg);
                    std::exit(EXIT_FAILURE);
                }
                cfg.occurrence = static_cast<int>(val);
            } else {
                cfg.occurrence = 1;
            }
            break;
        }
        case OPT_NO_SEEK:            cfg.seek = false; break;

        case OPT_HOLE_DETECTION: {
            std::string_view method(::optarg ? ::optarg : "");
            if (method != "seek" && method != "raw") {
                print(stderr, "mutar: invalid hole-detection method '{}': must be 'seek' or 'raw'\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.hole_detection = ::optarg;
            cfg.sparse = true;  // --hole-detection implies --sparse
            break;
        }
        case OPT_OVERWRITE_DIR:      cfg.overwrite_dir = true; cfg.no_overwrite_dir = false; break;
        case OPT_EXCLUDE_VCSIGNORE:  cfg.exclude_vcs_ignores = true; break;
        case OPT_LEVEL: {
            char* end = nullptr; errno = 0;
            long val = std::strtol(::optarg, &end, 10);
            if (errno != 0 || end == ::optarg || *end != '\0' || val < 0 ||
                static_cast<unsigned long>(val) > static_cast<unsigned long>(INT_MAX)) {
                print(stderr, "mutar: invalid level '{}'\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.level = static_cast<int>(val);
            break;
        }
        case OPT_WARNING: {
            // Accepted for compatibility; warning categories are parsed and stored
            // but not yet wired into emission sites (no behavioral effect currently).
            std::string_view kw(::optarg);
            if (kw == "all") {
                cfg.warn_all = true;
            } else if (kw == "none") {
                cfg.warn_none = true;
                cfg.warnings_enabled.clear();
            } else if (kw.starts_with("no-")) {
                cfg.warnings_disabled.insert(std::string(kw.substr(3)));
            } else {
                cfg.warnings_enabled.insert(std::string(kw));
            }
            break;
        }
        case OPT_HELP:    print_usage(argv[0]); std::exit(EXIT_SUCCESS);
        case OPT_VERSION: print_version();      std::exit(EXIT_SUCCESS);

        case OPT_PAX_OPTION: {
            // --pax-option=keyword[[:]=value][,keyword[[:]=value]]...
            // Repeatable. MVP: only delete=KEYWORD (empty delete= ignored).
            // Unknown keywords accepted silently (full GNU set is large).
            if (!::optarg) break;
            cfg.pax_options.emplace_back(::optarg);
            std::string_view all = ::optarg;
            while (!all.empty()) {
                auto comma = all.find(',');
                std::string_view item = (comma == std::string_view::npos)
                    ? all : all.substr(0, comma);
                if (comma == std::string_view::npos)
                    all = {};
                else
                    all.remove_prefix(comma + 1);

                if (item.starts_with("delete=")) {
                    std::string_view kw = item.substr(7);
                    if (!kw.empty())
                        cfg.pax_option_rules.delete_keywords.emplace(kw);
                }
                // else: unknown / not-yet-implemented keyword — ignore
            }
            break;
        }

        case OPT_CHECK_DEVICE:
            cfg.check_device = true;
            break;
        case OPT_NO_CHECK_DEVICE:
            cfg.check_device = false;
            break;

        // Accepted but no-op for now (complex features not yet implemented)
        case OPT_IGNORE_CMD_ERR:
        case OPT_NO_IGNORE_CMD_ERR:
        case OPT_INFO_SCRIPT: // longopt maps to 'F'
        case OPT_NO_SAME_ATTR:
        case OPT_REWIND:
        case OPT_AFTER_DATE:
            break;

        case OPT_VOLNO_FILE:
            cfg.volno_file = ::optarg ? ::optarg : "";
            break;

        case OPT_OWNER_MAP: cfg.owner_map_file = ::optarg; break;
        case OPT_GROUP_MAP: cfg.group_map_file = ::optarg; break;
        case OPT_TAPE_LENGTH: {
            // Dead if longopt maps to 'L'; keep as alias for safety
            char* endp = nullptr; errno = 0;
            long long val = std::strtoll(::optarg, &endp, 10);
            if (errno != 0 || endp == ::optarg || *endp != '\0' || val <= 0 ||
                val > static_cast<long long>(std::numeric_limits<std::int64_t>::max() / 1024)) {
                print(stderr, "mutar: invalid tape-length '{}'\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            cfg.tape_length = static_cast<long>(val);
            cfg.tape_length_str = ::optarg;
            cfg.multi_volume = true;
            break;
        }

        case OPT_KEEP_DIR_SYMLINK:    cfg.keep_dir_symlink = true; break;
        case OPT_CLAMP_MTIME:         cfg.clamp_mtime = true; break;
        case OPT_NO_DELAY_DIR_RESTORE: cfg.no_delay_dir_restore = true; break;

        case OPT_NULL:               cfg.null_terminated = true; break;
        case OPT_NO_NULL:            cfg.null_terminated = false; break;

        case OPT_INDEX_FILE:         cfg.index_file = ::optarg; break;
        case OPT_WRITE_INDEX:        cfg.write_index = true; break;
        case OPT_MUTAR_INDEX:
            // Explicit path for create (write) and list/extract (read).
            // Presence of mutar_index alone enables index write on create.
            cfg.mutar_index = ::optarg ? ::optarg : "";
            break;
        case OPT_SEEKABLE:
            cfg.seekable = true;
            // Seekable archives are most useful with a sidecar index.
            cfg.write_index = true;
            break;
        case OPT_EXCLUDE_FROM:       cfg.exclude_from.emplace_back(::optarg); break;
        case OPT_COMPRESS:           cfg.compress = Compress::CompressZ; break;
        case OPT_NO_RECURSION:       cfg.no_recursion = true; break;
        case OPT_RECURSION:          cfg.no_recursion = false; break;
        case OPT_NO_AUTO_COMPRESS:   cfg.no_auto_compress = true; break;
        case OPT_CHECKPOINT: {
            if (::optarg) {
                char* end = nullptr; errno = 0;
                long val = std::strtol(::optarg, &end, 10);
                if (errno != 0 || end == ::optarg || *end != '\0' || val <= 0) {
                    print(stderr, "mutar: invalid checkpoint '{}'\n", ::optarg);
                    std::exit(EXIT_FAILURE);
                }
                cfg.checkpoint = static_cast<int>(val);
            } else {
                cfg.checkpoint = 10;
            }
            break;
        }
        case OPT_CHECKPOINT_ACTION:  cfg.checkpoint_action = ::optarg; break;
        case OPT_STARTING_FILE:      cfg.starting_file = ::optarg; break;
        case OPT_EXCLUDE_BACKUPS:
            cfg.exclude_backups = true;
            cfg.exclude_patterns.emplace_back("*~");
            cfg.exclude_patterns.emplace_back(".#*");
            cfg.exclude_patterns.emplace_back("#*#");
            cfg.exclude_patterns.emplace_back(".*.swp");
            break;
        case OPT_EXCLUDE_VCS:
            cfg.exclude_vcs = true;
            cfg.exclude_patterns.emplace_back(".git");
            cfg.exclude_patterns.emplace_back(".svn");
            cfg.exclude_patterns.emplace_back(".hg");
            cfg.exclude_patterns.emplace_back(".bzr");
            cfg.exclude_patterns.emplace_back("CVS");
            cfg.exclude_patterns.emplace_back("RCS");
            cfg.exclude_patterns.emplace_back("SCCS");
            break;
        case OPT_EXCLUDE_CACHES:       cfg.exclude_caches = true; break;
        case OPT_EXCLUDE_CACHES_ALL:   cfg.exclude_caches_all = true; break;
        case OPT_EXCLUDE_CACHES_UNDER: cfg.exclude_caches_under = true; break;
        case OPT_BACKUP:
            if (::optarg) {
                cfg.backup_control = ::optarg;
                std::string norm = normalize_backup_control(cfg.backup_control);
                // "none" / "off" explicitly disables backups
                cfg.backup = (norm != "none");
                cfg.backup_control = norm;
            } else {
                // Bare --backup: simple suffix rename (GNU default when unset is existing)
                cfg.backup = true;
                if (cfg.backup_control.empty())
                    cfg.backup_control = "simple";
            }
            break;
        case OPT_SUFFIX:
            cfg.backup_suffix = ::optarg;
            break;
        case OPT_EXCLUDE_TAG:
            cfg.exclude_tags.emplace_back(::optarg);
            break;
        case OPT_EXCLUDE_TAG_ALL:
            cfg.exclude_tags_all.emplace_back(::optarg);
            break;
        case OPT_EXCLUDE_TAG_UNDER:
            cfg.exclude_tags_under.emplace_back(::optarg);
            break;
        case OPT_SHOW_DEFAULTS:
            print("blocking-factor={}, format=gnu, compression=auto\n",
                  static_cast<int>(DEFAULT_BLOCK));
            std::exit(EXIT_SUCCESS);
        case OPT_USAGE:
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        case OPT_ANCHORED:           cfg.anchored = true; break;
        case OPT_NO_ANCHORED:        cfg.anchored = false; break;
        case OPT_IGNORE_CASE:        cfg.ignore_case = true; break;
        case OPT_NO_IGNORE_CASE:     cfg.ignore_case = false; break;
        case OPT_VERBATIM_FILES_FROM: cfg.verbatim_files_from = true; break;
        case OPT_NO_VERBATIM_FILES_FROM: cfg.verbatim_files_from = false; break;
        case OPT_ADD_FILE:           cfg.files.emplace_back(::optarg); break;
        case 'K':                    cfg.starting_file = ::optarg; break;
        case 'X':                    cfg.exclude_from.emplace_back(::optarg); break;
        case 'I':
            cfg.compress = Compress::Custom;
            cfg.compress_prog = ::optarg;
            break;
        case 'Z':                    cfg.compress = Compress::CompressZ; break;
        case OPT_WILDCARDS:              cfg.wildcards = true; break;
        case OPT_NO_WILDCARDS:           cfg.wildcards = false; break;
        case OPT_WILDCARDS_MATCH_SLASH:  cfg.wildcards_match_slash = true; break;
        case OPT_NO_WILDCARDS_MATCH_SLASH: cfg.wildcards_match_slash = false; break;
        case OPT_UNQUOTE:                cfg.unquote = true; break;
        case OPT_NO_UNQUOTE:             cfg.unquote = false; break;
        case OPT_QUOTING_STYLE:          cfg.quoting_style = ::optarg; break;
        case 'o':
            cfg.compat_o = true;
            break;

        case '?':
        default:
            print(stderr, "Try 'mutar --help' for more information.\n");
            std::exit(EXIT_FAILURE);
        }
    }

    // Remaining args are files
    for (int i = ::optind; i < argc; ++i)
        cfg.files.emplace_back(argv[i]);

    // Read files from -T / --files-from
    for (const auto& fname : cfg.files_from) {
        std::ifstream ifs(fname);
        if (!ifs) { print(stderr, "mutar: {}: cannot open\n", fname); continue; }
        if (cfg.null_terminated) {
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
            std::size_t start = 0;
            while (start < content.size()) {
                auto end = content.find('\0', start);
                if (end == std::string::npos) end = content.size();
                std::string item = content.substr(start, end - start);
                if (!item.empty()) cfg.files.push_back(item);
                if (end == content.size()) break;
                start = end + 1;
            }
        } else {
            std::string line;
            while (std::getline(ifs, line)) {
                if (!line.empty()) cfg.files.push_back(line);
            }
        }
    }

    // Process --exclude-from / -X files
    for (const auto& fname : cfg.exclude_from) {
        std::ifstream ifs(fname);
        if (!ifs) { print(stderr, "mutar: {}: cannot open\n", fname); continue; }
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty()) cfg.exclude_patterns.push_back(line);
        }
    }

    // Default archive file from environment
    if (cfg.archive_file.empty()) {
        if (const char* e = ::getenv("TAPE")) cfg.archive_file = e;
        else cfg.archive_file = "-";
    }

    return cfg;
}

static void print_usage(const char* prog) {
    print(
        "Usage: {0} [OPTION...] [FILE]...\n"
        "µtar (mutar) — GNU tar-compatible archiver (C++23)\n"
        "Not Jörg Schilling's star (Schily tools).\n"
        "\nMain operations:\n"
        "  -A, --catenate, --concatenate   Append tar files to an archive\n"
        "  -c, --create                    Create a new archive\n"
        "  -d, --diff, --compare           Find differences between archive and file system\n"
        "      --delete                    Delete from the archive\n"
        "  -r, --append                    Append files to end of an archive\n"
        "  -t, --list                      List the contents of an archive\n"
        "      --test-label                Test the archive volume label and exit\n"
        "  -u, --update                    Only append files newer than copy in archive\n"
        "  -x, --extract, --get            Extract files from an archive\n"
        "\nOperation modifiers:\n"
        "      --check-device              Check device numbers in listed-incremental snapshot (default)\n"
        "  -g, --listed-incremental=FILE   Handle new GNU-format incremental backup\n"
        "                                  Snapshot records files+dirs (mtime+dev); skip filter is\n"
        "                                  regular-file mtime (+dev when --check-device); dirs always dumped\n"
        "  -G, --incremental               Handle old GNU-format incremental backup\n"
        "      --hole-detection=METHOD     Use METHOD to detect holes in sparse files (seek/raw)\n"
        "      --ignore-failed-read        Do not exit with nonzero on unreadable files\n"
        "      --level=NUMBER              Dump level for created listed-incremental archive\n"
        "  -n, --seek                      Archive is seekable\n"
        "      --no-check-device           Ignore device field in listed-incremental snapshot\n"
        "      --no-seek                   Archive is not seekable\n"
        "      --occurrence[=NUMBER]       Process only the NUMBERth occurrence of each file in the archive\n"
        "      --sparse-version=MAJOR[.MINOR]  Set version of the sparse format to use\n"
        "  -S, --sparse                    Handle sparse files efficiently\n"
        "\nOverwrite control:\n"
        "  -k, --keep-old-files            Don't replace existing files when extracting\n"
        "      --keep-directory-symlink    Preserve existing symlinks to directories when extracting\n"
        "      --keep-newer-files          Don't replace existing files that are newer than their archive copies\n"
        "      --no-overwrite-dir          Preserve metadata of existing directories\n"
        "      --one-top-level[=DIR]       Create a subdirectory to avoid having loose files extracted\n"
        "      --overwrite                 Overwrite existing files when extracting\n"
        "      --overwrite-dir             Overwrite metadata of existing directories when extracting\n"
        "      --recursive-unlink          Empty hierarchies prior to extracting directory\n"
        "      --remove-files              Remove files after adding them to the archive\n"
        "      --skip-old-files            Don't replace existing files when extracting, silently skip over them\n"
        "  -U, --unlink-first              Remove each file prior to extracting over it\n"
        "  -W, --verify                    Attempt to verify the archive after writing it\n"
        "\nOutput stream:\n"
        "      --ignore-command-error      Ignore subprocess exit codes\n"
        "      --no-ignore-command-error   Treat non-zero exit codes of children as error\n"
        "  -O, --to-stdout                 Extract files to standard output\n"
        "      --to-command=COMMAND        Pipe extracted files to another program\n"
        "\nHandling of file attributes:\n"
        "      --atime-preserve[=METHOD]   Preserve access times on dumped files\n"
        "      --clamp-mtime               Only set time when the file is more recent than what was given with --mtime\n"
        "      --delay-directory-restore   Delay setting modification times and permissions of extracted directories\n"
        "      --group=NAME                Force NAME as group for added files\n"
        "      --group-map=FILE            Use FILE to map file owner GIDs\n"
        "      --mode=CHANGES              Force (symbolic) mode CHANGES for added files\n"
        "      --mtime=DATE-OR-FILE        Set mtime for added files from DATE-OR-FILE\n"
        "      --no-delay-directory-restore  Cancel the effect of --delay-directory-restore option\n"
        "      --no-same-owner             Extract files as yourself (default for ordinary users)\n"
        "      --no-same-permissions       Apply the user's umask when extracting permissions\n"
        "      --numeric-owner             Always use numbers for user/group names\n"
        "      --owner=NAME                Force NAME as owner for added files\n"
        "      --owner-map=FILE            Use FILE to map file owner UIDs\n"
        "  -p, --preserve-permissions, --same-permissions  Extract information about file permissions\n"
        "      --preserve                  Same as both -p and -s\n"
        "  -s, --preserve-order, --same-order  Member arguments are listed in the same order as the files in the archive\n"
        "      --same-owner                Try extracting files with the same ownership as exists in the archive\n"
        "      --sort=ORDER                Directory sorting order: none (default), name or inode\n",
        prog);
    // Extended file attributes section — printed only when built with support
#if defined(MUTAR_HAVE_XATTR) || defined(MUTAR_HAVE_ACL)
    print("\nHandling of extended file attributes:\n");
#ifdef MUTAR_HAVE_ACL
    print(
        "      --acls                      Store/restore POSIX ACLs (SCHILY.acl.* in PAX headers)\n"
        "      --no-acls                   Disable the POSIX ACLs support\n");
#endif
#ifdef MUTAR_HAVE_XATTR
    print(
        "      --xattrs                    Store/restore extended attributes (SCHILY.xattr.* in PAX)\n"
        "      --no-xattrs                 Disable extended attributes support\n"
        "      --xattrs-exclude=MASK       Exclude xattr keys matching MASK (fnmatch)\n"
        "      --xattrs-include=MASK       Only include xattr keys matching MASK (fnmatch)\n");
#endif
#endif // xattr/acl
    print(
        "\nSELinux (not supported):\n"
        "      --selinux, --no-selinux     Accepted for GNU tar CLI compatibility; no-ops\n");
    print(
        "\nDevice selection and switching:\n"
        "  -f, --file=ARCHIVE              Use archive file or device ARCHIVE\n"
        "      --force-local               Archive file is local even if it has a colon\n"
        "  -F, --info-script=NAME, --new-volume-script=NAME\n"
        "                                  Run script at end of each volume (implies -M);\n"
        "                                  sets TAR_ARCHIVE, TAR_VOLUME; non-zero exit fails\n"
        "  -L, --tape-length=NUMBER        Change volume after NUMBER x 1024 bytes (implies -M);\n"
        "                                  between-member rotation only (no mid-file split)\n"
        "  -M, --multi-volume              Create/list/extract multi-volume archive\n"
        "      --rmt-command=COMMAND       Use given rmt COMMAND instead of rmt\n"
        "                                  (O/R/W/C only; rmt lseek/S and remote append not supported)\n"
        "      --rsh-command=COMMAND       Use remote COMMAND instead of rsh\n"
        "      --volno-file=F              Read/write current volume number in F (atomic)\n"
        "\nDevice blocking:\n"
        "  -b, --blocking-factor=BLOCKS    BLOCKS x 512 bytes per record\n"
        "  -B, --read-full-records         Reblock as we read (for 4.2BSD pipes)\n"
        "  -i, --ignore-zeros              Ignore zeroed blocks in archive (means EOF)\n"
        "      --record-size=NUMBER        NUMBER of bytes per record, multiple of 512\n"
        "\nArchive format selection:\n"
        "      --format=FORMAT, -H FORMAT  Create archive of the given format (v7 oldgnu gnu ustar pax)\n"
        "      --old-archive, --portability  Same as --format=v7\n"
        "      --pax-option=delete=KEYWORD[,...]  Suppress PAX keywords (delete= only; partial)\n"
        "      --posix                     Same as --format=posix\n"
        "  -V, --label=TEXT                Create archive with volume name TEXT\n"
        "\nCompression options:\n"
        "  -a, --auto-compress             Use archive suffix to determine the compression program\n"
        "  -I, --use-compress-program=PROG  Filter through PROG (must accept -d)\n"
        "  -j, --bzip2                     Filter the archive through bzip2\n"
        "  -J, --xz                        Filter the archive through xz\n"
        "      --lzip                      Filter the archive through lzip\n"
        "      --lzma                      Filter the archive through lzma\n"
        "      --lzop                      Filter the archive through lzop\n"
        "      --no-auto-compress          Do not use archive suffix to determine the compression program\n"
        "  -z, --gzip, --gunzip, --ungzip  Filter the archive through gzip\n"
        "  -Z, --compress, --uncompress    Filter the archive through compress\n"
        "      --zstd                      Filter the archive through zstd\n"
        "\nLocal file selection:\n"
        "      --add-file=FILE             Add given FILE to the archive\n"
        "      --backup[=CONTROL]          Backup before overwrite on extract; CONTROL:\n"
        "                                  none/off, simple/never (suffix), numbered/t (file.~N~),\n"
        "                                  existing/nil (numbered if .~1~ exists else simple)\n"
        "  -C, --directory=DIR             Change to directory DIR\n"
        "      --exclude=PATTERN           Exclude files matching PATTERN\n"
        "      --exclude-backups           Exclude backup and lock files\n"
        "      --exclude-caches            Exclude contents of directories containing CACHEDIR.TAG\n"
        "      --exclude-caches-all        Exclude directories containing CACHEDIR.TAG\n"
        "      --exclude-caches-under      Exclude everything under directories containing CACHEDIR.TAG\n"
        "      --exclude-ignore=FILE       Read exclude patterns from FILE\n"
        "      --exclude-ignore-recursive=FILE  Read exclude patterns from FILE, applying them recursively\n"
        "      --exclude-tag=FILE          Exclude contents of directories containing FILE\n"
        "      --exclude-tag-all=FILE      Exclude directories containing FILE\n"
        "      --exclude-tag-under=FILE    Exclude everything under directories containing FILE\n"
        "      --exclude-vcs               Exclude version control system directories\n"
        "      --exclude-vcs-ignores       Read exclude patterns from the VCS ignore files\n"
        "      --anchored                  Patterns match file name start\n"
        "      --no-anchored               Patterns match after any '/' (match basename)\n"
        "      --no-null                   Disable the effect of the previous --null option\n"
        "      --no-recursion              Avoid descending automatically in directories\n"
        "      --no-verbatim-files-from    -T treats file names beginning with dash as options (default)\n"
        "      --no-wildcards              Verbatim string matching\n"
        "      --no-wildcards-match-slash  Wildcard matches '/' is not allowed\n"
        "  -N, --newer=DATE-OR-FILE, --after-date=DATE-OR-FILE  Only store files newer than DATE-OR-FILE\n"
        "      --newer-mtime=DATE          Compare date and time when data changed only\n"
        "      --null                      -T reads null-terminated names; implies --verbatim-files-from\n"
        "      --one-file-system           Stay in local file system when creating archive\n"
        "  -P, --absolute-names            Don't strip leading '/'s from file names\n"
        "      --recursion                 Recurse into directories (default)\n"
        "      --suffix=STRING             Backup before removal, override usual suffix ('~')\n"
        "  -T, --files-from=FILE           Get names to extract or create from FILE\n"
        "      --verbatim-files-from       -T reads file names verbatim (no option handling)\n"
        "      --wildcards                 Use wildcards (default)\n"
        "      --wildcards-match-slash      Wildcards match '/' when on by default\n"
        "  -X, --exclude-from=FILE         Exclude patterns listed in FILE\n"
        "\nFile name transformations:\n"
        "      --strip-components=NUMBER   Strip NUMBER leading components from file names on extraction\n"
        "      --transform=EXPRESSION, --xform=EXPRESSION  Use sed replace EXPRESSION to transform file names\n"
        "\nInformative output:\n"
        "  -?, --help                      Give this help list\n"
        "      --checkpoint[=NUMBER]       Display progress messages every NUMBERth record (default 10)\n"
        "      --checkpoint-action=ACTION  Execute ACTION on each checkpoint\n"
        "      --full-time                 Print file time to its full resolution\n"
        "      --index-file=FILE           Send verbose output to FILE\n"
        "      --write-index               Write sidecar member index (ARCHIVE.mutaridx) on create\n"
        "      --mutar-index=FILE          Sidecar index path (create write / list+extract read)\n"
        "      --seekable                  Prefer seek-friendly compress (xz/zstd blocks); implies --write-index\n"
        "                                  Compressed extract materializes once then seeks via index\n"
        "      --no-quote-chars=STRING     Disable quoting for characters from STRING\n"
        "      --quote-chars=STRING        Additionally quote characters from STRING\n"
        "      --quoting-style=STYLE       Set name quoting for -t / verbose extract\n"
        "                                  (literal, escape, c, c-maybe, shell, shell-always)\n"
        "  -R, --block-number              Show block number within archive with each message\n"
        "      --restrict                  Forbid -P/--absolute-names, --to-command, multi-volume\n"
        "      --show-defaults             Show tar defaults\n"
        "      --show-omitted-dirs         When listing or extracting, list each directory that does not match search criteria\n"
        "      --show-stored-names         Same as --show-transformed-names\n"
        "      --show-transformed-names    Show file or archive names after transformation\n"
        "      --totals[=SIGNAL]           Print total bytes after processing the archive\n"
        "      --usage                     Give a short usage message\n"
        "      --utc                       Print file modification times in UTC\n"
        "  -v, --verbose                   Verbosely list files processed\n"
        "      --version                   Print program version and licensing information and exit\n"
        "  -w, --interactive, --confirmation  Ask for confirmation for every action\n"
        "      --warning=KEYWORD           Warning control\n"
        "  -K, --starting-file=MEMBER-NAME  Begin at member MEMBER-NAME when reading the archive\n"
        "\nFormats: v7, oldgnu, gnu (default), ustar, pax/posix\n");
}

static void print_version() {
    print("mutar (µtar) 0.1.0 — C++23 GNU tar-compatible archiver\n"
               "Not Jörg Schilling's star (Schily tools).\n"
               "Goal: ~99% GNU tar 1.35 compatibility (SELinux not supported).\n"
               "Formats: v7, oldgnu, gnu, ustar, pax/posix\n"
               "License: GPL-3.0-or-later\n");
}

} // namespace mutar

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    using namespace mutar;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Handle --help / --version early (before full parse)
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-?" ) { print_usage(argv[0]);  return EXIT_SUCCESS; }
        if (arg == "--version")              { print_version();        return EXIT_SUCCESS; }
    }

    Config cfg = parse_args(argc, argv);

    if (cfg.op == Operation::None) {
        print(stderr, "mutar: You must specify one of the '-Acdtrux', "
                           "'--delete' or '--test-label' options\n");
        return EXIT_FAILURE;
    }

    // --restrict: reject dangerous option combinations before any I/O
    if (!enforce_restrict(cfg))
        return EXIT_FAILURE;

    switch (cfg.op) {
    case Operation::Create:    return op_create(cfg);
    case Operation::Extract:   return op_extract(cfg);
    case Operation::List:      return op_list(cfg);
    case Operation::Append:    return op_append(cfg);
    case Operation::Update:    return op_update(cfg);
    case Operation::Delete:    return op_delete(cfg);
    case Operation::Diff:      return op_diff(cfg);
    case Operation::Cat:       return op_cat(cfg);
    case Operation::TestLabel: return op_test_label(cfg);
    default:                   return EXIT_FAILURE;
    }
}
