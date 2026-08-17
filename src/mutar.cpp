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
#include <atomic>
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
#include <functional>
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

// Forward decl: used by ArchiveStream before the full definition.
static void mutar_warn(const Config& cfg, std::string_view category, std::string_view msg);

// Index file for --index-file (verbose output redirection)
static FILE* g_index_fp = nullptr;

// --totals[=SIGNAL]: running byte counter + optional async-signal-safe dump
static std::atomic<std::int64_t> g_running_total{0};
static std::atomic<bool>         g_totals_signal_armed{false};

static void totals_signal_handler(int /*sig*/) {
    // async-signal-safe path only
    char buf[96];
    std::int64_t n = g_running_total.load(std::memory_order_relaxed);
    // "Total bytes processed: <n>\n"
    static const char prefix[] = "Total bytes processed: ";
    std::size_t pos = 0;
    for (const char* p = prefix; *p; ++p)
        buf[pos++] = *p;
    // format unsigned decimal into buf
    char num[32];
    std::size_t ni = 0;
    std::uint64_t v = static_cast<std::uint64_t>(n < 0 ? 0 : n);
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char tmp[32];
        std::size_t ti = 0;
        while (v > 0 && ti < sizeof(tmp)) {
            tmp[ti++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        while (ti > 0)
            num[ni++] = tmp[--ti];
    }
    for (std::size_t i = 0; i < ni && pos < sizeof(buf) - 2; ++i)
        buf[pos++] = num[i];
    buf[pos++] = '\n';
    (void)::write(STDERR_FILENO, buf, pos);
}

static void install_totals_signal(int sig) {
    if (sig <= 0)
        return;
    struct sigaction sa{};
    sa.sa_handler = totals_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (::sigaction(sig, &sa, nullptr) != 0) {
        print(stderr, "mutar: cannot install totals signal handler: {}\n",
              std::strerror(errno));
        return;
    }
    g_totals_signal_armed.store(true, std::memory_order_relaxed);
}

static void add_running_total(std::int64_t n) {
    if (n > 0)
        g_running_total.fetch_add(n, std::memory_order_relaxed);
}

static void set_running_total(std::int64_t n) {
    g_running_total.store(n, std::memory_order_relaxed);
}

/// Decode GNU-style backslash escapes in a name (octal \nnn, \a \b \f \n \r \t \v \\).
static std::string unquote_name(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c != '\\' || i + 1 >= in.size()) {
            out.push_back(c);
            continue;
        }
        char n = in[++i];
        if (n >= '0' && n <= '7') {
            int val = n - '0';
            int digits = 1;
            while (digits < 3 && i + 1 < in.size() &&
                   in[i + 1] >= '0' && in[i + 1] <= '7') {
                val = val * 8 + (in[++i] - '0');
                ++digits;
            }
            out.push_back(static_cast<char>(val));
        } else {
            switch (n) {
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'v': out.push_back('\v'); break;
            case '\\': out.push_back('\\'); break;
            default: out.push_back(n); break;
            }
        }
    }
    return out;
}

/// Print MUTAR_SNAPSHOT_V2 field ranges (GNU-style layout).
static void print_snapshot_field_ranges() {
    print("This mutar's snapshot file field ranges are\n"
          "   (field name      => [ min, max ]):\n"
          "\n"
          "    name            => [ 1, {} ],\n"
          "    mtime           => [ 0, {} ],\n"
          "    dev             => [ 0, {} ],\n"
          "\n",
          static_cast<unsigned long>(PATH_MAX > 0 ? PATH_MAX : 4096),
          static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max()),
          static_cast<unsigned long long>(std::numeric_limits<std::uint64_t>::max()));
}

/// Parse --totals[=SIGNAL] name into a signal number; 0 if end-of-op only.
static int parse_totals_signal(const char* name) {
    if (!name || !*name)
        return 0;
    std::string_view s(name);
    // Accept with or without SIG prefix
    if (s.size() > 3 && (s[0] == 'S' || s[0] == 's') &&
        (s[1] == 'I' || s[1] == 'i') && (s[2] == 'G' || s[2] == 'g'))
        s.remove_prefix(3);
    auto eq = [](std::string_view a, const char* b) {
        return strcasecmp(std::string(a).c_str(), b) == 0;
    };
    if (eq(s, "HUP"))  return SIGHUP;
    if (eq(s, "QUIT")) return SIGQUIT;
    if (eq(s, "INT"))  return SIGINT;
    if (eq(s, "USR1")) return SIGUSR1;
    if (eq(s, "USR2")) return SIGUSR2;
    return -1; // invalid
}

/// Parse --sparse-version=MAJOR[.MINOR]; returns false on error.
static bool parse_sparse_version(const char* s, unsigned& major, unsigned& minor) {
    if (!s || !*s)
        return false;
    char* end = nullptr;
    errno = 0;
    unsigned long maj = std::strtoul(s, &end, 10);
    if (errno || end == s)
        return false;
    unsigned long min = 0;
    if (*end == '.') {
        const char* mstart = end + 1;
        char* mend = nullptr;
        errno = 0;
        min = std::strtoul(mstart, &mend, 10);
        if (errno || mend == mstart || *mend != '\0')
            return false;
    } else if (*end != '\0') {
        return false;
    }
    // GNU tar accepts 0.0, 0.1, 1.0
    if (!((maj == 0 && (min == 0 || min == 1)) || (maj == 1 && min == 0)))
        return false;
    major = static_cast<unsigned>(maj);
    minor = static_cast<unsigned>(min);
    return true;
}

/// Apply GNU-style --mode=CHANGES (octal or symbolic) to @p mode.
/// Symbolic clauses: [ugoa]*[+-=][rwxXstugo]* separated by commas.
/// @p st_mode is the full st_mode (type + perms) for relative +/− and 'X'.
/// @return false if @p mode_str is empty or unparseable.
static bool apply_mode_changes(unsigned& mode, const std::string& mode_str,
                               mode_t st_mode) {
    if (mode_str.empty())
        return false;

    // Pure octal (optional leading 0): e.g. 640, 0755
    {
        char* endp = nullptr;
        errno = 0;
        long m = std::strtol(mode_str.c_str(), &endp, 8);
        if (endp && *endp == '\0' && errno == 0 && m >= 0 && m <= 07777) {
            mode = static_cast<unsigned>(m) & 07777;
            return true;
        }
    }

    unsigned result = static_cast<unsigned>(st_mode) & 07777;
    const bool is_dir = S_ISDIR(st_mode);
    const char* p = mode_str.c_str();
    while (*p) {
        while (*p == ',' || std::isspace(static_cast<unsigned char>(*p)))
            ++p;
        if (!*p)
            break;

        // who: default a if omitted before op
        unsigned who = 0;
        bool who_set = false;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            who_set = true;
            if (*p == 'u') who |= 04700;      // rwxu + setuid
            else if (*p == 'g') who |= 02070; // rwxg + setgid
            else if (*p == 'o') who |= 01007; // rwxo + sticky
            else who |= 07777;                // a
            ++p;
        }
        if (!who_set)
            who = 07777;

        char op = *p;
        if (op != '+' && op != '-' && op != '=')
            return false;
        ++p;

        unsigned perms = 0;
        bool copy_u = false, copy_g = false, copy_o = false;
        while (*p && *p != ',') {
            char c = *p++;
            switch (c) {
            case 'r': perms |= 0444; break;
            case 'w': perms |= 0222; break;
            case 'x': perms |= 0111; break;
            case 'X':
                // Execute only if directory or any execute bit already set
                if (is_dir || (result & 0111))
                    perms |= 0111;
                break;
            case 's': perms |= 06000; break;
            case 't': perms |= 01000; break;
            case 'u': copy_u = true; break;
            case 'g': copy_g = true; break;
            case 'o': copy_o = true; break;
            default:
                if (std::isspace(static_cast<unsigned char>(c)))
                    continue;
                return false;
            }
        }

        if (copy_u || copy_g || copy_o) {
            unsigned src = 0;
            if (copy_u) src = (result >> 6) & 7;
            else if (copy_g) src = (result >> 3) & 7;
            else src = result & 7;
            perms |= (src << 6) | (src << 3) | src;
        }

        unsigned bits = perms & who;
        if (op == '+') {
            result |= bits;
        } else if (op == '-') {
            result &= ~bits;
        } else { // '='
            result = (result & ~who) | bits;
        }
    }
    mode = result & 07777;
    return true;
}

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

/// True if the archive is (or was requested as) compressed.
/// -r / -u / --delete cannot rewrite a compressed stream in place.
static bool archive_is_compressed(const Config& cfg) {
    switch (cfg.compress) {
    case Compress::Gzip:
    case Compress::Bzip2:
    case Compress::Xz:
    case Compress::Zstd:
    case Compress::Lzma:
    case Compress::Lzip:
    case Compress::Lzop:
    case Compress::CompressZ:
    case Compress::Custom:
        return true;
    default:
        break;
    }
    if (cfg.archive_file.empty() || cfg.archive_file == "-")
        return false;
    if (detect_compress(cfg.archive_file) != Compress::None)
        return true;
    int fd = ::open(cfg.archive_file.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    Compress mag = detect_compress_magic(fd);
    ::close(fd);
    return mag != Compress::None;
}

static bool refuse_compressed_update(const Config& cfg) {
    if (!archive_is_compressed(cfg))
        return false;
    print(stderr, "mutar: Cannot update compressed archives\n");
    return true;
}

// ── Remote tape (rmt) helpers ─────────────────────────────────────────────────

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

/// Speaks the GNU/BSD rmt protocol (O/R/W/L/C) over pipes to `rsh host rmt`.
/// L is lseek (whence + offset); S is status/ioctl and is not used here.
class RmtSession {
public:
    static std::unique_ptr<RmtSession> connect_and_open(
            const std::string& archive_file,
            const std::string& rsh_cmd_arg,
            const std::string& rmt_cmd_arg,
            int open_flags) {
        auto [user, host, remote_path] = parse_remote_path(archive_file);
        const std::string rsh_bin = rsh_cmd_arg.empty() ? "rsh" : rsh_cmd_arg;
        const std::string rmt_bin = rmt_cmd_arg.empty() ? "rmt" : rmt_cmd_arg;

        int to_rmt[2]{-1, -1};
        int from_rmt[2]{-1, -1};
        if (::pipe(to_rmt) < 0 || ::pipe(from_rmt) < 0) {
            print(stderr, "mutar: rmt pipe: {}\n", std::strerror(errno));
            if (to_rmt[0] >= 0) { ::close(to_rmt[0]); ::close(to_rmt[1]); }
            if (from_rmt[0] >= 0) { ::close(from_rmt[0]); ::close(from_rmt[1]); }
            return nullptr;
        }

        pid_t pid = ::fork();
        if (pid < 0) {
            print(stderr, "mutar: rmt fork: {}\n", std::strerror(errno));
            ::close(to_rmt[0]); ::close(to_rmt[1]);
            ::close(from_rmt[0]); ::close(from_rmt[1]);
            return nullptr;
        }
        if (pid == 0) {
            // Child: rsh [ -l user ] host rmt
            ::dup2(to_rmt[0], STDIN_FILENO);
            ::dup2(from_rmt[1], STDOUT_FILENO);
            ::close(to_rmt[0]); ::close(to_rmt[1]);
            ::close(from_rmt[0]); ::close(from_rmt[1]);
            if (!user.empty())
                ::execlp(rsh_bin.c_str(), rsh_bin.c_str(),
                         "-l", user.c_str(), host.c_str(), rmt_bin.c_str(), nullptr);
            else
                ::execlp(rsh_bin.c_str(), rsh_bin.c_str(),
                         host.c_str(), rmt_bin.c_str(), nullptr);
            ::_exit(127);
        }
        ::close(to_rmt[0]);
        ::close(from_rmt[1]);

        auto sess = std::unique_ptr<RmtSession>(new RmtSession());
        sess->to_rmt_ = to_rmt[1];
        sess->from_rmt_ = from_rmt[0];
        sess->rsh_pid_ = pid;

        // O<path>\n<flags>\n  (no space after O — path is the rest of the line)
        std::string open_cmd = std::format("O{}\n{}\n", remote_path, open_flags);
        if (!sess->send_cmd(open_cmd) || sess->read_status() < 0) {
            print(stderr, "mutar: rmt: remote open failed for '{}'\n", remote_path);
            sess->close();
            return nullptr;
        }
        sess->file_open_ = true;
        return sess;
    }

    ~RmtSession() { close(); }

    RmtSession(const RmtSession&) = delete;
    RmtSession& operator=(const RmtSession&) = delete;
    RmtSession(RmtSession&& o) noexcept { *this = std::move(o); }
    RmtSession& operator=(RmtSession&& o) noexcept {
        if (this != &o) {
            close();
            to_rmt_ = o.to_rmt_; from_rmt_ = o.from_rmt_;
            rsh_pid_ = o.rsh_pid_; file_open_ = o.file_open_;
            o.to_rmt_ = o.from_rmt_ = -1;
            o.rsh_pid_ = -1; o.file_open_ = false;
        }
        return *this;
    }

    ssize_t read(void* buf, std::size_t n) {
        if (n == 0) return 0;
        auto* out = static_cast<char*>(buf);
        std::size_t total = 0;
        while (total < n) {
            std::size_t chunk = std::min(n - total, std::size_t{10240});
            if (!send_cmd(std::format("R{}\n", chunk)))
                return total == 0 ? ssize_t{-1} : static_cast<ssize_t>(total);
            long long got = read_status();
            if (got < 0)
                return total == 0 ? ssize_t{-1} : static_cast<ssize_t>(total);
            if (got == 0) break; // EOF
            if (static_cast<std::size_t>(got) > chunk) got = static_cast<long long>(chunk);
            if (!read_exact(out + total, static_cast<std::size_t>(got)))
                return total == 0 ? ssize_t{-1} : static_cast<ssize_t>(total);
            total += static_cast<std::size_t>(got);
            if (static_cast<std::size_t>(got) < chunk) break; // short read → EOF soon
        }
        return static_cast<ssize_t>(total);
    }

    bool write(const void* buf, std::size_t n) {
        const auto* p = static_cast<const char*>(buf);
        std::size_t total = 0;
        while (total < n) {
            std::size_t chunk = std::min(n - total, std::size_t{10240});
            if (!send_cmd(std::format("W{}\n", chunk)))
                return false;
            if (!send_all(p + total, chunk))
                return false;
            long long wr = read_status();
            if (wr < 0 || static_cast<std::size_t>(wr) != chunk)
                return false;
            total += chunk;
        }
        return true;
    }

    /// rmt L command: L<whence>\n<offset>\n → A<new_offset>\n
    off_t seek(off_t offset, int whence) {
        int w = 0;
        switch (whence) {
        case SEEK_SET: w = 0; break;
        case SEEK_CUR: w = 1; break;
        case SEEK_END: w = 2; break;
        default: errno = EINVAL; return static_cast<off_t>(-1);
        }
        if (!send_cmd(std::format("L{}\n{}\n", w, static_cast<long long>(offset)))) {
            errno = EIO;
            return static_cast<off_t>(-1);
        }
        long long pos = read_status();
        if (pos < 0) {
            errno = EIO;
            return static_cast<off_t>(-1);
        }
        return static_cast<off_t>(pos);
    }

    void close() {
        if (file_open_ && to_rmt_ >= 0) {
            send_cmd("C\n");
            (void)read_status();
            file_open_ = false;
        }
        if (to_rmt_ >= 0) { ::close(to_rmt_); to_rmt_ = -1; }
        if (from_rmt_ >= 0) { ::close(from_rmt_); from_rmt_ = -1; }
        if (rsh_pid_ > 0) {
            int st = 0;
            ::waitpid(rsh_pid_, &st, 0);
            rsh_pid_ = -1;
        }
    }

private:
    RmtSession() = default;

    int   to_rmt_   = -1;
    int   from_rmt_ = -1;
    pid_t rsh_pid_  = -1;
    bool  file_open_ = false;

    bool send_all(const void* data, std::size_t n) {
        const auto* p = static_cast<const char*>(data);
        while (n > 0) {
            ssize_t w = ::write(to_rmt_, p, n);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            if (w == 0) return false;
            p += w; n -= static_cast<std::size_t>(w);
        }
        return true;
    }

    bool send_cmd(std::string_view cmd) {
        return send_all(cmd.data(), cmd.size());
    }

    std::string read_line() {
        std::string resp;
        char c;
        while (::read(from_rmt_, &c, 1) == 1) {
            resp += c;
            if (c == '\n') break;
        }
        return resp;
    }

    bool read_exact(char* buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            ssize_t r = ::read(from_rmt_, buf + got, n - got);
            if (r < 0) { if (errno == EINTR) continue; return false; }
            if (r == 0) return false;
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    /// Parse A<number>\\n success or E... error. Returns number or -1.
    long long read_status() {
        std::string resp = read_line();
        if (resp.empty() || resp[0] != 'A') {
            if (!resp.empty() && resp[0] == 'E') {
                // Drain error message line
                (void)read_line();
            }
            return -1;
        }
        try {
            return std::stoll(resp.substr(1));
        } catch (...) {
            return -1;
        }
    }
};

// ── ArchiveStream: wraps a raw fd or rmt session, optionally via compressor ──

class ArchiveStream {
public:
    // Read: open archive for reading (decompress if needed)
    static Result<ArchiveStream> open_read(const Config& cfg) {
        ArchiveStream s;
        const bool use_stdin = cfg.archive_file.empty() || cfg.archive_file == "-";

        if (use_stdin) {
            s.fd_ = STDIN_FILENO;
        } else if (is_remote_archive(cfg.archive_file, cfg.force_local)) {
            // Remote archive via rmt protocol (seekable via L)
            auto rmt = RmtSession::connect_and_open(
                cfg.archive_file, cfg.rsh_command, cfg.rmt_command, O_RDONLY);
            if (!rmt) return std::unexpected(msg_error(
                std::format("cannot open remote archive '{}' for reading", cfg.archive_file)));
            s.rmt_ = std::move(rmt);
            s.child_pid_ = -2; // sentinel: rmt session owns the child
        } else {
            s.fd_ = ::open(cfg.archive_file.c_str(), O_RDONLY);
            if (s.fd_ < 0) return std::unexpected(sys_error(cfg.archive_file));
            s.owns_fd_ = true;
        }

        // Determine compression
        Compress comp = cfg.compress;
        if (comp == Compress::Auto) {
            comp = detect_compress(cfg.archive_file);
            if (comp == Compress::None && s.owns_fd_ && !s.rmt_)
                comp = detect_compress_magic(s.fd_);
        } else if (comp != Compress::None && s.owns_fd_ && !s.rmt_) {
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

            // Remote + compress: download compressed bytes to a local temp, then
            // decompress from that file (rmt is not a plain fd for execl).
            if (s.rmt_) {
                char tmpl[] = "/tmp/mutar_rcmp_XXXXXX";
                int tfd = ::mkstemp(tmpl);
                if (tfd < 0)
                    return std::unexpected(sys_error("mkstemp for remote compress"));
                char buf[1 << 16];
                for (;;) {
                    ssize_t n = s.rmt_->read(buf, sizeof(buf));
                    if (n < 0) {
                        ::close(tfd); ::unlink(tmpl);
                        return std::unexpected(msg_error(
                            "remote read failed while materializing compressed archive"));
                    }
                    if (n == 0) break;
                    std::size_t off = 0;
                    while (off < static_cast<std::size_t>(n)) {
                        ssize_t w = ::write(tfd, buf + off,
                                            static_cast<std::size_t>(n) - off);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            ::close(tfd); ::unlink(tmpl);
                            return std::unexpected(sys_error("write remote materialize"));
                        }
                        off += static_cast<std::size_t>(w);
                    }
                }
                s.rmt_->close();
                s.rmt_.reset();
                s.child_pid_ = -1;
                if (::lseek(tfd, 0, SEEK_SET) < 0) {
                    ::close(tfd); ::unlink(tmpl);
                    return std::unexpected(sys_error("lseek remote materialize"));
                }
                s.fd_ = tfd;
                s.owns_fd_ = true;
                s.temp_unlink_ = tmpl;
            }

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
            // temp_unlink_ kept until close() so the child can still read it
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

        // Remote + compress: write compressed stream to a local temp, upload via
        // rmt on close(). Plain remote (no compress) still uses live rmt.
        const bool remote = !use_stdout
            && is_remote_archive(cfg.archive_file, cfg.force_local);
        const bool want_compress = (comp != Compress::None && comp != Compress::Auto);

        if (use_stdout) {
            s.fd_ = STDOUT_FILENO;
        } else if (remote && want_compress) {
            char tmpl[] = "/tmp/mutar_wcmp_XXXXXX";
            int tfd = ::mkstemp(tmpl);
            if (tfd < 0)
                return std::unexpected(sys_error("mkstemp for remote compress write"));
            s.fd_ = tfd;
            s.owns_fd_ = true;
            s.temp_unlink_ = tmpl;
            s.remote_upload_ = cfg.archive_file;
            s.rsh_command_ = cfg.rsh_command;
            s.rmt_command_ = cfg.rmt_command;
        } else if (remote) {
            // Remote create: O_WRONLY|O_CREAT|O_TRUNC via rmt
            int flags = O_WRONLY | O_CREAT | O_TRUNC;
            auto rmt = RmtSession::connect_and_open(
                cfg.archive_file, cfg.rsh_command, cfg.rmt_command, flags);
            if (!rmt) return std::unexpected(msg_error(
                std::format("cannot open remote archive '{}' for writing", cfg.archive_file)));
            s.rmt_ = std::move(rmt);
            s.child_pid_ = -2; // sentinel: rmt session
        } else {
            int flags = O_WRONLY | O_CREAT | O_TRUNC;
            s.fd_ = ::open(cfg.archive_file.c_str(), flags, 0666);
            if (s.fd_ < 0) return std::unexpected(sys_error(cfg.archive_file));
            s.owns_fd_ = true;
        }

        if (want_compress) {
            const char* prog = compress_prog_for(comp, cfg.compress_prog);
            if (!prog || !prog[0])
                return std::unexpected(msg_error("unknown compression program"));

            // --seekable: prefer multi-block xz / chunked zstd; warn for solid gzip/bzip2.
            if (cfg.seekable) {
                if (comp == Compress::Gzip || comp == Compress::Bzip2 ||
                    comp == Compress::CompressZ || comp == Compress::Lzop) {
                    mutar_warn(cfg, "compress",
                        std::format("--seekable with {} still needs full "
                                    "decompress for random access (solid stream)",
                                    prog));
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
            ssize_t n = src.read_buf(buf, sizeof(buf));
            if (n < 0) {
                int e = errno ? errno : EIO;
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
        if (src.child_failed()) {
            ::close(tfd);
            ::unlink(tmpl);
            return std::unexpected(msg_error(src.child_error()));
        }
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
        if (is_remote_archive(cfg.archive_file, cfg.force_local)) {
            // Remote append/update: O_RDWR + rmt L (lseek) for end-of-archive
            auto rmt = RmtSession::connect_and_open(
                cfg.archive_file, cfg.rsh_command, cfg.rmt_command, O_RDWR);
            if (!rmt) return std::unexpected(msg_error(
                std::format("cannot open remote archive '{}' for update",
                            cfg.archive_file)));
            s.rmt_ = std::move(rmt);
            s.child_pid_ = -2;
            return s;
        }
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
          patch_fd_(o.patch_fd_), patch_mtime_(o.patch_mtime_),
          rmt_(std::move(o.rmt_)),
          temp_unlink_(std::move(o.temp_unlink_)),
          remote_upload_(std::move(o.remote_upload_)),
          rsh_command_(std::move(o.rsh_command_)),
          rmt_command_(std::move(o.rmt_command_)),
          child_failed_(o.child_failed_),
          child_status_(o.child_status_),
          child_error_(std::move(o.child_error_)) {
        o.fd_ = -1; o.owns_fd_ = false; o.child_pid_ = -1;
        o.patch_fd_ = -1; o.patch_mtime_ = 0;
        o.child_failed_ = false; o.child_status_ = 0; o.child_error_.clear();
    }
    ArchiveStream& operator=(ArchiveStream&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_; owns_fd_ = o.owns_fd_; child_pid_ = o.child_pid_;
            patch_fd_ = o.patch_fd_; patch_mtime_ = o.patch_mtime_;
            rmt_ = std::move(o.rmt_);
            temp_unlink_ = std::move(o.temp_unlink_);
            remote_upload_ = std::move(o.remote_upload_);
            rsh_command_ = std::move(o.rsh_command_);
            rmt_command_ = std::move(o.rmt_command_);
            child_failed_ = o.child_failed_;
            child_status_ = o.child_status_;
            child_error_ = std::move(o.child_error_);
            o.fd_ = -1; o.owns_fd_ = false; o.child_pid_ = -1;
            o.patch_fd_ = -1; o.patch_mtime_ = 0;
            o.child_failed_ = false; o.child_status_ = 0; o.child_error_.clear();
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
        if (rmt_) {
            rmt_->close();
            rmt_.reset();
            child_pid_ = -1;
        }
        // Close write end of compressor pipe first so the child can finish.
        if (owns_fd_ && fd_ >= 0) { ::close(fd_); fd_ = -1; owns_fd_ = false; }
        if (child_pid_ > 0) {
            int st = 0;
            if (::waitpid(child_pid_, &st, 0) < 0) {
                child_failed_ = true;
                child_error_ = std::format("waitpid: {}", std::strerror(errno));
            } else {
                child_status_ = st;
                if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
                    child_failed_ = true;
                    if (WIFEXITED(st))
                        child_error_ = std::format(
                            "child process exited with status {}", WEXITSTATUS(st));
                    else if (WIFSIGNALED(st))
                        child_error_ = std::format(
                            "child process killed by signal {}", WTERMSIG(st));
                    else
                        child_error_ = "child process failed";
                }
            }
            child_pid_ = -1;
            // After compressor finishes, patch gzip mtime header (bytes 4-7, LE uint32).
            // Only patch when the compressor exited successfully to avoid writing
            // into a corrupt/partial stream.
            if (patch_fd_ >= 0) {
                bool compressor_ok = !child_failed_ &&
                    WIFEXITED(st) && WEXITSTATUS(st) == 0;
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
        // Deferred remote upload of compressed temp (create -z -f host:path)
        if (!remote_upload_.empty() && !temp_unlink_.empty()) {
            auto rmt = RmtSession::connect_and_open(
                remote_upload_, rsh_command_, rmt_command_,
                O_WRONLY | O_CREAT | O_TRUNC);
            if (rmt) {
                int rfd = ::open(temp_unlink_.c_str(), O_RDONLY);
                if (rfd >= 0) {
                    char buf[1 << 16];
                    for (;;) {
                        ssize_t n = ::read(rfd, buf, sizeof(buf));
                        if (n < 0) { if (errno == EINTR) continue; break; }
                        if (n == 0) break;
                        if (!rmt->write(buf, static_cast<std::size_t>(n)))
                            break;
                    }
                    ::close(rfd);
                }
                rmt->close();
            } else {
                print(stderr, "mutar: warning: failed to upload compressed archive to '{}'\n",
                      remote_upload_);
            }
            remote_upload_.clear();
        }
        if (!temp_unlink_.empty()) {
            ::unlink(temp_unlink_.c_str());
            temp_unlink_.clear();
        }
    }

    int fd() const noexcept { return fd_; }

    /// True when a compressor/decompressor child exited non-zero (after close()).
    [[nodiscard]] bool child_failed() const noexcept { return child_failed_; }
    [[nodiscard]] const std::string& child_error() const noexcept { return child_error_; }

    // Full-buffer reads (for block-aligned I/O)
    ssize_t read_buf(void* buf, std::size_t n) {
        if (rmt_) return rmt_->read(buf, n);
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
        if (rmt_) return rmt_->write(buf, n);
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
        if (rmt_) return rmt_->seek(pos, whence);
        return ::lseek(fd_, pos, whence);
    }

    /// True when the underlying fd supports lseek and is not a compression pipe.
    /// Remote rmt sessions are seekable via the L protocol command.
    /// Honours Config::seek: --no-seek forces false; -n/--seek allows auto-detect.
    [[nodiscard]] bool is_seekable(bool allow_seek = true) const noexcept {
        if (!allow_seek) return false;
        if (rmt_) return true;
        if (fd_ < 0) return false;
        if (child_pid_ > 0) return false; // decompressor/compressor pipe
        errno = 0;
        off_t cur = ::lseek(fd_, 0, SEEK_CUR);
        return cur != static_cast<off_t>(-1);
    }

    bool truncate_at(off_t pos) {
        if (rmt_) return false; // rmt has no ftruncate
        return ::ftruncate(fd_, pos) == 0;
    }

private:
    int   fd_        = -1;
    bool  owns_fd_   = false;
    pid_t child_pid_ = -1;
    int   patch_fd_  = -1;       // dup of output fd for gzip mtime patching
    time_t patch_mtime_ = 0;     // archive creation time to embed in gzip header
    std::unique_ptr<RmtSession> rmt_;
    std::string temp_unlink_;    // local temp for remote+compress materialize
    std::string remote_upload_;  // host:path to upload temp_unlink_ on close
    std::string rsh_command_;
    std::string rmt_command_;
    bool        child_failed_ = false;
    int         child_status_ = 0;
    std::string child_error_;
};

// ── Block buffer (blocking-factor–aligned I/O) ───────────────────────────────

class BlockBuffer {
public:
    explicit BlockBuffer(int blocking = DEFAULT_BLOCK, bool read_full_records = false) {
        // Validate before any use — a zero/negative factor wraps; a huge
        // factor (e.g. --record-size=16MiB+) ASan-aborts on resize.
        if (blocking < 1 || blocking > MAX_BLOCKING_FACTOR) {
            std::fprintf(stderr,
                         "mutar: invalid blocking factor %d (must be 1..%d): "
                         "memory exhausted\n",
                         blocking, MAX_BLOCKING_FACTOR);
            std::exit(EXIT_FAILURE);
        }
        blocking_          = blocking;
        record_size_       = static_cast<std::size_t>(blocking) * BLOCKSIZE;
        read_full_records_ = read_full_records;
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
        // Advance pos_ so a subsequent call hits fill() instead of spinning
        // on the same short tail (skip_entry used to loop forever this way).
        if (pos_ + BLOCKSIZE > used_) {
            eof_ = true;
            pos_ = used_;
            return false;
        }
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
        // -B/--read-full-records: reblock short pipe reads to a full record
        // (GNU 4.2BSD pipe semantics). Without -B, a single read is enough.
        std::size_t got = 0;
        while (got < record_size_) {
            ssize_t n = s.read_buf(buf_.data() + got, record_size_ - got);
            if (n < 0) { used_ = 0; return false; }
            if (n == 0) break;
            got += static_cast<std::size_t>(n);
            if (!read_full_records_) break;
        }
        if (got == 0) { used_ = 0; return false; }
        used_ = got;
        pos_  = 0;
        return true;
    }

    int              blocking_;
    std::size_t      record_size_;
    bool             read_full_records_ = false;
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

    /// Write index. Optional @p note becomes a `# …` comment after the magic
    /// (ignored by load). Used for `--seekable` compressed archives.
    Result<void> save(const std::filesystem::path& path,
                      std::string_view note = {}) const {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return std::unexpected(sys_error(path.string()));
        out << "MUTAR.INDEX.V1\n";
        if (!note.empty())
            out << "# " << note << '\n';
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
    {
        std::int64_t sz = 0;
        if (!read_number_i64({h.size, 12}, sz) || sz < 0) {
            e.size = -1;
            e.size_invalid = true;
        } else {
            e.size = sz;
        }
    }
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

    // GNUTYPE_MULTIVOL ('M'): offset at oldgnu.offset even when magic is empty
    // (GNU tar often writes continuation headers with a blank magic field).
    if (h.typeflag == GNUTYPE_MULTIVOL) {
        e.multivol_offset =
            static_cast<std::int64_t>(read_octal({blk.oldgnu.offset, 12}));
        if (e.fmt == Format::V7)
            e.fmt = Format::GNU;
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

    // POSIX ustar: name/linkname/prefix may occupy the full field with no
    // trailing NUL when the string is exactly the field width.
    auto write_field = [](char* dst, std::size_t n, std::string_view s,
                          bool allow_full = false) {
        std::size_t maxn = allow_full ? n : (n > 0 ? n - 1 : 0);
        std::size_t len = std::min(s.size(), maxn);
        if (len > 0)
            std::memcpy(dst, s.data(), len);
        if (len < n)
            dst[len] = '\0';
    };

    write_field(h.name,     100, name, true);
    write_field(h.linkname, 100, e.linkname, true);
    write_field(h.prefix,   155, prefix, true);

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

    // Multi-volume continuation: GNU oldgnu offset at byte 369 (overlays prefix).
    if (e.typeflag == GNUTYPE_MULTIVOL || e.multivol_offset > 0) {
        write_octal(blk.oldgnu.offset, 12,
                    static_cast<std::uint64_t>(e.multivol_offset));
    }

    write_checksum(blk);
    return blk;
}

// ── PAX extended header encode/decode ─────────────────────────────────────────

// Defined later; used by --pax-option brace/{date} expansion.
static std::time_t parse_date_string(const std::string& s);

/// Keywords GNU tar marks XHDR_PROTECTED (cannot delete= / override).
[[nodiscard]] static bool pax_protected_keyword(std::string_view kw) noexcept
{
    static constexpr std::string_view kProtected[] = {
        "GNU.sparse.name", "GNU.sparse.major", "GNU.sparse.minor",
        "GNU.sparse.realsize", "GNU.sparse.numblocks", "GNU.sparse.size",
        "GNU.sparse.offset", "GNU.sparse.numbytes",
        "GNU.dumpdir",
        "GNU.volume.label", "GNU.volume.filename",
        "GNU.volume.size", "GNU.volume.offset",
    };
    for (auto p : kProtected)
        if (kw == p) return true;
    return false;
}

/// True if delete=PATTERN would match a protected keyword (GNU rejects).
[[nodiscard]] static bool pax_protected_pattern(std::string_view pattern)
{
    static constexpr std::string_view kProtected[] = {
        "GNU.sparse.name", "GNU.sparse.major", "GNU.sparse.minor",
        "GNU.sparse.realsize", "GNU.sparse.numblocks", "GNU.sparse.size",
        "GNU.sparse.offset", "GNU.sparse.numbytes",
        "GNU.dumpdir",
        "GNU.volume.label", "GNU.volume.filename",
        "GNU.volume.size", "GNU.volume.offset",
    };
    std::string pat(pattern);
    for (auto p : kProtected)
        if (::fnmatch(pat.c_str(), std::string(p).c_str(), 0) == 0)
            return true;
    return false;
}

[[nodiscard]] static bool pax_keyword_deleted(const PaxOptionRules& rules,
                                              std::string_view key)
{
    std::string k(key);
    for (const auto& pat : rules.delete_patterns)
        if (::fnmatch(pat.c_str(), k.c_str(), 0) == 0)
            return true;
    return false;
}

[[nodiscard]] static bool pax_keyword_file_override(const PaxOptionRules& rules,
                                                    std::string_view key)
{
    for (const auto& [k, v] : rules.file_overrides) {
        (void)v;
        if (k == key) return true;
    }
    return false;
}

/// True if keyword may be auto-emitted (not deleted, not file-overridden).
[[nodiscard]] static bool pax_allowed(const Config& cfg, std::string_view key)
{
    if (pax_keyword_deleted(cfg.pax_option_rules, key)) return false;
    if (pax_keyword_file_override(cfg.pax_option_rules, key)) return false;
    return true;
}

/// Expand exthdr/globexthdr name template: %d %f %p %n %%.
[[nodiscard]] static std::string pax_format_name(std::string_view fmt,
                                                 std::string_view filepath,
                                                 std::size_t seq)
{
    std::string dir, base;
    {
        auto slash = filepath.find_last_of('/');
        if (slash == std::string_view::npos) {
            dir  = ".";
            base = std::string(filepath);
        } else {
            dir = std::string(filepath.substr(0, slash));
            if (dir.empty()) dir = "/";
            base = std::string(filepath.substr(slash + 1));
        }
    }
    const std::string pid  = std::to_string(::getpid());
    const std::string nstr = std::to_string(seq);
    std::string out;
    out.reserve(fmt.size() + dir.size() + base.size() + 16);
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            switch (fmt[i + 1]) {
            case '%': out.push_back('%'); ++i; break;
            case 'd': out += dir;  ++i; break;
            case 'f': out += base; ++i; break;
            case 'p': out += pid;  ++i; break;
            case 'n': out += nstr; ++i; break;
            default:
                out.push_back(fmt[i]);
                out.push_back(fmt[i + 1]);
                ++i;
                break;
            }
        } else {
            out.push_back(fmt[i]);
        }
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

[[nodiscard]] static std::string pax_default_exthdr_template()
{
    return std::getenv("POSIXLY_CORRECT")
        ? std::string("%d/PaxHeaders.%p/%f")
        : std::string("%d/PaxHeaders/%f");
}

[[nodiscard]] static std::string pax_default_globexthdr_template()
{
    const char* tmpdir = std::getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    std::string t(tmpdir);
    if (std::getenv("POSIXLY_CORRECT"))
        t += "/GlobalHead.%p.%n";
    else
        t += "/GlobalHead.%n";
    return t;
}

[[nodiscard]] static std::string pax_exthdr_member_name(const Entry& e,
                                                       const Config& cfg)
{
    std::string tmpl = cfg.pax_option_rules.exthdr_name;
    if (tmpl.empty()) tmpl = pax_default_exthdr_template();
    std::string name = pax_format_name(tmpl, e.name, 0);
    if (name.size() > 99) name.resize(99);
    return name;
}

/// Expand {date-or-file} brace values used in --pax-option (GNU get_date style).
[[nodiscard]] static std::string pax_expand_brace_value(std::string_view val)
{
    if (val.size() < 2 || val.front() != '{' || val.back() != '}')
        return std::string(val);
    std::string inner(val.substr(1, val.size() - 2));
    if (inner == "now")
        return std::to_string(static_cast<long long>(std::time(nullptr)));
    std::time_t t = parse_date_string(inner);
    if (t != static_cast<std::time_t>(-1))
        return std::to_string(static_cast<long long>(t));
    return std::string(val);
}

/// Parse one comma-separated --pax-option item into rules (GNU xheader_set_option).
static void parse_pax_option_item(PaxOptionRules& rules, std::string_view item)
{
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ')  item.remove_suffix(1);
    if (item.empty()) return;

    auto eq = item.find('=');
    if (eq == std::string_view::npos) {
        print(stderr, "mutar: Keyword {} is unknown or not yet implemented\n", item);
        std::exit(EXIT_FAILURE);
    }

    bool per_file = false;
    std::size_t kw_end = eq;
    if (eq > 0 && item[eq - 1] == ':') {
        per_file = true;
        kw_end = eq - 1;
    }
    while (kw_end > 0 && item[kw_end - 1] == ' ') --kw_end;
    std::string_view kw = item.substr(0, kw_end);
    std::string_view raw_val = item.substr(eq + 1);
    while (!raw_val.empty() && raw_val.front() == ' ') raw_val.remove_prefix(1);
    std::string val = pax_expand_brace_value(raw_val);

    if (kw == "delete") {
        if (val.empty()) return;
        if (pax_protected_pattern(val)) {
            print(stderr, "mutar: Pattern {} cannot be used\n", val);
            std::exit(EXIT_FAILURE);
        }
        rules.delete_patterns.push_back(std::move(val));
        return;
    }
    if (kw == "exthdr.name") {
        rules.exthdr_name = std::move(val);
        return;
    }
    if (kw == "globexthdr.name") {
        rules.globexthdr_name = std::move(val);
        return;
    }
    if (kw == "exthdr.mtime") {
        std::time_t t = parse_date_string(val);
        if (t == static_cast<std::time_t>(-1)) {
            print(stderr, "mutar: Time stamp is out of allowed range\n");
            std::exit(EXIT_FAILURE);
        }
        rules.has_exthdr_mtime = true;
        rules.exthdr_mtime = static_cast<std::int64_t>(t);
        return;
    }
    if (kw == "globexthdr.mtime") {
        std::time_t t = parse_date_string(val);
        if (t == static_cast<std::time_t>(-1)) {
            print(stderr, "mutar: Time stamp is out of allowed range\n");
            std::exit(EXIT_FAILURE);
        }
        rules.has_globexthdr_mtime = true;
        rules.globexthdr_mtime = static_cast<std::int64_t>(t);
        return;
    }

    if (pax_protected_keyword(kw)) {
        print(stderr, "mutar: Keyword {} cannot be overridden\n", kw);
        std::exit(EXIT_FAILURE);
    }
    if (per_file)
        rules.file_overrides.emplace_back(std::string(kw), std::move(val));
    else
        rules.global_overrides.emplace_back(std::string(kw), std::move(val));
}

static void parse_pax_option_string(PaxOptionRules& rules, std::string_view all)
{
    while (!all.empty()) {
        auto comma = all.find(',');
        std::string_view item = (comma == std::string_view::npos)
            ? all : all.substr(0, comma);
        if (comma == std::string_view::npos)
            all = {};
        else
            all.remove_prefix(comma + 1);
        parse_pax_option_item(rules, item);
    }
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

/// Append a PAX record only when the keyword is not deleted / file-overridden.
static void pax_append_if(std::string& out, const Config& cfg,
                          std::string_view key, std::string_view val)
{
    if (pax_allowed(cfg, key))
        pax_append(out, key, val);
}

/// Append keyword:=value file overrides (after auto-coded fields).
static void pax_append_file_overrides(std::string& out, const Config& cfg)
{
    for (const auto& [k, v] : cfg.pax_option_rules.file_overrides) {
        if (!pax_keyword_deleted(cfg.pax_option_rules, k))
            pax_append(out, k, v);
    }
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

/// Privileged xattr namespaces must not be stored or restored from archives
/// (privilege escalation / policy bypass). user.* is the safe default set.
static bool xattr_namespace_privileged(std::string_view key) {
    return key.starts_with("security.")
        || key.starts_with("trusted.")
        || key.starts_with("system.");
}

/// Whether to store this xattr key under --xattrs (include/exclude + hard skips).
static bool xattr_key_wanted(const Config& cfg, std::string_view key) {
    // Never store privileged namespaces (security.*, trusted.*, system.*).
    // POSIX ACLs are handled by --acls → SCHILY.acl.*; SELinux unsupported.
    if (xattr_namespace_privileged(key)) return false;
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
        // Restore only non-privileged namespaces (typically user.*).
        // security.*, trusted.*, and system.* are never applied from archives.
        if (xattr_namespace_privileged(name)) continue;
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

// Forward-declared earlier for ArchiveStream; definition here.
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

/// Resolve a local archive path against the current working directory.
/// Used so -C chdir (extract) / later volumes do not rewrite relative -f paths.
static std::string absolutize_local_archive(const Config& cfg) {
    const std::string& path = cfg.archive_file;
    if (path.empty() || path == "-")
        return path;
    if (is_remote_archive(path, cfg.force_local))
        return path;
    std::filesystem::path p(path);
    if (p.is_absolute())
        return path;
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec)
        return path;
    return abs.lexically_normal().string();
}

// Generate the archive filename for volume N (1-based).
// If base contains "%d", substitute the first occurrence with the volume number
// (GNU tar convention). Never pass the user path to printf-family as a format.
// Otherwise volume 1 = base, volume N = base.N
static std::string make_volume_name(const std::string& base, int vol_num) {
    auto pos = base.find("%d");
    if (pos != std::string::npos) {
        std::string out = base;
        out.replace(pos, 2, std::to_string(vol_num));
        return out;
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
// Forward declarations of helpers used inside ArchiveWriter (defined later)
static std::time_t parse_date_string(const std::string& s);
static std::string apply_transform(const std::string& name, const std::string& expr);
static void apply_owner_group_cli(Entry& e, const Config& cfg);

struct SparseSegment { std::int64_t offset; std::int64_t length; };
static std::vector<SparseSegment> detect_sparse_segments(int fd, std::int64_t file_size,
                                                          std::string_view hole_detection = "seek");

// ── Archive reader ────────────────────────────────────────────────────────────

// Reads entries sequentially from an archive.
// Handles: GNU LongName/LongLink, PAX extended headers, sparse maps.
class ArchiveReader {
public:
    ArchiveReader(ArchiveStream& s, int blocking, bool ignore_zeros = false,
                  bool read_full_records = false)
        : stream_(&s), buf_(blocking, read_full_records), ignore_zeros_(ignore_zeros) {}

    /// Optional Config for --warning filtering of reader-side messages.
    void set_warn_config(const Config* cfg) { warn_cfg_ = cfg; }

    /// Apply --pax-option rules on read (delete=, keyword=/:= overrides).
    void set_pax_rules(const PaxOptionRules* rules) {
        pax_rules_ = rules;
        // CLI keyword=value acts as initial global overrides (seed only once).
        cli_global_pax_.clear();
        if (rules) {
            for (const auto& [k, v] : rules->global_overrides)
                cli_global_pax_[k] = v;
        }
    }

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
            if (!buf_.read_block(*stream_, blk)) {
                if (!saw_any_block_) {
                    print(stderr, "mutar: This does not look like a tar archive\n");
                    failed_ = true;
                }
                return {{}, false, true};
            }
            saw_any_block_ = true;

            // EOF block: two consecutive zero blocks (or single if --ignore-zeros)
            if (is_zero_block(blk)) {
                if (ignore_zeros_) continue; // skip zero blocks, keep reading
                Block blk2{};
                buf_.read_block(*stream_, blk2); // consume second zero block
                return {{}, false, true};
            }

            if (!valid_checksum(blk)) {
                if (warn_cfg_)
                    mutar_warn(*warn_cfg_, "checksum",
                               std::format("invalid block checksum at block {}",
                                           buf_.block_no()));
                else
                    print(stderr, "mutar: invalid block checksum at block {}\n",
                          buf_.block_no());
                return {{}, false, false};
            }

            Entry e = decode_header(blk);
            e.block_offset = buf_.block_no() - 1;

            if (e.size_invalid) {
                print(stderr, "mutar: invalid or overflowing member size\n");
                failed_ = true;
                return {{}, false, false};
            }

            // GNU LongName / LongLink
            if (e.typeflag == GNUTYPE_LONGNAME || e.typeflag == GNUTYPE_LONGLINK) {
                std::string longstr;
                if (!read_data_string(e.size, longstr))
                    return {{}, false, true};
                // Strip trailing NUL
                while (!longstr.empty() && longstr.back() == '\0') longstr.pop_back();
                if (e.typeflag == GNUTYPE_LONGNAME) pending_longname = std::move(longstr);
                else                                pending_longlink = std::move(longstr);
                continue;
            }

            // PAX extended / global headers
            if (e.typeflag == XHDTYPE || e.typeflag == XGLTYPE) {
                std::string pax_data;
                if (!read_data_string(e.size, pax_data))
                    return {{}, false, true};
                auto parsed = pax_parse_filtered(pax_data);
                if (e.typeflag == XGLTYPE) {
                    // Global header replaces prior archive-global map (GNU).
                    archive_global_pax_ = std::move(parsed);
                } else {
                    pending_pax = std::move(parsed);
                }
                continue;
            }

            // Apply pending long names
            if (!pending_longname.empty()) { e.name     = pending_longname; pending_longname.clear(); }
            if (!pending_longlink.empty()) { e.linkname = pending_longlink; pending_longlink.clear(); }

            // Merge PAX attrs: CLI global → archive global → file x → CLI :=
            e.pax_attrs = merge_pax_attrs(pending_pax);
            pending_pax.clear();
            apply_pax_attrs(e);

            // GNU sparse: read sparse map from header + extension blocks
            if (e.typeflag == GNUTYPE_SPARSE || e.is_sparse)
                read_gnu_sparse_map(e, blk);

            return {std::move(e), true, false};
        }
    }

    /// True after a fatal read error (bad size, unexpected EOF, etc.).
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // Skip over the data blocks for the current entry.
    // Stops on EOF / short read (never spins on a huge claimed size).
    bool skip_entry(const Entry& e) {
        if (failed_)
            return false;
        std::int64_t to_skip = e.asize;
        if (to_skip < 0) {
            print(stderr, "mutar: invalid member size {}\n", to_skip);
            failed_ = true;
            return false;
        }
        if (to_skip == 0)
            return true;
        Block dummy{};
        const std::int64_t max_pad = std::numeric_limits<std::int64_t>::max()
            - static_cast<std::int64_t>(BLOCKSIZE - 1);
        if (to_skip > max_pad) {
            while (buf_.read_block(*stream_, dummy)) {}
            print(stderr, "mutar: unexpected EOF in archive\n");
            failed_ = true;
            return false;
        }
        std::int64_t blocks = (to_skip + BLOCKSIZE - 1) / BLOCKSIZE;
        for (std::int64_t i = 0; i < blocks; ++i) {
            if (!buf_.read_block(*stream_, dummy)) {
                print(stderr, "mutar: unexpected EOF in archive\n");
                failed_ = true;
                return false;
            }
        }
        return true;
    }

    // Read entry data into a string (used for LongName, PAX headers, dumpdir).
    // Rejects negative / oversized claims before any allocation.
    bool read_data_string(std::int64_t sz, std::string& out) {
        out.clear();
        if (failed_)
            return false;
        if (sz < 0) {
            print(stderr, "mutar: invalid member size {}\n", sz);
            failed_ = true;
            return false;
        }
        // Metadata (L/K/x/g/dumpdir) is not a payload file; cap before resize.
        static constexpr std::int64_t k_max_metadata = 16 * 1024 * 1024;
        if (sz > k_max_metadata) {
            print(stderr, "mutar: member size {} exceeds metadata limit ({})\n",
                  sz, k_max_metadata);
            failed_ = true;
            return false;
        }
        if (sz == 0)
            return true;
        std::int64_t blocks = (sz + BLOCKSIZE - 1) / BLOCKSIZE;
        std::string blockbuf(static_cast<std::size_t>(blocks * BLOCKSIZE), '\0');
        Block blk{};
        for (std::int64_t i = 0; i < blocks; ++i) {
            if (!buf_.read_block(*stream_, blk)) {
                print(stderr, "mutar: unexpected EOF in archive\n");
                failed_ = true;
                return false;
            }
            std::memcpy(blockbuf.data() + i * BLOCKSIZE, blk.buffer, BLOCKSIZE);
        }
        out.assign(blockbuf.data(), static_cast<std::size_t>(sz));
        return true;
    }

    // Read entry data, writing to fd (or string if fd < 0).
    // On clean volume EOF before asize bytes (multi-volume mid-file), returns
    // true and sets *got_out to bytes actually read (may be < e.asize).
    // Returns false only on write/I/O error.
    bool read_entry_data(const Entry& e, int out_fd,
                         std::function<void(const char*, std::size_t)> on_data = {},
                         std::int64_t* got_out = nullptr) {
        std::int64_t remaining = e.asize;
        std::int64_t got_total = 0;
        Block blk{};
        while (remaining > 0) {
            if (!buf_.read_block(*stream_, blk)) {
                if (got_out) *got_out = got_total;
                // Short read is OK for multi-volume; caller decides.
                return true;
            }
            std::size_t chunk = static_cast<std::size_t>(
                std::min<std::int64_t>(BLOCKSIZE, remaining));
            if (out_fd >= 0) {
                const char* p = blk.buffer;
                std::size_t left = chunk;
                while (left > 0) {
                    ssize_t w = ::write(out_fd, p, left);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        // Command closed the pipe (--to-command); keep
                        // consuming the member so the archive stays aligned.
                        if (errno == EPIPE) {
                            out_fd = -1;
                            break;
                        }
                        if (got_out) *got_out = got_total;
                        return false;
                    }
                    p += w; left -= static_cast<std::size_t>(w);
                }
            }
            if (on_data) on_data(blk.buffer, chunk);
            got_total += static_cast<std::int64_t>(chunk);
            remaining -= BLOCKSIZE;
        }
        if (got_out) *got_out = got_total;
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

    /// Parse PAX payload, dropping keywords matched by delete= patterns.
    std::map<std::string, std::string> pax_parse_filtered(std::string_view data) const {
        auto attrs = pax_parse(data);
        if (!pax_rules_) return attrs;
        for (auto it = attrs.begin(); it != attrs.end(); ) {
            if (pax_keyword_deleted(*pax_rules_, it->first) ||
                pax_keyword_file_override(*pax_rules_, it->first))
                it = attrs.erase(it);
            else
                ++it;
        }
        return attrs;
    }

    /// GNU decode order: CLI =, archive g, file x, CLI :=
    std::map<std::string, std::string>
    merge_pax_attrs(const std::map<std::string, std::string>& file_pax) const {
        std::map<std::string, std::string> merged = cli_global_pax_;
        for (const auto& [k, v] : archive_global_pax_)
            merged[k] = v;
        for (const auto& [k, v] : file_pax)
            merged[k] = v;
        if (pax_rules_) {
            for (const auto& [k, v] : pax_rules_->file_overrides) {
                if (!pax_keyword_deleted(*pax_rules_, k))
                    merged[k] = v;
            }
        }
        // Final delete= filter (covers CLI globals too)
        if (pax_rules_) {
            for (auto it = merged.begin(); it != merged.end(); ) {
                if (pax_keyword_deleted(*pax_rules_, it->first))
                    it = merged.erase(it);
                else
                    ++it;
            }
        }
        return merged;
    }

    void apply_pax_attrs(Entry& e) {
        for (auto& [k, v] : e.pax_attrs) {
            try {
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
                    while (std::getline(ss, tok, ',')) {
                        std::int64_t off = std::stoll(tok);
                        if (!std::getline(ss, tok, ',')) break;
                        std::int64_t nb  = std::stoll(tok);
                        e.sparse_map.push_back({off, nb});
                    }
                    e.asize = 0;
                    for (auto& sm : e.sparse_map) e.asize += sm.numbytes;
                }
            } catch (const std::exception&) {
                // Malformed numeric / sparse PAX field — skip this key only.
                if (k == "GNU.sparse.map" || k == "GNU.sparse.realsize") {
                    e.sparse_map.clear();
                    e.is_sparse = false;
                }
                if (warn_cfg_)
                    mutar_warn(*warn_cfg_, "unknown-keyword",
                               std::format("ignoring malformed pax field '{}'", k));
                else
                    print(stderr, "mutar: warning: ignoring malformed pax field '{}'\n", k);
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
    bool           failed_ = false;
    bool           saw_any_block_ = false;
    const Config*  warn_cfg_ = nullptr;
    const PaxOptionRules* pax_rules_ = nullptr;
    std::map<std::string, std::string> cli_global_pax_;
    std::map<std::string, std::string> archive_global_pax_;
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

    /// Enable mid-file multi-volume split. @p max_blocks is tape capacity in
    /// 512-byte blocks; @p rotate(mid_member) switches volumes (caller opens
    /// the next stream and calls swap_stream). When mid_member is true the
    /// current volume has already been flushed without EOF zero blocks.
    void set_multivol(std::int64_t max_blocks,
                      std::function<bool(bool mid_member)> rotate) {
        max_blocks_ = max_blocks;
        rotate_     = std::move(rotate);
    }

    /// Flush pending record bytes without writing EOF zero blocks.
    void flush_volume() { buf_.flush(*stream_); }

    // Set optional owner/group remapping maps (pointers may be null).
    void set_owner_map(const std::map<std::string,std::string>* om,
                       const std::map<std::string,std::string>* gm) {
        owner_map_ = om; group_map_ = gm;
    }

    /// Enable sidecar index collection (pointer owned by caller).
    void set_index(ArchiveIndex* idx) { index_ = idx; }

    /// Write a global PAX 'g' header for keyword=value overrides (once per archive).
    void write_global_pax_header(const Config& cfg) {
        if (cfg.pax_option_rules.global_overrides.empty()) return;
        std::string pax_data;
        for (const auto& [k, v] : cfg.pax_option_rules.global_overrides) {
            if (!pax_keyword_deleted(cfg.pax_option_rules, k))
                pax_append(pax_data, k, v);
        }
        if (pax_data.empty()) return;

        ++global_header_count_;
        Entry meta;
        meta.typeflag = XGLTYPE;
        std::string tmpl = cfg.pax_option_rules.globexthdr_name;
        if (tmpl.empty()) tmpl = pax_default_globexthdr_template();
        meta.name = pax_format_name(tmpl, {}, global_header_count_);
        if (meta.name.size() > 99) meta.name.resize(99);
        meta.size  = static_cast<std::int64_t>(pax_data.size());
        meta.mtime = cfg.pax_option_rules.has_globexthdr_mtime
                         ? cfg.pax_option_rules.globexthdr_mtime
                         : static_cast<std::int64_t>(std::time(nullptr));
        meta.mode  = 0600;
        meta.fmt   = Format::PAX;

        Config tmp_cfg;
        tmp_cfg.fmt = Format::USTAR;
        Block hblk = encode_header(meta, tmp_cfg);
        buf_.write_block(*stream_, hblk);
        write_data_bytes(pax_data.data(), pax_data.size());
    }

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
            mutar_warn(cfg, "failed-read",
                       std::format("{}: {}", fspath, std::strerror(errno)));
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
        apply_owner_group_cli(e, cfg);
        if (cfg.numeric_owner) { e.uname.clear(); e.gname.clear(); }
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
        // --mode: override permissions (octal or GNU symbolic: u+x,go-w,a=r,…)
        if (!cfg.mode_str.empty()) {
            unsigned new_mode = e.mode;
            if (apply_mode_changes(new_mode, cfg.mode_str, st.st_mode))
                e.mode = new_mode;
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

    // Write PAX extended header (honours full --pax-option rules).
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
        pax_append_file_overrides(pax_data, cfg);

        if (pax_data.empty()) return;

        Entry meta;
        meta.typeflag = XHDTYPE;
        meta.name     = pax_exthdr_member_name(e, cfg);
        meta.size     = static_cast<std::int64_t>(pax_data.size());
        meta.mtime    = cfg.pax_option_rules.has_exthdr_mtime
                            ? cfg.pax_option_rules.exthdr_mtime
                            : e.mtime;
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

    /// Write a GNU dumpdir directory member (typeflag 'D') with body.
    /// Body is NUL-separated "Cname" records (C in {Y,N,D,...}) plus a final NUL.
    bool write_dumpdir(Entry& e, std::string_view body, const Config& cfg) {
        if (!e.name.empty() && e.name.back() != '/') e.name += '/';
        e.typeflag = GNUTYPE_DUMPDIR;
        e.size     = static_cast<std::int64_t>(body.size());
        e.asize    = e.size;
        record_index_entry(e);
        maybe_write_extensions(e, cfg);
        Block hblk = encode_header(e, cfg);
        buf_.write_block(*stream_, hblk);
        if (!body.empty())
            write_data_bytes(body.data(), body.size());
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
        bool need_file_ov = !cfg.pax_option_rules.file_overrides.empty();
        bool need_pax = long_name || long_link || e.size > 077777777777LL
                        || e.mtime_nsec != 0 || need_schily || need_file_ov;

        // SCHILY xattr/ACL records require a PAX 'x' header even under GNU format.
        // keyword:=value file overrides also force a per-file 'x' header.
        if (fmt_ == Format::PAX || fmt_ == Format::USTAR || need_schily || need_file_ov) {
            if (need_pax)
                write_pax_header(e, long_name, long_link,
                                 e.size > 077777777777LL, e.mtime_nsec != 0, cfg);
        } else {
            // GNU LongName/LongLink
            if (long_name) write_long_ext(GNUTYPE_LONGNAME, e.name);
            if (long_link) write_long_ext(GNUTYPE_LONGLINK, e.linkname);
        }
    }

    /// Open file for archiving; with --atime-preserve=system try O_NOATIME.
    static int open_for_dump(const std::string& fspath, const Config& cfg) {
        int flags = O_RDONLY;
#ifdef O_NOATIME
        if (cfg.atime_preserve && cfg.atime_preserve_method == "system")
            flags |= O_NOATIME;
#endif
        int fd = ::open(fspath.c_str(), flags);
#ifdef O_NOATIME
        // O_NOATIME requires ownership; fall back if EPERM
        if (fd < 0 && (flags & O_NOATIME) && errno == EPERM)
            fd = ::open(fspath.c_str(), O_RDONLY);
#endif
        return fd;
    }

    static void restore_atime_if_needed(const std::string& fspath,
                                        const Config& cfg,
                                        const struct stat& st) {
        // METHOD=replace (default): restore atime after read.
        // METHOD=system: rely on O_NOATIME; no restore.
        if (!cfg.atime_preserve)
            return;
        if (cfg.atime_preserve_method == "system")
            return;
        struct timespec ts[2];
        ts[0] = {.tv_sec = st.st_atim.tv_sec, .tv_nsec = st.st_atim.tv_nsec};
        ts[1] = {.tv_sec = st.st_mtim.tv_sec, .tv_nsec = st.st_mtim.tv_nsec};
        ::utimensat(AT_FDCWD, fspath.c_str(), ts, AT_SYMLINK_NOFOLLOW);
    }

    bool write_regular(Entry& e, const std::string& fspath,
                       const Config& cfg, const struct stat& st) {
        record_index_entry(e);
        maybe_write_extensions(e, cfg);

        // Room for the member header (and later data) on this volume.
        if (max_blocks_ > 0 && buf_.block_no() >= max_blocks_) {
            if (!rotate_ || !rotate_(false))
                return false;
        }

        // First fragment: typeflag '0', size = full logical file size (GNU).
        Block hblk = encode_header(e, cfg);
        buf_.write_block(*stream_, hblk);

        int fd = open_for_dump(fspath, cfg);
        if (fd < 0) {
            mutar_warn(cfg, "failed-read",
                       std::format("{}: {}", fspath, std::strerror(errno)));
            write_data_zeros(e.size);
            return false;
        }

        std::int64_t remaining   = e.size;
        std::int64_t file_offset = 0; // bytes successfully archived so far
        char blkbuf[BLOCKSIZE];
        bool io_error = false;
        while (remaining > 0 && !io_error) {
            // Mid-file multi-volume: volume full → flush, rotate, write 'M' header.
            if (max_blocks_ > 0 && buf_.block_no() >= max_blocks_) {
                buf_.flush(*stream_);
                if (!rotate_ || !rotate_(true)) {
                    ::close(fd);
                    return false;
                }
                Entry cont     = e;
                cont.typeflag  = GNUTYPE_MULTIVOL;
                cont.multivol_offset = file_offset;
                cont.size      = remaining; // remaining bytes from this offset
                cont.asize     = remaining;
                Block cblk     = encode_header(cont, cfg);
                buf_.write_block(*stream_, cblk);
            }

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
                remaining   -= got;
                file_offset += got;
            }
        }
        ::close(fd);
        restore_atime_if_needed(fspath, cfg, st);
        // Pad any remaining blocks (file shorter than stated size)
        write_data_zeros(remaining);
        return !io_error;
    }

    /// Emit one data block with mid-file multi-volume rotation if needed.
    /// @p arch_offset is bytes of sparse payload already committed.
    bool emit_sparse_data_block(Entry& e, const Config& cfg,
                                std::int64_t& arch_offset,
                                std::int64_t arch_size,
                                const Block& b) {
        if (max_blocks_ > 0 && buf_.block_no() >= max_blocks_) {
            buf_.flush(*stream_);
            if (!rotate_ || !rotate_(true))
                return false;
            Entry cont = e;
            cont.typeflag = GNUTYPE_MULTIVOL;
            cont.multivol_offset = arch_offset;
            cont.size  = arch_size - arch_offset;
            cont.asize = cont.size;
            cont.is_sparse = false;
            cont.sparse_map.clear();
            Block cblk = encode_header(cont, cfg);
            buf_.write_block(*stream_, cblk);
        }
        buf_.write_block(*stream_, b);
        // Each archive data block advances by up to BLOCKSIZE of payload;
        // caller tracks exact byte count for the final partial block.
        return true;
    }

    /// Write concatenated sparse data segments as archive payload blocks.
    /// Supports mid-file multi-volume (GNUTYPE_MULTIVOL) like write_regular.
    bool write_sparse_payload(int fd, const std::vector<SparseSegment>& segs,
                              Entry& e, const Config& cfg,
                              std::int64_t arch_size) {
        std::int64_t arch_offset = 0;
        Block b{};
        std::memset(b.buffer, 0, BLOCKSIZE);
        std::size_t buf_filled = 0;
        bool ok = true;

        for (auto& seg : segs) {
            if (::lseek(fd, static_cast<off_t>(seg.offset), SEEK_SET) < 0) {
                ok = false;
                break;
            }
            std::int64_t seg_rem = seg.length;
            while (seg_rem > 0) {
                std::size_t space = BLOCKSIZE - buf_filled;
                if (space == 0) {
                    if (!emit_sparse_data_block(e, cfg, arch_offset, arch_size, b)) {
                        ok = false;
                        break;
                    }
                    arch_offset += BLOCKSIZE;
                    std::memset(b.buffer, 0, BLOCKSIZE);
                    buf_filled = 0;
                    space = BLOCKSIZE;
                }
                std::int64_t to_read = std::min<std::int64_t>(
                    seg_rem, static_cast<std::int64_t>(space));
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
                seg_rem    -= got;
            }
            if (!ok) break;
        }

        if (ok && buf_filled > 0) {
            if (!emit_sparse_data_block(e, cfg, arch_offset, arch_size, b))
                return false;
            arch_offset += static_cast<std::int64_t>(buf_filled);
        }
        return ok;
    }

    // Write a GNU sparse ('S' type) entry using SEEK_DATA/SEEK_HOLE hole detection.
    bool write_sparse(Entry& e, const std::string& fspath,
                      const Config& cfg, const struct stat& st) {
        int fd = open_for_dump(fspath, cfg);
        if (fd < 0) {
            mutar_warn(cfg, "failed-read",
                       std::format("{}: {}", fspath, std::strerror(errno)));
            return false;
        }

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

        // Room for headers on this volume (between-member rotate if full)
        if (max_blocks_ > 0 && buf_.block_no() >= max_blocks_) {
            if (!rotate_ || !rotate_(false)) {
                ::close(fd);
                return false;
            }
        }

        // Sparse 1.x: PAX extended header (GNU.sparse.*). Sparse 0.x: old GNU 'S' type.
        if (cfg.sparse_major >= 1) {
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
            // Sparse version from --sparse-version (default 1.0)
            std::string pax_data;
            pax_append_if(pax_data, cfg, "GNU.sparse.major",
                          std::to_string(cfg.sparse_major));
            pax_append_if(pax_data, cfg, "GNU.sparse.minor",
                          std::to_string(cfg.sparse_minor));
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
            pax_append_file_overrides(pax_data, cfg);

            // Emit PAX 'x' extended header block
            Entry pax_meta;
            pax_meta.typeflag = XHDTYPE;
            pax_meta.name     = pax_exthdr_member_name(e, cfg);
            pax_meta.size     = static_cast<std::int64_t>(pax_data.size());
            pax_meta.mtime    = cfg.pax_option_rules.has_exthdr_mtime
                                    ? cfg.pax_option_rules.exthdr_mtime
                                    : e.mtime;
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

            bool ok = write_sparse_payload(fd, segs, e, cfg, arch_size);
            ::close(fd);
            restore_atime_if_needed(fspath, cfg, st);
            return ok;
        }

        // GNU sparse ('S' type) format for sparse-version 0.x
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

        bool ok = write_sparse_payload(fd, segs, e, cfg, arch_size);
        ::close(fd);
        restore_atime_if_needed(fspath, cfg, st);
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
    std::size_t    global_header_count_ = 0;  // for globexthdr.name %n
    std::int64_t   max_blocks_ = 0;           // multi-volume tape capacity (0 = off)
    std::function<bool(bool mid_member)> rotate_;
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

#ifdef O_NOFOLLOW
static constexpr int k_extract_nofollow = O_NOFOLLOW;
#else
static constexpr int k_extract_nofollow = 0;
#endif

/// Split outpath into components. Sets *is_absolute if path begins with '/'.
/// Empty, ".", and trailing slashes are dropped; ".." is kept as a component
/// (caller should only see ".." when --absolute-names left it in place).
static std::vector<std::string> split_extract_components(const std::string& path,
                                                         bool* is_absolute) {
    *is_absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    std::size_t start = *is_absolute ? 1 : 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (end > start) {
            std::string part = path.substr(start, end - start);
            if (part != ".")
                parts.push_back(std::move(part));
        }
        if (end == path.size()) break;
        start = end + 1;
    }
    return parts;
}

/// Open or create one directory component under dirfd without following a
/// symlink at that component (unless keep_dir_symlink).
/// @return directory fd on success (>=0), -1 on error (errno set).
static int openat_dir_component(int dirfd, const std::string& name, const Config& cfg) {
    int fd = ::openat(dirfd, name.c_str(),
                      O_RDONLY | O_DIRECTORY | k_extract_nofollow);
    if (fd >= 0) return fd;

    if (errno == ENOENT) {
        if (::mkdirat(dirfd, name.c_str(), 0777) < 0 && errno != EEXIST)
            return -1;
        return ::openat(dirfd, name.c_str(),
                        O_RDONLY | O_DIRECTORY | k_extract_nofollow);
    }

    // Exists but not a real directory we can open with O_NOFOLLOW (symlink,
    // file, or race). Inspect with AT_SYMLINK_NOFOLLOW.
    struct stat st{};
    if (::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0)
        return -1;

    if (S_ISLNK(st.st_mode)) {
        if (cfg.keep_dir_symlink) {
            // GNU-like: extract through an existing directory symlink.
            return ::openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY);
        }
        if (cfg.keep_old_files) {
            errno = EEXIST;
            return -1;
        }
        // Replace intermediate symlink so nested members cannot escape the tree.
        if (::unlinkat(dirfd, name.c_str(), 0) < 0 && errno != ENOENT)
            return -1;
        if (::mkdirat(dirfd, name.c_str(), 0777) < 0 && errno != EEXIST)
            return -1;
        return ::openat(dirfd, name.c_str(),
                        O_RDONLY | O_DIRECTORY | k_extract_nofollow);
    }

    if (S_ISDIR(st.st_mode)) {
        // Directory without O_NOFOLLOW success is unexpected; try plain open.
        return ::openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY);
    }

    errno = ENOTDIR;
    return -1;
}

/// Walk parent components of outpath with openat(O_NOFOLLOW), creating real
/// directories and replacing intermediate symlinks (unless keep_dir_symlink).
/// On success, *out_dirfd is the directory fd for the final component's parent
/// (AT_FDCWD if no parent). Caller must close *out_dirfd when != AT_FDCWD.
/// *out_base is the final path component.
/// @return 0 on success, -1 on error.
static int walk_extract_parent(const std::string& outpath, const Config& cfg,
                               int* out_dirfd, std::string* out_base) {
    bool abs = false;
    auto parts = split_extract_components(outpath, &abs);
    // sanitize_path returns "." for empty / "." / "./" members; split drops
    // those components so parts is empty. No parent walk is needed (cwd or /).
    if (parts.empty()) {
        *out_base = ".";
        if (abs) {
            int rootfd = ::open("/", O_RDONLY | O_DIRECTORY);
            if (rootfd < 0) return -1;
            *out_dirfd = rootfd;
            return 0;
        }
        *out_dirfd = AT_FDCWD;
        return 0;
    }
    *out_base = parts.back();

    int dirfd = AT_FDCWD;
    std::vector<int> owned; // fds we must close on failure or when superseded
    auto fail = [&]() {
        for (int fd : owned) ::close(fd);
        return -1;
    };
    auto adopt = [&](int fd) {
        if (fd != AT_FDCWD) owned.push_back(fd);
        dirfd = fd;
    };

    if (abs) {
        int rootfd = ::open("/", O_RDONLY | O_DIRECTORY);
        if (rootfd < 0) return -1;
        adopt(rootfd);
    }

    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        int next = openat_dir_component(dirfd, parts[i], cfg);
        if (next < 0) return fail();
        // Close previous owned dirfds; keep only `next`.
        for (int fd : owned) ::close(fd);
        owned.clear();
        adopt(next);
    }

    *out_dirfd = dirfd;
    // Transfer ownership of dirfd to caller (remove from owned without close).
    owned.clear();
    return 0;
}

/// Ensure parent directories of outpath exist as real directories.
/// Intermediate symlinks are replaced (unless --keep-directory-symlink).
/// @return 0 on success, -1 on error.
static int ensure_parent_dirs_nofollow(const std::string& outpath, const Config& cfg) {
    int dirfd = AT_FDCWD;
    std::string base;
    if (walk_extract_parent(outpath, cfg, &dirfd, &base) < 0)
        return -1;
    if (dirfd != AT_FDCWD) ::close(dirfd);
    return 0;
}

/// Open a regular file for extract write without following any symlink in the
/// path. Covers:
///  - same-name: symlink member then regular member (final O_NOFOLLOW + unlink)
///  - intermediate: dir symlink then nested regular (openat walk, replace link)
///
/// Policy: intermediate symlinks are replaced with real directories unless
/// --keep-directory-symlink. Final symlink is unlinked unless --keep-old-files.
/// @param no_trunc  When true (multi-volume continuation), open without O_TRUNC.
static int open_extract_regular(const std::string& outpath, const Config& cfg,
                                bool no_trunc = false) {
    int dirfd = AT_FDCWD;
    std::string base;
    if (walk_extract_parent(outpath, cfg, &dirfd, &base) < 0)
        return -1;

    auto close_dir = [&]() {
        if (dirfd != AT_FDCWD) ::close(dirfd);
    };

    struct stat st{};
    if (::fstatat(dirfd, base.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISDIR(st.st_mode)) {
            errno = EISDIR;
            close_dir();
            return -1;
        }
        if (cfg.keep_old_files) {
            errno = EEXIST;
            close_dir();
            return -1;
        }
        // Never write through a symlink. Never O_TRUNC a hardlinked regular
        // file (nlink>1 can alias a path outside the extract tree). Other
        // non-dirs (fifo/sock/dev) are replaced. Multi-volume continuation
        // (no_trunc) must keep the same inode.
        const bool unlink_first =
            S_ISLNK(st.st_mode) ||
            (!no_trunc && (!S_ISREG(st.st_mode) || st.st_nlink > 1));
        if (unlink_first) {
            if (::unlinkat(dirfd, base.c_str(), 0) < 0 && errno != ENOENT) {
                close_dir();
                return -1;
            }
        }
    }

    int flags = O_WRONLY | O_CREAT | k_extract_nofollow;
    if (!no_trunc) flags |= O_TRUNC;
    int fd = ::openat(dirfd, base.c_str(), flags, 0666);
    if (fd < 0 && (errno == ELOOP || errno == EEXIST)) {
        if (cfg.keep_old_files) {
            close_dir();
            return -1;
        }
        if (::fstatat(dirfd, base.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
            if (::unlinkat(dirfd, base.c_str(), 0) < 0 && errno != ENOENT) {
                close_dir();
                return -1;
            }
            flags = O_WRONLY | O_CREAT | O_EXCL | k_extract_nofollow;
            fd = ::openat(dirfd, base.c_str(), flags, 0666);
            if (fd < 0) {
                flags = O_WRONLY | O_CREAT | k_extract_nofollow;
                if (!no_trunc) flags |= O_TRUNC;
                fd = ::openat(dirfd, base.c_str(), flags, 0666);
            }
        }
    }

    close_dir();
    return fd;
}

/// Open an existing directory component with O_NOFOLLOW (no mkdir, no replace).
/// With --keep-directory-symlink, a symlink-to-dir may be followed.
static int openat_existing_dir_nofollow(int dirfd, const std::string& name,
                                        const Config& cfg) {
    int fd = ::openat(dirfd, name.c_str(),
                      O_RDONLY | O_DIRECTORY | k_extract_nofollow);
    if (fd >= 0) return fd;
    if (cfg.keep_dir_symlink && (errno == ELOOP || errno == ENOTDIR))
        return ::openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY);
    return -1;
}

/// Walk parent components of an existing path without following dir symlinks
/// and without creating missing directories. Used for hardlink targets.
static int walk_existing_parent_nofollow(const std::string& path, const Config& cfg,
                                         int* out_dirfd, std::string* out_base) {
    bool abs = false;
    auto parts = split_extract_components(path, &abs);
    if (parts.empty()) {
        *out_base = ".";
        if (abs) {
            int rootfd = ::open("/", O_RDONLY | O_DIRECTORY);
            if (rootfd < 0) return -1;
            *out_dirfd = rootfd;
            return 0;
        }
        *out_dirfd = AT_FDCWD;
        return 0;
    }
    *out_base = parts.back();

    int dirfd = AT_FDCWD;
    std::vector<int> owned;
    auto fail = [&]() {
        for (int fd : owned) ::close(fd);
        return -1;
    };
    auto adopt = [&](int fd) {
        if (fd != AT_FDCWD) owned.push_back(fd);
        dirfd = fd;
    };

    if (abs) {
        int rootfd = ::open("/", O_RDONLY | O_DIRECTORY);
        if (rootfd < 0) return -1;
        adopt(rootfd);
    }

    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        int next = openat_existing_dir_nofollow(dirfd, parts[i], cfg);
        if (next < 0) return fail();
        for (int fd : owned) ::close(fd);
        owned.clear();
        adopt(next);
    }

    *out_dirfd = dirfd;
    owned.clear();
    return 0;
}

/// Create a hard link using openat/O_NOFOLLOW walks for both names so a
/// directory symlink in the extract tree cannot redirect the link outside.
static int extract_hardlink(const std::string& outpath, const std::string& target,
                            const Config& cfg) {
    int tdir = AT_FDCWD;
    std::string tbase;
    if (walk_existing_parent_nofollow(target, cfg, &tdir, &tbase) < 0)
        return -1;

    int ddir = AT_FDCWD;
    std::string dbase;
    if (walk_extract_parent(outpath, cfg, &ddir, &dbase) < 0) {
        if (tdir != AT_FDCWD) ::close(tdir);
        return -1;
    }

    auto cleanup = [&]() {
        if (tdir != AT_FDCWD) ::close(tdir);
        if (ddir != AT_FDCWD) ::close(ddir);
    };

    if (cfg.keep_old_files) {
        struct stat st{};
        if (::fstatat(ddir, dbase.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            errno = EEXIST;
            cleanup();
            return -1;
        }
    } else if (::unlinkat(ddir, dbase.c_str(), 0) < 0 && errno != ENOENT) {
        cleanup();
        return -1;
    }

    int rc = ::linkat(tdir, tbase.c_str(), ddir, dbase.c_str(), 0);
    int saved = errno;
    cleanup();
    if (rc < 0) {
        errno = saved;
        return -1;
    }
    return 0;
}

/// RAII unlink of a temp path (materialized seek view).
struct TempPathUnlink {
    std::string path;
    explicit TempPathUnlink(std::string p = {}) : path(std::move(p)) {}
    TempPathUnlink(const TempPathUnlink&) = delete;
    TempPathUnlink& operator=(const TempPathUnlink&) = delete;
    TempPathUnlink(TempPathUnlink&& o) noexcept : path(std::move(o.path)) { o.path.clear(); }
    TempPathUnlink& operator=(TempPathUnlink&& o) noexcept {
        if (this != &o) {
            reset();
            path = std::move(o.path);
            o.path.clear();
        }
        return *this;
    }
    void reset() {
        if (!path.empty()) {
            ::unlink(path.c_str());
            path.clear();
        }
    }
    ~TempPathUnlink() { reset(); }
};

// Normalize an archive member name: strip leading ./ (mirrors walk_dir normalization).
// Used when building the 'want' set from user-supplied names so that
// "./file1.txt" and "file1.txt" both match the stored name "file1.txt".
static std::string normalize_member(std::string_view name) {
    while (name.size() > 1 && name.starts_with("./"))
        name.remove_prefix(2);
    return std::string(name);
}

// Prepend --one-top-level directory unless the member is already under it.
static std::string apply_one_top_level(std::string path, const std::string& one_top) {
    if (one_top.empty() || path == "." || path == "./")
        return path;
    std::string top = one_top;
    while (top.size() > 1 && top.back() == '/')
        top.pop_back();
    if (path == top)
        return path;
    if (path.size() > top.size() && path.starts_with(top) && path[top.size()] == '/')
        return path;
    return top + "/" + path;
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

// Match a single exclude pattern against name (full path and/or components).
static bool pattern_matches_name(const Config& cfg, std::string_view name,
                                 const std::string& pat) {
    if (!cfg.wildcards) {
        // Literal string matching
        std::string sname(name);
        if (cfg.anchored) {
            if (cfg.ignore_case ? ::strcasecmp(sname.c_str(), pat.c_str()) == 0
                                : sname == pat)
                return true;
        } else {
            if (cfg.ignore_case ? ::strcasecmp(sname.c_str(), pat.c_str()) == 0
                                : sname == pat)
                return true;
            std::string_view rem = name;
            while (!rem.empty()) {
                auto slash = rem.find('/');
                if (slash == std::string_view::npos)
                    break;
                rem = rem.substr(slash + 1);
                if (rem.empty())
                    break;
                std::string ssuf(rem);
                if (cfg.ignore_case ? ::strcasecmp(ssuf.c_str(), pat.c_str()) == 0
                                    : ssuf == pat)
                    return true;
            }
            rem = name;
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
        int flags = FNM_LEADING_DIR;
        if (!cfg.wildcards_match_slash) flags |= FNM_PATHNAME;
        if (cfg.ignore_case)            flags |= FNM_CASEFOLD;

        if (::fnmatch(pat.c_str(), std::string(name).c_str(), flags) == 0)
            return true;
        if (!cfg.anchored) {
            // GNU --no-anchored: match against each path suffix so
            // --exclude=sub/*.o matches dir/sub/bar.o.
            std::string_view rem = name;
            while (!rem.empty()) {
                auto slash = rem.find('/');
                if (slash == std::string_view::npos)
                    break;
                rem = rem.substr(slash + 1);
                if (rem.empty())
                    break;
                if (::fnmatch(pat.c_str(), std::string(rem).c_str(), flags) == 0)
                    return true;
            }
        }
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
    return false;
}

// Check if 'name' matches any exclude pattern, respecting cfg matching flags.
static bool is_excluded(const Config& cfg, std::string_view name) {
    for (const auto& pat : cfg.exclude_patterns) {
        if (pattern_matches_name(cfg, name, pat))
            return true;
    }
    return false;
}

// Read exclude patterns from a per-directory ignore file (one pattern per line).
// Skips empty lines and '#' comments. Returns patterns in file order.
static std::vector<std::string> read_ignore_file_patterns(const std::string& path) {
    std::vector<std::string> pats;
    std::ifstream ifs(path);
    if (!ifs) return pats;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                 line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            pats.push_back(std::move(line));
    }
    return pats;
}

// True if basename or archname matches any pattern in the list (fnmatch).
static bool matches_ignore_patterns(const Config& cfg,
                                    const std::vector<std::string>& pats,
                                    std::string_view basename,
                                    std::string_view archname) {
    for (const auto& pat : pats) {
        int flags = 0;
        if (cfg.ignore_case) flags |= FNM_CASEFOLD;
        if (!cfg.wildcards_match_slash) flags |= FNM_PATHNAME;
        if (cfg.wildcards) {
            if (::fnmatch(pat.c_str(), std::string(basename).c_str(), flags) == 0)
                return true;
            if (::fnmatch(pat.c_str(), std::string(archname).c_str(), flags) == 0)
                return true;
        } else {
            if (pattern_matches_name(cfg, basename, pat) ||
                pattern_matches_name(cfg, archname, pat))
                return true;
        }
    }
    return false;
}

// ── Date parsing helper (--newer / --mtime) ──────────────────────────────────
// Accepts: @SECONDS (Unix epoch), file path (uses its mtime), ISO date, or
// seconds-since-epoch. Calendar dates are UTC (timegm): local mktime of
// 1970-01-01 fails in TZ east of UTC and write_octal((uint64_t)-1) becomes
// year 2242 (11 octal digits of all 7s).
static std::time_t parse_date_string(const std::string& s) {
    if (s.empty()) return (std::time_t)-1;
    // GNU: --mtime=@SECONDS is seconds since the Unix epoch
    if (s[0] == '@') {
        char* endp = nullptr; errno = 0;
        long long v = std::strtoll(s.c_str() + 1, &endp, 10);
        if (!errno && endp && *endp == '\0')
            return static_cast<std::time_t>(v);
        return (std::time_t)-1;
    }
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
            tm.tm_isdst = 0;
            return ::timegm(&tm);
        }
    }
    // Seconds since epoch
    char* endp = nullptr; errno = 0;
    long long v = std::strtoll(s.c_str(), &endp, 10);
    if (!errno && endp && *endp == '\0') return static_cast<std::time_t>(v);
    return (std::time_t)-1;
}

// GNU --owner=NAME[:UID] / --owner=+UID  (and the same for --group).
struct IdSpec {
    std::string name;
    unsigned    id = 0;
    bool        have_id = false;
    bool        numeric_only = false;
    bool        ok = false;
};

static IdSpec parse_id_spec(const std::string& spec, bool is_group) {
    IdSpec r;
    if (spec.empty())
        return r;
    if (spec[0] == '+') {
        char* end = nullptr; errno = 0;
        unsigned long v = std::strtoul(spec.c_str() + 1, &end, 10);
        if (errno || end == spec.c_str() + 1 || !end || *end != '\0' || v > UINT_MAX)
            return r;
        r.id = static_cast<unsigned>(v);
        r.have_id = true;
        r.numeric_only = true;
        r.ok = true;
        return r;
    }
    auto colon = spec.find(':');
    if (colon != std::string::npos) {
        r.name = spec.substr(0, colon);
        const char* num = spec.c_str() + colon + 1;
        char* end = nullptr; errno = 0;
        unsigned long v = std::strtoul(num, &end, 10);
        if (errno || end == num || !end || *end != '\0' || v > UINT_MAX)
            return r;
        r.id = static_cast<unsigned>(v);
        r.have_id = true;
        r.ok = true;
        return r;
    }
    char* end = nullptr; errno = 0;
    unsigned long v = std::strtoul(spec.c_str(), &end, 10);
    if (end && *end == '\0' && errno == 0 && v <= UINT_MAX) {
        r.id = static_cast<unsigned>(v);
        r.have_id = true;
        if (is_group) {
            if (struct group* gr = ::getgrgid(static_cast<gid_t>(r.id)))
                r.name = gr->gr_name;
        } else if (struct passwd* pw = ::getpwuid(static_cast<uid_t>(r.id))) {
            r.name = pw->pw_name;
        }
        r.ok = true;
        return r;
    }
    r.name = spec;
    if (is_group) {
        if (struct group* gr = ::getgrnam(spec.c_str())) {
            r.id = static_cast<unsigned>(gr->gr_gid);
            r.have_id = true;
        }
    } else if (struct passwd* pw = ::getpwnam(spec.c_str())) {
        r.id = static_cast<unsigned>(pw->pw_uid);
        r.have_id = true;
    }
    r.ok = true;
    return r;
}

static void apply_owner_group_cli(Entry& e, const Config& cfg) {
    if (!cfg.owner.empty()) {
        IdSpec o = parse_id_spec(cfg.owner, false);
        if (o.ok) {
            if (o.numeric_only)
                e.uname.clear();
            else if (!o.name.empty())
                e.uname = o.name;
            if (o.have_id)
                e.uid = o.id;
        }
    }
    if (!cfg.group.empty()) {
        IdSpec g = parse_id_spec(cfg.group, true);
        if (g.ok) {
            if (g.numeric_only)
                e.gname.clear();
            else if (!g.name.empty())
                e.gname = g.name;
            if (g.have_id)
                e.gid = g.id;
        }
    }
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

// Recursive directory walker.
// @param inherited_ignore_pats  Patterns from ancestor --exclude-ignore-recursive
//        files; applied to this directory's children and passed further down.
// @return false if a path could not be read (lstat/opendir); true on success.
//         Exclusion skips still return true. Callers honour --ignore-failed-read.
static bool walk_dir(const std::string& base_dir, const std::string& relpath,
                     const Config& cfg,
                     std::function<void(const std::string& archname, const std::string& fspath)> cb,
                     dev_t same_dev = static_cast<dev_t>(-1),
                     const std::vector<std::string>& inherited_ignore_pats = {}) {
    std::string full;
    if (relpath.empty())
        full = base_dir.empty() ? "." : base_dir;
    else if (relpath[0] == '/' || base_dir.empty())
        full = relpath;
    else
        full = base_dir + "/" + relpath;

    struct stat st{};
    if (::lstat(full.c_str(), &st) < 0) {
        mutar_warn(cfg, "failed-read",
                   std::format("{}: {}", full, std::strerror(errno)));
        return false;
    }

    std::string archname = relpath;
    if (!cfg.directory.empty() && archname.starts_with(cfg.directory))
        archname = archname.substr(cfg.directory.size());
    if (archname.starts_with("/") && !cfg.absolute_names)
        archname = archname.substr(1);
    // Normalize: strip leading ./ (e.g. "./dir1/f" → "dir1/f")
    while (archname.size() > 1 && archname.starts_with("./"))
        archname = archname.substr(2);

    if (archname.empty()) archname = ".";

    if (is_excluded(cfg, archname)) {
        if (cfg.show_omitted_dirs && S_ISDIR(st.st_mode))
            print(stderr, "mutar: {}/\n", archname);
        return true;
    }

    cb(archname, full);

    if (S_ISDIR(st.st_mode) && !cfg.no_recursion) {
        // Establish root device on first descent
        dev_t this_dev = (same_dev == static_cast<dev_t>(-1)) ? st.st_dev : same_dev;
        // Walk contents
        DIR* d = ::opendir(full.c_str());
        if (!d) {
            mutar_warn(cfg, "failed-read",
                       std::format("opendir {}: {}", full, std::strerror(errno)));
            return false;
        }
        std::vector<std::string> entries;
        while (struct dirent* de = ::readdir(d)) {
            std::string_view dname(de->d_name);
            if (dname == "." || dname == "..") continue;
            entries.emplace_back(dname);
        }
        ::closedir(d);
        if (cfg.sort_order == "name") {
            std::ranges::sort(entries);
        } else if (cfg.sort_order == "inode") {
            // GNU --sort=inode: primary key st_ino, secondary name
            std::ranges::sort(entries, [&](const std::string& a, const std::string& b) {
                struct stat sa{}, sb{};
                const std::string fa = full + "/" + a;
                const std::string fb = full + "/" + b;
                const ino_t ia = (::lstat(fa.c_str(), &sa) == 0) ? sa.st_ino : 0;
                const ino_t ib = (::lstat(fb.c_str(), &sb) == 0) ? sb.st_ino : 0;
                if (ia != ib) return ia < ib;
                return a < b;
            });
        }

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
            return true; // skip all contents entirely

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
            return true; // skip all contents (--exclude-tag-all / --exclude-tag-under)

        // --exclude-vcs-ignores: read .gitignore/.hgignore/.cvsignore/.bzrignore patterns
        std::vector<std::string> vcs_patterns;
        if (cfg.exclude_vcs_ignores) {
            for (const char* ignore_file : {".gitignore", ".hgignore", ".cvsignore", ".bzrignore"}) {
                auto more = read_ignore_file_patterns(full + "/" + ignore_file);
                vcs_patterns.insert(vcs_patterns.end(), more.begin(), more.end());
            }
        }

        // --exclude-ignore / --exclude-ignore-recursive: per-directory ignore files
        // Local (non-recursive) patterns apply only to this directory's children.
        // Recursive patterns from this dir are merged into the inherited set for
        // children (and also apply here).
        std::vector<std::string> local_ignore_pats;
        std::vector<std::string> child_ignore_pats = inherited_ignore_pats;
        for (const auto& ign : cfg.exclude_ignore) {
            auto more = read_ignore_file_patterns(full + "/" + ign);
            local_ignore_pats.insert(local_ignore_pats.end(), more.begin(), more.end());
        }
        for (const auto& ign : cfg.exclude_ignore_recursive) {
            auto more = read_ignore_file_patterns(full + "/" + ign);
            child_ignore_pats.insert(child_ignore_pats.end(), more.begin(), more.end());
        }

        bool walk_ok = true;
        for (const auto& ent : entries) {
            std::string child_arch = (archname == "." || archname.empty())
                ? ent : (archname + "/" + ent);

            // --exclude-vcs-ignores: check vcs ignore patterns for this entry
            if (!vcs_patterns.empty() &&
                matches_ignore_patterns(cfg, vcs_patterns, ent, child_arch))
                continue;

            // --exclude-ignore: patterns from FILE in this directory only
            if (!local_ignore_pats.empty() &&
                matches_ignore_patterns(cfg, local_ignore_pats, ent, child_arch))
                continue;
            // --exclude-ignore-recursive: this dir + ancestors
            if (!child_ignore_pats.empty() &&
                matches_ignore_patterns(cfg, child_ignore_pats, ent, child_arch))
                continue;

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
            std::string child_rel;
            if (relpath.empty() || relpath == ".")
                child_rel = ent;
            else
                child_rel = relpath + "/" + ent;
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
            if (!walk_dir(base_dir, child_rel, cfg, cb, this_dev, child_ignore_pats))
                walk_ok = false;
        }
        return walk_ok;
    }
    return true;
}

// ── Name quoting (--quoting-style / --quote-chars / --no-quote-chars) ─────────
// Styles: literal, escape, c, c-maybe, shell, shell-always,
//         shell-escape, shell-escape-always, locale, clocale.
// Unknown styles fall back to literal. Default (empty) keeps historical raw output.

static bool char_in_set(unsigned char c, const std::string& set) {
    return set.find(static_cast<char>(c)) != std::string::npos;
}

static bool name_needs_shell_quote(const std::string& name,
                                   const std::string& extra_quote = {},
                                   const std::string& never_quote = {}) {
    if (name.empty())
        return true;
    for (unsigned char c : name) {
        if (char_in_set(c, never_quote))
            continue;
        if (char_in_set(c, extra_quote))
            return true;
        if (std::isalnum(c) || c == '.' || c == '/' || c == '_' || c == '-' ||
            c == '+' || c == ',' || c == ':' || c == '@' || c == '%')
            continue;
        return true;
    }
    return false;
}

static bool name_needs_c_quote(const std::string& name,
                               const std::string& extra_quote = {},
                               const std::string& never_quote = {}) {
    for (unsigned char c : name) {
        if (char_in_set(c, never_quote))
            continue;
        if (char_in_set(c, extra_quote))
            return true;
        if (c <= 0x1f || c == '"' || c == '\\' || c == ' ' || c == '\'' || c >= 0x7f)
            return true;
    }
    return false;
}

static std::string quote_name(const std::string& name, const std::string& style,
                              const std::string& extra_quote = {},
                              const std::string& never_quote = {}) {
    std::string effective = style.empty() ? "literal" : style;
    // locale / clocale: approximate with shell-escape (C-locale octal escapes)
    if (effective == "locale" || effective == "clocale")
        effective = "shell-escape";

    auto force_quote = [&](unsigned char c) {
        return char_in_set(c, extra_quote) && !char_in_set(c, never_quote);
    };

    if (effective == "literal") {
        if (extra_quote.empty())
            return name;
        // Honour --quote-chars even with literal style (escape those chars)
        std::string out;
        out.reserve(name.size() + 4);
        for (unsigned char c : name) {
            if (force_quote(c) || (c == '\\' && !char_in_set(c, never_quote)))
                out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    if (effective == "escape") {
        std::string out;
        out.reserve(name.size() + 8);
        for (unsigned char c : name) {
            bool esc = force_quote(c) ||
                       c == ' ' || c == '\\' || c == '\t' || c == '\n' || c == '"' ||
                       c == '\'' || c == '$' || c == '`' || c <= 0x1f || c >= 0x7f;
            if (esc && !char_in_set(c, never_quote))
                out.push_back('\\');
            if (c == '\n') { out.push_back('n'); continue; }
            if (c == '\t') { out.push_back('t'); continue; }
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    if (effective == "c" || effective == "c-maybe") {
        if (effective == "c-maybe" && !name_needs_c_quote(name, extra_quote, never_quote))
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
            } else if (force_quote(c) || c < 0x20 || c >= 0x7f) {
                out += std::format("\\{:03o}", static_cast<unsigned>(c));
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
        return out;
    }

    if (effective == "shell" || effective == "shell-always") {
        if (effective == "shell" && !name_needs_shell_quote(name, extra_quote, never_quote))
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

    // shell-escape / shell-escape-always: $'...' ANSI-C quoting
    if (effective == "shell-escape" || effective == "shell-escape-always") {
        bool needs = (effective == "shell-escape-always") ||
                     name_needs_shell_quote(name, extra_quote, never_quote);
        if (!needs) {
            for (unsigned char c : name) {
                if (c < 0x20 || c >= 0x7f) { needs = true; break; }
            }
        }
        if (!needs)
            return name;
        std::string out;
        out.reserve(name.size() + 8);
        out += "$'";
        for (unsigned char c : name) {
            if (c == '\'') {
                out += "\\'";
            } else if (c == '\\') {
                out += "\\\\";
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c == '\r') {
                out += "\\r";
            } else if (force_quote(c) || c < 0x20 || c >= 0x7f) {
                out += std::format("\\{:03o}", static_cast<unsigned>(c));
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('\'');
        return out;
    }

    // unknown style → literal
    return name;
}

static std::string quote_name_cfg(const std::string& name, const Config& cfg) {
    return quote_name(name, cfg.quoting_style, cfg.quote_chars, cfg.no_quote_chars);
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

/// True if path is unchanged vs snapshot (mtime + optional device).
static bool snap_unchanged(const SnapRec& rec, const struct stat& st,
                           bool check_device) {
    if (static_cast<std::int64_t>(st.st_mtime) > rec.mtime)
        return false;
    if (check_device && rec.has_dev &&
        static_cast<std::uint64_t>(st.st_dev) != rec.dev)
        return false;
    return true;
}

/// Read a NUL-terminated decimal integer from @p in. Returns false on EOF.
static bool snap_read_num(std::istream& in, std::int64_t& out) {
    std::string tok;
    std::getline(in, tok, '\0');
    if (!in && tok.empty())
        return false;
    if (tok.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(tok.c_str(), &end, 10);
    if (errno || end == tok.c_str() || *end != '\0')
        return false;
    out = static_cast<std::int64_t>(v);
    return true;
}

/// Read a NUL-terminated string from @p in.
static bool snap_read_str(std::istream& in, std::string& out) {
    std::getline(in, out, '\0');
    return static_cast<bool>(in) || !out.empty();
}

/// Load GNU tar listed-incremental snapshot (format 0/1 text or 2 binary-NUL).
/// Best-effort: maps dumpdir basenames to start_time so mutar's skip filter
/// approximates GNU level≥1 (files with mtime ≤ dump start are unchanged).
/// @return true if any entries were loaded.
static bool load_gnu_snapshot(std::istream& in, const std::string& header,
                              std::map<std::string, SnapRec>& snapshot_map) {
    // Header: "GNU tar-<version>-<N>" where N is incremental format version
    unsigned long incr_ver = 0;
    {
        auto dash = header.rfind('-');
        if (dash == std::string::npos)
            return false;
        char* end = nullptr;
        incr_ver = std::strtoul(header.c_str() + dash + 1, &end, 10);
        if (end == header.c_str() + dash + 1)
            return false;
    }

    std::int64_t start_sec = 0;

    if (incr_ver >= 2) {
        // Format 2: start_time sec\0 nsec\0 then directory records
        std::int64_t start_nsec = 0;
        if (!snap_read_num(in, start_sec) || !snap_read_num(in, start_nsec))
            return false;
        (void)start_nsec;

        for (;;) {
            std::int64_t nfs = 0;
            if (!snap_read_num(in, nfs))
                break; // normal EOF
            std::int64_t mtime_sec = 0, mtime_nsec = 0, dev = 0, ino = 0;
            if (!snap_read_num(in, mtime_sec) || !snap_read_num(in, mtime_nsec)
                || !snap_read_num(in, dev) || !snap_read_num(in, ino))
                break;
            (void)mtime_nsec;
            (void)ino;
            (void)nfs;
            std::string dirname;
            if (!snap_read_str(in, dirname))
                break;

            // Dumpdir entries: "Yname" / "Nname" / "Dname" NUL-terminated,
            // terminated by empty string then another empty (double NUL).
            for (;;) {
                std::string ent;
                if (!snap_read_str(in, ent))
                    break;
                if (ent.empty()) {
                    // record terminator: one more empty
                    std::string term;
                    (void)snap_read_str(in, term);
                    break;
                }
                char tag = ent[0];
                std::string base = ent.substr(1);
                if (tag == 'D') {
                    // subdirectory — directory itself always dumped by mutar
                    continue;
                }
                if (tag != 'Y' && tag != 'N')
                    continue;
                std::string full = dirname;
                if (!full.empty() && full.back() != '/' && !base.empty())
                    full += '/';
                full += base;
                SnapRec rec{};
                // Use dump start time as threshold (GNU-like: mtime ≤ start → skip)
                rec.mtime = start_sec;
                rec.dev = static_cast<std::uint64_t>(dev);
                rec.has_dev = true;
                // Also allow directory mtime as a floor if newer than start
                if (mtime_sec > start_sec)
                    rec.mtime = mtime_sec;
                snapshot_map[full] = rec;
            }
            // Directory entry itself
            {
                SnapRec drec{};
                drec.mtime = mtime_sec > 0 ? mtime_sec : start_sec;
                drec.dev = static_cast<std::uint64_t>(dev);
                drec.has_dev = true;
                snapshot_map[dirname] = drec;
            }
        }
        return !snapshot_map.empty() || start_sec > 0;
    }

    // Format 0/1: line-oriented (not fully implemented; accept header only)
    (void)start_sec;
    return false;
}

/// Load mutar MUTAR_SNAPSHOT_V1/V2 text snapshot into @p snapshot_map.
static void load_mutar_snapshot(std::istream& in,
                                std::map<std::string, SnapRec>& snapshot_map) {
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string sname = line.substr(0, tab);
        std::string rest  = line.substr(tab + 1);
        SnapRec rec{};
        auto tab2 = rest.find('\t');
        std::string smts = (tab2 == std::string::npos) ? rest : rest.substr(0, tab2);
        char* endp = nullptr; errno = 0;
        rec.mtime = std::strtoll(smts.c_str(), &endp, 10);
        if (endp == smts.c_str() || (*endp != '\0' && *endp != '\t') || errno != 0)
            continue;
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
}

/// Build GNU dumpdir body for directory at @p fspath (archive name @p archname).
/// Control codes: 'Y' = dump this non-dir, 'D' = subdirectory, 'N' = not dumped.
/// @p snap / @p do_listed_inc control Y vs N for listed-incremental level≥1.
static std::string build_dumpdir_body(
    const std::string& fspath,
    const std::string& archname,
    const Config& cfg,
    const std::map<std::string, SnapRec>* snap,
    bool do_listed_inc)
{
    DIR* d = ::opendir(fspath.c_str());
    if (!d) return std::string(1, '\0');

    struct Ent { char code; std::string name; };
    std::vector<Ent> ents;
    while (struct dirent* de = ::readdir(d)) {
        std::string_view dname(de->d_name);
        if (dname == "." || dname == "..") continue;
        std::string name(dname);
        std::string child_fs = fspath + "/" + name;
        std::string child_arch = (archname.empty() || archname == ".")
            ? name
            : (archname.back() == '/' ? archname + name : archname + "/" + name);
        // Strip trailing slash from arch for snapshot lookup
        while (!child_arch.empty() && child_arch.back() == '/')
            child_arch.pop_back();

        char code = 'Y';
        if (is_excluded(cfg, child_arch)) {
            code = 'N';
        } else {
            struct stat cst{};
            if (::lstat(child_fs.c_str(), &cst) < 0) {
                code = 'N';
            } else if (S_ISDIR(cst.st_mode)) {
                code = 'D';
            } else if (do_listed_inc && snap) {
                auto it = snap->find(child_arch);
                if (it != snap->end() &&
                    snap_unchanged(it->second, cst, cfg.check_device))
                    code = 'N';
                else
                    code = 'Y';
            } else {
                code = 'Y';
            }
        }
        ents.push_back({code, std::move(name)});
    }
    ::closedir(d);

    std::ranges::sort(ents, [](const Ent& a, const Ent& b) {
        return a.name < b.name;
    });

    std::string body;
    for (const auto& e : ents) {
        body.push_back(e.code);
        body += e.name;
        body.push_back('\0');
    }
    body.push_back('\0');
    return body;
}

/// Recursively remove a filesystem path (for -G dumpdir purge).
static bool remove_path_recursive(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

/// Empty a directory's children for --recursive-unlink (keeps the directory itself).
/// @return true if every child was removed (or the directory was empty).
static bool empty_directory_contents(const std::string& dirpath) {
    DIR* d = ::opendir(dirpath.c_str());
    if (!d)
        return false;
    std::vector<std::string> names;
    while (struct dirent* de = ::readdir(d)) {
        std::string_view n(de->d_name);
        if (n == "." || n == "..")
            continue;
        names.emplace_back(n);
    }
    ::closedir(d);
    bool ok = true;
    for (const auto& name : names) {
        const std::string full = dirpath + "/" + name;
        if (!remove_path_recursive(full))
            ok = false;
    }
    return ok;
}

/// Prepare an existing path so a directory member can be extracted.
/// --recursive-unlink: remove non-dir at path, or empty existing directory hierarchy.
static void recursive_unlink_prepare(const std::string& dpath, const Config& cfg) {
    if (!cfg.recursive_unlink)
        return;
    struct stat st{};
    if (::lstat(dpath.c_str(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (!empty_directory_contents(dpath))
            print(stderr, "mutar: {}: cannot empty directory: {}\n",
                  dpath, std::strerror(errno));
    } else {
        if (::unlink(dpath.c_str()) < 0)
            print(stderr, "mutar: {}: cannot unlink: {}\n",
                  dpath, std::strerror(errno));
    }
}

/// Parse dumpdir body into map name → control code (first char of each record).
static std::map<std::string, char> parse_dumpdir(std::string_view body) {
    std::map<std::string, char> out;
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] == '\0') break;
        char code = body[i];
        ++i;
        std::size_t start = i;
        while (i < body.size() && body[i] != '\0') ++i;
        if (i > start)
            out[std::string(body.substr(start, i - start))] = code;
        if (i < body.size() && body[i] == '\0') ++i;
    }
    return out;
}

/// Purge directory contents not listed (or type-mismatched) in dumpdir (GNU -G).
static void purge_directory_dumpdir(const std::string& dirpath,
                                    const std::map<std::string, char>& dump,
                                    const Config& cfg) {
    DIR* d = ::opendir(dirpath.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (struct dirent* de = ::readdir(d)) {
        std::string_view n(de->d_name);
        if (n == "." || n == "..") continue;
        names.emplace_back(n);
    }
    ::closedir(d);

    for (const auto& name : names) {
        auto it = dump.find(name);
        std::string full = dirpath + "/" + name;
        struct stat st{};
        if (::lstat(full.c_str(), &st) < 0) continue;

        bool should_delete = false;
        if (it == dump.end()) {
            should_delete = true;
        } else if (it->second == 'D' && !S_ISDIR(st.st_mode)) {
            should_delete = true;
        } else if (it->second == 'Y' && S_ISDIR(st.st_mode)) {
            should_delete = true;
        }

        if (!should_delete) continue;

        if (cfg.interactive) {
            std::fprintf(stderr, "mutar: delete `%s'? [y/N] ", full.c_str());
            std::fflush(stderr);
            std::string ans;
            std::ifstream tty("/dev/tty");
            if (tty.is_open()) std::getline(tty, ans);
            else if (::isatty(STDIN_FILENO)) std::getline(std::cin, ans);
            else ans = "n";
            if (ans.empty() || (ans[0] != 'y' && ans[0] != 'Y'))
                continue;
        }
        if (cfg.verbose)
            print(stderr, "mutar: Deleting {}\n", full);
        if (!remove_path_recursive(full))
            print(stderr, "mutar: {}: cannot remove: {}\n", full, std::strerror(errno));
    }
}

// ── Verbose line output (matching GNU tar format) ─────────────────────────────

static std::string format_mode(char typeflag, unsigned mode) {
    char buf[11];
    switch (typeflag) {
    case DIRTYPE:
    case GNUTYPE_DUMPDIR: buf[0] = 'd'; break;
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
    const std::string qname = quote_name_cfg(e.name, cfg);
    const std::string qlink = (e.typeflag == SYMTYPE)
                                  ? quote_name_cfg(e.linkname, cfg)
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
                        print("{}\n", quote_name_cfg(display_name, cfg));
                        total_bytes += ie.size;
                    }
                    if (cfg.totals)
                        print(stderr, "Total bytes listed: {}\n", total_bytes);
                    return EXIT_SUCCESS;
                }
                mutar_warn(cfg, "index",
                    std::format("cannot load index '{}': {}; scanning archive",
                                idx_path, lr.error().message));
            }
        }
    }

    auto res = ArchiveStream::open_read(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& s = *res;
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    reader.set_warn_config(&cfg);
    if (cfg.pax_option_rules.any())
        reader.set_pax_rules(&cfg.pax_option_rules);

    if (!cfg.index_file.empty()) {
        g_index_fp = std::fopen(cfg.index_file.c_str(), "w");
        if (!g_index_fp)
            print(stderr, "mutar: {}: cannot open index file\n", cfg.index_file);
    }

    // Normalize user-supplied member names so "./file.txt" matches stored "file.txt"
    std::set<std::string> want;
    std::vector<std::string> want_order;
    std::size_t want_order_pos = 0;
    for (const auto& f : cfg.files) {
        std::string n = normalize_member(f);
        want.insert(n);
        if (cfg.preserve_order)
            want_order.push_back(std::move(n));
    }

    std::int64_t total_bytes = 0;
    std::int64_t checkpoint_count = 0;
    std::map<std::string, int> occurrence_map;
    bool started = cfg.starting_file.empty();
    int exit_code = EXIT_SUCCESS;
    for (;;) {
        auto [e, ok, eof] = reader.next_entry();
        if (eof) break;
        if (!ok) { exit_code = EXIT_FAILURE; break; }
        if (reader.failed()) { exit_code = EXIT_FAILURE; break; }
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
        if (!want.empty()) {
            bool selected = false;
            if (cfg.preserve_order && !want_order.empty()) {
                if (want_order_pos < want_order.size()) {
                    const std::string& head = want_order[want_order_pos];
                    std::string en = normalize_member(e.name);
                    if (head == e.name || head == en || head == display_name ||
                        head == normalize_member(display_name)) {
                        selected = true;
                        ++want_order_pos;
                    }
                }
            } else {
                selected = want.contains(e.name) || want.contains(display_name);
            }
            if (!selected) {
                if (cfg.show_omitted_dirs &&
                    (e.typeflag == DIRTYPE || (!e.name.empty() && e.name.back() == '/')))
                    print(stderr, "mutar: {}\n", e.name);
                reader.skip_entry(e);
                continue;
            }
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
            print("{}\n", quote_name_cfg(display_name, cfg));
        }

        total_bytes += (e.asize + BLOCKSIZE - 1) / BLOCKSIZE * BLOCKSIZE;
        add_running_total((e.asize + BLOCKSIZE - 1) / BLOCKSIZE * BLOCKSIZE);
        if (!reader.skip_entry(e)) {
            exit_code = EXIT_FAILURE;
            break;
        }
    }
    if (cfg.totals)
        print(stderr, "Total bytes listed: {}\n", total_bytes);
    if (g_index_fp) { std::fclose(g_index_fp); g_index_fp = nullptr; }
    if (reader.failed())
        exit_code = EXIT_FAILURE;
    s.close();
    if (s.child_failed()) {
        print(stderr, "mutar: {}\n", s.child_error());
        exit_code = EXIT_FAILURE;
    }
    return exit_code;
}

// ── create (-c) ───────────────────────────────────────────────────────────────

static int op_create(const Config& cfg) {
    // GNU tar: no member names and no -T → refuse (do not archive CWD, and
    // do not O_TRUNC an existing archive path).
    if (cfg.files.empty() && cfg.files_from.empty()) {
        print(stderr, "mutar: Cowardly refusing to create an empty archive\n");
        print(stderr, "Try 'mutar --help' for more information.\n");
        return EXIT_FAILURE;
    }

    // Multi-volume: starting volume number from --volno-file (default 1)
    int vol_num = 1;
    if (cfg.multi_volume && !cfg.volno_file.empty())
        vol_num = read_volno_file(cfg.volno_file);

    // Pin -f to the original CWD so later volumes are not created under -C.
    const std::string archive_base = cfg.multi_volume
        ? absolutize_local_archive(cfg)
        : cfg.archive_file;

    Config vol_cfg = cfg;
    if (cfg.multi_volume)
        vol_cfg.archive_file = make_volume_name(archive_base, vol_num);

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
    // -o on create: same as --old-archive (V7), matching GNU tar
    if (cfg.old_archive || cfg.compat_o) fmt = Format::V7;

    // GNU: --pax-option on create requires POSIX/PAX format
    if (cfg.pax_option_rules.any() && fmt != Format::PAX) {
        print(stderr, "mutar: --pax-option can be used only on POSIX archives\n");
        return EXIT_FAILURE;
    }

    ArchiveWriter writer(s, cfg.blocking_factor, fmt);

    // Optional sidecar index collection (--write-index / --mutar-index / --seekable)
    ArchiveIndex create_index;
    const bool collect_index =
        cfg.write_index || !cfg.mutar_index.empty() || cfg.seekable;
    if (collect_index)
        writer.set_index(&create_index);

    // Global PAX 'g' header for keyword=value overrides (before members)
    if (cfg.pax_option_rules.any())
        writer.write_global_pax_header(cfg);

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
    // Detects: MUTAR_SNAPSHOT_V1/V2 text, or GNU tar "GNU tar-*-N" format 2.
    std::map<std::string, SnapRec> snapshot_map;
    bool do_incremental = !cfg.listed_incremental.empty() && cfg.level >= 1;
    if (do_incremental) {
        std::ifstream snap_in(cfg.listed_incremental, std::ios::binary);
        if (snap_in) {
            std::string header;
            std::getline(snap_in, header);
            if (header.rfind("GNU tar-", 0) == 0) {
                if (!load_gnu_snapshot(snap_in, header, snapshot_map)) {
                    print(stderr,
                          "mutar: warning: could not parse GNU snapshot '{}'; "
                          "treating as level 0\n",
                          cfg.listed_incremental);
                    do_incremental = false;
                }
            } else if (header.rfind("MUTAR_SNAPSHOT", 0) == 0) {
                load_mutar_snapshot(snap_in, snapshot_map);
            } else {
                // Unknown / empty header: try mutar line format from start
                snap_in.clear();
                snap_in.seekg(0);
                load_mutar_snapshot(snap_in, snapshot_map);
            }
        } else {
            // Snapshot file missing → archive all (treat as level 0)
            do_incremental = false;
        }
    }
    // Collect entries for snapshot write/update (files + directories + specials)
    std::vector<std::pair<std::string, SnapRec>> snapshot_entries;

    // Create does not global-chdir: each member uses file_chdir / -C prefix
    // so -f volume paths stay in the original CWD.

    int exit_code = EXIT_SUCCESS;
    std::int64_t total_bytes = 0;
    std::int64_t checkpoint_count = 0;
    std::vector<std::string> files_to_remove; // for --remove-files
    const std::int64_t max_blocks = tape_max_blocks(cfg);
    const bool do_multivol = cfg.multi_volume && max_blocks > 0;

    // Finish current volume, run info-script, open next volume, swap writer stream.
    // mid_member=true: volume already flushed without EOF zero blocks (GNUTYPE_MULTIVOL).
    // mid_member=false: write EOF zeros then rotate (between-member boundary).
    auto rotate_volume = [&](bool mid_member) -> bool {
        if (!mid_member)
            writer.finish();
        total_bytes += writer.block_no() * static_cast<std::int64_t>(BLOCKSIZE);
        std::string finished_path = vol_cfg.archive_file;
        s.close();
        if (s.child_failed()) {
            print(stderr, "mutar: {}\n", s.child_error());
            exit_code = EXIT_FAILURE;
            return false;
        }

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

        vol_cfg.archive_file = make_volume_name(archive_base, vol_num);
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

    if (do_multivol)
        writer.set_multivol(max_blocks, rotate_volume);

    auto add_file = [&](const std::string& raw_archname, const std::string& fspath) {
        // Apply --newer / --newer-mtime filter (all non-dir types GNU covers)
        if (newer_cutoff != (std::time_t)-1) {
            struct stat fst{};
            if (::lstat(fspath.c_str(), &fst) == 0 && !S_ISDIR(fst.st_mode)) {
                const std::time_t stamp = cfg.newer_use_ctime
                    ? static_cast<std::time_t>(fst.st_ctime)
                    : static_cast<std::time_t>(fst.st_mtime);
                if (stamp <= newer_cutoff) {
                    mutar_warn(cfg, "newer",
                              std::format("{}: file is not newer than cutoff; not dumped", fspath));
                    return;
                }
            }
        }
        // Listed-incremental filter (level≥1): skip unchanged non-directories.
        // GNU always dumps directory headers (structure); files, symlinks and
        // specials are omitted when mtime (+ device with --check-device) match.
        if (do_incremental) {
            struct stat inc_st{};
            if (::lstat(fspath.c_str(), &inc_st) == 0 && !S_ISDIR(inc_st.st_mode)) {
                auto sit = snapshot_map.find(raw_archname);
                if (sit != snapshot_map.end() &&
                    snap_unchanged(sit->second, inc_st, cfg.check_device))
                    return; // unchanged; keep prior snapshot entry
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
                    apply_owner_group_cli(link_e, cfg);
                    if (cfg.numeric_owner) { link_e.uname.clear(); link_e.gname.clear(); }
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

        // Multi-volume: pre-rotate when the current volume has no room for a
        // new member header. Large regular files are split mid-file via
        // GNUTYPE_MULTIVOL ('M') inside ArchiveWriter::write_regular.
        if (do_multivol) {
            if (writer.block_no() > 0 && writer.block_no() >= max_blocks) {
                if (!rotate_volume(false)) return;
            } else {
                struct stat mst{};
                if (::lstat(fspath.c_str(), &mst) == 0 && S_ISREG(mst.st_mode)) {
                    // If the whole member fits with EOF margin, keep it here.
                    // Otherwise still start here so mid-file split can fill the tape
                    // (unless fewer than 2 blocks remain — rotate for a clean start).
                    const std::int64_t data_blocks =
                        (mst.st_size + static_cast<std::int64_t>(BLOCKSIZE) - 1)
                        / static_cast<std::int64_t>(BLOCKSIZE);
                    const std::int64_t need_blocks = 1 + data_blocks;
                    const std::int64_t left = max_blocks - writer.block_no();
                    if (writer.block_no() > 0 && need_blocks + 2 > left && left < 2) {
                        if (!rotate_volume(false)) return;
                    }
                } else if (writer.block_no() > 0 &&
                           writer.block_no() + 3 > max_blocks) {
                    if (!rotate_volume(false)) return;
                }
            }
        }

        // -G / --incremental (and -g): emit GNU dumpdir directory members
        if (cfg.incremental) {
            struct stat dst{};
            if (::lstat(fspath.c_str(), &dst) == 0 && S_ISDIR(dst.st_mode)) {
                Entry de;
                de.name       = archname;
                de.mode       = dst.st_mode & 07777;
                de.uid        = static_cast<unsigned>(dst.st_uid);
                de.gid        = static_cast<unsigned>(dst.st_gid);
                de.mtime      = dst.st_mtim.tv_sec;
                de.mtime_nsec = static_cast<long>(dst.st_mtim.tv_nsec);
                de.atime      = dst.st_atim.tv_sec;
                de.ctime      = dst.st_ctim.tv_sec;
                de.fmt        = fmt;
                if (struct passwd* pw = ::getpwuid(dst.st_uid)) de.uname = pw->pw_name;
                if (struct group*  gr = ::getgrgid(dst.st_gid)) de.gname = gr->gr_name;
                apply_owner_group_cli(de, cfg);
                if (cfg.numeric_owner) { de.uname.clear(); de.gname.clear(); }
                if (!cfg.mtime.empty()) {
                    std::time_t mt = parse_date_string(cfg.mtime);
                    if (mt != (std::time_t)-1 &&
                        (!cfg.clamp_mtime || de.mtime > mt)) {
                        de.mtime = mt; de.mtime_nsec = 0;
                    }
                }
                collect_xattrs(de, fspath, cfg);
                collect_acls(de, fspath, cfg, true);
                std::string body = build_dumpdir_body(
                    fspath, raw_archname, cfg,
                    do_incremental ? &snapshot_map : nullptr,
                    do_incremental);
                if (!writer.write_dumpdir(de, body, cfg))
                    exit_code = EXIT_FAILURE;
                else if (cfg.remove_files)
                    files_to_remove.push_back(fspath);
                // --checkpoint progress
                if (cfg.checkpoint > 0) {
                    ++checkpoint_count;
                    if (checkpoint_count % cfg.checkpoint == 0)
                        do_checkpoint(cfg, checkpoint_count);
                }
                return;
            }
        }

        if (!writer.add_path(archname, fspath, cfg)) {
            // --ignore-failed-read: warn already emitted; do not fail the create
            if (!cfg.ignore_failed_read)
                exit_code = EXIT_FAILURE;
        } else if (cfg.remove_files) {
            files_to_remove.push_back(fspath);
        }

        // --checkpoint progress
        if (cfg.checkpoint > 0) {
            ++checkpoint_count;
            if (checkpoint_count % cfg.checkpoint == 0)
                do_checkpoint(cfg, checkpoint_count);
        }
    };

    auto note_walk_fail = [&](bool ok) {
        if (!ok && !cfg.ignore_failed_read)
            exit_code = EXIT_FAILURE;
    };
    if (cfg.files.empty()) {
        // -T / --files-from was given but produced no names: empty archive.
    } else {
        for (std::size_t i = 0; i < cfg.files.size(); ++i) {
            const std::string& use_dir = (i < cfg.file_chdir.size())
                ? cfg.file_chdir[i]
                : cfg.directory;
            note_walk_fail(walk_dir(use_dir, cfg.files[i], cfg, add_file));
        }
    }

    writer.finish();
    total_bytes += writer.block_no() * static_cast<std::int64_t>(BLOCKSIZE);
    set_running_total(total_bytes);
    s.close();
    if (s.child_failed()) {
        print(stderr, "mutar: {}\n", s.child_error());
        exit_code = EXIT_FAILURE;
    }

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
            // Optional note: compressed + --seekable still uses materialize-then-seek
            std::string idx_note;
            if (cfg.seekable) {
                Compress c = cfg.compress;
                if (c == Compress::Auto) {
                    c = cfg.no_auto_compress ? Compress::None
                                             : detect_compress(cfg.archive_file);
                }
                if (c != Compress::None && c != Compress::Auto) {
                    const char* prog = compress_prog_for(c, cfg.compress_prog);
                    if (prog && prog[0])
                        idx_note = std::format(
                            "compressed={} seekable=materialize", prog);
                }
            }
            auto sr = create_index.save(idx_path, idx_note);
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
            ArchiveReader vreader(vs, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    vreader.set_warn_config(&cfg);
            int verify_count = 0;
            for (;;) {
                auto [ve, vok, veof] = vreader.next_entry();
                if (veof) break;
                if (!vok) { verify_ok = false; break; }
                ++verify_count;
                if (!vreader.skip_entry(ve)) {
                    verify_ok = false;
                    break;
                }
            }
            vs.close();
            if (vreader.failed() || vs.child_failed())
                verify_ok = false;
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
    set_running_total(total_bytes);
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
    // RAII: any early return after materialize unlinks the temp automatically.
    TempPathUnlink materialize_guard;

    if (!cfg.index_file.empty()) {
        g_index_fp = std::fopen(cfg.index_file.c_str(), "w");
        if (!g_index_fp)
            print(stderr, "mutar: {}: cannot open index file\n", cfg.index_file);
    }

    // Normalize user-supplied member names so "./file.txt" matches stored "file.txt"
    std::set<std::string> want;
    // Ordered list for -s/--preserve-order/--same-order (GNU single-pass semantics)
    std::vector<std::string> want_order;
    std::size_t want_order_pos = 0;
    for (const auto& f : cfg.files) {
        std::string n = normalize_member(f);
        want.insert(n);
        if (cfg.preserve_order)
            want_order.push_back(std::move(n));
    }

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
                mutar_warn(cfg, "index",
                    std::format("cannot load index '{}': {}",
                                idx_path, lr.error().message));
            }
        } else if (!cfg.mutar_index.empty()) {
            mutar_warn(cfg, "index",
                std::format("index '{}' not found; sequential extract",
                            cfg.mutar_index));
        }
    }

    // Compressed streams are pipes (not seekable). When we have an index and
    // named members, materialise decompressed bytes to a temp file so seek
    // extract works for .tar.xz / .tar.zst / etc. (full decompress once).
    const bool want_seek =
        have_index && !want.empty() && !s.is_seekable(cfg.seek) && !cfg.preserve_order;
    if (want_seek) {
        std::string materialize_temp;
        auto mat = ArchiveStream::materialize_seekable(s, materialize_temp);
        if (!mat) {
            print(stderr, "mutar: cannot materialize seekable view: {}\n",
                  mat.error().message);
            return EXIT_FAILURE;
        }
        s = std::move(*mat);
        materialize_guard = TempPathUnlink(std::move(materialize_temp));
        if (std::getenv("MUTAR_DEBUG_SEEK"))
            print(stderr, "mutar: materialized compressed archive to {} for seek\n",
                  materialize_guard.path);
    }

    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    reader.set_warn_config(&cfg);
    if (cfg.pax_option_rules.any())
        reader.set_pax_rules(&cfg.pax_option_rules);

    // Pin -f before -C so multi-volume follow-on files stay at original CWD.
    const std::string extract_archive_base = cfg.multi_volume
        ? absolutize_local_archive(cfg)
        : cfg.archive_file;

    if (!cfg.directory.empty()) {
        if (::chdir(cfg.directory.c_str()) < 0) {
            print(stderr, "mutar: -C {}: {}\n", cfg.directory, std::strerror(errno));
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

    // Open the next multi-volume archive file and swap the reader stream.
    // Used both at member boundaries (EOF zeros) and mid-file (short data read).
    auto switch_extract_volume = [&]() -> bool {
        std::string cur_vol = make_volume_name(extract_archive_base, extract_vol_num);
        if (!run_info_script(cfg, cur_vol, extract_vol_num, "-x"))
            return false;
        ++extract_vol_num;
        if (!cfg.volno_file.empty())
            write_volno_file(cfg.volno_file, extract_vol_num);

        std::string next_vol = make_volume_name(extract_archive_base, extract_vol_num);
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
                return false;
            }
            if (tty != stdin) std::fclose(tty);
        }

        Config next_cfg = cfg;
        next_cfg.archive_file = next_vol;
        auto next_res = ArchiveStream::open_read(next_cfg);
        if (!next_res) {
            print(stderr, "mutar: cannot open volume {}: {}\n",
                  next_vol, next_res.error().message);
            return false;
        }
        print(stderr, "mutar: reading volume #{} from {}\n",
              extract_vol_num, next_vol);
        s.close();
        if (s.child_failed()) {
            print(stderr, "mutar: {}\n", s.child_error());
            return false;
        }
        s = std::move(*next_res);
        reader.swap_stream(s);
        return true;
    };

    // Seek-based selective extract when index + seekable stream + named members.
    // Disabled with --preserve-order: must honour archive order vs want-list order.
    std::vector<IndexEntry> seek_queue;
    bool use_index_seek = false;
    std::size_t seek_qi = 0;
    if (have_index && !want.empty() && s.is_seekable(cfg.seek) && !cfg.preserve_order) {
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
            if (reader.failed()) {
                exit_code = EXIT_FAILURE;
                break;
            }
            if (use_index_seek) break; // done with seek queue
            // Multi-volume: on EOF, try next volume; absence is a clean end.
            if (cfg.multi_volume) {
                if (!switch_extract_volume())
                    break;
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
        // Prepend --one-top-level directory (do not double-prefix)
        if (!one_top.empty())
            outpath = apply_one_top_level(outpath, one_top);

        // Member selection: set match, or ordered single-pass with -s
        if (!want.empty()) {
            bool selected = false;
            if (cfg.preserve_order && !want_order.empty()) {
                // GNU --same-order: only the current head of the want-list matches.
                // Non-matching archive members are skipped; head advances on match.
                if (want_order_pos < want_order.size()) {
                    const std::string& head = want_order[want_order_pos];
                    std::string en = normalize_member(e.name);
                    if (head == e.name || head == en || head == outpath ||
                        head == normalize_member(outpath)) {
                        selected = true;
                        ++want_order_pos;
                    }
                }
            } else {
                selected = want.contains(e.name) || want.contains(outpath) ||
                           want.contains(normalize_member(e.name));
            }
            if (!selected) {
                reader.skip_entry(e);
                continue;
            }
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

        // Create parent directories without following intermediate symlinks
        // (dir-symlink zip-slip: archive "d"→outside then "d/evil" regular).
        if (ensure_parent_dirs_nofollow(outpath, cfg) < 0) {
            print(stderr, "mutar: {}: cannot create parent directories: {}\n",
                  outpath, std::strerror(errno));
            reader.skip_entry(e);
            exit_code = EXIT_FAILURE;
            continue;
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
                        if (cfg.delay_dir_restore && !cfg.no_delay_dir_restore)
                            dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
                        reader.skip_entry(e);
                        break;
                    }
                }
            }

            // --recursive-unlink: empty existing hierarchy (or remove non-dir) first
            recursive_unlink_prepare(dpath, cfg);

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

            // GNU: delay only with --delay-directory-restore (cancelled by --no-delay-…)
            if (cfg.delay_dir_restore && !cfg.no_delay_dir_restore) {
                dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
            } else {
                // Default / --no-delay-directory-restore: apply immediately
                if (!cfg.touch) {
                    struct timespec ts[2];
                    ts[0].tv_sec = ts[1].tv_sec   = e.mtime;
                    ts[0].tv_nsec = ts[1].tv_nsec  = e.mtime_nsec;
                    ::utimensat(AT_FDCWD, dpath.c_str(), ts, 0);
                }
                bool set_perm = cfg.same_permissions ||
                                (::getuid() == 0 && !cfg.no_same_permissions);
                if (set_perm) ::chmod(dpath.c_str(), e.mode & 07777);
            }
            restore_xattrs_acls(e, dpath, cfg);
            reader.skip_entry(e);
            break;
        }
        case SYMTYPE:
        {
            if (cfg.keep_old_files) {
                struct stat st{};
                if (::lstat(outpath.c_str(), &st) == 0) {
                    print(stderr, "mutar: {}: file exists\n", outpath);
                    reader.skip_entry(e);
                    exit_code = EXIT_FAILURE;
                    break;
                }
            }
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
            std::string link_target = sanitize_path(e.linkname, cfg.absolute_names);
            // Apply the same path rewrites as for outpath so the hard link target
            // remains consistent when --transform / --strip-components / --one-top-level
            // are in use.
            if (!cfg.transform_expr.empty())
                link_target = apply_transform(link_target, cfg.transform_expr);
            if (cfg.strip_components > 0)
                link_target = strip_components(link_target, cfg.strip_components);
            if (!one_top.empty())
                link_target = apply_one_top_level(link_target, one_top);
            if (extract_hardlink(outpath, link_target, cfg) < 0) {
                print(stderr, "mutar: hardlink {} -> {}: {}\n",
                           outpath, link_target, std::strerror(errno));
                exit_code = EXIT_FAILURE;
            }
            if (!reader.skip_entry(e))
                exit_code = EXIT_FAILURE;
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
            if (cfg.keep_old_files) {
                struct stat st{};
                if (::lstat(outpath.c_str(), &st) == 0) {
                    print(stderr, "mutar: {}: file exists\n", outpath);
                    reader.skip_entry(e);
                    exit_code = EXIT_FAILURE;
                    break;
                }
            }
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
            reader.skip_entry(e);
            break;
        case GNUTYPE_DUMPDIR:
        {
            // GNU old incremental directory: create dir, optional purge, consume body
            std::string dpath = outpath;
            while (!dpath.empty() && dpath.back() == '/') dpath.pop_back();
            if (dpath.empty()) dpath = ".";

            // Same keep-directory-symlink / overwrite-dir guards as DIRTYPE
            if (cfg.keep_dir_symlink) {
                struct stat lnk_st{};
                if (::lstat(dpath.c_str(), &lnk_st) == 0 && S_ISLNK(lnk_st.st_mode)) {
                    struct stat tgt_st{};
                    if (::stat(dpath.c_str(), &tgt_st) == 0 && S_ISDIR(tgt_st.st_mode)) {
                        if (cfg.incremental && e.size > 0) {
                            std::string body;
                            if (!reader.read_data_string(e.size, body)) {
                                exit_code = EXIT_FAILURE;
                                break;
                            }
                            auto dump = parse_dumpdir(body);
                            purge_directory_dumpdir(dpath, dump, cfg);
                        } else {
                            if (!reader.skip_entry(e))
                                exit_code = EXIT_FAILURE;
                        }
                        if (cfg.delay_dir_restore && !cfg.no_delay_dir_restore)
                            dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
                        break;
                    }
                }
            }

            // --recursive-unlink: empty existing hierarchy (or remove non-dir) first
            recursive_unlink_prepare(dpath, cfg);

            struct stat existing_st{};
            bool path_exists = (::lstat(dpath.c_str(), &existing_st) == 0);
            if (path_exists && !S_ISDIR(existing_st.st_mode)) {
                print(stderr, "mutar: {}: Cannot overwrite non-directory with directory\n", dpath);
                reader.skip_entry(e);
                break;
            }
            ::mkdir(dpath.c_str(), 0777);

            // Read dumpdir body then purge extras when -G/--incremental
            if (e.size > 0) {
                std::string body;
                if (!reader.read_data_string(e.size, body)) {
                    exit_code = EXIT_FAILURE;
                    break;
                }
                if (cfg.incremental) {
                    auto dump = parse_dumpdir(body);
                    purge_directory_dumpdir(dpath, dump, cfg);
                }
            }

            if (path_exists && cfg.no_overwrite_dir) {
                if (cfg.verbose)
                    print(stderr, "mutar: {}: directory already exists, skipping metadata update\n", dpath);
                break;
            }
            if (cfg.delay_dir_restore && !cfg.no_delay_dir_restore) {
                dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
            } else {
                if (!cfg.touch) {
                    struct timespec ts[2];
                    ts[0].tv_sec = ts[1].tv_sec  = e.mtime;
                    ts[0].tv_nsec = ts[1].tv_nsec = e.mtime_nsec;
                    ::utimensat(AT_FDCWD, dpath.c_str(), ts, 0);
                }
                bool set_perm = cfg.same_permissions ||
                                (::getuid() == 0 && !cfg.no_same_permissions);
                if (set_perm) ::chmod(dpath.c_str(), e.mode & 07777);
            }
            restore_xattrs_acls(e, dpath, cfg);
            break;
        }
        default: // REGTYPE ('0'), AREGTYPE ('\0' old V7 regular), CONTTYPE ('7')
        {
            // AREGTYPE ('\0') with a name ending in '/' is an old implicit-directory
            // entry (used by some historical tar implementations).  Treat as a dir.
            if (e.typeflag == AREGTYPE && !outpath.empty() && outpath.back() == '/') {
                std::string dpath = outpath;
                dpath.pop_back();
                ::mkdir(dpath.c_str(), 0777);
                if (cfg.delay_dir_restore && !cfg.no_delay_dir_restore)
                    dir_fixups.push_back({dpath, e.mtime, e.mtime_nsec, e.mode});
                else if (!cfg.touch) {
                    struct timespec ts[2];
                    ts[0].tv_sec = ts[1].tv_sec  = e.mtime;
                    ts[0].tv_nsec = ts[1].tv_nsec = e.mtime_nsec;
                    ::utimensat(AT_FDCWD, dpath.c_str(), ts, 0);
                }
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
                add_running_total(e.size);
                int child_rc = 0;
                if (WIFEXITED(status)) {
                    child_rc = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    if (sig == SIGPIPE && cfg.ignore_command_error)
                        child_rc = 0;
                    else
                        child_rc = 128 + sig;
                } else {
                    child_rc = 1;
                }
                if (!data_ok || child_rc != 0) {
                    // GNU default: --no-ignore-command-error → non-zero is failure
                    if (cfg.ignore_command_error) {
                        mutar_warn(cfg, "to-command",
                            std::format("--to-command '{}' exited {}",
                                        cfg.to_command, child_rc));
                    } else {
                        print(stderr, "mutar: --to-command '{}' exited {}\n",
                              cfg.to_command, child_rc);
                        exit_code = EXIT_FAILURE;
                    }
                }
                break;
            }

            // --backup: rename existing file before overwriting
            if (cfg.backup && exists && !cfg.keep_old_files && !cfg.skip_old_files) {
                std::string bak = make_backup_path(outpath, cfg);
                if (!bak.empty())
                    ::rename(outpath.c_str(), bak.c_str());
            }

            // O_NOFOLLOW + unlink-if-symlink: never write through a symlink
            // (symlink-mediated zip-slip when archive has link then regular).
            // Multi-volume continuation ('M') reopens without truncating.
            const bool is_mvol = (e.typeflag == GNUTYPE_MULTIVOL);
            int fd = open_extract_regular(outpath, cfg, /*no_trunc=*/is_mvol);
            if (fd < 0) {
                print(stderr, "mutar: {}: {}\n", outpath, std::strerror(errno));
                reader.skip_entry(e);
                exit_code = EXIT_FAILURE;
                break;
            }

            // Sparse extraction (with multi-volume mid-file support): place
            // concatenated archive data into sparse map offsets. Continuation
            // volumes use type 'M' with multivol_offset into the *archived*
            // sparse stream (not logical file offset).
            if (e.is_sparse && !e.sparse_map.empty()) {
                std::int64_t real_sz = e.real_size > 0 ? e.real_size : e.size;
                ::ftruncate(fd, static_cast<off_t>(real_sz));

                std::size_t seg_idx = 0;
                std::int64_t seg_pos = 0;
                auto place_sparse = [&](const char* data, std::size_t n) {
                    std::size_t off = 0;
                    while (off < n && seg_idx < e.sparse_map.size()) {
                        const auto& sm = e.sparse_map[seg_idx];
                        std::int64_t room = sm.numbytes - seg_pos;
                        if (room <= 0) {
                            ++seg_idx;
                            seg_pos = 0;
                            continue;
                        }
                        std::size_t chunk = static_cast<std::size_t>(std::min<std::int64_t>(
                            static_cast<std::int64_t>(n - off), room));
                        if (::lseek(fd, static_cast<off_t>(sm.offset + seg_pos),
                                    SEEK_SET) < 0)
                            return;
                        const char* p = data + off;
                        std::size_t left = chunk;
                        while (left > 0) {
                            ssize_t w = ::write(fd, p, left);
                            if (w < 0) {
                                if (errno == EINTR) continue;
                                return;
                            }
                            p += w;
                            left -= static_cast<std::size_t>(w);
                        }
                        off += chunk;
                        seg_pos += static_cast<std::int64_t>(chunk);
                        if (seg_pos >= sm.numbytes) {
                            ++seg_idx;
                            seg_pos = 0;
                        }
                    }
                };

                const std::int64_t arch_total = e.asize;
                std::int64_t arch_done = 0;
                Entry cur = e;
                for (;;) {
                    std::int64_t got = 0;
                    if (!reader.read_entry_data(cur, /*out_fd=*/-1, place_sparse, &got)) {
                        print(stderr, "mutar: {}: write error during sparse extract\n",
                              outpath);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    arch_done += got;
                    if (arch_done >= arch_total)
                        break;
                    if (got >= cur.asize && arch_done < arch_total) {
                        // Fragment claimed complete but total not reached — need next vol
                    } else if (got >= cur.asize) {
                        break;
                    }
                    if (!cfg.multi_volume) {
                        print(stderr,
                              "mutar: {}: unexpected EOF in sparse archive "
                              "(got {} of {} archived bytes)\n",
                              outpath, arch_done, arch_total);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    if (!switch_extract_volume()) {
                        print(stderr,
                              "mutar: {}: multi-volume sparse: need next volume "
                              "(have {} of {} archived bytes)\n",
                              outpath, arch_done, arch_total);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    auto [e2, ok2, eof2] = reader.next_entry();
                    if (eof2 || !ok2 || e2.typeflag != GNUTYPE_MULTIVOL) {
                        print(stderr,
                              "mutar: {}: multi-volume sparse: missing type 'M' "
                              "continuation\n",
                              outpath);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    if (e2.multivol_offset > 0 && e2.multivol_offset != arch_done)
                        arch_done = e2.multivol_offset;
                    cur = std::move(e2);
                }
                total_bytes += arch_done;
            } else {
                // Regular / multi-volume: first fragment has size=full; 'M' has
                // size=remaining and multivol_offset into the file.
                std::int64_t bytes_done = is_mvol ? e.multivol_offset : 0;
                if (is_mvol && e.multivol_offset > 0)
                    ::lseek(fd, static_cast<off_t>(e.multivol_offset), SEEK_SET);

                const std::int64_t start_off = bytes_done;
                Entry cur = e;
                for (;;) {
                    std::int64_t got = 0;
                    if (!reader.read_entry_data(cur, fd, {}, &got)) {
                        print(stderr, "mutar: {}: write error during extract\n", outpath);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    bytes_done += got;
                    if (got >= cur.asize)
                        break; // this fragment complete

                    // Short read (or header-only volume): need next multi-vol part
                    if (!cfg.multi_volume) {
                        print(stderr,
                              "mutar: {}: unexpected EOF in archive (got {} of {} bytes)\n",
                              outpath, got, cur.asize);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    if (!switch_extract_volume()) {
                        print(stderr,
                              "mutar: {}: multi-volume: need next volume "
                              "(have {} bytes so far)\n",
                              outpath, bytes_done);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    auto [e2, ok2, eof2] = reader.next_entry();
                    if (eof2 || !ok2) {
                        print(stderr,
                              "mutar: {}: multi-volume: missing continuation header\n",
                              outpath);
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    if (e2.typeflag != GNUTYPE_MULTIVOL) {
                        print(stderr,
                              "mutar: {}: multi-volume: expected type 'M' continuation, "
                              "got typeflag '{}'\n",
                              outpath, e2.typeflag ? e2.typeflag : '?');
                        exit_code = EXIT_FAILURE;
                        break;
                    }
                    if (e2.multivol_offset != bytes_done) {
                        ::lseek(fd, static_cast<off_t>(e2.multivol_offset), SEEK_SET);
                        bytes_done = e2.multivol_offset;
                    }
                    cur = std::move(e2);
                }
                if (bytes_done > start_off)
                    ::ftruncate(fd, static_cast<off_t>(bytes_done));
                total_bytes += (bytes_done - start_off);
            }
            set_running_total(total_bytes);
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
                if (::lchown(outpath.c_str(), uid_val, gid_val) < 0 &&
                    cfg.same_owner) {
                    print(stderr, "mutar: {}: Cannot change ownership to uid {}, gid {}: {}\n",
                          outpath, static_cast<unsigned>(uid_val),
                          static_cast<unsigned>(gid_val), std::strerror(errno));
                    exit_code = EXIT_FAILURE;
                }
            }

            // Extended attributes and POSIX ACLs (SCHILY.* from PAX header)
            restore_xattrs_acls(e, outpath, cfg);
            break;
        }
        }
    }

    // Report members requested but not found (especially with --preserve-order)
    if (!want.empty()) {
        if (cfg.preserve_order && !want_order.empty()) {
            for (std::size_t i = want_order_pos; i < want_order.size(); ++i) {
                print(stderr, "mutar: {}: Not found in archive\n", want_order[i]);
                exit_code = EXIT_FAILURE;
            }
        } else {
            // Non-order mode: we do not track found-set today; skip bulk notfound
            // to avoid false positives for directory recursion / wildcards.
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
    // running total already updated per member when signal armed

    if (g_index_fp) { std::fclose(g_index_fp); g_index_fp = nullptr; }

    if (reader.failed())
        exit_code = EXIT_FAILURE;
    s.close();
    if (s.child_failed()) {
        print(stderr, "mutar: {}\n", s.child_error());
        exit_code = EXIT_FAILURE;
    }

    // materialize_guard destructor unlinks temp if armed
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
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    reader.set_warn_config(&cfg);
    if (cfg.pax_option_rules.any())
        reader.set_pax_rules(&cfg.pax_option_rules);

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
        if (!reader.skip_entry(e)) {
            exit_code = EXIT_FAILURE;
            break;
        }
    }
    if (reader.failed())
        exit_code = EXIT_FAILURE;
    s.close();
    if (s.child_failed()) {
        print(stderr, "mutar: {}\n", s.child_error());
        exit_code = EXIT_FAILURE;
    }
    return exit_code;
}

// ── append (-r) ───────────────────────────────────────────────────────────────

static int op_append(const Config& cfg) {
    // Find end of archive, then write new entries
    // The archive must be seekable (no compression); remote uses rmt L.
    if (refuse_compressed_update(cfg))
        return EXIT_FAILURE;
    auto res = ArchiveStream::open_rdwr(cfg);
    if (!res) {
        print(stderr, "mutar: {}\n", res.error().message);
        return EXIT_FAILURE;
    }
    ArchiveStream& s = *res;
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    reader.set_warn_config(&cfg);
    if (cfg.pax_option_rules.any())
        reader.set_pax_rules(&cfg.pax_option_rules);

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
    if (s.seek(write_pos) < 0) {
        print(stderr, "mutar: cannot seek to end of archive for append\n");
        return EXIT_FAILURE;
    }

    Format fmt = cfg.fmt;
    if (fmt == Format::Default) fmt = Format::GNU;
    ArchiveWriter writer(s, cfg.blocking_factor, fmt);

    // -C applies to member paths after the archive is open
    if (!cfg.directory.empty()) {
        if (::chdir(cfg.directory.c_str()) < 0) {
            print(stderr, "mutar: -C {}: {}\n", cfg.directory, std::strerror(errno));
            return EXIT_FAILURE;
        }
    }

    int exit_code = EXIT_SUCCESS;
    for (const auto& f : cfg.files) {
        if (cfg.verbose) print("{}\n", f);
        if (!writer.add_path(f, f, cfg) && !cfg.ignore_failed_read)
            exit_code = EXIT_FAILURE;
    }
    writer.finish();
    s.close();
    return exit_code;
}

// ── update (-u): like append but only for newer files ─────────────────────────

static int op_update(const Config& cfg) {
    if (refuse_compressed_update(cfg))
        return EXIT_FAILURE;
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
        ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    reader.set_warn_config(&cfg);
        if (cfg.pax_option_rules.any())
            reader.set_pax_rules(&cfg.pax_option_rules);
        for (;;) {
            auto [e, ok, eof] = reader.next_entry();
            if (eof || !ok) break;
            archive_mtimes[e.name] = e.mtime;
            reader.skip_entry(e);
        }
    }

    // Build list of files newer than archived version (-C prefixes local paths)
    std::vector<std::string> to_append;
    for (const auto& f : cfg.files) {
        std::string fspath = f;
        if (!cfg.directory.empty() && fspath[0] != '/')
            fspath = cfg.directory + "/" + f;
        struct stat st{};
        if (::stat(fspath.c_str(), &st) < 0) continue;
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
    if (refuse_compressed_update(cfg))
        return EXIT_FAILURE;
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
        ArchiveReader reader(src, cfg.blocking_factor, false, cfg.read_full_records);
    reader.set_warn_config(&cfg);
        if (cfg.pax_option_rules.any())
            reader.set_pax_rules(&cfg.pax_option_rules);

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

            // Re-emit via ArchiveWriter so extensions are regenerated.
            // Stream payload — never reserve(e.size) (INT64_MAX / 1TiB OOM).
            writer.write_header_only(e, cfg);
            if (e.asize > 0) {
                std::int64_t got = 0;
                if (!reader.read_entry_data(e, -1,
                        [&](const char* p, std::size_t n) {
                            writer.write_data_bytes(p, n);
                        }, &got) || reader.failed()) {
                    print(stderr, "mutar: error reading member '{}'\n", e.name);
                    ::unlink(tmpfile.c_str());
                    return EXIT_FAILURE;
                }
                if (got < e.asize) {
                    print(stderr, "mutar: unexpected EOF in archive\n");
                    ::unlink(tmpfile.c_str());
                    return EXIT_FAILURE;
                }
            } else if (!reader.skip_entry(e)) {
                ::unlink(tmpfile.c_str());
                return EXIT_FAILURE;
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
    ArchiveReader reader(s, cfg.blocking_factor, cfg.ignore_zeros, cfg.read_full_records);
    reader.set_warn_config(&cfg);
    if (cfg.pax_option_rules.any())
        reader.set_pax_rules(&cfg.pax_option_rules);

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
        ArchiveReader dummy(dst, cfg.blocking_factor, false, cfg.read_full_records);
    dummy.set_warn_config(&cfg);
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
    OPT_SHOW_SNAPSHOT_FIELD_RANGES,
    OPT_PRESERVE,
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
    {"preserve",         no_argument,       nullptr, OPT_PRESERVE},
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
    {"show-snapshot-field-ranges", no_argument, nullptr, OPT_SHOW_SNAPSHOT_FIELD_RANGES},
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
    {"exclude-ignore",   required_argument, nullptr, OPT_EXCLUDE_IGNORE},
    {"exclude-ignore-recursive",required_argument,nullptr,OPT_EXCLUDE_IGNORE_RECURSIVE},
    {"quote-chars",      required_argument, nullptr, OPT_QUOTE_CHARS},
    {"no-quote-chars",   required_argument, nullptr, OPT_NO_QUOTE_CHARS},
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

static bool short_opt_takes_arg(char c) noexcept {
    return std::strchr("fCgbLFVHTNKXI", c) != nullptr;
}

static bool long_opt_takes_separate_arg(std::string_view name) noexcept {
    for (const option* o = long_opts; o->name; ++o) {
        if (name == o->name)
            return o->has_arg == required_argument;
    }
    return false;
}

static std::string join_chdir_path(const std::string& cur, const std::string& next) {
    if (next.empty())
        return cur;
    if (next[0] == '/')
        return next;
    if (cur.empty())
        return next;
    if (cur.back() == '/')
        return cur + next;
    return cur + "/" + next;
}

/// Walk original argv (pre-getopt permutation) so each -C applies to
/// following member names. Fills cfg.files and parallel cfg.file_chdir.
static void collect_names_in_order(const std::vector<std::string>& args, Config& cfg) {
    std::string cur_dir;
    auto add_name = [&](std::string name) {
        if (cfg.unquote)
            name = unquote_name(name);
        if (name.empty())
            return;
        cfg.files.push_back(std::move(name));
        cfg.file_chdir.push_back(cur_dir);
    };
    auto apply_dir = [&](const std::string& d) {
        cur_dir = join_chdir_path(cur_dir, d);
        cfg.directory = cur_dir;
    };

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") {
            for (++i; i < args.size(); ++i)
                add_name(args[i]);
            break;
        }
        if (a == "-C" || a == "--directory") {
            if (i + 1 < args.size())
                apply_dir(args[++i]);
            continue;
        }
        if (a.starts_with("--directory=")) {
            apply_dir(a.substr(12));
            continue;
        }
        if (a == "--add-file") {
            if (i + 1 < args.size())
                add_name(args[++i]);
            continue;
        }
        if (a.starts_with("--add-file=")) {
            add_name(a.substr(11));
            continue;
        }
        if (a.starts_with("--") && a.size() > 2) {
            const auto eq = a.find('=');
            const std::string_view name = std::string_view(a).substr(
                2, eq == std::string::npos ? std::string_view::npos : eq - 2);
            if (eq == std::string::npos && long_opt_takes_separate_arg(name) &&
                i + 1 < args.size())
                ++i;
            continue;
        }
        if (a.size() >= 2 && a[0] == '-' && a[1] != '-') {
            for (std::size_t k = 1; k < a.size(); ++k) {
                const char c = a[k];
                if (c == 'C') {
                    if (k + 1 < a.size())
                        apply_dir(a.substr(k + 1));
                    else if (i + 1 < args.size())
                        apply_dir(args[++i]);
                    break;
                }
                if (short_opt_takes_arg(c)) {
                    if (k + 1 < a.size())
                        break; // attached argument
                    if (i + 1 < args.size())
                        ++i;
                    break;
                }
            }
            continue;
        }
        add_name(a);
    }
}

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

    // Snapshot argv before getopt permutes options ahead of operands.
    std::vector<std::string> argv_copy;
    argv_copy.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
        argv_copy.emplace_back(argv[i] ? argv[i] : "");

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

        case 'f':
            if (!::optarg || ::optarg[0] == '\0') {
                print(stderr, "mutar: empty archive name\n");
                std::exit(EXIT_FAILURE);
            }
            cfg.archive_file = ::optarg;
            break;
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
            break;
        case 'm': cfg.touch             = true; break;
        case 'S': cfg.sparse            = true; break;
        case 'G': cfg.incremental       = true; break;
        case 'g': cfg.listed_incremental = ::optarg; cfg.incremental = true; break;
        case 'b': {
            char* end = nullptr; errno = 0;
            long val = std::strtol(::optarg, &end, 10);
            if (errno != 0 || end == ::optarg || *end != '\0' || val <= 0 ||
                val > MAX_BLOCKING_FACTOR) {
                print(stderr, "mutar: invalid blocking factor '{}': must be 1..{}\n",
                      ::optarg, MAX_BLOCKING_FACTOR);
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
        case 'N':
            cfg.newer_than = ::optarg;
            cfg.newer_use_ctime = true; // GNU --newer / -N / --after-date → ctime
            break;
        case 'w': cfg.interactive       = true; break;
        case 'W': cfg.verify            = true; break;
        case 'U': cfg.unlink_first      = true; break;
        case '0':
            cfg.null_terminated = true;
            cfg.verbatim_files_from = true; // GNU: --null implies verbatim
            break;

        case OPT_OLD_ARCHIVE:        cfg.fmt = Format::V7; break;
        case OPT_POSIX:              cfg.fmt = Format::PAX; cfg.posix = true; break;
        case OPT_OWNER: {
            IdSpec o = parse_id_spec(::optarg ? ::optarg : "", false);
            if (!o.ok) {
                print(stderr, "mutar: invalid owner '{}'\n", ::optarg ? ::optarg : "");
                std::exit(EXIT_FAILURE);
            }
            cfg.owner = ::optarg;
            break;
        }
        case OPT_GROUP: {
            IdSpec g = parse_id_spec(::optarg ? ::optarg : "", true);
            if (!g.ok) {
                print(stderr, "mutar: invalid group '{}'\n", ::optarg ? ::optarg : "");
                std::exit(EXIT_FAILURE);
            }
            cfg.group = ::optarg;
            break;
        }
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
            long long val = std::strtoll(::optarg, &end, 10);
            if (errno != 0 || end == ::optarg || *end != '\0' || val < 0 ||
                val > static_cast<long long>(INT_MAX)) {
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
            if (errno || endp == ::optarg || *endp != '\0' || sz <= 0 ||
                sz % static_cast<long long>(BLOCKSIZE) != 0) {
                print(stderr, "mutar: invalid record-size '{}': must be a positive multiple of 512\n", ::optarg);
                std::exit(EXIT_FAILURE);
            }
            if (sz > static_cast<long long>(MAX_RECORD_SIZE)) {
                print(stderr,
                      "mutar: record-size {} is too large (maximum {}): memory exhausted\n",
                      sz, MAX_RECORD_SIZE);
                std::exit(EXIT_FAILURE);
            }
            cfg.blocking_factor  = static_cast<int>(sz / static_cast<long long>(BLOCKSIZE));
            cfg.record_size_str  = ::optarg;
            break;
        }
        case OPT_EXCLUDE:            cfg.exclude_patterns.emplace_back(::optarg); break;
        case OPT_DELAY_DIR_RESTORE:  cfg.delay_dir_restore = true; break;
        case OPT_TOTALS: {
            cfg.totals = true;
            if (::optarg && ::optarg[0]) {
                int sig = parse_totals_signal(::optarg);
                if (sig < 0) {
                    print(stderr,
                          "mutar: invalid signal name '{}' for --totals\n"
                          "Valid signals: HUP QUIT INT USR1 USR2 (optional SIG prefix)\n",
                          ::optarg);
                    std::exit(EXIT_FAILURE);
                }
                cfg.totals_signal = sig;
            }
            break;
        }
        case OPT_UTC:                cfg.utc = true; break;
        case OPT_FULL_TIME:          cfg.full_time = true; break;
        case OPT_SHOW_OMITTED_DIRS:  cfg.show_omitted_dirs = true; break;
        case OPT_SHOW_TRANSFORMED:   cfg.show_transformed = true; break;
        case OPT_SHOW_SNAPSHOT_FIELD_RANGES:
            cfg.show_snapshot_field_ranges = true;
            break;
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
        case OPT_ATIME_PRESERVE: {
            cfg.atime_preserve = true;
            if (::optarg && ::optarg[0]) {
                std::string_view m(::optarg);
                if (m == "replace" || m == "system") {
                    cfg.atime_preserve_method = std::string(m);
                } else {
                    print(stderr,
                          "mutar: invalid --atime-preserve method '{}': "
                          "must be 'replace' or 'system'\n", ::optarg);
                    std::exit(EXIT_FAILURE);
                }
            } else {
                cfg.atime_preserve_method = "replace";
            }
            break;
        }
        case OPT_SPARSE_VERSION: {
            unsigned maj = 0, min = 0;
            if (!parse_sparse_version(::optarg, maj, min)) {
                print(stderr,
                      "mutar: invalid sparse version '{}': "
                      "supported versions are 0.0, 0.1, 1.0\n",
                      ::optarg ? ::optarg : "");
                std::exit(EXIT_FAILURE);
            }
            cfg.sparse_version = ::optarg;
            cfg.sparse_major = maj;
            cfg.sparse_minor = min;
            cfg.sparse = true; // implies --sparse
            break;
        }
        case OPT_ONE_FILE_SYSTEM:    cfg.one_file_system = true; break;
        case OPT_TO_COMMAND:         cfg.to_command = ::optarg; break;
        case OPT_NEWER_MTIME:
            cfg.newer_than = ::optarg;
            cfg.newer_use_ctime = false; // --newer-mtime → mtime only
            break;
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
            // GNU: delete=, exthdr.*, globexthdr.*, keyword=/:= overrides.
            // Bare keyword (no '=') → error. Protected keywords rejected.
            if (!::optarg) break;
            cfg.pax_options.emplace_back(::optarg);
            parse_pax_option_string(cfg.pax_option_rules, ::optarg);
            break;
        }

        case OPT_CHECK_DEVICE:
            cfg.check_device = true;
            break;
        case OPT_NO_CHECK_DEVICE:
            cfg.check_device = false;
            break;

        case OPT_IGNORE_CMD_ERR:
            cfg.ignore_command_error = true;
            break;
        case OPT_NO_IGNORE_CMD_ERR:
            cfg.ignore_command_error = false;
            break;
        case OPT_PRESERVE:
            // GNU: --preserve = -p + -s
            cfg.same_permissions = true;
            cfg.preserve_order = true;
            break;
        case OPT_QUOTE_CHARS:
            if (::optarg)
                cfg.quote_chars += ::optarg;
            break;
        case OPT_NO_QUOTE_CHARS:
            if (::optarg)
                cfg.no_quote_chars += ::optarg;
            break;

        // Accepted but no-op for now (complex features not yet implemented)
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

        case OPT_NULL:
            cfg.null_terminated = true;
            cfg.verbatim_files_from = true; // GNU: --null implies verbatim
            break;
        case OPT_NO_NULL:
            cfg.null_terminated = false;
            break;

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
        case OPT_EXCLUDE_IGNORE:
            cfg.exclude_ignore.emplace_back(::optarg);
            break;
        case OPT_EXCLUDE_IGNORE_RECURSIVE:
            cfg.exclude_ignore_recursive.emplace_back(::optarg);
            break;
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
        case OPT_QUOTING_STYLE: {
            std::string_view st(::optarg ? ::optarg : "");
            static const char* valid[] = {
                "literal", "shell", "shell-always", "shell-escape",
                "shell-escape-always", "c", "c-maybe", "escape",
                "locale", "clocale", nullptr
            };
            bool ok = false;
            for (const char** p = valid; *p; ++p) {
                if (st == *p) { ok = true; break; }
            }
            if (!ok) {
                print(stderr,
                      "mutar: invalid quoting style '{}'\n"
                      "Valid styles: literal shell shell-always shell-escape "
                      "shell-escape-always c c-maybe escape locale clocale\n",
                      ::optarg ? ::optarg : "");
                std::exit(EXIT_FAILURE);
            }
            cfg.quoting_style = ::optarg;
            break;
        }
        case 'o':
            // GNU: create → --old-archive (V7); extract → --no-same-owner
            cfg.compat_o = true;
            cfg.fmt = Format::V7; // encode_header reads cfg.fmt on create
            break;

        case '?':
        default:
            print(stderr, "Try 'mutar --help' for more information.\n");
            std::exit(EXIT_FAILURE);
        }
    }

    // Rebuild member list in original order so each -C applies to following names.
    cfg.files.clear();
    cfg.file_chdir.clear();
    collect_names_in_order(argv_copy, cfg);

    // Read files from -T / --files-from
    // GNU: --null implies verbatim; --verbatim-files-from disables option/unquote
    // handling of lines; default (--no-verbatim) treats leading '-' as options.
    // "-" means stdin (ifstream("-") would open a file named "-").
    for (const auto& fname : cfg.files_from) {
        std::ifstream ifs;
        std::istream* in = nullptr;
        if (fname == "-") {
            in = &std::cin;
        } else {
            ifs.open(fname);
            if (!ifs) { print(stderr, "mutar: {}: cannot open\n", fname); continue; }
            in = &ifs;
        }
        if (cfg.null_terminated) {
            // --null: NUL-separated, always verbatim (no option parse / no unquote)
            std::string content((std::istreambuf_iterator<char>(*in)),
                                 std::istreambuf_iterator<char>());
            std::size_t start = 0;
            while (start < content.size()) {
                auto end = content.find('\0', start);
                if (end == std::string::npos) end = content.size();
                std::string item = content.substr(start, end - start);
                if (!item.empty()) {
                    cfg.files.push_back(item);
                    cfg.file_chdir.push_back(cfg.directory);
                }
                if (end == content.size()) break;
                start = end + 1;
            }
        } else {
            std::string line;
            int lineno = 0;
            while (std::getline(*in, line)) {
                ++lineno;
                if (line.empty())
                    continue;
                if (cfg.verbatim_files_from) {
                    // Verbatim: filename as-is (no option parse, no unquote)
                    cfg.files.push_back(line);
                    cfg.file_chdir.push_back(cfg.directory);
                    continue;
                }
                // Non-verbatim: strip whitespace; leading '-' is an option
                auto l = line.find_first_not_of(" \t\r");
                if (l == std::string::npos)
                    continue;
                auto r = line.find_last_not_of(" \t\r");
                line = line.substr(l, r - l + 1);
                if (line.empty())
                    continue;
                if (line[0] == '-') {
                    if (line == "--null") {
                        cfg.null_terminated = true;
                        cfg.verbatim_files_from = true;
                        continue;
                    }
                    if (line == "--no-null") {
                        cfg.null_terminated = false;
                        continue;
                    }
                    if (line == "--verbatim-files-from") {
                        cfg.verbatim_files_from = true;
                        continue;
                    }
                    if (line == "--no-verbatim-files-from") {
                        cfg.verbatim_files_from = false;
                        continue;
                    }
                    if (line == "--unquote") {
                        cfg.unquote = true;
                        continue;
                    }
                    if (line == "--no-unquote") {
                        cfg.unquote = false;
                        continue;
                    }
                    if (line.starts_with("--exclude=")) {
                        cfg.exclude_patterns.emplace_back(line.substr(10));
                        continue;
                    }
                    if (line.starts_with("--add-file=")) {
                        std::string n = line.substr(11);
                        if (cfg.unquote) n = unquote_name(n);
                        if (!n.empty()) {
                            cfg.files.push_back(std::move(n));
                            cfg.file_chdir.push_back(cfg.directory);
                        }
                        continue;
                    }
                    print(stderr, "mutar: {}:{}: unrecognized option\n",
                          fname, lineno);
                    std::exit(EXIT_FAILURE);
                }
                if (cfg.unquote)
                    line = unquote_name(line);
                if (!line.empty()) {
                    cfg.files.push_back(line);
                    cfg.file_chdir.push_back(cfg.directory);
                }
            }
        }
    }

    // Process --exclude-from / -X files ("-" = stdin)
    for (const auto& fname : cfg.exclude_from) {
        std::ifstream ifs;
        std::istream* in = nullptr;
        if (fname == "-") {
            in = &std::cin;
        } else {
            ifs.open(fname);
            if (!ifs) { print(stderr, "mutar: {}: cannot open\n", fname); continue; }
            in = &ifs;
        }
        std::string line;
        while (std::getline(*in, line)) {
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
        "  -g, --listed-incremental=FILE   Handle listed-incremental backup (MUTAR_SNAPSHOT_V2;\n"
        "                                  also reads GNU tar snapshot format 2 best-effort)\n"
        "                                  Snapshot records files+dirs (mtime+dev); level>=1 skips\n"
        "                                  unchanged files/symlinks/specials (mtime+dev when\n"
        "                                  --check-device); directories always dumped\n"
        "  -G, --incremental               Handle old GNU-format incremental backup (dumpdir type D)\n"
        "      --hole-detection=METHOD     Use METHOD to detect holes in sparse files (seek/raw)\n"
        "      --ignore-failed-read        Do not exit with nonzero on unreadable files\n"
        "      --level=NUMBER              Dump level for created listed-incremental archive\n"
        "  -n, --seek                      Assume archive is seekable (default: auto-detect)\n"
        "      --no-check-device           Ignore device field in listed-incremental snapshot\n"
        "      --no-seek                   Do not seek in archive (disable index seek paths)\n"
        "      --occurrence[=NUMBER]       Process only the NUMBERth occurrence of each file in the archive\n"
        "      --sparse-version=MAJOR[.MINOR]  Set sparse format version (0.0, 0.1, 1.0); implies -S\n"
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
        "                                  METHOD=replace (default) or system (O_NOATIME)\n"
        "      --clamp-mtime               Only set time when the file is more recent than what was given with --mtime\n"
        "      --delay-directory-restore   Delay setting mtime/mode of extracted directories until end\n"
        "      --group=NAME[:GID]          Force NAME as group for added files (+GID or NAME:GID)\n"
        "      --group-map=FILE            Use FILE to map file owner GIDs\n"
        "      --mode=CHANGES              Force mode CHANGES for added files (octal or symbolic u+x,go-w)\n"
        "      --mtime=DATE-OR-FILE        Set mtime from DATE, @SECONDS (UTC epoch), or FILE\n"
        "      --no-delay-directory-restore  Apply directory mtime/mode immediately (default)\n"
        "      --no-same-owner             Extract files as yourself (default for ordinary users)\n"
        "      --no-same-permissions       Apply the user's umask when extracting permissions\n"
        "      --numeric-owner             Always use numbers for user/group names\n"
        "      --owner=NAME[:UID]          Force NAME as owner (+UID or NAME:UID like GNU)\n"
        "      --owner-map=FILE            Use FILE to map file owner UIDs\n"
        "  -m, --touch                    Don't extract file modified time\n"
        "  -p, --preserve-permissions, --same-permissions  Extract information about file permissions\n"
        "      --preserve                  Same as both -p and -s\n"
        "  -s, --preserve-order, --same-order  Member arguments listed in the same order as the archive\n"
        "      --same-owner                Try extracting files with the same ownership as exists in the archive\n"
        "      --sort=ORDER                Directory sorting order: none (default), name, or inode\n",
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
        "                                  between-member rotation and mid-file split (type M)\n"
        "  -M, --multi-volume              Create/list/extract multi-volume archive\n"
        "      --rmt-command=COMMAND       Use given rmt COMMAND instead of rmt\n"
        "                                  (O/R/W/L/C; L=lseek enables remote -r/-u)\n"
        "      --rsh-command=COMMAND       Use remote COMMAND instead of rsh\n"
        "      --volno-file=F              Read/write current volume number in F (atomic)\n"
        "\nDevice blocking:\n"
        "  -b, --blocking-factor=BLOCKS    BLOCKS x 512 bytes per record\n"
        "  -B, --read-full-records         Reblock short reads to full records (4.2BSD pipes)\n"
        "  -i, --ignore-zeros              Ignore zeroed blocks in archive (means EOF)\n"
        "      --record-size=NUMBER        NUMBER of bytes per record, multiple of 512\n"
        "                                  (maximum 16776704 = 32767 blocks)\n"
        "\nArchive format selection:\n"
        "      --format=FORMAT, -H FORMAT  Create archive of the given format (v7 oldgnu gnu ustar pax)\n"
        "      --old-archive, --portability  Same as --format=v7\n"
        "      --pax-option=keyword[[:]=value][,...]  Control PAX keywords (delete=, exthdr.name,\n"
        "                                  globexthdr.name, exthdr.mtime, keyword=/:= overrides)\n"
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
        "  -C, --directory=DIR             Change to DIR; each -C applies to following names\n"
        "      --exclude=PATTERN           Exclude files matching PATTERN\n"
        "      --exclude-backups           Exclude backup and lock files\n"
        "      --exclude-caches            Exclude contents of directories containing CACHEDIR.TAG\n"
        "      --exclude-caches-all        Exclude directories containing CACHEDIR.TAG\n"
        "      --exclude-caches-under      Exclude everything under directories containing CACHEDIR.TAG\n"
        "      --exclude-ignore=FILE       If FILE exists in a directory, read patterns\n"
        "                                  and apply them to that directory's children only\n"
        "      --exclude-ignore-recursive=FILE  Like --exclude-ignore, but patterns apply\n"
        "                                  to the whole subtree under that directory\n"
        "      --exclude-tag=FILE          Exclude contents of directories containing FILE\n"
        "      --exclude-tag-all=FILE      Exclude directories containing FILE\n"
        "      --exclude-tag-under=FILE    Exclude everything under directories containing FILE\n"
        "      --exclude-vcs               Exclude version control system directories\n"
        "      --exclude-vcs-ignores       Read exclude patterns from the VCS ignore files\n"
        "      --anchored                  Patterns match file name start\n"
        "      --no-anchored               Patterns match after any '/' (match basename)\n"
        "  -h, --dereference              Follow symlinks; archive and dump the files they point to\n"
        "      --hard-dereference         Follow hard links; archive and dump the files they refer to\n"
        "      --ignore-case              Ignore case when matching patterns\n"
        "      --no-ignore-case           Case-sensitive pattern matching (default)\n"
        "      --no-null                   Disable the effect of the previous --null option\n"
        "      --no-recursion              Avoid descending automatically in directories\n"
        "      --no-unquote                Do not unquote input file or member names\n"
        "      --no-verbatim-files-from    -T treats file names beginning with dash as options (default)\n"
        "      --no-wildcards              Verbatim string matching\n"
        "      --no-wildcards-match-slash  Wildcard matches '/' is not allowed\n"
        "  -N, --newer=DATE-OR-FILE, --after-date=DATE-OR-FILE  Only store files with ctime newer than DATE\n"
        "      --newer-mtime=DATE          Only store files with mtime newer than DATE\n"
        "      --null                      -T reads null-terminated names; implies --verbatim-files-from\n"
        "  -l, --check-links              Print a message if not all hard links are dumped\n"
        "      --one-file-system           Stay in local file system when creating archive\n"
        "  -P, --absolute-names            Don't strip leading '/'s from file names\n"
        "      --recursion                 Recurse into directories (default)\n"
        "      --suffix=STRING             Backup before removal, override usual suffix ('~')\n"
        "  -T, --files-from=FILE           Get names to extract or create from FILE (- = stdin)\n"
        "      --unquote                   Unquote input file or member names (default)\n"
        "      --verbatim-files-from       -T reads file names verbatim (no option/unquote handling)\n"
        "      --wildcards                 Use wildcards (default)\n"
        "      --wildcards-match-slash      Wildcards match '/' when on by default\n"
        "  -X, --exclude-from=FILE         Exclude patterns listed in FILE (- = stdin)\n"
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
        "                                  Selective extract of compressed archives: full decompress once\n"
        "                                  (materialize-then-seek via index). Uncompressed seekable\n"
        "                                  archives never materialize (direct lseek). True frame-level\n"
        "                                  seek without materialize is future (liblzma/libzstd).\n"
        "      --no-quote-chars=STRING     Disable quoting for characters from STRING\n"
        "      --quote-chars=STRING        Additionally quote characters from STRING\n"
        "      --quoting-style=STYLE       Set name quoting for -t / verbose extract\n"
        "                                  (literal escape c c-maybe shell shell-always\n"
        "                                   shell-escape shell-escape-always locale clocale)\n"
        "  -R, --block-number              Show block number within archive with each message\n"
        "      --restrict                  Forbid -P/--absolute-names, --to-command, multi-volume\n"
        "      --show-defaults             Show tar defaults\n"
        "      --show-omitted-dirs         When listing or extracting, list each directory that does not match search criteria\n"
        "      --show-snapshot-field-ranges  Show valid ranges for snapshot-file fields\n"
        "      --show-stored-names         Same as --show-transformed-names\n"
        "      --show-transformed-names    Show file or archive names after transformation\n"
        "      --totals[=SIGNAL]           Print total bytes after processing the archive;\n"
        "                                  with SIGNAL (HUP/QUIT/INT/USR1/USR2) print on signal too\n"
        "      --usage                     Give a short usage message\n"
        "      --utc                       Print file modification times in UTC\n"
        "  -v, --verbose                   Verbosely list files processed\n"
        "      --version                   Print program version and licensing information and exit\n"
        "  -w, --interactive, --confirmation  Ask for confirmation for every action\n"
        "      --warning=KEYWORD           Warning control\n"
        "  -K, --starting-file=MEMBER-NAME  Begin at member MEMBER-NAME when reading the archive\n"
        "\nCompatibility options:\n"
        "  -o                             Create: same as --old-archive (v7); extract: --no-same-owner\n"
        "\nFormats: v7, oldgnu, gnu (default), ustar, pax/posix\n");
}

static void print_version() {
    print("mutar (µtar) 0.3.0 — C++23 GNU tar-compatible archiver\n"
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

    // Handle --help / --version / --show-snapshot-field-ranges early
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-?" ) { print_usage(argv[0]);  return EXIT_SUCCESS; }
        if (arg == "--version")              { print_version();        return EXIT_SUCCESS; }
        if (arg == "--show-snapshot-field-ranges") {
            print_snapshot_field_ranges();
            return EXIT_SUCCESS;
        }
    }

    // GNU tar ignores SIGPIPE so --to-command that closes stdin early
    // (true, head, …) does not kill the process with rc=-13 / 141.
    ::signal(SIGPIPE, SIG_IGN);

    Config cfg = parse_args(argc, argv);

    // --show-snapshot-field-ranges may also appear after other opts via getopt
    if (cfg.show_snapshot_field_ranges) {
        print_snapshot_field_ranges();
        return EXIT_SUCCESS;
    }

    if (cfg.op == Operation::None) {
        print(stderr, "mutar: You must specify one of the '-Acdtrux', "
                           "'--delete' or '--test-label' options\n");
        return EXIT_FAILURE;
    }

    // --restrict: reject dangerous option combinations before any I/O
    if (!enforce_restrict(cfg))
        return EXIT_FAILURE;

    // --totals=SIGNAL: install handler for running totals dump
    if (cfg.totals && cfg.totals_signal > 0)
        install_totals_signal(cfg.totals_signal);

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
