// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "convert.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

void convertSampleFormat(PixelBuffer& px, SampleFormat target, double lower, double upper) {
    if (px.format == target) return;
    if (!(upper > lower)) { lower = 0; upper = 1; }
    const size_t n = static_cast<size_t>(px.samples());
    std::vector<uint8_t> out(n * sampleBytes(target));
    const uint8_t* src = px.data.data();
    uint8_t* dst = out.data();
    switch (px.format) {
        case SampleFormat::UInt8: dispatchTarget<uint8_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::UInt16: dispatchTarget<uint16_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::UInt32: dispatchTarget<uint32_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::UInt64: dispatchTarget<uint64_t>(src, dst, n, target, lower, upper); break;
        case SampleFormat::Float32: dispatchTarget<float>(src, dst, n, target, lower, upper); break;
        case SampleFormat::Float64: dispatchTarget<double>(src, dst, n, target, lower, upper); break;
    }
    px.data.swap(out);
    px.format = target;
}

namespace {

// Reads channel `c`, normalized to [0,1] (integers by their full range, floats by [lower,upper]).
// With step > 1 only every step-th sample is read (for statistics).
template <class T>
void readNormalized(const PixelBuffer& px, uint64_t c, double lower, double upper, size_t step,
                    std::vector<float>& out) {
    const size_t n = static_cast<size_t>(px.planeSamples());
    const T* src = reinterpret_cast<const T*>(px.data.data()) + c * n;
    double offset = 0, scale = 1;
    if (std::is_floating_point<T>::value) {
        offset = lower;
        scale = 1.0 / (upper - lower);
    } else {
        scale = 1.0 / maxValue<T>();
    }
    out.clear();
    out.reserve(n / step + 1);
    for (size_t i = 0; i < n; i += step) {
        double v = (static_cast<double>(src[i]) - offset) * scale;
        if (!(v > 0)) v = 0;  // also NaN
        if (v > 1) v = 1;
        out.push_back(static_cast<float>(v));
    }
}

void channelNormalized(const PixelBuffer& px, uint64_t c, double lower, double upper, size_t step,
                       std::vector<float>& out) {
    if (!(upper > lower)) { lower = 0; upper = 1; }
    switch (px.format) {
        case SampleFormat::UInt8: readNormalized<uint8_t>(px, c, lower, upper, step, out); break;
        case SampleFormat::UInt16: readNormalized<uint16_t>(px, c, lower, upper, step, out); break;
        case SampleFormat::UInt32: readNormalized<uint32_t>(px, c, lower, upper, step, out); break;
        case SampleFormat::UInt64: readNormalized<uint64_t>(px, c, lower, upper, step, out); break;
        case SampleFormat::Float32: readNormalized<float>(px, c, lower, upper, step, out); break;
        case SampleFormat::Float64: readNormalized<double>(px, c, lower, upper, step, out); break;
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

std::vector<StretchParams> autoStretch(const PixelBuffer& px, double lower, double upper, size_t colorChannels,
                                       bool linked) {
    constexpr double kShadowsClip = -2.8;     // in units of normalized MAD
    constexpr double kTargetBackground = 0.25;
    constexpr size_t kMaxSamples = 4u << 20;  // statistics on at most ~4M samples per channel

    const size_t channels = std::min<size_t>(colorChannels, static_cast<size_t>(px.channels));
    const size_t n = static_cast<size_t>(px.planeSamples());
    const size_t step = std::max<size_t>(1, n / kMaxSamples);
    std::vector<double> med(channels), madn(channels);
    std::vector<float> buf;
    for (size_t c = 0; c < channels; ++c) {
        channelNormalized(px, c, lower, upper, step, buf);
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

void applyStretch(PixelBuffer& px, const std::vector<StretchParams>& params, double lower, double upper) {
    const size_t n = static_cast<size_t>(px.planeSamples());
    std::vector<uint8_t> outBytes(static_cast<size_t>(px.samples()) * sizeof(float));
    float* out = reinterpret_cast<float*>(outBytes.data());
    std::vector<float> buf;
    for (uint64_t c = 0; c < px.channels; ++c) {
        channelNormalized(px, c, lower, upper, 1, buf);
        float* dst = out + c * n;
        if (c >= params.size()) {
            std::copy(buf.begin(), buf.end(), dst);
            continue;
        }
        const StretchParams& p = params[c];
        const double range = p.highlights - p.shadows;
        const double erange = p.high - p.low;
        for (size_t i = 0; i < n; ++i) {
            double x = range > 0 ? (buf[i] - p.shadows) / range : (buf[i] >= p.highlights ? 1.0 : 0.0);
            x = mtf(p.midtones, std::min(1.0, std::max(0.0, x)));
            if (erange > 0 && (p.low != 0 || p.high != 1)) x = (x - p.low) / erange;
            dst[i] = static_cast<float>(std::min(1.0, std::max(0.0, x)));
        }
    }
    px.data.swap(outBytes);
    px.format = SampleFormat::Float32;
}

void normalizeFloat(PixelBuffer& px, double lower, double upper) {
    if (!isFloat(px.format) || !(upper > lower) || (lower == 0 && upper == 1)) return;
    const size_t n = static_cast<size_t>(px.samples());
    const double scale = 1.0 / (upper - lower);
    if (px.format == SampleFormat::Float32) {
        float* p = reinterpret_cast<float*>(px.data.data());
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<float>((static_cast<double>(p[i]) - lower) * scale);
    } else {
        double* p = reinterpret_cast<double*>(px.data.data());
        for (size_t i = 0; i < n; ++i) p[i] = (p[i] - lower) * scale;
    }
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

template <class T>
void downsamplePlane(const T* src, uint64_t stride, T* dst, const std::vector<Span>& columns, const std::vector<Span>& rows) {
    // (the type is asked for where it is needed: a constant of this function is not one inside the lambda
    // below for every compiler)
    const size_t outW = columns.size();
    // one row of the image, made as narrow as the picture: the sums, and the shares that went into them
    std::vector<double> rowSum(outW), rowShare(outW), sum(outW), share(outW);
    uint64_t reduced = std::numeric_limits<uint64_t>::max();   // the row of the image that rowSum holds
    auto reduce = [&](uint64_t y) {
        if (y == reduced) return;   // (the last row of one pixel of the picture is often the first of the next)
        const T* line = src + y * stride;
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
    for (size_t r = 0; r < rows.size(); ++r) {
        const Span& rowSpan = rows[r];
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
                        const T* line = src + (rowSpan.first + ky) * stride;
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
        switch (px.format) {
            case SampleFormat::UInt8: downsamplePlane(src, px.width, dst, columns, rows); break;
            case SampleFormat::UInt16:
                downsamplePlane(reinterpret_cast<const uint16_t*>(src), px.width, reinterpret_cast<uint16_t*>(dst), columns, rows);
                break;
            case SampleFormat::UInt32:
                downsamplePlane(reinterpret_cast<const uint32_t*>(src), px.width, reinterpret_cast<uint32_t*>(dst), columns, rows);
                break;
            case SampleFormat::UInt64:
                downsamplePlane(reinterpret_cast<const uint64_t*>(src), px.width, reinterpret_cast<uint64_t*>(dst), columns, rows);
                break;
            case SampleFormat::Float32:
                downsamplePlane(reinterpret_cast<const float*>(src), px.width, reinterpret_cast<float*>(dst), columns, rows);
                break;
            case SampleFormat::Float64:
                downsamplePlane(reinterpret_cast<const double*>(src), px.width, reinterpret_cast<double*>(dst), columns, rows);
                break;
        }
    }
    px.data.swap(out);
    px.width = size.width;
    px.height = size.height;
}

}  // namespace xisfconv
