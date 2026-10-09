// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "bytes.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace xisfconv {

namespace {

// "123", "64K", "8M", "1G" (and k, m, g): a number of bytes. False for anything else.
bool parseByteCount(const char* text, uint64_t& out) {
    if (!text || !*text) return false;
    std::string s = trim(text);
    uint64_t factor = 1;
    if (!s.empty()) {
        const char unit = static_cast<char>(std::toupper(static_cast<unsigned char>(s.back())));
        if (unit == 'K' || unit == 'M' || unit == 'G') {
            factor = unit == 'K' ? 1024 : unit == 'M' ? 1024 * 1024 : 1024 * 1024 * 1024;
            s.pop_back();
        }
    }
    uint64_t n = 0;
    if (!parseUInt64(trim(s), n)) return false;
    if (n > std::numeric_limits<uint64_t>::max() / factor) return false;
    out = n * factor;
    return true;
}

thread_local std::string t_tempDirectory;

std::atomic<uint64_t> g_heldInMemory{0};   // what the stores in memory take, together

std::string systemError() {
    return std::strerror(errno);
}

}  // namespace

const PieceSettings& pieceSettings() {
    static const PieceSettings settings = [] {
        PieceSettings s;
        uint64_t n = 0;
        if (parseByteCount(std::getenv("XISFCONV_PIECE_BYTES"), n)) s.pieceBytes = std::max<uint64_t>(n, 1);
        if (parseByteCount(std::getenv("XISFCONV_MEMORY_LIMIT"), n)) s.memoryLimit = n;
        return s;
    }();
    return settings;
}

TempDirectoryScope::TempDirectoryScope(std::string directory) : previous_(t_tempDirectory) {
    t_tempDirectory = std::move(directory);
}
TempDirectoryScope::~TempDirectoryScope() { t_tempDirectory = previous_; }

// ------------------------------------------------------------------------------------- RawFile

std::shared_ptr<RawFile> RawFile::openForReading(const std::string& path) {
    std::shared_ptr<RawFile> file(new RawFile());
    file->path_ = path;
#ifdef _WIN32
    if (_wsopen_s(&file->fd_, toPath(path).c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, 0) != 0) file->fd_ = -1;
#else
    file->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (file->fd_ < 0) failToOpen(path);
    return file;
}

std::unique_ptr<RawFile> RawFile::temporary() {
    std::string dir = t_tempDirectory;
    if (dir.empty()) {
        std::error_code ec;
        const fs::path system = fs::temp_directory_path(ec);
        dir = ec ? std::string(".") : fromPath(system);
    }
    std::unique_ptr<RawFile> file(new RawFile());
    file->temporary_ = true;
#ifdef _WIN32
    static std::atomic<uint64_t> counter{0};
    std::random_device random;
    for (int attempt = 0; attempt < 100 && file->fd_ < 0; ++attempt) {
        const uint64_t n = (static_cast<uint64_t>(random()) << 32) ^ random() ^ counter++;
        char name[48];
        std::snprintf(name, sizeof name, ".xisfconv-%016llx.tmp", static_cast<unsigned long long>(n));
        const fs::path path = toPath(dir) / name;
        // (removed by the system when it is closed, also when the program ends without closing it)
        if (_wsopen_s(&file->fd_, path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY | _O_NOINHERIT,
                      _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0) {
            file->fd_ = -1;
            if (errno != EEXIST) break;
        } else {
            file->path_ = fromPath(path);
        }
    }
#else
    std::string pattern = fromPath(toPath(dir) / ".xisfconv-XXXXXX");
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    file->fd_ = ::mkstemp(name.data());
    if (file->fd_ >= 0) {
        ::fcntl(file->fd_, F_SETFD, FD_CLOEXEC);
        ::unlink(name.data());   // no name from now on: the file is gone when it is closed, whatever happens
        file->path_ = name.data();
    }
#endif
    if (file->fd_ < 0) {
        throw Error("cannot create a temporary file in " + dir + " (" + systemError() + "); the image does not fit the memory "
                    "a conversion takes (XISFCONV_MEMORY_LIMIT) and has to be kept there while it is converted",
                    ErrorKind::Io);
    }
    return file;
}

RawFile::~RawFile() {
    if (fd_ < 0) return;
#ifdef _WIN32
    _close(fd_);
#else
    ::close(fd_);
#endif
}

uint64_t RawFile::size() const {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd_, &st) != 0) throw Error("cannot read " + path_, ErrorKind::Io);
#else
    struct stat st;
    if (::fstat(fd_, &st) != 0) throw Error("cannot read " + path_, ErrorKind::Io);
#endif
    return static_cast<uint64_t>(st.st_size);
}

void RawFile::read(uint64_t position, void* out, size_t n, const char* what) const {
    uint8_t* p = static_cast<uint8_t*>(out);
    while (n > 0) {
        const size_t want = std::min<size_t>(n, size_t(1) << 30);
#ifdef _WIN32
        if (_lseeki64(fd_, static_cast<__int64>(position), SEEK_SET) < 0) throw Error(std::string("read error in ") + what, ErrorKind::Io);
        const int got = _read(fd_, p, static_cast<unsigned>(want));
#else
        const ssize_t got = ::pread(fd_, p, want, static_cast<off_t>(position));
        if (got < 0 && errno == EINTR) continue;
#endif
        if (got <= 0) throw Error(std::string("read error in ") + what, ErrorKind::Io);
        p += got;
        n -= static_cast<size_t>(got);
        position += static_cast<uint64_t>(got);
    }
}

void RawFile::write(uint64_t position, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const size_t want = std::min<size_t>(n, size_t(1) << 30);
#ifdef _WIN32
        if (_lseeki64(fd_, static_cast<__int64>(position), SEEK_SET) < 0) throw Error("cannot write to " + path_, ErrorKind::Io);
        const int put = _write(fd_, p, static_cast<unsigned>(want));
#else
        const ssize_t put = ::pwrite(fd_, p, want, static_cast<off_t>(position));
        if (put < 0 && errno == EINTR) continue;
#endif
        if (put <= 0) {
            const std::string why = put < 0 ? systemError() : std::string("nothing written");
            throw Error(std::string(temporary_ ? "cannot write the temporary file " : "cannot write ") + path_ + " (" + why + ")",
                        ErrorKind::Io);
        }
        p += put;
        n -= static_cast<size_t>(put);
        position += static_cast<uint64_t>(put);
    }
}

// --------------------------------------------------------------------------------- RandomBytes

void MemoryBytes::read(uint64_t position, size_t n, uint8_t* out) {
    if (position > size_ || n > size_ - position) throw Error("read beyond the end of the data (internal error)");
    if (n) std::memcpy(out, data_ + position, n);
}

void FileBytes::read(uint64_t position, size_t n, uint8_t* out) {
    if (position > size_ || n > size_ - position) throw Error("read beyond the end of the data (internal error)");
    try {
        file_->read(offset_ + position, out, n);
    } catch (const Error& e) {
        if (e.kind != ErrorKind::Io) throw;
        throw Error(failure_, ErrorKind::Io);
    }
}

// --------------------------------------------------------------------------------------- Store

Store::Store(uint64_t size) {
    makeRoom(size, true);
    if (file_) {
        if (size) {
            const uint8_t zero = 0;
            file_->write(size - 1, &zero, 1);
        }
    } else {
        memory_.assign(static_cast<size_t>(size), 0);
    }
    size_ = size;
}

Store::~Store() { g_heldInMemory -= counted_; }

// Room for `size` bytes: in memory as long as all stores together fit the limit, else the file.
// What is appended grows the memory by doubling it; where that does not fit, the store goes into
// the file. (Growing it a little at a time instead would copy all of it again and again.)
void Store::makeRoom(uint64_t size, bool exact) {
    if (file_ || size <= memory_.capacity()) return;
    const uint64_t limit = pieceSettings().memoryLimit;
    const uint64_t wanted = exact ? size : std::max<uint64_t>(size, static_cast<uint64_t>(memory_.capacity()) * 2);
    const uint64_t more = wanted - counted_;
    uint64_t held = g_heldInMemory.load();
    bool fits = false;
    // (while the bytes are copied to their new place, the old one is held too: that has to fit)
    while (held + wanted >= held && held + wanted <= limit && wanted <= std::numeric_limits<size_t>::max() / 2) {
        if (g_heldInMemory.compare_exchange_weak(held, held + more)) {
            fits = true;
            break;
        }
    }
    if (fits) {
        try {
            memory_.reserve(static_cast<size_t>(wanted));
            counted_ = wanted;
            return;
        } catch (const std::bad_alloc&) {
            g_heldInMemory -= more;   // (the system has less than the limit allows: the file)
        } catch (const std::length_error&) {
            g_heldInMemory -= more;
        }
    }
    // Into a file: what was held so far goes there first.
    file_ = RawFile::temporary();
    if (size_) file_->write(0, memory_.data(), static_cast<size_t>(size_));
    std::vector<uint8_t>().swap(memory_);
    g_heldInMemory -= counted_;
    counted_ = 0;
}

bool Store::fitsInMemory(uint64_t size) {
    const uint64_t held = g_heldInMemory.load();
    return held + size >= held && held + size <= pieceSettings().memoryLimit;
}

void Store::reserve(uint64_t size) {
    if (size > size_) makeRoom(size, true);
}

void Store::read(uint64_t position, size_t n, uint8_t* out) {
    if (position > size_ || n > size_ - position) throw Error("read beyond the end of the data (internal error)");
    if (n == 0) return;
    if (file_) {
        flush();
        file_->read(position, out, n, "a temporary file");
    } else {
        std::memcpy(out, memory_.data() + position, n);
    }
}

void Store::append(const uint8_t* data, size_t n) {
    if (n == 0) return;
    makeRoom(size_ + n);
    if (file_) toFile(size_, data, n);
    else memory_.insert(memory_.end(), data, data + n);
    size_ += n;
}

void Store::write(uint64_t position, const uint8_t* data, size_t n) {
    if (position > size_ || n > size_ - position) throw Error("write beyond the end of the data (internal error)");
    if (n == 0) return;
    if (file_) toFile(position, data, n);
    else std::memcpy(memory_.data() + position, data, n);
}

// Small writes that follow each other (the lines of a tile, the runs of a row) are collected
// and go to the file together.
void Store::toFile(uint64_t position, const uint8_t* data, size_t n) {
    constexpr size_t kPending = size_t(1) << 20;
    if (!pending_.empty() && position == pendingAt_ + pending_.size() && pending_.size() + n <= kPending) {
        pending_.insert(pending_.end(), data, data + n);
        return;
    }
    flush();
    if (n >= kPending) {
        file_->write(position, data, n);
        return;
    }
    pendingAt_ = position;
    pending_.assign(data, data + n);
}

void Store::flush() {
    if (pending_.empty()) return;
    file_->write(pendingAt_, pending_.data(), pending_.size());
    pending_.clear();
}

namespace {
class SliceBytes : public RandomBytes {
public:
    SliceBytes(std::shared_ptr<RandomBytes> bytes, uint64_t offset, uint64_t size) : bytes_(std::move(bytes)), offset_(offset), size_(size) {
        if (offset > bytes_->size() || size > bytes_->size() - offset) throw Error("a part beyond the end of the data (internal error)");
    }
    uint64_t size() const override { return size_; }
    void read(uint64_t position, size_t n, uint8_t* out) override {
        if (position > size_ || n > size_ - position) throw Error("read beyond the end of the data (internal error)");
        bytes_->read(offset_ + position, n, out);
    }
    const uint8_t* contiguous() const override {
        const uint8_t* all = bytes_->contiguous();
        return all ? all + offset_ : nullptr;
    }

private:
    std::shared_ptr<RandomBytes> bytes_;
    uint64_t offset_, size_;
};
}  // namespace

std::shared_ptr<RandomBytes> sliceBytes(std::shared_ptr<RandomBytes> bytes, uint64_t offset, uint64_t size) {
    if (offset == 0 && size == bytes->size()) return bytes;
    return std::make_shared<SliceBytes>(std::move(bytes), offset, size);
}

void copyBytes(RandomBytes& from, uint64_t position, uint64_t size, ByteSink& to) {
    const size_t piece = static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(pieceSettings().pieceBytes, 4096), size));
    std::vector<uint8_t> buffer(piece);
    for (uint64_t done = 0; done < size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(piece, size - done));
        from.read(position + done, n, buffer.data());
        to.write(buffer.data(), n);
        done += n;
        progressTick(n);
    }
}

}  // namespace xisfconv
