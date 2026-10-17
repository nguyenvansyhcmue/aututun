#pragma once

#include "CorrectionTrajectory.h"

#include <algorithm>
#include <cmath>

// Renderer-independent musical control for the causal Live engine.  This
// deliberately reuses the production trajectory: target selection must have
// one definition across Live and Studio.  The Live renderer receives only a
// timestamped, de-zipped ratio and never makes note decisions itself.
struct LiveControlParameters
{
    double referencePitchHz = 440.0;
    int transpose = 0;
    double retuneSpeedMs = 100.0;
    double flexTunePercent = 30.0;
    double humanizePercent = 0.0;
    double naturalVibratoPercent = 0.0;
};

struct LiveControlFrame
{
    double stableF0Hz = 0.0;
    double pitchRatio = 1.0;
    double correctionCents = 0.0;
    int targetNote = -1;
    bool voiced = false;
    bool valid = false;
    bool targetChanged = false;
    bool transient = false;
};

class LiveControlPipeline
{
public:
    // 64 samples is 1.33 ms at 48 kHz.  It gives the renderer a continuous
    // control cadence without claiming that the F0 detector itself has zero
    // look-ahead.  The caller may provide F0 less often; the last valid
    // trajectory is advanced safely between detector observations.
    void prepare (double newSampleRate, int newControlIntervalSamples = 64) noexcept
    {
        sampleRate = std::max (1.0, newSampleRate);
        controlIntervalSamples = std::clamp (newControlIntervalSamples, 16, 128);
        reset();
    }

    void reset() noexcept
    {
        trajectory.reset();
        last = {};
        haveTrajectory = false;
        deliveredCorrectionCents = 0.0;
        haveDeliveredCorrection = false;
    }

    int controlInterval() const noexcept { return controlIntervalSamples; }

    // Call only when the causal detector publishes a new stable F0.  The
    // detector confidence/voiced decision is authoritative: noisy breaths
    // must never become a target candidate.
    LiveControlFrame observePitch (double stableF0Hz, bool voiced, bool transient,
                                   const LiveControlParameters& parameters,
                                   const NotePolicySnapshot& notePolicy) noexcept
    {
        const auto data = trajectory.update (stableF0Hz, parameters.referencePitchHz,
                                             parameters.transpose, notePolicy,
                                             parameters.retuneSpeedMs,
                                             parameters.flexTunePercent,
                                             parameters.humanizePercent,
                                             parameters.naturalVibratoPercent,
                                             sampleRate, controlIntervalSamples,
                                             voiced, transient,
                                             TuningPolicy::Standard,
                                             true);
        last = fromTrajectory (data, stableF0Hz, voiced, transient);
        haveTrajectory = data.hasValidCorrection;
        return last;
    }

    // Call once for every renderer control tick after observePitch().  It
    // advances only the already committed musical trajectory: no target can
    // switch at a host-block boundary or inside a renderer grain.
    LiveControlFrame advance() noexcept
    {
        if (! haveTrajectory)
            return last;
        const auto data = trajectory.advanceScheduledTransition (controlIntervalSamples, sampleRate);
        last = fromTrajectory (data, last.stableF0Hz, last.voiced, false);
        dezipperRendererControl (last);
        haveTrajectory = data.hasValidCorrection;
        return last;
    }

private:
    // Musical target selection remains untouched. This last handoff limiter
    // prevents a newly published pitch-centre state becoming a grain-control
    // discontinuity. 16 cents stays below a 1% ratio step per control tick.
    static constexpr double maximumRendererCorrectionStepCents = 16.0;

    void dezipperRendererControl (LiveControlFrame& frame) noexcept
    {
        if (! frame.valid || ! std::isfinite (frame.correctionCents))
            return;

        if (! haveDeliveredCorrection)
        {
            deliveredCorrectionCents = frame.correctionCents;
            haveDeliveredCorrection = true;
        }
        else
        {
            const auto delta = std::clamp (frame.correctionCents - deliveredCorrectionCents,
                                           -maximumRendererCorrectionStepCents,
                                            maximumRendererCorrectionStepCents);
            deliveredCorrectionCents += delta;
        }

        frame.correctionCents = deliveredCorrectionCents;
        frame.pitchRatio = std::exp2 (deliveredCorrectionCents / 1200.0);
    }

    static LiveControlFrame fromTrajectory (const CorrectionTrajectoryData& data,
                                            double stableF0Hz, bool voiced, bool transient) noexcept
    {
        LiveControlFrame result;
        result.stableF0Hz = stableF0Hz;
        result.pitchRatio = std::isfinite (data.pitchRatio) ? data.pitchRatio : 1.0;
        result.correctionCents = std::isfinite (data.correctionCents) ? data.correctionCents : 0.0;
        result.targetNote = data.targetNote;
        result.voiced = voiced;
        result.valid = data.hasValidCorrection;
        result.targetChanged = data.targetChanged;
        result.transient = transient;
        return result;
    }

    double sampleRate = 48000.0;
    int controlIntervalSamples = 64;
    CorrectionTrajectory trajectory;
    LiveControlFrame last;
    bool haveTrajectory = false;
    double deliveredCorrectionCents = 0.0;
    bool haveDeliveredCorrection = false;
};
