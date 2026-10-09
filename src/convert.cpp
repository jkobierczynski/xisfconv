// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "convert.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>

namespace xisfconv {

namespace {

template <class T>
double maxValue() {
    return static_cast<double>(std::numeric_limits<T>::max());
}

template <class S, class D>
void convertTyped(const uint8_t* srcBytes, uint8_t* dstBytes, size_t n, double lower, double upper) {
    const S* src = reinterpret_cast<const S*>(srcBytes);
    D* dst = reinterpret_cast<D*>(dstBytes);
    constexpr bool srcFloat = std::is_floating_point<S>::value;
    constexpr bool dstFloat = std::is_floating_point<D>::value;

    if constexpr (srcFloat && dstFloat) {
        for (size_t i = 0; i < n; ++i) dst[i] = static_cast<D>(src[i]);
    } else if constexpr (!srcFloat && dstFloat) {
        const double scale = 1.0 / maxValue<S>();
        for (size_t i = 0; i < n; ++i) dst[i] = static_cast<D>(static_cast<double>(src[i]) * scale);
    } else if constexpr (srcFloat && !dstFloat) {
        const double dmax = maxValue<D>();
        const double range = upper - lower;
        for (size_t i = 0; i < n; ++i) {
            double t = (static_cast<double>(src[i]) - lower) / range;
            if (!(t > 0)) t = 0;  // also maps NaN to 0
            if (t > 1) t = 1;
            const double v = std::floor(t * dmax + 0.5);
            dst[i] = v >= dmax ? std::numeric_limits<D>::max() : static_cast<D>(v);
        }
    } else {
        if (sizeof(S) == sizeof(D)) {
            std::memcpy(dstBytes, srcBytes, n * sizeof(S));
            return;
        }
        // Integer rescaling. Widening by an exact factor (e.g. 255 -> 65535 is x257)
        // and narrowing by rounding division keep the extremes exact.
        const double scale = maxValue<D>() / maxValue<S>();
        const double dmax = maxValue<D>();
        for (size_t i = 0; i < n; ++i) {
            const double v = std::floor(static_cast<double>(src[i]) * scale + 0.5);
            dst[i] = v >= dmax ? std::numeric_limits<D>::max() : static_cast<D>(v);
        }
    }
}

template <class S>
void dispatchTarget(const uint8_t* src, uint8_t* dst, size_t n, SampleFormat target, double lo, double hi) {
    switch (target) {
        case SampleFormat::UInt8: convertTyped<S, uint8_t>(src, dst, n, lo, hi); break;
        case SampleFormat::UInt16: convertTyped<S, uint16_t>(src, dst, n, lo, hi); break;
        case SampleFormat::UInt32: convertTyped<S, uint32_t>(src, dst, n, lo, hi); break;
        case SampleFormat::UInt64: convertTyped<S, uint64_t>(src, dst, n, lo, hi); break;
        case SampleFormat::Float32: convertTyped<S, float>(src, dst, n, lo, hi); break;
        case SampleFormat::Float64: convertTyped<S, double>(src, dst, n, lo, hi); break;
    }
}

}  // namespace

void convertSamples(const uint8_t* src, SampleFormat from, uint8_t* dst, SampleFormat target, size_t n, double lower, double upper) {
    if (!(upper > lower)) { lower = 0; upper = 1; }
    if (from == target) {
        std::memcpy(dst, src, n * sampleBytes(from));
        return;
    }
    switch (from) {
        case SampleFormat::UInt8: dispatchTarget<uint8_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::UInt16: dispatchTarget<uint16_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::UInt32: dispatchTarget<uint32_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::UInt64: dispatchTarget<uint64_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::Float32: dispatchTarget<float>(src, dst, n, target, lower, upper); break;
        case SampleFormat::Float64: dispatchTarget<double>(src, dst, n, target, lower, upper); break;
    }
}

void convertSampleFormat(PixelBuffer& px, SampleFormat target, double lower, double upper) {
    if (px.format == target) return;
    const size_t n = static_cast<size_t>(px.samples());
    std::vector<uint8_t> out(n * sampleBytes(target));
    convertSamples(px.data.data(), px.format, out.data(), target, n, lower, upper);
    px.data.swap(out);
    px.format = target;
}

namespace {

// Appends samples first, first + step, ... (below count) of `data`, normalized to [0,1]
// (integers by their full range, floats by [lower,upper]).
template <class T>
void appendNormalized(const uint8_t* data, size_t count, size_t first, size_t step, double lower, double upper,
                      std::vector<float>& out) {
    const T* src = reinterpret_cast<const T*>(data);
    double offset = 0, scale = 1;
    if (std::is_floating_point<T>::value) {
        offset = lower;
        scale = 1.0 / (upper - lower);
    } else {
        scale = 1.0 / maxValue<T>();
    }
    for (size_t i = first; i < count; i += step) {
        double v = (static_cast<double>(src[i]) - offset) * scale;
        if (!(v > 0)) v = 0;  // also NaN
        if (v > 1) v = 1;
        out.push_back(static_cast<float>(v));
    }
}

void normalizedSamples(const uint8_t* data, SampleFormat format, size_t count, size_t first, size_t step, double lower,
                       double upper, std::vector<float>& out) {
    if (!(upper > lower)) { lower = 0; upper = 1; }
    switch (format) {
        case SampleFormat::UInt8: appendNormalized<uint8_t>(data, count, first, step, lower, upper, out); break;
        case SampleFormat::UInt16: appendNormalized<uint16_t>(data, count, first, step, lower, upper, out); break;
        case SampleFormat::UInt32: appendNormalized<uint32_t>(data, count, first, step, lower, upper, out); break;
        case SampleFormat::UInt64: appendNormalized<uint64_t>(data, count, first, step, lower, upper, out); break;
        case SampleFormat::Float32: appendNormalized<float>(data, count, first, step, lower, upper, out); break;
        case SampleFormat::Float64: appendNormalized<double>(data, count, first, step, lower, upper, out); break;
    }
}

double median(std::vector<float>& v) {
    if (v.empty()) return 0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return (lo + hi) / 2;
}

// Midtones transfer function: maps m to 0.5, keeps 0 and 1 fixed.
inline double mtf(double m, double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    if (m == 0.5) return x;
    return (m - 1) * x / ((2 * m - 1) * x - m);
}

}  // namespace

std::vector<StretchParams> autoStretch(ImageSource& source, double lower, double upper, size_t colorChannels, bool linked) {
    constexpr double kShadowsClip = -2.8;     // in units of normalized MAD
    constexpr double kTargetBackground = 0.25;
    constexpr uint64_t kMaxSamples = 4u << 20;  // statistics on at most ~4M samples per channel

    const size_t channels = std::min<size_t>(colorChannels, static_cast<size_t>(source.channels));
    const uint64_t n = source.width * source.height;
    const uint64_t step = std::max<uint64_t>(1, n / kMaxSamples);
    std::vector<double> med(channels), madn(channels);
    std::vector<float> buf;
    for (size_t c = 0; c < channels; ++c) {
        // every step-th sample of the channel, counted from its first one
        buf.clear();
        buf.reserve(static_cast<size_t>(n / step + 1));
        const uint64_t rows = rowsPerPiece(source.rowBytes());
        std::vector<uint8_t> band(static_cast<size_t>(std::min(rows, source.height) * source.rowBytes()));
        for (uint64_t y = 0; y < source.height; y += rows) {
            const uint64_t count = std::min(rows, source.height - y);
            const uint64_t begin = y * source.width, end = (y + count) * source.width;
            const uint64_t first = (begin + step - 1) / step * step;   // the first sample of the band that is taken
            if (first >= end) continue;
            source.readRows(c, y, count, band.data());
            normalizedSamples(band.data(), source.format, static_cast<size_t>(end - begin), static_cast<size_t>(first - begin),
                              static_cast<size_t>(step), lower, upper, buf);
            progressTick(count * source.rowBytes());
        }
        med[c] = median(buf);
        for (auto& v : buf) v = static_cast<float>(std::fabs(v - med[c]));
        madn[c] = 1.4826 * median(buf);
    }

    auto params = [&](double m, double d) {
        StretchParams p;
        if (m > 0.5) {  // inverted image (bright background), as handled by PixInsight's AutoSTF
            p.highlights = (d == 0) ? 1.0 : std::min(1.0, m - kShadowsClip * d);
            p.midtones = mtf(kTargetBackground, p.highlights - m);
            p.midtones = 1 - p.midtones;  // mirror for the upper half
            return p;
        }
        p.shadows = (d == 0) ? 0.0 : std::max(0.0, m + kShadowsClip * d);
        // A flat channel (median at the shadows point) gets no midtones change.
        p.midtones = m > p.shadows ? mtf(kTargetBackground, m - p.shadows) : 0.5;
        return p;
    };

    std::vector<StretchParams> out(channels);
    if (linked) {
        double m = 0, d = 0;
        for (size_t c = 0; c < channels; ++c) { m += med[c]; d += madn[c]; }
        m /= static_cast<double>(channels);
        d /= static_cast<double>(channels);
        const StretchParams p = params(m, d);
        for (auto& o : out) o = p;
    } else {
        for (size_t c = 0; c < channels; ++c) out[c] = params(med[c], madn[c]);
    }
    return out;
}

std::vector<StretchParams> autoStretch(const PixelBuffer& px, double lower, double upper, size_t colorChannels, bool linked) {
    return autoStretch(*borrowedSource(px), lower, upper, colorChannels, linked);
}

void stretchSamples(const uint8_t* src, SampleFormat format, size_t n, const StretchParams* params, double lower, double upper,
                    float* dst) {
    std::vector<float> buf;
    buf.reserve(n);
    normalizedSamples(src, format, n, 0, 1, lower, upper, buf);
    if (!params) {
        std::copy(buf.begin(), buf.end(), dst);
        return;
    }
    const StretchParams& p = *params;
    const double range = p.highlights - p.shadows;
    const double erange = p.high - p.low;
    for (size_t i = 0; i < n; ++i) {
        double x = range > 0 ? (buf[i] - p.shadows) / range : (buf[i] >= p.highlights ? 1.0 : 0.0);
        x = mtf(p.midtones, std::min(1.0, std::max(0.0, x)));
        if (erange > 0 && (p.low != 0 || p.high != 1)) x = (x - p.low) / erange;
        dst[i] = static_cast<float>(std::min(1.0, std::max(0.0, x)));
    }
}

void applyStretch(PixelBuffer& px, const std::vector<StretchParams>& params, double lower, double upper) {
    const size_t n = static_cast<size_t>(px.planeSamples());
    const size_t sb = sampleBytes(px.format);
    std::vector<uint8_t> outBytes(static_cast<size_t>(px.samples()) * sizeof(float));
    float* out = reinterpret_cast<float*>(outBytes.data());
    for (uint64_t c = 0; c < px.channels; ++c) {
        stretchSamples(px.data.data() + c * n * sb, px.format, n, c < params.size() ? &params[c] : nullptr, lower, upper, out + c * n);
    }
    px.data.swap(outBytes);
    px.format = SampleFormat::Float32;
}

void normalizeSamples(uint8_t* data, SampleFormat format, size_t n, double lower, double upper) {
    if (!isFloat(format) || !(upper > lower) || (lower == 0 && upper == 1)) return;
    const double scale = 1.0 / (upper - lower);
    if (format == SampleFormat::Float32) {
        float* p = reinterpret_cast<float*>(data);
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<float>((static_cast<double>(p[i]) - lower) * scale);
    } else {
        double* p = reinterpret_cast<double*>(data);
        for (size_t i = 0; i < n; ++i) p[i] = (p[i] - lower) * scale;
    }
}

void normalizeFloat(PixelBuffer& px, double lower, double upper) {
    normalizeSamples(px.data.data(), px.format, static_cast<size_t>(px.samples()), lower, upper);
}

void flipVertical(PixelBuffer& px) {
    const size_t rowBytes = static_cast<size_t>(px.width) * sampleBytes(px.format);
    std::vector<uint8_t> tmp(rowBytes);
    for (uint64_t c = 0; c < px.channels; ++c) {
        uint8_t* plane = px.data.data() + c * px.height * rowBytes;
        for (uint64_t y = 0; y < px.height / 2; ++y) {
            uint8_t* a = plane + y * rowBytes;
            uint8_t* b = plane + (px.height - 1 - y) * rowBytes;
            std::memcpy(tmp.data(), a, rowBytes);
            std::memcpy(a, b, rowBytes);
            std::memcpy(b, tmp.data(), rowBytes);
        }
    }
}

DownsampledSize downsampledSize(const Downsample& how, uint64_t width, uint64_t height) {
    DownsampledSize out;
    out.useWidth = out.width = width;
    out.useHeight = out.height = height;
    if (width == 0 || height == 0) return out;
    const uint64_t bin = std::max<uint64_t>(1, how.bin);
    // (an image smaller than one block is one block)
    uint64_t w = std::max<uint64_t>(1, width / bin), h = std::max<uint64_t>(1, height / bin);
    out.useWidth = std::min(width, w * bin);
    out.useHeight = std::min(height, h * bin);
    double factor = how.scale > 0 && how.scale < 1 ? how.scale : 1.0;
    if (how.fitWidth && static_cast<double>(w) * factor > static_cast<double>(how.fitWidth)) {
        factor = static_cast<double>(how.fitWidth) / static_cast<double>(w);
    }
    if (how.fitHeight && static_cast<double>(h) * factor > static_cast<double>(how.fitHeight)) {
        factor = static_cast<double>(how.fitHeight) / static_cast<double>(h);
    }
    if (factor < 1) {
        auto scaled = [&](uint64_t n, uint64_t limit) {
            uint64_t v = static_cast<uint64_t>(std::floor(static_cast<double>(n) * factor + 0.5));
            v = std::max<uint64_t>(1, std::min(v, n));
            return limit ? std::min(v, limit) : v;
        };
        w = scaled(w, how.fitWidth);
        h = scaled(h, how.fitHeight);
    }
    out.width = w;
    out.height = h;
    out.changes = out.width != width || out.height != height;
    return out;
}

namespace {

// The pixels of the image that one pixel of the picture covers along one axis, and the share
// of the first and of the last of them (the ones between are covered whole).
struct Span {
    uint64_t first = 0, count = 1;
    double firstShare = 1, lastShare = 1;
};

std::vector<Span> spans(uint64_t from, uint64_t to) {
    std::vector<Span> out(static_cast<size_t>(to));
    // [j, j + 1) of the picture is [j * from / to, (j + 1) * from / to) of the image. Counted in
    // whole numbers of 1 / to of a pixel, so that every share is exact but for the one division
    // that makes it a double: a whole ratio gives shares of exactly 1.
    const bool exact = from <= 0xFFFFFFFFull && to <= 0xFFFFFFFFull;
    for (uint64_t j = 0; j < to; ++j) {
        Span& s = out[static_cast<size_t>(j)];
        if (exact) {
            const uint64_t a = j * from, b = (j + 1) * from;   // in units of 1 / to
            const uint64_t first = a / to, last = (b - 1) / to;
            s.first = first;
            s.count = last - first + 1;
            if (s.count == 1) {
                s.firstShare = s.lastShare = static_cast<double>(b - a) / static_cast<double>(to);
            } else {
                s.firstShare = static_cast<double>((first + 1) * to - a) / static_cast<double>(to);
                s.lastShare = static_cast<double>(b - last * to) / static_cast<double>(to);
            }
            continue;
        }
        // (no image is that large; the same in floating point)
        const double a = static_cast<double>(j) * static_cast<double>(from) / static_cast<double>(to);
        const double b = static_cast<double>(j + 1) * static_cast<double>(from) / static_cast<double>(to);
        uint64_t first = std::min<uint64_t>(from - 1, static_cast<uint64_t>(std::floor(a)));
        uint64_t last = std::min<uint64_t>(from - 1, static_cast<uint64_t>(std::ceil(b)) - 1);
        if (last < first) last = first;
        s.first = first;
        s.count = last - first + 1;
        if (s.count == 1) {
            s.firstShare = s.lastShare = std::max(b - a, 0.0);
        } else {
            s.firstShare = std::min(1.0, std::max(0.0, static_cast<double>(first + 1) - a));
            s.lastShare = std::min(1.0, std::max(0.0, b - static_cast<double>(last)));
        }
    }
    return out;
}

// Output rows [0, rowCount) of a plane, which are the rows of `rowSpans`; `rowAt(y)` gives row y
// of the image (any row the spans cover).
template <class T, class Rows>
void downsamplePlane(Rows&& rowAt, T* dst, const std::vector<Span>& columns, const Span* rowSpans, size_t rowCount) {
    // (the type is asked for where it is needed: a constant of this function is not one inside the lambda
    // below for every compiler)
    const size_t outW = columns.size();
    // one row of the image, made as narrow as the picture: the sums, and the shares that went into them
    std::vector<double> rowSum(outW), rowShare(outW), sum(outW), share(outW);
    uint64_t reduced = std::numeric_limits<uint64_t>::max();   // the row of the image that rowSum holds
    auto reduce = [&](uint64_t y) {
        if (y == reduced) return;   // (the last row of one pixel of the picture is often the first of the next)
        const T* line = rowAt(y);
        for (size_t j = 0; j < outW; ++j) {
            const Span& c = columns[j];
            double s = 0, w = 0;
            for (uint64_t k = 0; k < c.count; ++k) {
                const double part = k == 0 ? c.firstShare : k + 1 == c.count ? c.lastShare : 1.0;
                const T v = line[c.first + k];
                if constexpr (std::is_floating_point<T>::value) {
                    if (!std::isfinite(v)) continue;
                }
                s += static_cast<double>(v) * part;
                w += part;
            }
            rowSum[j] = s;
            rowShare[j] = w;
        }
        reduced = y;
    };
    for (size_t r = 0; r < rowCount; ++r) {
        const Span& rowSpan = rowSpans[r];
        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(share.begin(), share.end(), 0.0);
        for (uint64_t k = 0; k < rowSpan.count; ++k) {
            const double part = k == 0 ? rowSpan.firstShare : k + 1 == rowSpan.count ? rowSpan.lastShare : 1.0;
            reduce(rowSpan.first + k);
            for (size_t j = 0; j < outW; ++j) {
                sum[j] += rowSum[j] * part;
                share[j] += rowShare[j] * part;
            }
        }
        T* out = dst + r * outW;
        for (size_t j = 0; j < outW; ++j) {
            if constexpr (std::is_floating_point<T>::value) {
                double mean = share[j] > 0 ? sum[j] / share[j] : std::numeric_limits<double>::quiet_NaN();
                if (share[j] > 0 && !std::isfinite(sum[j])) {
                    // The sum of finite samples went beyond what a double holds (samples near its
                    // largest value): this pixel again, every sample divided before it is added.
                    mean = 0;
                    for (uint64_t ky = 0; ky < rowSpan.count; ++ky) {
                        const double py = ky == 0 ? rowSpan.firstShare : ky + 1 == rowSpan.count ? rowSpan.lastShare : 1.0;
                        const T* line = rowAt(rowSpan.first + ky);
                        const Span& c = columns[j];
                        for (uint64_t kx = 0; kx < c.count; ++kx) {
                            const double px = kx == 0 ? c.firstShare : kx + 1 == c.count ? c.lastShare : 1.0;
                            const T v = line[c.first + kx];
                            if (std::isfinite(v)) mean += static_cast<double>(v) * (px * py / share[j]);
                        }
                    }
                }
                out[j] = static_cast<T>(mean);
            } else {
                // (from 2^52 on a double is a whole number, and adding a half would round it up)
                double v = share[j] > 0 ? sum[j] / share[j] : 0.0;
                if (v < 4503599627370496.0) v = std::floor(v + 0.5);
                out[j] = v >= maxValue<T>() ? std::numeric_limits<T>::max() : v <= 0 ? T(0) : static_cast<T>(v);
            }
        }
    }
}

}  // namespace

namespace {

// Rows [y0, y1) of the three planes, each (y1 - y0) * width samples at `out`; `rowAt(y)` gives row
// y of the mosaic (from y0 - 1 to y1, within the image).
template <typename T, class Rows>
void debayerPlanes(Rows&& rowAt, uint64_t width, uint64_t height, uint64_t y0, uint64_t y1, const int colour[2][2], T* out) {
    const uint64_t plane = width * (y1 - y0);
    for (uint64_t y = y0; y < y1; ++y) {
        const T* rows[3] = {y ? rowAt(y - 1) : nullptr, rowAt(y), y + 1 < height ? rowAt(y + 1) : nullptr};
        auto at = [&](uint64_t yy, uint64_t xx) { return rows[yy + 1 - y][xx]; };
        for (uint64_t x = 0; x < width; ++x) {
            const int own = colour[y & 1][x & 1];
            const T v = at(y, x);
            for (int c = 0; c < 3; ++c) {
                T result;
                if (c == own) {
                    result = v;
                } else {
                    // the neighbours of colour c (with a 2 x 2 pattern that holds c and an image of
                    // at least 2 x 2 pixels, every window of 3 x 3 cut by the edges holds one);
                    // floating point samples that are not finite are left out, as from --bin
                    uint64_t n = 0;
                    for (uint64_t yy = y ? y - 1 : 0; yy <= y + 1 && yy < height; ++yy) {
                        for (uint64_t xx = x ? x - 1 : 0; xx <= x + 1 && xx < width; ++xx) {
                            if ((yy == y && xx == x) || colour[yy & 1][xx & 1] != c) continue;
                            if constexpr (std::is_floating_point<T>::value) {
                                if (!std::isfinite(at(yy, xx))) continue;
                            }
                            ++n;
                        }
                    }
                    double sum = 0;
                    uint64_t quotients = 0, remainders = 0;
                    for (uint64_t yy = y ? y - 1 : 0; yy <= y + 1 && yy < height; ++yy) {
                        for (uint64_t xx = x ? x - 1 : 0; xx <= x + 1 && xx < width; ++xx) {
                            if ((yy == y && xx == x) || colour[yy & 1][xx & 1] != c) continue;
                            const T w = at(yy, xx);
                            if constexpr (std::is_floating_point<T>::value) {
                                // (each divided first: a sum of values near the largest double would overflow)
                                if (std::isfinite(w)) sum += static_cast<double>(w) / static_cast<double>(n);
                            } else {
                                quotients += static_cast<uint64_t>(w) / n;
                                remainders += static_cast<uint64_t>(w) % n;
                            }
                        }
                    }
                    if constexpr (std::is_floating_point<T>::value) {
                        result = n ? static_cast<T>(sum) : std::numeric_limits<T>::quiet_NaN();
                    } else {
                        // the sum is n * quotients + remainders: its mean, rounded half up, is
                        // quotients + (remainders + n / 2) / n, with nothing that can overflow
                        result = static_cast<T>(quotients + (remainders + n / 2) / n);
                    }
                }
                out[static_cast<uint64_t>(c) * plane + (y - y0) * width + x] = result;
            }
        }
    }
}

// The colours of a 2 x 2 pattern of R, G and B (0, 1, 2); throws Error (Argument) for one that is not.
void patternColours(const std::string& pattern, int colour[2][2]) {
    bool seen[3] = {false, false, false};
    if (pattern.size() != 4) throw Error("--debayer: a pattern of 2 x 2 is needed, not \"" + pattern + "\"", ErrorKind::Argument);
    for (int k = 0; k < 4; ++k) {
        const char ch = pattern[static_cast<size_t>(k)];
        const int c = ch == 'R' ? 0 : ch == 'G' ? 1 : ch == 'B' ? 2 : -1;
        if (c < 0) throw Error("--debayer: a pattern of R, G and B is needed, not \"" + pattern + "\"", ErrorKind::Argument);
        colour[k / 2][k % 2] = c;
        seen[c] = true;
    }
    if (!seen[0] || !seen[1] || !seen[2]) {
        throw Error("--debayer: the pattern \"" + pattern + "\" lacks a colour", ErrorKind::Argument);
    }
}

// Calls fn with a pointer of the type of the samples.
template <class F>
void withType(SampleFormat format, F&& fn) {
    switch (format) {
        case SampleFormat::UInt8: fn(static_cast<uint8_t*>(nullptr)); break;
        case SampleFormat::UInt16: fn(static_cast<uint16_t*>(nullptr)); break;
        case SampleFormat::UInt32: fn(static_cast<uint32_t*>(nullptr)); break;
        case SampleFormat::UInt64: fn(static_cast<uint64_t*>(nullptr)); break;
        case SampleFormat::Float32: fn(static_cast<float*>(nullptr)); break;
        case SampleFormat::Float64: fn(static_cast<double*>(nullptr)); break;
    }
}

}  // namespace

void debayerBilinear(PixelBuffer& px, const std::string& pattern) {
    if (px.channels != 1) throw Error("--debayer: the image has " + std::to_string(px.channels) + " channels, not one", ErrorKind::Argument);
    if (px.width < 2 || px.height < 2) throw Error("--debayer: an image smaller than 2 x 2 pixels", ErrorKind::Argument);
    int colour[2][2];
    patternColours(pattern, colour);
    const size_t sb = sampleBytes(px.format);
    std::vector<uint8_t> out(static_cast<size_t>(checkedMul(px.planeSamples(), 3, "the size of the image")) * sb);
    const uint8_t* src = px.data.data();
    withType(px.format, [&](auto* type) {
        using T = std::remove_pointer_t<decltype(type)>;
        const T* mosaic = reinterpret_cast<const T*>(src);
        debayerPlanes([&](uint64_t y) { return mosaic + y * px.width; }, px.width, px.height, 0, px.height, colour,
                      reinterpret_cast<T*>(out.data()));
    });
    px.data = std::move(out);
    px.channels = 3;
}

void downsample(PixelBuffer& px, const DownsampledSize& size) {
    if (!size.changes || size.width == 0 || size.height == 0 || px.width == 0 || px.height == 0) return;
    if (size.useWidth > px.width || size.useHeight > px.height || size.width > size.useWidth || size.height > size.useHeight) {
        throw Error("a picture cannot be larger than the image it is made of");
    }
    const size_t sb = sampleBytes(px.format);
    const std::vector<Span> columns = spans(size.useWidth, size.width), rows = spans(size.useHeight, size.height);
    const size_t outPlane = static_cast<size_t>(size.width) * static_cast<size_t>(size.height);
    std::vector<uint8_t> out(outPlane * static_cast<size_t>(px.channels) * sb);
    const size_t inPlane = static_cast<size_t>(px.planeSamples());
    for (uint64_t c = 0; c < px.channels; ++c) {
        const uint8_t* src = px.data.data() + static_cast<size_t>(c) * inPlane * sb;
        uint8_t* dst = out.data() + static_cast<size_t>(c) * outPlane * sb;
        withType(px.format, [&](auto* type) {
            using T = std::remove_pointer_t<decltype(type)>;
            const T* plane = reinterpret_cast<const T*>(src);
            downsamplePlane([&](uint64_t y) { return plane + y * px.width; }, reinterpret_cast<T*>(dst), columns, rows.data(),
                            rows.size());
        });
    }
    px.data.swap(out);
    px.width = size.width;
    px.height = size.height;
}

// ------------------------------------------------------------------------------------------
// The same, a piece at a time

namespace {

class ConvertedSource : public ImageSource {
public:
    ConvertedSource(Source s, SampleFormat target, double lower, double upper) : s_(std::move(s)), lower_(lower), upper_(upper) {
        width = s_->width;
        height = s_->height;
        channels = s_->channels;
        format = target;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        const size_t n = static_cast<size_t>(rows * width);
        in_.resize(n * sampleBytes(s_->format));
        s_->readRows(channel, y, rows, in_.data());
        convertSamples(in_.data(), s_->format, out, format, n, lower_, upper_);
    }

private:
    Source s_;
    double lower_, upper_;
    std::vector<uint8_t> in_;
};

class StretchedSource : public ImageSource {
public:
    StretchedSource(Source s, std::vector<StretchParams> params, double lower, double upper)
        : s_(std::move(s)), params_(std::move(params)), lower_(lower), upper_(upper) {
        width = s_->width;
        height = s_->height;
        channels = s_->channels;
        format = SampleFormat::Float32;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        const size_t n = static_cast<size_t>(rows * width);
        in_.resize(n * sampleBytes(s_->format));
        s_->readRows(channel, y, rows, in_.data());
        stretchSamples(in_.data(), s_->format, n, channel < params_.size() ? &params_[channel] : nullptr, lower_, upper_,
                       reinterpret_cast<float*>(out));
    }

private:
    Source s_;
    std::vector<StretchParams> params_;
    double lower_, upper_;
    std::vector<uint8_t> in_;
};

class NormalizedSource : public ImageSource {
public:
    NormalizedSource(Source s, double lower, double upper) : s_(std::move(s)), lower_(lower), upper_(upper) {
        width = s_->width;
        height = s_->height;
        channels = s_->channels;
        format = s_->format;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        s_->readRows(channel, y, rows, out);
        normalizeSamples(out, format, static_cast<size_t>(rows * width), lower_, upper_);
    }

private:
    Source s_;
    double lower_, upper_;
};

// Rows of one channel of a source, a band of them at hand at a time.
class RowCache {
public:
    RowCache(ImageSource& s, uint64_t channel) : s_(s), channel_(channel), rows_(std::max<uint64_t>(3, rowsPerPiece(s.rowBytes()))) {}
    const uint8_t* row(uint64_t y) {
        if (y < first_ || y >= first_ + count_) {
            // (going on to the next band, the row before it is wanted again too: the band begins there)
            first_ = count_ && y == first_ + count_ ? y - 1 : y;
            count_ = std::min(rows_, s_.height - first_);
            band_.resize(static_cast<size_t>(count_ * s_.rowBytes()));
            s_.readRows(channel_, first_, count_, band_.data());
        }
        return band_.data() + (y - first_) * s_.rowBytes();
    }

private:
    ImageSource& s_;
    uint64_t channel_, rows_;
    uint64_t first_ = 0, count_ = 0;
    std::vector<uint8_t> band_;
};

class DebayeredSource : public ImageSource {
public:
    DebayeredSource(Source s, const std::string& pattern) : s_(std::move(s)) {
        if (s_->channels != 1) throw Error("--debayer: the image has " + std::to_string(s_->channels) + " channels, not one", ErrorKind::Argument);
        if (s_->width < 2 || s_->height < 2) throw Error("--debayer: an image smaller than 2 x 2 pixels", ErrorKind::Argument);
        patternColours(pattern, colour_);
        width = s_->width;
        height = s_->height;
        channels = 3;
        format = s_->format;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        const size_t plane = static_cast<size_t>(rows * rowBytes());
        if (!(y == y_ && rows == rows_)) {
            // the three planes of the band at once (TIFF and PNG ask for the channels of a band one
            // after the other)
            planes_.resize(3 * plane);
            // the rows of the band and one above and below it, which are looked at three at a time
            const uint64_t from = y ? y - 1 : 0, to = std::min(height, y + rows + 1);
            mosaic_.resize(static_cast<size_t>((to - from) * rowBytes()));
            s_->readRows(0, from, to - from, mosaic_.data());
            withType(format, [&](auto* type) {
                using T = std::remove_pointer_t<decltype(type)>;
                const T* first = reinterpret_cast<const T*>(mosaic_.data());
                debayerPlanes([&](uint64_t yy) { return first + (yy - from) * width; }, width, height, y, y + rows, colour_,
                              reinterpret_cast<T*>(planes_.data()));
            });
            y_ = y;
            rows_ = rows;
        }
        std::memcpy(out, planes_.data() + channel * plane, plane);
    }

private:
    Source s_;
    int colour_[2][2];
    std::vector<uint8_t> planes_, mosaic_;
    uint64_t y_ = std::numeric_limits<uint64_t>::max(), rows_ = 0;
};

class DownsampledSource : public ImageSource {
public:
    DownsampledSource(Source s, const DownsampledSize& size) : s_(std::move(s)) {
        if (size.useWidth > s_->width || size.useHeight > s_->height || size.width > size.useWidth || size.height > size.useHeight) {
            throw Error("a picture cannot be larger than the image it is made of");
        }
        columns_ = spans(size.useWidth, size.width);
        rows_ = spans(size.useHeight, size.height);
        width = size.width;
        height = size.height;
        channels = s_->channels;
        format = s_->format;
    }
    void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) override {
        // (a band of the channels asked for last is kept: the next piece of a channel begins where
        // its last one ended; TIFF and PNG ask for the channels of a band one after the other)
        auto found = std::find_if(caches_.begin(), caches_.end(), [&](const Cache& c) { return c.channel == channel; });
        if (found == caches_.end()) {
            if (caches_.size() >= 4) caches_.erase(caches_.begin());
            caches_.push_back({channel, std::make_unique<RowCache>(*s_, channel)});
            found = caches_.end() - 1;
        }
        RowCache& image = *found->rows;
        withType(format, [&](auto* type) {
            using T = std::remove_pointer_t<decltype(type)>;
            downsamplePlane([&](uint64_t yy) { return reinterpret_cast<const T*>(image.row(yy)); }, reinterpret_cast<T*>(out), columns_,
                            rows_.data() + y, static_cast<size_t>(rows));
        });
    }

private:
    Source s_;
    std::vector<Span> columns_, rows_;
    struct Cache {
        uint64_t channel;
        std::unique_ptr<RowCache> rows;
    };
    std::vector<Cache> caches_;   // of at most four channels, in the order they came (the first goes first)
};

}  // namespace

Source convertedSource(Source source, SampleFormat target, double lower, double upper) {
    if (source->format == target) return source;
    return std::make_shared<ConvertedSource>(std::move(source), target, lower, upper);
}

Source stretchedSource(Source source, const std::vector<StretchParams>& params, double lower, double upper) {
    return std::make_shared<StretchedSource>(std::move(source), params, lower, upper);
}

Source normalizedSource(Source source, double lower, double upper) {
    if (!isFloat(source->format) || !(upper > lower) || (lower == 0 && upper == 1)) return source;
    return std::make_shared<NormalizedSource>(std::move(source), lower, upper);
}

Source debayeredSource(Source source, const std::string& pattern) {
    return std::make_shared<DebayeredSource>(std::move(source), pattern);
}

Source downsampledSource(Source source, const DownsampledSize& size) {
    if (!size.changes || size.width == 0 || size.height == 0 || source->width == 0 || source->height == 0) return source;
    return std::make_shared<DownsampledSource>(std::move(source), size);
}

}  // namespace xisfconv
