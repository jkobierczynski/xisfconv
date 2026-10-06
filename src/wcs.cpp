// SPDX-License-Identifier: LGPL-3.0-or-later
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

namespace {

bool numericValue(const std::string& text, double& out) {
    std::string v = trim(text);
    if (v.empty() || v[0] == '\'') return false;
    for (auto& c : v)
        if (c == 'D' || c == 'd') c = 'E';
    return parseDouble(v, out);
}

// Negates a numeric keyword value by editing its sign, so no digits are lost.
void negateValue(FitsKeyword& k) {
    double v;
    if (!numericValue(k.value, v) || v == 0) return;
    std::string t = trim(k.value);
    if (t[0] == '-') t.erase(0, 1);
    else if (t[0] == '+') t[0] = '-';
    else t.insert(0, "-");
    k.value = t;
}

// Parses SIP coefficient names: A_p_q, B_p_q, AP_p_q, BP_p_q. Returns false for *_ORDER etc.
bool sipIndices(const std::string& name, bool& isB, int& q) {
    size_t prefix;
    if (name.compare(0, 3, "AP_") == 0) { isB = false; prefix = 3; }
    else if (name.compare(0, 3, "BP_") == 0) { isB = true; prefix = 3; }
    else if (name.compare(0, 2, "A_") == 0) { isB = false; prefix = 2; }
    else if (name.compare(0, 2, "B_") == 0) { isB = true; prefix = 2; }
    else return false;
    const auto parts = split(name.substr(prefix), '_');
    uint64_t p = 0, qq = 0;
    if (parts.size() != 2 || !parseUInt64(parts[0], p) || !parseUInt64(parts[1], qq)) return false;
    q = static_cast<int>(qq);
    return true;
}

// Splits the name of a WCS keyword into its base and the letter of an alternate description
// (CRPIX2A -> CRPIX2, 'A'); ' ' for the primary one. Only the keywords that depend on the
// direction of the second axis are recognized.
bool rowKeyword(const std::string& n, std::string& base, char& alt) {
    alt = ' ';
    base = n;
    auto known = [](const std::string& b) {
        return b == "CRPIX2" || b == "CDELT2" || b == "CTYPE1" || b == "PC001002" || b == "PC002002" || b == "PC001001" ||
               b == "PC002001" || ((b.compare(0, 2, "CD") == 0 || b.compare(0, 2, "PC") == 0) && b.size() == 5 && b[3] == '_' &&
                                   b[2] >= '1' && b[2] <= '9' && b[4] >= '1' && b[4] <= '9');
    };
    if (known(base)) return true;
    if (n.size() > 1 && n.back() >= 'A' && n.back() <= 'Z') {
        base = n.substr(0, n.size() - 1);
        alt = n.back();
        // the old PCiiijjj form has no alternates
        if (base.compare(0, 4, "PC00") != 0 && known(base)) return true;
    }
    return false;
}

std::string rowNumber(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15G", v);
    std::string s = cNumber(buf);
    if (s.find_first_of(".E") == std::string::npos) s += ".0";
    return s;
}

}  // namespace

bool flipWcsRowOrder(std::vector<FitsKeyword>& keywords, uint64_t height) {
    // What each description (the primary one and the alternates A to Z) consists of.
    struct Description {
        bool present = false, hasCd = false, hasPc = false, hasCrpix2 = false;
        size_t ctypeAt = 0;
    };
    Description descriptions[27];
    auto of = [&](char alt) -> Description& { return descriptions[alt == ' ' ? 0 : 1 + (alt - 'A')]; };
    bool hasWcs = false;
    for (size_t i = 0; i < keywords.size(); ++i) {
        const FitsKeyword& k = keywords[i];
        const std::string n = toUpper(trim(k.name));
        std::string base;
        char alt;
        if (!rowKeyword(n, base, alt)) continue;
        Description& d = of(alt);
        if (base == "CRPIX2") d.present = d.hasCrpix2 = true;
        if (base == "CTYPE1") {
            d.present = true;
            d.ctypeAt = i;
            const std::string t = toUpper(k.value);
            if (t.find("TPV") != std::string::npos || t.find("TNX") != std::string::npos ||
                t.find("ZPX") != std::string::npos) {
                warn("WCS uses a distortion model other than SIP; its coefficients are not adjusted for the "
                     "changed row order");
            }
        }
        if (base.compare(0, 2, "CD") == 0 && base.size() == 5) d.hasCd = true;
        if (base.compare(0, 2, "PC") == 0) d.hasPc = true;
        if (d.present) hasWcs = true;
    }
    if (!hasWcs) return false;
    for (auto& k : keywords) {
        const std::string n = toUpper(trim(k.name));
        double v;
        bool isB = false;
        int q = 0;
        std::string base;
        char alt;
        if (rowKeyword(n, base, alt)) {
            const Description& d = of(alt);
            if (base == "CRPIX2") {
                if (numericValue(k.value, v)) k.value = rowNumber(static_cast<double>(height) + 1.0 - v);
            } else if (base == "CD1_2" || base == "CD2_2" || base == "PC1_2" || base == "PC2_2" || base == "PC001002" ||
                       base == "PC002002") {
                negateValue(k);
            } else if (base == "CDELT2" && !d.hasCd && !d.hasPc) {
                negateValue(k);
            }
        } else if (sipIndices(n, isB, q)) {
            // v -> -v: x-distortion terms change sign for odd powers of v, y-distortion terms for even ones.
            if ((q % 2 == 1) != isB) negateValue(k);
        }
    }
    // A description without CRPIX2 has its reference point at row 0, which is row height + 1
    // counted from the other end. Inserted from the back, so that the positions noted stay valid.
    for (int a = 26; a >= 0; --a) {
        const Description& d = descriptions[a];
        if (!d.present || d.hasCrpix2) continue;
        const std::string name = a == 0 ? std::string("CRPIX2") : std::string("CRPIX2") + static_cast<char>('A' + a - 1);
        keywords.insert(keywords.begin() + static_cast<std::ptrdiff_t>(d.ctypeAt + 1),
                        {name, rowNumber(static_cast<double>(height) + 1.0), "reference pixel (row order changed)"});
    }
    return true;
}

namespace {

const FitsKeyword* findCard(const std::vector<FitsKeyword>& kw, const std::string& name) {
    for (const auto& k : kw)
        if (toUpper(trim(k.name)) == name) return &k;
    return nullptr;
}

bool cardNumber(const std::vector<FitsKeyword>& kw, const std::string& name, double& out) {
    const FitsKeyword* k = findCard(kw, name);
    return k && numericValue(k->value, out);
}

std::string cardString(const std::vector<FitsKeyword>& kw, const std::string& name) {
    const FitsKeyword* k = findCard(kw, name);
    if (!k) return {};
    std::string v = trim(k->value);
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') v = trim(v.substr(1, v.size() - 2));
    return v;
}

// PixInsight projection names for the zenithal projections (native reference point at the pole).
std::string projectionName(const std::string& code) {
    static const struct { const char* fits; const char* pi; } map[] = {
        {"TAN", "Gnomonic"}, {"STG", "Stereographic"}, {"ZEA", "ZenithalEqualArea"},
        {"SIN", "Orthographic"}, {"ARC", "ZenithalEqualDistance"},
    };
    for (const auto& m : map)
        if (code == m.fits) return m.pi;
    return {};
}

}  // namespace

bool wcsToAstrometricSolution(const std::vector<FitsKeyword>& kw, uint64_t width, uint64_t height,
                              std::vector<Property>& properties, std::string& summary) {
    const std::string ctype1 = toUpper(cardString(kw, "CTYPE1")), ctype2 = toUpper(cardString(kw, "CTYPE2"));
    if (ctype1.empty() || ctype2.empty()) return false;  // no WCS at all: nothing to say
    if (ctype1.compare(0, 4, "RA--") != 0 || ctype2.compare(0, 4, "DEC-") != 0 || ctype1.size() < 8 ||
        ctype2.size() < 8) {
        summary = "WCS axes are not RA/Dec in that order (" + ctype1 + ", " + ctype2 + ")";
        return false;
    }
    const std::string code = ctype1.substr(5, 3);
    const std::string projection = projectionName(code);
    if (projection.empty() || ctype2.substr(5, 3) != code) {
        summary = "projection " + code + " has no PixInsight solution mapping here";
        return false;
    }
    for (const char* unit : {"CUNIT1", "CUNIT2"}) {
        const std::string u = toLower(cardString(kw, unit));
        if (!u.empty() && u != "deg" && u != "degree" && u != "degrees") {
            summary = std::string(unit) + " is not degrees";
            return false;
        }
    }
    double crval1, crval2, crpix1, crpix2;
    if (!cardNumber(kw, "CRVAL1", crval1) || !cardNumber(kw, "CRVAL2", crval2) ||
        !cardNumber(kw, "CRPIX1", crpix1) || !cardNumber(kw, "CRPIX2", crpix2)) {
        summary = "WCS keywords are incomplete (CRVAL/CRPIX)";
        return false;
    }

    // Linear part as a CD matrix [deg/pixel], from CD, PC + CDELT, or CDELT + CROTA2.
    double cd[4] = {0, 0, 0, 0};
    const char* cdNames[4] = {"CD1_1", "CD1_2", "CD2_1", "CD2_2"};
    bool hasCd = false;
    for (int i = 0; i < 4; ++i) hasCd = cardNumber(kw, cdNames[i], cd[i]) || hasCd;
    if (!hasCd) {
        double cdelt1, cdelt2;
        if (!cardNumber(kw, "CDELT1", cdelt1) || !cardNumber(kw, "CDELT2", cdelt2)) {
            summary = "WCS keywords are incomplete (no CD matrix or CDELT)";
            return false;
        }
        double pc[4] = {1, 0, 0, 1};
        const char* pcNames[4] = {"PC1_1", "PC1_2", "PC2_1", "PC2_2"};
        bool hasPc = false;
        for (int i = 0; i < 4; ++i) hasPc = cardNumber(kw, pcNames[i], pc[i]) || hasPc;
        double crota = 0;
        if (!hasPc && cardNumber(kw, "CROTA2", crota)) {
            const double r = crota * 3.14159265358979323846 / 180;
            cd[0] = cdelt1 * std::cos(r);
            cd[1] = -cdelt2 * std::sin(r);
            cd[2] = cdelt1 * std::sin(r);
            cd[3] = cdelt2 * std::cos(r);
        } else {
            cd[0] = cdelt1 * pc[0];
            cd[1] = cdelt1 * pc[1];
            cd[2] = cdelt2 * pc[2];
            cd[3] = cdelt2 * pc[3];
        }
    }
    const double det = cd[0] * cd[3] - cd[1] * cd[2];
    if (!(std::fabs(det) > 0)) {
        summary = "WCS linear transformation is singular";
        return false;
    }

    // PixInsight image coordinates: origin at the top-left corner of the top-left pixel, y down.
    // The keywords use FITS pixel coordinates of the bottom-up image: i = x + 0.5, j = H + 0.5 - y.
    const double H = static_cast<double>(height), W = static_cast<double>(width);
    const double x0 = crpix1 - 0.5, y0 = H + 0.5 - crpix2;
    const double m[4] = {cd[0], -cd[1], cd[2], -cd[3]};  // native = m * (image - reference)

    // SIP distortion polynomials, if any.
    std::vector<Term> terms;
    std::vector<double> A, B;
    int sipOrder = 0;
    if (ctype1.size() >= 12 && ctype1.compare(ctype1.size() - 4, 4, "-SIP") == 0) {
        double ao = 0, bo = 0;
        if (cardNumber(kw, "A_ORDER", ao) && cardNumber(kw, "B_ORDER", bo) && ao >= 2 && bo >= 2 && ao <= 9 &&
            bo <= 9) {
            sipOrder = static_cast<int>(std::max(ao, bo));
            terms = sipTerms(2, sipOrder);
            for (const auto& t : terms) {
                const std::string suffix = "_" + std::to_string(t.p) + "_" + std::to_string(t.q);
                double a = 0, b = 0;
                cardNumber(kw, "A" + suffix, a);
                cardNumber(kw, "B" + suffix, b);
                A.push_back(a);
                B.push_back(b);
            }
        }
    }

    double lonpole = 180, latpole = 90;
    cardNumber(kw, "LONPOLE", lonpole);
    cardNumber(kw, "LATPOLE", latpole);
    std::string system = toUpper(cardString(kw, "RADESYS"));
    if (system.empty()) system = toUpper(cardString(kw, "RADECSYS"));
    if (system != "ICRS" && system != "FK5" && system != "FK4" && system != "GCRS") system = "ICRS";
    double equinox = 2000;
    cardNumber(kw, "EQUINOX", equinox);

    const std::string P = "PCL:AstrometricSolution:";
    auto& out = properties;
    out.push_back(scalarProperty("Observation:CelestialReferenceSystem", "String", system));
    out.push_back(scalarProperty("Observation:Equinox", "Float64", formatDouble(equinox)));
    out.push_back(vectorProperty(P + "CelestialPoleNativeCoordinates", {lonpole, latpole}));
    out.push_back(scalarProperty(P + "CreationTime", "TimePoint", utcTimestamp()));
    out.push_back(scalarProperty(P + "CreatorApplication", "String", std::string("xisfconv ") + kVersion));
    out.push_back(matrixProperty(P + "LinearTransformationMatrix", 2, 2, {m[0], m[1], m[2], m[3]}));
    out.push_back(scalarProperty(P + "ProjectionSystem", "String", projection));
    out.push_back(vectorProperty(P + "ReferenceCelestialCoordinates", {crval1, crval2}));
    out.push_back(vectorProperty(P + "ReferenceImageCoordinates", {x0, y0}));
    out.push_back(vectorProperty(P + "ReferenceNativeCoordinates", {0.0, 90.0}));

    size_t nPoints = 0;
    if (sipOrder >= 2) {
        // Sample the distortion model on a regular grid that includes the image borders. PixInsight
        // rebuilds its thin plate splines from these control points when it loads the file.
        const double longSide = std::max(W, H);
        const size_t nx = std::max<size_t>(4, static_cast<size_t>(std::lround(23 * W / longSide)) + 1);
        const size_t ny = std::max<size_t>(4, static_cast<size_t>(std::lround(23 * H / longSide)) + 1);
        std::vector<double> image, world;
        image.reserve(2 * nx * ny);
        world.reserve(2 * nx * ny);
        for (size_t r = 0; r < ny; ++r) {
            for (size_t c = 0; c < nx; ++c) {
                const double x = W * static_cast<double>(c) / static_cast<double>(nx - 1);
                const double y = H * static_cast<double>(r) / static_cast<double>(ny - 1);
                const double dx = (x + 0.5) - crpix1, dy = (H + 0.5 - y) - crpix2;
                const double u = dx + evalPoly(terms, A, dx, dy), v = dy + evalPoly(terms, B, dx, dy);
                image.push_back(x);
                image.push_back(y);
                world.push_back(cd[0] * u + cd[1] * v);
                world.push_back(cd[2] * u + cd[3] * v);
            }
        }
        nPoints = nx * ny;
        const std::string S = P + "SplineWorldTransformation:";
        out.push_back(vectorProperty(S + "ControlPoints:Image", std::move(image)));
        out.push_back(vectorProperty(S + "ControlPoints:World", std::move(world)));
        out.push_back(matrixProperty(S + "LinearApproximation", 2, 3,
                                     {m[0], m[1], -(m[0] * x0 + m[1] * y0), m[2], m[3], -(m[2] * x0 + m[3] * y0)}));
        // Generation parameters. The control points are exact samples of a smooth model, not
        // measured star positions, so the splines must interpolate them: no smoothing and no
        // surface simplification. (With ImageSolver's defaults for noisy data, smoothing 0.005
        // and simplifiers on, PixInsight 1.9.3 missed the corner points by about 1.4 arcsec.)
        out.push_back(scalarProperty(S + "MaxSplinePoints", "Int32", "4000"));
        out.push_back(scalarProperty(S + "RBFType", "String", "DDMThinPlateSpline"));
        out.push_back(scalarProperty(S + "SimplifierRejectFraction", "Float32", "0.1"));
        out.push_back(scalarProperty(S + "SplineOrder", "Int32", "2"));
        out.push_back(scalarProperty(S + "SplineSmoothness", "Float32", "0"));
        out.push_back(scalarProperty(S + "Truncated", "Boolean", "false"));
        out.push_back(scalarProperty(S + "UseSimplifiers", "Boolean", "false"));
        out.push_back(scalarProperty(S + "Version", "String", "2.0"));
    }

    char buf[200];
    const double scale = std::sqrt(std::fabs(det)) * 3600;
    if (nPoints) {
        std::snprintf(buf, sizeof buf, "PixInsight solution properties: %s, %.3f\"/px, spline with %zu control points "
                      "from SIP order %d", projection.c_str(), scale, nPoints, sipOrder);
    } else {
        std::snprintf(buf, sizeof buf, "PixInsight solution properties: %s, %.3f\"/px, linear", projection.c_str(), scale);
    }
    summary = buf;
    return true;
}

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
