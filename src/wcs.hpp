// FITS WCS keywords from a PixInsight astrometric solution (PCL:AstrometricSolution:* properties).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "xisf.hpp"
#include "xisfwrite.hpp"

namespace xisfconv {

struct WcsResult {
    std::vector<FitsKeyword> keywords;
    std::string summary;  // one line for the console
};

// Builds WCS keywords for image `index`. `bottomUp` must match the row order written to FITS.
// sipOrder >= 2 fits SIP distortion polynomials to the solution's control points (0 = linear only).
// Returns false if the image carries no usable astrometric solution.
bool astrometricSolutionToWcs(XisfFile& file, size_t index, bool bottomUp, int sipOrder, WcsResult& out);

// Rewrites WCS keywords (CRPIX2, CD/PC/CDELT, SIP coefficients) for an image whose row order is
// reversed, i.e. converts between the bottom-up and top-down pixel conventions. Applying it twice
// restores the original values. Returns true if WCS keywords were found.
bool flipWcsRowOrder(std::vector<FitsKeyword>& keywords, uint64_t height);

// Builds PixInsight's native astrometric solution properties (PCL:AstrometricSolution:*) from
// FITS WCS keywords, which must be in the bottom-up convention. A SIP distortion model becomes a
// spline world transformation described by control points sampled from the SIP polynomials, from
// which PixInsight rebuilds its surface splines. Returns false (and says why in `summary`) when
// the keywords hold no solution that can be expressed this way.
bool wcsToAstrometricSolution(const std::vector<FitsKeyword>& keywords, uint64_t width, uint64_t height,
                              std::vector<XisfOutProperty>& properties, std::string& summary);

}  // namespace xisfconv
