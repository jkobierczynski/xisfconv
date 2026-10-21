// Bytes held for later, and read or written a piece at a time: the parts of a file a conversion
// reads from, and what it keeps between reading and writing. Nothing here needs a whole image
// in memory; what does not fit the memory a conversion may take goes into a temporary file.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

// How much of an image is worked on at once, and how much memory the data a conversion holds
// for later may take. Beyond that limit it goes into temporary files.
struct PieceSettings {
    uint64_t pieceBytes = uint64_t(4) << 20;     // about what one piece of an image holds
    uint64_t memoryLimit = uint64_t(256) << 20;  // what the bytes held in memory may take together (all threads)
};
// The defaults, as far as the environment does not set them: XISFCONV_PIECE_BYTES and
// XISFCONV_MEMORY_LIMIT, numbers of bytes with an optional K, M or G (1024, 1024², 1024³).
// (Read once.)
const PieceSettings& pieceSettings();

// Where the temporary files of the calling thread go while the object lives: beside the output
// of a conversion. Without one: the system's directory for temporary files.
class TempDirectoryScope {
public:
    explicit TempDirectoryScope(std::string directory);
    ~TempDirectoryScope();
    TempDirectoryScope(const TempDirectoryScope&) = delete;
    TempDirectoryScope& operator=(const TempDirectoryScope&) = delete;

private:
    std::string previous_;
};

// A file read or written at any place, with 64-bit positions on every system.
class RawFile {
public:
    // Throws Error (Io) if the file cannot be opened (see failToOpen).
    static std::shared_ptr<RawFile> openForReading(const std::string& path);
    // A new file in the temporary directory of the thread that is gone when it is closed, also
    // if the program is stopped (it has no name from the start where the system allows that).
    static std::unique_ptr<RawFile> temporary();
    // A file that is written: made, or made empty if it is there. nullptr if it cannot be.
    static std::unique_ptr<RawFile> createForWriting(const std::filesystem::path& path);
    ~RawFile();
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    uint64_t size() const;
    // Exactly n bytes, or Error (Io; `what` names the data in the message).
    void read(uint64_t position, void* out, size_t n, const char* what = "data") const;
    void write(uint64_t position, const void* data, size_t n);

private:
    RawFile() = default;
    int fd_ = -1;
    std::string path_;      // for messages, and to remove a temporary file where it keeps its name
    bool temporary_ = false;
};

// Bytes that are read at any place.
class RandomBytes {
public:
    virtual ~RandomBytes() = default;
    virtual uint64_t size() const = 0;
    // [position, position + n), which lies within size(); throws Error otherwise.
    virtual void read(uint64_t position, size_t n, uint8_t* out) = 0;
    // All of the bytes, if they are in memory one after the other (valid while nothing is
    // written to them); nullptr otherwise.
    virtual const uint8_t* contiguous() const { return nullptr; }
    // True if several threads may read the bytes at once (each its own part, or the same):
    // bytes in memory, and a file read at a place of its own (RawFile).
    virtual bool readableByThreads() const { return false; }
};

// Bytes in memory that somebody else keeps (`keep` keeps them, if given).
class MemoryBytes : public RandomBytes {
public:
    MemoryBytes(const uint8_t* data, uint64_t size, std::shared_ptr<const void> keep = nullptr)
        : data_(data), size_(size), keep_(std::move(keep)) {}
    uint64_t size() const override { return size_; }
    void read(uint64_t position, size_t n, uint8_t* out) override;
    const uint8_t* data() const { return data_; }
    const uint8_t* contiguous() const override { return data_; }
    bool readableByThreads() const override { return true; }

private:
    const uint8_t* data_;
    uint64_t size_;
    std::shared_ptr<const void> keep_;
};

// A part of a file: `size` bytes from `offset`.
class FileBytes : public RandomBytes {
public:
    // `failure`: what a read error says (Error, Io).
    FileBytes(std::shared_ptr<RawFile> file, uint64_t offset, uint64_t size, std::string failure = "read error in data block")
        : file_(std::move(file)), offset_(offset), size_(size), failure_(std::move(failure)) {}
    uint64_t size() const override { return size_; }
    void read(uint64_t position, size_t n, uint8_t* out) override;
    bool readableByThreads() const override { return true; }

private:
    std::shared_ptr<RawFile> file_;
    uint64_t offset_, size_;
    std::string failure_;
};

// Where bytes are put, one piece after the other.
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual void write(const uint8_t* data, size_t n) = 0;
};

class VectorSink : public ByteSink {
public:
    explicit VectorSink(std::vector<uint8_t>& out) : out_(out) {}
    void write(const uint8_t* data, size_t n) override { out_.insert(out_.end(), data, data + n); }

private:
    std::vector<uint8_t>& out_;
};

// Bytes held for later: in memory as long as all that is held fits PieceSettings::memoryLimit,
// in a temporary file beyond. Written at the end (append) or at any place within its size.
class Store : public RandomBytes {
public:
    Store() = default;
    explicit Store(uint64_t size);   // that many zeros
    ~Store() override;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    uint64_t size() const override { return size_; }
    void read(uint64_t position, size_t n, uint8_t* out) override;
    void append(const uint8_t* data, size_t n);
    // That the store will hold `size` bytes in all: room for exactly that many, in memory or the file.
    void reserve(uint64_t size);
    void write(uint64_t position, const uint8_t* data, size_t n);
    // The same at any place: beyond the end, the store grows (with zeros where nothing was written).
    void put(uint64_t position, const uint8_t* data, size_t n);
    // The bytes, if they are in memory (nullptr if they are in a file).
    const uint8_t* memory() const { return file_ ? nullptr : memory_.data(); }
    const uint8_t* contiguous() const override { return memory(); }
    bool readableByThreads() const override { return !file_; }   // (a file it writes when it is read)
    bool inFile() const { return file_ != nullptr; }
    // True if `size` more bytes would be held in memory now (as far as the limit tells).
    static bool fitsInMemory(uint64_t size);

private:
    std::vector<uint8_t> memory_;
    uint64_t counted_ = 0;             // what memory_ adds to the bytes held in memory
    std::unique_ptr<RawFile> file_;
    uint64_t size_ = 0;
    std::vector<uint8_t> pending_;     // what goes into the file at pendingAt_, not written yet
    uint64_t pendingAt_ = 0;
    void makeRoom(uint64_t size, bool exact = false);   // for size bytes: memory, or the file
    void toFile(uint64_t position, const uint8_t* data, size_t n);
    void flush();
};

// Appends to a store.
class StoreSink : public ByteSink {
public:
    explicit StoreSink(Store& store) : store_(store) {}
    void write(const uint8_t* data, size_t n) override { store_.append(data, n); }

private:
    Store& store_;
};

// [offset, offset + size) of other bytes.
std::shared_ptr<RandomBytes> sliceBytes(std::shared_ptr<RandomBytes> bytes, uint64_t offset, uint64_t size);

// Copies `size` bytes from `position` to the sink, a piece at a time.
void copyBytes(RandomBytes& from, uint64_t position, uint64_t size, ByteSink& to);

// Bytes that stand for a file during one call, under a name that is valid in this thread while
// the scope lives: Input::file takes the bytes for the file of that name (bytes in memory, what a
// stream brought), and OutputFile writes the file of that name into the store (a file that goes
// to a stream when it is complete). The steps around a file that is written (a temporary name,
// the rename, the checks of what is there) are skipped for it: see isVirtualFile.
// The virtual files of one call: a call of the API made while another runs (from its progress
// handler) has its own, and finds none of the other's.
class VirtualFileFrame {
public:
    VirtualFileFrame();
    ~VirtualFileFrame();
    VirtualFileFrame(const VirtualFileFrame&) = delete;
    VirtualFileFrame& operator=(const VirtualFileFrame&) = delete;

private:
    std::shared_ptr<void> outer_;
};

class VirtualFileScope {
public:
    VirtualFileScope(std::string name, std::shared_ptr<RandomBytes> bytes);   // a file that is read
    VirtualFileScope(std::string name, std::shared_ptr<Store> store);         // a file that is written
    ~VirtualFileScope();
    VirtualFileScope(const VirtualFileScope&) = delete;
    VirtualFileScope& operator=(const VirtualFileScope&) = delete;

private:
    std::string name_;
};
bool isVirtualFile(const std::string& name);
// The bytes of a virtual file (of one that is written: what has been written), or nullptr.
std::shared_ptr<RandomBytes> virtualFileBytes(const std::string& name);
// The store a virtual file is written into, made empty (as a file that is opened for writing), or nullptr.
std::shared_ptr<Store> virtualFileForWriting(const std::string& name);

// What a reader reads: a file, by its path, or bytes it is handed (borrowed memory, or what a
// stream brought, kept in a Store). `name` is what messages call it: the path of a file, the name
// a caller gave bytes in memory.
struct Input {
    std::string name;
    std::string path;                     // a file: its path; empty for bytes in memory
    std::shared_ptr<RandomBytes> bytes;   // bytes in memory (a file is opened by open())

    static Input file(const std::string& path);
    static Input memory(std::string name, std::shared_ptr<RandomBytes> bytes);
    bool inMemory() const { return path.empty(); }
    // The bytes to read: of a file, the file opened now (failToOpen if it cannot be).
    std::shared_ptr<RandomBytes> open() const;
};

// Bytes read as a stream: what std::ifstream is to a file, for any RandomBytes (with seeking).
class InputStream : public std::istream {
public:
    explicit InputStream(std::shared_ptr<RandomBytes> bytes);
    InputStream(const InputStream&) = delete;
    InputStream& operator=(const InputStream&) = delete;

private:
    class Buffer : public std::streambuf {
    public:
        explicit Buffer(std::shared_ptr<RandomBytes> bytes) : bytes_(std::move(bytes)), data_(size_t(1) << 16) {}

    protected:
        int_type underflow() override;
        pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override;
        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;
        std::streamsize showmanyc() override;

    private:
        std::shared_ptr<RandomBytes> bytes_;
        std::vector<char> data_;
        uint64_t at_ = 0;   // where in the bytes the buffer begins
    };
    Buffer buffer_;
};

// A file that is written as std::ofstream writes one (binary, made empty first), but through a
// buffer of its own, straight to the file at the place each byte belongs (pwrite). The reason is
// fork(): a process forked from a program that is writing a file inherits what an ofstream has
// not written yet where the C++ library keeps that in a C FILE (libc++ does, on macOS and
// elsewhere), and exit() in the child writes it to the file a second time, into the middle of
// what the parent writes. A buffer of this class is memory that nothing in the child writes out.
class OutputFile : public std::ostream {
public:
    explicit OutputFile(const std::filesystem::path& path);
    ~OutputFile() override;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    bool is_open() const { return buffer_.isOpen(); }
    // Writes what is left and closes the file; failbit if either fails.
    void close();

private:
    class Buffer : public std::streambuf {
    public:
        bool open(const std::filesystem::path& path);
        bool close();
        bool isOpen() const { return file_ != nullptr || store_ != nullptr; }

    protected:
        int_type overflow(int_type c) override;
        std::streamsize xsputn(const char* s, std::streamsize n) override;
        int sync() override;
        pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override;
        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;

    private:
        bool drain();   // what is in the buffer to the file
        void put(uint64_t position, const char* data, size_t n);   // to the file or the store
        std::unique_ptr<RawFile> file_;
        std::shared_ptr<Store> store_;   // the file is a virtual one (see VirtualFileScope)
        std::vector<char> data_;
        uint64_t at_ = 0;   // where in the file the first byte of the buffer goes
    };
    Buffer buffer_;
};

}  // namespace xisfconv
