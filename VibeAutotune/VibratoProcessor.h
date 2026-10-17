#pragma once

#include <algorithm>
#include <cmath>

// Final, allocation-free vibrato delivery stage. The trajectory owns musical
// policy (the calibrated gain/phase surfaces); this class owns only safe
// recombination of periodic residual into the renderer-facing pitch control.
struct VibratoShape
{
    double residualGain = 0.0;
    double phaseLeadRadians = 0.0;
    double slowRetuneInputWeight = 0.0;
    bool enabled = false;
};

class VibratoProcessor
{
public:
    double process (double rawResidualCents, double quadratureResidualCents,
                    const VibratoShape& shape) const noexcept
    {
        if (! shape.enabled)
            return 0.0;

        // Preserve the historical zero-phase path bit-for-bit. In a
        // grain-scheduled renderer, even `raw * 1 + raw * 0` can eventually
        // choose a different floating-point scheduling branch.
        const auto phaseRotated = shape.phaseLeadRadians == 0.0
                                ? rawResidualCents
                                : rawResidualCents * std::cos (shape.phaseLeadRadians)
                                + quadratureResidualCents * std::sin (shape.phaseLeadRadians);
        const auto delivered = clamp (phaseRotated * shape.residualGain,
                                      -maximumOutputDepthCents, maximumOutputDepthCents);
        // The correction reference may already carry a fraction of the
        // source residual at slow Retune settings. Never deliver it twice.
        return delivered * (1.0 - clamp (shape.slowRetuneInputWeight, 0.0, 1.0));
    }

private:
    static constexpr double maximumOutputDepthCents = 80.0;
    static double clamp (double value, double minimum, double maximum) noexcept
    {
        return std::max (minimum, std::min (maximum, value));
    }
};
