// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/Window.cpp

#include "Window.h"

#include "MathUtils.h"

namespace chronos::dsp
{

void Window::design(int size, WindowType type, int analysisHop)
{
    coefficients.resize(static_cast<size_t>(size));

    // Raw window shape. Formulas use N-1 in the denominator (symmetric windows).
    const double denom = static_cast<double>(size - 1);
    for (int n = 0; n < size; ++n)
    {
        const double x = n / denom;
        double w = 0.0;
        switch (type)
        {
            case WindowType::Hann:
                w = 0.5 - 0.5 * std::cos(kTwoPi * x);
                break;
            case WindowType::Hamming:
                w = 0.54 - 0.46 * std::cos(kTwoPi * x);
                break;
            case WindowType::BlackmanHarris:
                w = 0.35875 - 0.48829 * std::cos(kTwoPi * x)
                            + 0.14128 * std::cos(2.0 * kTwoPi * x)
                            - 0.01168 * std::cos(3.0 * kTwoPi * x);
                break;
        }
        coefficients[static_cast<size_t>(n)] = static_cast<Sample>(w);
    }

    // COLA normalisation for weighted overlap-add. We apply the same window on
    // analysis and synthesis, so the reconstruction weight at each output sample
    // is sum_k window(n + k*hop)^2. Normalising by that constant gives unity gain.
    if (analysisHop > 0)
    {
        // Evaluate the overlap sum at the window centre, where all overlapping
        // frames contribute — a robust, position-independent estimate for the
        // symmetric windows we use.
        double overlapSum = 0.0;
        const int centre = size / 2;
        for (int offset = -size; offset <= size; offset += analysisHop)
        {
            const int idx = centre + offset;
            if (idx >= 0 && idx < size)
            {
                const double w = coefficients[static_cast<size_t>(idx)];
                overlapSum += w * w;
            }
        }

        if (overlapSum > 1.0e-9)
        {
            const auto scale = static_cast<Sample>(1.0 / std::sqrt(overlapSum));
            for (auto& c : coefficients)
                c *= scale;
        }
    }
}

void Window::apply(Sample* frame) const noexcept
{
    const int n = size();
    for (int i = 0; i < n; ++i)
        frame[i] *= coefficients[static_cast<size_t>(i)];
}

} // namespace chronos::dsp
