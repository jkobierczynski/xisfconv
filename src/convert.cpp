// SPDX-License-Identifier: GPL-3.0-or-later
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

}  // namespace xisfconv
