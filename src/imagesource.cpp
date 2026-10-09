// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "imagesource.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace xisfconv {

uint64_t rowsPerPiece(uint64_t rowBytes) {
    return std::max<uint64_t>(1, pieceSettings().pieceBytes / std::max<uint64_t>(rowBytes, 1));
}

void forEachBand(ImageSource& source, const BandFunction& fn, uint64_t rows) {
    const uint64_t rowBytes = source.rowBytes();
    if (rows == 0) rows = rowsPerPiece(rowBytes);
    if (source.width == 0 || source.height == 0) return;
    std::vector<uint8_t> band(static_cast<size_t>(checkedMul(std::min(rows, source.height), rowBytes, "the size of a piece")));
    for (uint64_t c = 0; c < source.channels; ++c) {
        for (uint64_t y = 0; y < source.height; y += rows) {
            const uint64_t n = std::min(rows, source.height - y);
            source.readRows(c, y, n, band.data());
            fn(c, y, n, band.data());
            progressTick(n * rowBytes);
        }
    }
}

PixelBuffer readAll(ImageSource& source) {
    PixelBuffer px;
    px.width = source.width;
    px.height = source.height;
    px.channels = source.channels;
    px.format = source.format;
    const uint64_t bytes = checkedMul(checkedMul(source.samples(), 1, "image size"), sampleBytes(source.format), "image size");
    if (bytes > std::numeric_limits<size_t>::max()) throw Error("image too large for this platform");
    px.data.resize(static_cast<size_t>(bytes));
    const uint64_t plane = source.height * source.rowBytes();
    forEachBand(source, [&](uint64_t c, uint64_t y, uint64_t rows, const uint8_t* data) {
        std::memcpy(px.data.data() + c * plane + y * source.rowBytes(), data, static_cast<size_t>(rows * source.rowBytes()));
    });
    return px;
}

namespace {

class BufferSource : public ImageSource {
public:
    BufferSource(const PixelBuffer& px, std::shared_ptr<const PixelBuffer> keep) : px_(px), keep_(std::move(keep)) {
        width = px.width;
        height = px.height;
        channels = px.channels;
        format = px.format;
        if (px.data.size() < samples() * sampleBytes(format)) throw Error("the pixel buffer is smaller than its geometry (internal error)");
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        const uint64_t at = (channel * height + y) * rowBytes();
        std::memcpy(out, px_.data.data() + at, static_cast<size_t>(rows * rowBytes()));
    }

private:
    const PixelBuffer& px_;
    std::shared_ptr<const PixelBuffer> keep_;
};

class StoredSource : public ImageSource {
public:
    StoredSource(std::shared_ptr<RandomBytes> bytes, uint64_t w, uint64_t h, uint64_t c, SampleFormat f, StoredLayout layout)
        : bytes_(std::move(bytes)), layout_(layout) {
        width = w;
        height = h;
        channels = c;
        format = f;
        if (bytes_->size() < checkedMul(checkedMul(checkedMul(w, h, "image size"), c, "image size"), sampleBytes(f), "image size")) {
            throw Error("the data block is smaller than the image (internal error)");
        }
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        const size_t sb = sampleBytes(format);
        const size_t n = static_cast<size_t>(rows * width);
        if (layout_.planar || channels == 1) {
            bytes_->read(((channel * height + y) * width) * sb, n * sb, out);
        } else {
            // Pixel after pixel: the band of all channels is read once and kept for the others.
            if (!(bandY_ == y && bandRows_ == rows)) {
                band_.resize(static_cast<size_t>(rows * width * channels) * sb);
                bytes_->read(y * width * channels * sb, band_.size(), band_.data());
                bandY_ = y;
                bandRows_ = rows;
            }
            const size_t stride = static_cast<size_t>(channels) * sb;
            const uint8_t* s = band_.data() + channel * sb;
            for (size_t i = 0; i < n; ++i) std::memcpy(out + i * sb, s + i * stride, sb);
        }
        if (layout_.swap && sb > 1) byteSwapInPlace(out, n, sb);
    }

private:
    std::shared_ptr<RandomBytes> bytes_;
    StoredLayout layout_;
    std::vector<uint8_t> band_;
    uint64_t bandY_ = std::numeric_limits<uint64_t>::max(), bandRows_ = 0;
};

class UnshuffledBytes : public RandomBytes {
public:
    UnshuffledBytes(std::shared_ptr<RandomBytes> shuffled, uint64_t itemSize)
        : in_(std::move(shuffled)), item_(itemSize), size_(in_->size()), items_(itemSize ? size_ / itemSize : 0) {}
    uint64_t size() const override { return size_; }
    void read(uint64_t position, size_t n, uint8_t* out) override {
        if (position > size_ || n > size_ - position) throw Error("read beyond the end of the data (internal error)");
        if (item_ <= 1 || size_ < item_) {
            in_->read(position, n, out);
            return;
        }
        const uint64_t whole = items_ * item_;   // the tail beyond is where it was
        const uint64_t end = position + n;
        if (end > whole) {
            const uint64_t from = std::max(position, whole);
            in_->read(from, static_cast<size_t>(end - from), out + (from - position));
        }
        if (position >= whole) return;
        const uint64_t last = std::min(end, whole);
        // the items that [position, last) touches, and of each byte of them the run in its plane
        const uint64_t first = position / item_, beyond = (last + item_ - 1) / item_;
        const size_t count = static_cast<size_t>(beyond - first);
        run_.resize(count);
        for (uint64_t k = 0; k < item_; ++k) {
            in_->read(k * items_ + first, count, run_.data());
            for (size_t i = 0; i < count; ++i) {
                const uint64_t at = (first + i) * item_ + k;   // where the byte is in the item order
                if (at >= position && at < last) out[at - position] = run_[i];
            }
        }
    }

private:
    std::shared_ptr<RandomBytes> in_;
    uint64_t item_, size_, items_;
    std::vector<uint8_t> run_;
};

class ShuffledBytes : public RandomBytes {
public:
    ShuffledBytes(std::shared_ptr<RandomBytes> items, uint64_t itemSize)
        : in_(std::move(items)), item_(itemSize), size_(in_->size()), items_(itemSize ? size_ / itemSize : 0) {}
    uint64_t size() const override { return size_; }
    void read(uint64_t position, size_t n, uint8_t* out) override {
        if (position > size_ || n > size_ - position) throw Error("read beyond the end of the data (internal error)");
        if (item_ <= 1 || size_ < item_) {
            in_->read(position, n, out);
            return;
        }
        const uint64_t whole = items_ * item_;
        while (n > 0 && position < whole) {
            // a run within the plane of one byte of the items
            const uint64_t k = position / items_, first = position % items_;
            const size_t count = static_cast<size_t>(std::min<uint64_t>(n, items_ - first));
            const size_t chunk = std::min<size_t>(count, size_t(1) << 18);
            run_.resize(chunk * static_cast<size_t>(item_));
            in_->read(first * item_, run_.size(), run_.data());
            for (size_t i = 0; i < chunk; ++i) out[i] = run_[i * static_cast<size_t>(item_) + static_cast<size_t>(k)];
            out += chunk;
            n -= chunk;
            position += chunk;
        }
        if (n > 0) in_->read(position, n, out);   // the tail is where it was
    }

private:
    std::shared_ptr<RandomBytes> in_;
    uint64_t item_, size_, items_;
    std::vector<uint8_t> run_;
};

class FlippedSource : public ImageSource {
public:
    explicit FlippedSource(Source s) : s_(std::move(s)) {
        width = s_->width;
        height = s_->height;
        channels = s_->channels;
        format = s_->format;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        if (rows == 0) return;
        s_->readRows(channel, height - y - rows, rows, out);
        const size_t rb = static_cast<size_t>(rowBytes());
        tmp_.resize(rb);
        for (uint64_t a = 0, b = rows - 1; a < b; ++a, --b) {
            std::memcpy(tmp_.data(), out + a * rb, rb);
            std::memcpy(out + a * rb, out + b * rb, rb);
            std::memcpy(out + b * rb, tmp_.data(), rb);
        }
    }

private:
    Source s_;
    std::vector<uint8_t> tmp_;
};

class ChannelSource : public ImageSource {
public:
    ChannelSource(Source s, uint64_t first, uint64_t count) : s_(std::move(s)), first_(first) {
        width = s_->width;
        height = s_->height;
        channels = count;
        format = s_->format;
        if (first + count > s_->channels) throw Error("no such channel (internal error)");
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override { s_->readRows(first_ + channel, y, rows, out); }

private:
    Source s_;
    uint64_t first_;
};

class MappedSource : public ImageSource {
public:
    MappedSource(Source s, SampleFormat f, SampleMap map) : s_(std::move(s)), map_(std::move(map)) {
        width = s_->width;
        height = s_->height;
        channels = s_->channels;
        format = f;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        const size_t n = static_cast<size_t>(rows * width);
        in_.resize(n * sampleBytes(s_->format));
        s_->readRows(channel, y, rows, in_.data());
        map_(channel, in_.data(), out, n);
    }

private:
    Source s_;
    SampleMap map_;
    std::vector<uint8_t> in_;
};

template <class T>
void rangeOf(const uint8_t* data, size_t n, double& lo, double& hi, bool& nan) {
    const T* p = reinterpret_cast<const T*>(data);
    for (size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(p[i]);
        if (std::isfinite(v)) {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        } else {
            nan = true;
        }
    }
}

}  // namespace

Source bufferSource(std::shared_ptr<const PixelBuffer> pixels) {
    const PixelBuffer& px = *pixels;
    return std::make_shared<BufferSource>(px, std::move(pixels));
}

Source borrowedSource(const PixelBuffer& pixels) { return std::make_shared<BufferSource>(pixels, nullptr); }

Source storedSource(std::shared_ptr<RandomBytes> bytes, uint64_t width, uint64_t height, uint64_t channels, SampleFormat format,
                    StoredLayout layout) {
    return std::make_shared<StoredSource>(std::move(bytes), width, height, channels, format, layout);
}

std::shared_ptr<RandomBytes> unshuffledBytes(std::shared_ptr<RandomBytes> shuffled, uint64_t itemSize) {
    return std::make_shared<UnshuffledBytes>(std::move(shuffled), itemSize);
}

std::shared_ptr<RandomBytes> shuffledBytes(std::shared_ptr<RandomBytes> items, uint64_t itemSize) {
    return std::make_shared<ShuffledBytes>(std::move(items), itemSize);
}

Source flippedSource(Source source) { return std::make_shared<FlippedSource>(std::move(source)); }

Source channelSource(Source source, uint64_t first, uint64_t count) {
    return std::make_shared<ChannelSource>(std::move(source), first, count);
}

Source mappedSource(Source source, SampleFormat format, SampleMap map) {
    return std::make_shared<MappedSource>(std::move(source), format, std::move(map));
}

FloatRange floatRange(ImageSource& source) {
    FloatRange r;
    if (!isFloat(source.format)) return r;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    bool nan = false;
    forEachBand(source, [&](uint64_t, uint64_t, uint64_t rows, const uint8_t* data) {
        const size_t n = static_cast<size_t>(rows * source.width);
        if (source.format == SampleFormat::Float32) rangeOf<float>(data, n, lo, hi, nan);
        else rangeOf<double>(data, n, lo, hi, nan);
    });
    if (lo > hi) lo = hi = 0;
    r.min = lo;
    r.max = hi;
    r.hasNaN = nan;
    return r;
}

}  // namespace xisfconv
