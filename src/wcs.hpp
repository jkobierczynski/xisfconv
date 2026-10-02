// FITS WCS keywords from a PixInsight astrometric solution (PCL:AstrometricSolution:* properties).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "xisf.hpp"

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

}  // namespace xisfconv
