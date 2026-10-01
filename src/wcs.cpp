// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "wcs.hpp"

#include <cmath>
#include <cstdio>

#include "fits.hpp"

namespace xisfconv {

namespace {

const std::string kPrefix = "PCL:AstrometricSolution:";

struct Term { int p, q; };

std::vector<Term> sipTerms(int minOrder, int maxOrder) {
    std::vector<Term> t;
    for (int order = minOrder; order <= maxOrder; ++order)
        for (int p = order; p >= 0; --p) t.push_back({p, order - p});
    return t;
}

// Least squares via normal equations (k is small, inputs are pre-scaled). Returns false if singular.
bool leastSquares(const std::vector<std::vector<double>>& X, const std::vector<double>& y, std::vector<double>& c) {
    const size_t k = X.empty() ? 0 : X[0].size();
    std::vector<std::vector<double>> A(k, std::vector<double>(k + 1, 0.0));
    for (size_t r = 0; r < X.size(); ++r) {
        for (size_t i = 0; i < k; ++i) {
            for (size_t j = 0; j < k; ++j) A[i][j] += X[r][i] * X[r][j];
            A[i][k] += X[r][i] * y[r];
        }
    }
    for (size_t col = 0; col < k; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < k; ++r)
            if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
        if (std::fabs(A[piv][col]) < 1e-300) return false;
        std::swap(A[col], A[piv]);
        for (size_t r = 0; r < k; ++r) {
            if (r == col) continue;
            const double f = A[r][col] / A[col][col];
            for (size_t j = col; j <= k; ++j) A[r][j] -= f * A[col][j];
        }
    }
    c.resize(k);
    for (size_t i = 0; i < k; ++i) c[i] = A[i][k] / A[i][i];
    return true;
}

double evalPoly(const std::vector<Term>& terms, const std::vector<double>& coef, double x, double y) {
    double s = 0;
    for (size_t i = 0; i < terms.size(); ++i) s += coef[i] * std::pow(x, terms[i].p) * std::pow(y, terms[i].q);
    return s;
}

// Fits target = poly(x, y) over `terms`, with coordinates scaled by `scale` for conditioning.
// Returns coefficients in unscaled units.
bool fitPoly(const std::vector<Term>& terms, const std::vector<double>& xs, const std::vector<double>& ys,
             const std::vector<double>& target, double scale, std::vector<double>& coef) {
    std::vector<std::vector<double>> X(xs.size(), std::vector<double>(terms.size()));
    for (size_t r = 0; r < xs.size(); ++r)
        for (size_t i = 0; i < terms.size(); ++i)
            X[r][i] = std::pow(xs[r] / scale, terms[i].p) * std::pow(ys[r] / scale, terms[i].q);
    if (!leastSquares(X, target, coef)) return false;
    for (size_t i = 0; i < terms.size(); ++i) coef[i] /= std::pow(scale, terms[i].p + terms[i].q);
    return true;
}

std::string projectionCode(const std::string& name) {
    static const struct { const char* pi; const char* fits; } map[] = {
        {"Gnomonic", "TAN"},       {"Stereographic", "STG"},     {"PlateCarree", "CAR"}, {"Mercator", "MER"},
        {"HammerAitoff", "AIT"},   {"ZenithalEqualArea", "ZEA"}, {"Orthographic", "SIN"},
        {"ZenithalEqualDistance", "ARC"},
    };
    for (const auto& m : map)
        if (name == m.pi) return m.fits;
    return {};
}

}  // namespace

bool astrometricSolutionToWcs(XisfFile& file, size_t index, bool bottomUp, int sipOrder, WcsResult& out) {
    const XisfImage& img = file.images().at(index);
    const XisfProperty* proj = file.findProperty(index, kPrefix + "ProjectionSystem");
    if (!proj) return false;
    const std::string code = projectionCode(trim(proj->value));
    if (code.empty()) {
        warn("astrometric solution uses projection '" + proj->value + "', which has no FITS WCS mapping here; "
             "no WCS keywords written");
        return false;
    }

    std::vector<double> refCel, refImg, M, poleNative, refNative;
    size_t rows = 0, cols = 0;
    if (!file.readNumericProperty(index, kPrefix + "ReferenceCelestialCoordinates", refCel) || refCel.size() != 2 ||
        !file.readNumericProperty(index, kPrefix + "ReferenceImageCoordinates", refImg) || refImg.size() != 2 ||
        !file.readNumericProperty(index, kPrefix + "LinearTransformationMatrix", M, &rows, &cols) || rows != 2 ||
        cols != 2) {
        warn("incomplete astrometric solution; no WCS keywords written");
        return false;
    }
    if (file.readNumericProperty(index, kPrefix + "ReferenceNativeCoordinates", refNative) && refNative.size() == 2 &&
        (std::fabs(refNative[0]) > 1e-9 || std::fabs(refNative[1] - 90) > 1e-9)) {
        warn("astrometric solution has an unusual native reference point; WCS may be inaccurate");
    }
    double lonpole = 180;
    if (file.readNumericProperty(index, kPrefix + "CelestialPoleNativeCoordinates", poleNative) && poleNative.size() == 2)
        lonpole = poleNative[0];

    // PixInsight image coordinates: origin at the top-left corner of the top-left pixel, y down,
    // pixel centers at +0.5. FITS: pixel centers at integers starting at 1, and with bottom-up
    // storage the first row is the bottom row.
    const double H = static_cast<double>(img.height);
    const double ySign = bottomUp ? -1.0 : 1.0;
    auto toFits = [&](double x, double y, double& i, double& j) {
        i = x + 0.5;
        j = bottomUp ? H + 0.5 - y : y + 0.5;
    };
    double crpix1, crpix2;
    toFits(refImg[0], refImg[1], crpix1, crpix2);
    const double cd11 = M[0], cd12 = ySign * M[1], cd21 = M[2], cd22 = ySign * M[3];

    // Optional SIP distortion. PixInsight models distortion with a thin-plate spline; its
    // image->native point grid is sampled (or, without a grid, the star control points are used)
    // and a polynomial is fitted. The constant and linear parts of the fit are folded back into
    // CRPIX/CD so that the SIP terms carry only the non-linear distortion, as the convention expects.
    std::vector<Term> fwdTerms, invTerms;
    std::vector<double> A, B, AP, BP;
    double rmsLinear = -1, rmsSip = -1, maxInverse = 0;
    size_t nStars = 0;
    std::string sampleSource;
    double crpix1Sip = crpix1, crpix2Sip = crpix2, cd[4] = {cd11, cd12, cd21, cd22};

    std::vector<double> cpImg, cpWorld;
    const bool haveStars =
        file.readNumericProperty(index, kPrefix + "SplineWorldTransformation:ControlPoints:Image", cpImg) &&
        file.readNumericProperty(index, kPrefix + "SplineWorldTransformation:ControlPoints:World", cpWorld) &&
        cpImg.size() == cpWorld.size() && cpImg.size() % 2 == 0 && !cpImg.empty();
    if (haveStars) nStars = cpImg.size() / 2;

    if (sipOrder >= 2) {
        std::vector<double> si, sj, su, sv;  // samples: FITS pixel coordinates -> native (u, v) [deg]
        const std::string g = kPrefix + "SplineWorldTransformation:PointGridInterpolation:ImageToNative:";
        std::vector<double> gx, gy, rect;
        size_t gr = 0, gc = 0, gr2 = 0, gc2 = 0;
        const XisfProperty* deltaProp = file.findProperty(index, g + "Delta");
        double delta = 0;
        if (deltaProp && parseDouble(deltaProp->value, delta) && delta > 0 &&
            file.readNumericProperty(index, g + "GridX", gx, &gr, &gc) &&
            file.readNumericProperty(index, g + "GridY", gy, &gr2, &gc2) && gr == gr2 && gc == gc2 &&
            file.readNumericProperty(index, g + "Rect", rect) && rect.size() == 4 && gr > 1 && gc > 1) {
            const size_t step = std::max<size_t>(1, static_cast<size_t>(std::sqrt(double(gr * gc) / 6000.0)));
            for (size_t r = 0; r < gr; r += step) {
                for (size_t c = 0; c < gc; c += step) {
                    const double x = rect[0] + double(c) * delta, y = rect[1] + double(r) * delta;
                    if (x > rect[2] || y > rect[3]) continue;  // the grid may overhang the image
                    double i, j;
                    toFits(x, y, i, j);
                    si.push_back(i); sj.push_back(j);
                    su.push_back(gx[r * gc + c]); sv.push_back(gy[r * gc + c]);
                }
            }
            sampleSource = "spline grid";
        } else if (haveStars) {
            for (size_t k = 0; k < nStars; ++k) {
                double i, j;
                toFits(cpImg[2 * k], cpImg[2 * k + 1], i, j);
                si.push_back(i); sj.push_back(j);
                su.push_back(cpWorld[2 * k]); sv.push_back(cpWorld[2 * k + 1]);
            }
            sampleSource = "control points";
        }

        fwdTerms = sipTerms(2, sipOrder);
        invTerms = sipTerms(1, sipOrder);
        const size_t n = si.size();
        const double scale = std::max(static_cast<double>(img.width), H) / 2;
        bool ok = n >= 3 * sipTerms(0, sipOrder).size();
        std::vector<double> dx(n), dy(n), qx(n), qy(n), fx(n), fy(n);
        auto prepare = [&]() {
            const double det = cd[0] * cd[3] - cd[1] * cd[2];
            for (size_t k = 0; k < n; ++k) {
                dx[k] = si[k] - crpix1Sip;
                dy[k] = sj[k] - crpix2Sip;
                qx[k] = (cd[3] * su[k] - cd[1] * sv[k]) / det;
                qy[k] = (-cd[2] * su[k] + cd[0] * sv[k]) / det;
                fx[k] = qx[k] - dx[k];
                fy[k] = qy[k] - dy[k];
            }
        };
        // Fold constant + linear terms of a full fit into CRPIX/CD (a few passes converge).
        const std::vector<Term> full = sipTerms(0, sipOrder);
        for (int pass = 0; ok && pass < 4; ++pass) {
            prepare();
            std::vector<double> cx, cy;
            if (!fitPoly(full, dx, dy, fx, scale, cx) || !fitPoly(full, dx, dy, fy, scale, cy)) { ok = false; break; }
            // full[0] = (0,0); full[1] = (1,0) -> x; full[2] = (0,1) -> y
            const double l00 = 1 + cx[1], l01 = cx[2], l10 = cy[1], l11 = 1 + cy[2];
            const double ldet = l00 * l11 - l01 * l10;
            if (std::fabs(ldet) < 1e-12) { ok = false; break; }
            // crpix' = crpix - (I+L)^-1 c0
            crpix1Sip -= (l11 * cx[0] - l01 * cy[0]) / ldet;
            crpix2Sip -= (-l10 * cx[0] + l00 * cy[0]) / ldet;
            const double n0 = cd[0] * l00 + cd[1] * l10, n1 = cd[0] * l01 + cd[1] * l11;
            const double n2 = cd[2] * l00 + cd[3] * l10, n3 = cd[2] * l01 + cd[3] * l11;
            cd[0] = n0; cd[1] = n1; cd[2] = n2; cd[3] = n3;
        }
        if (ok) {
            prepare();
            std::vector<double> gxr(n), gyr(n), qpx(n), qpy(n);
            ok = fitPoly(fwdTerms, dx, dy, fx, scale, A) && fitPoly(fwdTerms, dx, dy, fy, scale, B);
            if (ok) {
                // inverse polynomials: from distorted offsets back to pixel offsets
                for (size_t k = 0; k < n; ++k) {
                    qpx[k] = dx[k] + evalPoly(fwdTerms, A, dx[k], dy[k]);
                    qpy[k] = dy[k] + evalPoly(fwdTerms, B, dx[k], dy[k]);
                    gxr[k] = dx[k] - qpx[k];
                    gyr[k] = dy[k] - qpy[k];
                }
                ok = fitPoly(invTerms, qpx, qpy, gxr, scale, AP) && fitPoly(invTerms, qpx, qpy, gyr, scale, BP);
            }
            if (ok) {
                for (size_t k = 0; k < n; ++k) {
                    const double bx = qpx[k] + evalPoly(invTerms, AP, qpx[k], qpy[k]) - dx[k];
                    const double by = qpy[k] + evalPoly(invTerms, BP, qpx[k], qpy[k]) - dy[k];
                    maxInverse = std::max(maxInverse, std::hypot(bx, by));
                }
            }
        }
        if (!ok) A.clear();
    }

    // Quality against the matched stars, for the linear solution and for the SIP model.
    if (haveStars) {
        double ssLin = 0, ssSip = 0;
        for (size_t k = 0; k < nStars; ++k) {
            double i, j;
            toFits(cpImg[2 * k], cpImg[2 * k + 1], i, j);
            const double u = cpWorld[2 * k], v = cpWorld[2 * k + 1];
            double ex = cd11 * (i - crpix1) + cd12 * (j - crpix2) - u;
            double ey = cd21 * (i - crpix1) + cd22 * (j - crpix2) - v;
            ssLin += ex * ex + ey * ey;
            if (!A.empty()) {
                const double dx = i - crpix1Sip, dy = j - crpix2Sip;
                const double px = dx + evalPoly(fwdTerms, A, dx, dy), py = dy + evalPoly(fwdTerms, B, dx, dy);
                ex = cd[0] * px + cd[1] * py - u;
                ey = cd[2] * px + cd[3] * py - v;
                ssSip += ex * ex + ey * ey;
            }
        }
        rmsLinear = std::sqrt(ssLin / double(nStars)) * 3600;
        if (!A.empty()) {
            rmsSip = std::sqrt(ssSip / double(nStars)) * 3600;
            if (!(rmsSip < rmsLinear)) A.clear();  // no improvement: keep the linear solution
        }
    }
    const bool sip = !A.empty();

    auto kw = [&](const std::string& name, const std::string& value, const std::string& comment) {
        out.keywords.push_back({name, value, comment});
    };
    const char* src = " (PixInsight astrometric solution)";
    kw("WCSAXES", "2", "number of WCS axes");
    kw("CTYPE1", fitsString("RA---" + code + (sip ? "-SIP" : "")), std::string("projection") + src);
    kw("CTYPE2", fitsString("DEC--" + code + (sip ? "-SIP" : "")), "projection");
    kw("CUNIT1", fitsString("deg"), "");
    kw("CUNIT2", fitsString("deg"), "");
    kw("CRVAL1", fitsReal(refCel[0]), "RA of reference point [deg]");
    kw("CRVAL2", fitsReal(refCel[1]), "Dec of reference point [deg]");
    kw("CRPIX1", fitsReal(sip ? crpix1Sip : crpix1), "reference pixel, x");
    kw("CRPIX2", fitsReal(sip ? crpix2Sip : crpix2), "reference pixel, y");
    kw("CD1_1", fitsReal(sip ? cd[0] : cd11), "linear transformation [deg/px]");
    kw("CD1_2", fitsReal(sip ? cd[1] : cd12), "");
    kw("CD2_1", fitsReal(sip ? cd[2] : cd21), "");
    kw("CD2_2", fitsReal(sip ? cd[3] : cd22), "");
    kw("LONPOLE", fitsReal(lonpole), "native longitude of celestial pole [deg]");
    if (const XisfProperty* rs = file.findProperty(index, "Observation:CelestialReferenceSystem")) {
        const std::string sys = toUpper(trim(rs->value));
        if (sys == "ICRS" || sys == "FK5" || sys == "FK4") kw("RADESYS", fitsString(sys), "celestial reference system");
        if (sys == "FK5" || sys == "FK4") {
            const XisfProperty* eq = file.findProperty(index, "Observation:Equinox");
            double e;
            if (eq && parseDouble(eq->value, e)) kw("EQUINOX", fitsReal(e), "equinox [yr]");
        }
    } else {
        kw("RADESYS", fitsString("ICRS"), "celestial reference system (assumed)");
    }
    if (sip) {
        auto emit = [&](const char* prefix, int order, const std::vector<Term>& terms, const std::vector<double>& c) {
            kw(std::string(prefix) + "_ORDER", std::to_string(order), "SIP polynomial order");
            for (size_t i = 0; i < terms.size(); ++i) {
                kw(std::string(prefix) + "_" + std::to_string(terms[i].p) + "_" + std::to_string(terms[i].q),
                   fitsReal(c[i]), "");
            }
        };
        emit("A", sipOrder, fwdTerms, A);
        emit("B", sipOrder, fwdTerms, B);
        emit("AP", sipOrder, invTerms, AP);
        emit("BP", sipOrder, invTerms, BP);
    }

    char buf[256];
    const double scaleArcsec = std::sqrt(std::fabs(cd11 * cd22 - cd12 * cd21)) * 3600;
    if (sip) {
        std::snprintf(buf, sizeof buf, "WCS %s %.3f\"/px, SIP order %d from %s; vs %zu stars rms %.2f\" "
                      "(linear %.2f\"), inverse max err %.3f px", code.c_str(), scaleArcsec, sipOrder,
                      sampleSource.c_str(), nStars, rmsSip, rmsLinear, maxInverse);
    } else if (rmsLinear >= 0) {
        std::snprintf(buf, sizeof buf, "WCS %s %.3f\"/px, linear only: rms %.2f\" over %zu stars", code.c_str(),
                      scaleArcsec, rmsLinear, nStars);
    } else {
        std::snprintf(buf, sizeof buf, "WCS %s %.3f\"/px, linear only", code.c_str(), scaleArcsec);
    }
    out.summary = buf;
    return true;
}

}  // namespace xisfconv
