// Bytes held for later, and read or written a piece at a time: the parts of a file a conversion
// reads from, and what it keeps between reading and writing. Nothing here needs a whole image
// in memory; what does not fit the memory a conversion may take goes into a temporary file.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <memory>
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
    // The bytes, if they are in memory (nullptr if they are in a file).
    const uint8_t* memory() const { return file_ ? nullptr : memory_.data(); }
    const uint8_t* contiguous() const override { return memory(); }
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

}  // namespace xisfconv
