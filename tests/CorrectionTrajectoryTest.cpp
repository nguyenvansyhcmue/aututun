#include "CorrectionTrajectory.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int analysisHop = 512;

NotePolicySnapshot policyFor (int key, int scale)
{
    NotePolicySnapshot policy;
    static constexpr std::array<bool, 12> major {{ true, false, true, false, true, true, false, true, false, true, false, true }};
    for (int note = 0; note <= 127; ++note)
    {
        const auto pitchClass = note % 12;
        const auto legal = scale == 0 || major[static_cast<size_t> ((pitchClass - key + 12) % 12)];
        policy.pitchClassDefaults[static_cast<size_t> (pitchClass)] = legal ? NoteRule::Legal : NoteRule::IllegalNearest;
    }
    return policy;
}

struct Frame
{
    int sample = 0;
    CorrectionTrajectoryData data;
};

template <typename Contour>
std::vector<Frame> runContour (int hostBlockSize, int totalSamples, double retuneMs, Contour&& contour,
                               double flexTunePercent = 0.0, double humanizePercent = 0.0,
                               double naturalVibratoPercent = 0.0,
                               bool diagnosticContinuousRapidReturnExcursion = false,
                               double diagnosticSlowRetuneInputReferenceAmountOverride = -1.0)
{
    CorrectionTrajectory trajectory;
    trajectory.setDiagnosticContinuousRapidReturnExcursion (diagnosticContinuousRapidReturnExcursion);
    trajectory.setDiagnosticSlowRetuneInputReferenceAmountOverride (
        diagnosticSlowRetuneInputReferenceAmountOverride);
    std::vector<Frame> frames;
    int nextAnalysis = analysisHop;
    for (int blockStart = 0; blockStart < totalSamples; blockStart += hostBlockSize)
    {
        const auto blockEnd = std::min (blockStart + hostBlockSize, totalSamples);
        while (nextAnalysis <= blockEnd)
        {
            const auto point = contour (nextAnalysis);
            const auto frequency = std::isfinite (point.cents)
                                 ? 440.0 * std::exp2 (point.cents / 1200.0)
                                 : std::numeric_limits<double>::quiet_NaN();
            frames.push_back ({ nextAnalysis,
                trajectory.update (frequency, 440.0, point.transpose, policyFor (point.key, point.scale),
                                   retuneMs, flexTunePercent, humanizePercent,
                                   naturalVibratoPercent,
                                   sampleRate, analysisHop, point.voiced, point.audioTransient) });
            nextAnalysis += analysisHop;
        }
    }
    return frames;
}

struct ContourPoint
{
    double cents = 0.0;
    bool voiced = true;
    int key = 0;
    int scale = 0;
    int transpose = 0;
    bool audioTransient = false;
};

double millisecondsForProgress (const std::vector<Frame>& frames, double inputCents, double fraction)
{
    const auto correctionMagnitude = std::abs (inputCents);
    for (const auto& frame : frames)
    {
        if (! frame.data.hasValidCorrection)
            continue;
        const auto progress = std::abs (frame.data.correctedPitchCents - inputCents) / correctionMagnitude;
        if (progress >= fraction)
            return static_cast<double> (frame.sample) * 1000.0 / sampleRate;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

int targetSwitchCount (const std::vector<Frame>& frames)
{
    auto previous = -1;
    auto switches = 0;
    for (const auto& frame : frames)
    {
        if (! frame.data.hasValidCorrection)
            continue;
        if (previous >= 0 && frame.data.targetNote != previous)
            ++switches;
        previous = frame.data.targetNote;
    }
    return switches;
}

bool hasOnlyFiniteValidOutputs (const std::vector<Frame>& frames)
{
    return std::all_of (frames.begin(), frames.end(), [] (const Frame& frame)
    {
        return ! frame.data.hasValidCorrection
            || (std::isfinite (frame.data.inputPitchCents)
                && std::isfinite (frame.data.targetPitchCents)
                && std::isfinite (frame.data.correctedPitchCents)
                && std::isfinite (frame.data.correctionCents)
                && std::isfinite (frame.data.pitchRatio));
    });
}

const CorrectionTrajectoryData& validFrameAtOrAfter (const std::vector<Frame>& frames, int sample)
{
    for (const auto& frame : frames)
        if (frame.sample >= sample && frame.data.hasValidCorrection)
            return frame.data;
    std::cerr << "FAILED: expected a valid trajectory frame\n";
    std::exit (1);
}

double maximumBlockDifference (const std::vector<Frame>& reference, const std::vector<Frame>& compared)
{
    if (reference.size() != compared.size())
        return std::numeric_limits<double>::infinity();
    auto maximum = 0.0;
    for (size_t index = 0; index < reference.size(); ++index)
    {
        if (reference[index].data.hasValidCorrection != compared[index].data.hasValidCorrection
            || reference[index].data.targetNote != compared[index].data.targetNote)
            return std::numeric_limits<double>::infinity();
        if (reference[index].data.hasValidCorrection)
            maximum = std::max (maximum, std::abs (reference[index].data.correctedPitchCents
                                                   - compared[index].data.correctedPitchCents));
    }
    return maximum;
}

double centredOutputPeak (const std::vector<Frame>& frames, int firstSample)
{
    auto sum = 0.0;
    auto count = 0;
    for (const auto& frame : frames)
        if (frame.sample >= firstSample && frame.data.hasValidCorrection)
        {
            sum += frame.data.correctedPitchCents;
            ++count;
        }

    if (count == 0)
        return std::numeric_limits<double>::quiet_NaN();

    const auto mean = sum / static_cast<double> (count);
    auto peak = 0.0;
    for (const auto& frame : frames)
        if (frame.sample >= firstSample && frame.data.hasValidCorrection)
            peak = std::max (peak, std::abs (frame.data.correctedPitchCents - mean));
    return peak;
}

double meanCorrectedPitch (const std::vector<Frame>& frames, int firstSample)
{
    auto sum = 0.0;
    auto count = 0;
    for (const auto& frame : frames)
        if (frame.sample >= firstSample && frame.data.hasValidCorrection)
        {
            sum += frame.data.correctedPitchCents;
            ++count;
        }
    return count > 0 ? sum / static_cast<double> (count) : std::numeric_limits<double>::quiet_NaN();
}

double meanVibratoEnergy (const std::vector<Frame>& frames, int firstSample)
{
    auto sum = 0.0;
    auto count = 0;
    for (const auto& frame : frames)
        if (frame.sample >= firstSample && frame.data.hasValidCorrection)
        {
            sum += frame.data.vibratoEnergyCents;
            ++count;
        }
    return count > 0 ? sum / static_cast<double> (count) : std::numeric_limits<double>::quiet_NaN();
}

void require (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit (1);
    }
}
}

int main()
{
    // The decomposer is analysis-only. Verify its periodicity classification
    // independently so future Humanize/Natural routing cannot silently turn
    // ordinary drift into vibrato or vice versa.
    {
        PitchDecomposer decomposer;
        PitchComponents flat;
        PitchComponents periodic;
        for (int frame = 0; frame < 140; ++frame)
        {
            const auto seconds = static_cast<double> (frame * analysisHop) / sampleRate;
            flat = decomposer.process (0.0, 220.0, sampleRate, analysisHop, 0.250);
            (void) seconds;
        }
        require (std::abs (flat.residualCents) < 1.0e-12 && flat.vibratoConfidence < 1.0e-12,
                 "flat pitch was classified as vibrato");

        decomposer.reset();
        for (int frame = 0; frame < 140; ++frame)
        {
            const auto seconds = static_cast<double> (frame * analysisHop) / sampleRate;
            periodic = decomposer.process (20.0 * std::sin (2.0 * std::numbers::pi * 5.0 * seconds),
                                            220.0, sampleRate, analysisHop, 0.250);
        }
        std::cout << "pitch_decomposition | rate_hz=" << periodic.vibratoRateHz
                  << " confidence=" << periodic.vibratoConfidence
                  << " stability=" << periodic.noteStability << '\n';
        require (periodic.periodicVibrato && periodic.vibratoConfidence > 0.7,
                 "periodic vibrato was not recognised");
    }

    std::cout << std::fixed << std::setprecision (3);
    CorrectionTrajectory retuneLaw;
    require (retuneLaw.getEffectiveRetuneTimeMs (0.0) == 0.0,
             "0 ms must remain a hard-tune snap");
    require (retuneLaw.getEffectiveRetuneTimeMs (0.1) >= 8.0,
             "every non-zero retune setting must remain continuous");
    // The Hook-2 calibrated control profile uses the verified 20/40 ms T90
    // knots here. Keep this test coupled to that production contract rather
    // than to the older pre-calibration fixture.
    require (std::abs (retuneLaw.getEffectiveRetuneTimeMs (10.0) - (20.0 / 2.302585092994046)) < 0.01,
             "10 ms must use the calibrated Hook-2 Artist time constant");
    require (std::abs (retuneLaw.getEffectiveRetuneTimeMs (25.0) - (40.0 / 2.302585092994046)) < 0.01,
             "25 ms must use the calibrated Hook-2 Artist time constant");
    require (std::abs (retuneLaw.getEffectiveRetuneTimeMs (50.0) - 76.436) < 0.01,
             "50 ms must use the measured Artist time constant");
    constexpr auto ln10 = 2.302585092994046;
    require (std::abs (retuneLaw.getEffectiveRetuneTimeMs (300.0) * ln10 - 300.0) < 0.01,
             "300 ms must reach 90 percent correction in 300 ms");
    require (std::abs (retuneLaw.getEffectiveRetuneTimeMs (400.0) * ln10 - 400.0) < 0.01,
             "400 ms must reach 90 percent correction in 400 ms");
    require (CorrectionTrajectory::getNoteTransitionDurationMs (TuningPolicy::Standard, 400.0)
                 > CorrectionTrajectory::getNoteTransitionDurationMs (TuningPolicy::Standard, 100.0),
             "note transitions must soften with the extended retune range");
    const auto staticOffset = [] (int) { return ContourPoint { 50.0, true }; };

    for (const auto retuneMs : { 0.0, 20.0, 50.0, 100.0, 300.0, 400.0 })
    {
        const auto frames = runContour (64, 48000, retuneMs, staticOffset);
        const auto time63 = millisecondsForProgress (frames, 50.0, 0.63);
        const auto time90 = millisecondsForProgress (frames, 50.0, 0.90);
        const auto time99 = millisecondsForProgress (frames, 50.0, 0.99);
        std::cout << "retune_static_50cents | retune_ms=" << retuneMs
                  << " t63_ms=" << time63 << " t90_ms=" << time90 << " t99_ms=" << time99
                  << " target_switches=" << targetSwitchCount (frames)
                  << " finite=" << hasOnlyFiniteValidOutputs (frames) << '\n';
        require (hasOnlyFiniteValidOutputs (frames), "static trajectory produced NaN");
        if (retuneMs == 0.0)
            require (time99 <= 4.0 * static_cast<double> (analysisHop) * 1000.0 / sampleRate,
                    "0 ms hard-lock guard did not settle");
        if (retuneMs >= 300.0)
            require (std::abs (time90 - retuneMs) <= 2.0 * analysisHop * 1000.0 / sampleRate,
                     "high Retune did not meet its displayed 90-percent correction time");
    }

    // A one-frame note candidate must not move the target. The three-frame
    // target median is intentionally evaluated before the target selector.
    const auto oneFrameOutlier = [] (int sample)
    {
        const auto frameIndex = sample / analysisHop;
        return ContourPoint { frameIndex == 8 ? 100.0 : 0.0, true };
    };
    const auto outlierFrames = runContour (64, 16000, 20.0, oneFrameOutlier);
    std::cout << "target_median_outlier | target_switches=" << targetSwitchCount (outlierFrames) << '\n';
    require (targetSwitchCount (outlierFrames) == 0, "one-frame target outlier bypassed median filtering");

    // At Retune 0, a transient must briefly override the hard snap with the
    // transient safety time constant, then immediately return to hard-tune.
    const auto transientContour = [] (int sample)
    {
        const auto frameIndex = sample / analysisHop;
        return ContourPoint { frameIndex < 8 ? 0.0 : 50.0, true, 0, 0, 0, frameIndex == 8 };
    };
    const auto hardFrames = runContour (64, 12000, 0.0,
                                        [] (int sample) { return ContourPoint { sample / analysisHop < 8 ? 0.0 : 50.0, true }; });
    const auto transientFrames = runContour (64, 12000, 0.0, transientContour);
    const auto hardFirst = std::abs (validFrameAtOrAfter (hardFrames, analysisHop * 9).correctionCents);
    const auto transientFirst = std::abs (validFrameAtOrAfter (transientFrames, analysisHop * 9).correctionCents);
    const auto hardRecovered = std::abs (validFrameAtOrAfter (hardFrames, analysisHop * 12).correctionCents);
    const auto transientRecovered = std::abs (validFrameAtOrAfter (transientFrames, analysisHop * 12).correctionCents);
    std::cout << "transient_retune_guard | hard_correction_cents=" << hardFirst
              << " transient_correction_cents=" << transientFirst
              << " hard_recovered_cents=" << hardRecovered
              << " transient_recovered_cents=" << transientRecovered << '\n';
    require (transientFirst + 1.0 < hardFirst, "audio transient did not temporarily relax hard retune");
    require (transientRecovered >= hardRecovered - 2.0,
             "transient protection did not promptly return to hard retune");

    // Flex-Tune is evaluated before Retune smoothing. At 0 ms the final frame
    // is the exact soft correction curve, making the gain behavior directly
    // measurable without renderer or host-block effects.
    for (const auto errorCents : { 10.0, 25.0, 50.0, 100.0 })
    {
        auto previousMagnitude = std::numeric_limits<double>::infinity();
        for (const auto flex : { 0.0, 25.0, 50.0, 75.0, 100.0 })
        {
            // C major excludes A#, so offsets through 100 cents retain A as
            // the target (the exact midpoint resolves to the lower note).
            const auto constantError = [errorCents] (int) { return ContourPoint { errorCents, true, 0, 1 }; };
            const auto frames = runContour (64, 12000, 0.0, constantError, flex);
            const auto& final = frames.back().data;
            const auto magnitude = std::abs (final.correctionCents);
            std::cout << "flex_static | error_cents=" << errorCents << " flex_percent=" << flex
                      << " correction_cents=" << final.correctionCents
                      << " corrected_cents=" << final.correctedPitchCents
                      << " target_switches=" << targetSwitchCount (frames) << '\n';
            require (final.hasValidCorrection && std::isfinite (final.pitchRatio), "flex produced invalid output");
            require (magnitude <= previousMagnitude + 1.0e-12, "flex did not reduce near-target correction monotonically");
            previousMagnitude = magnitude;
        }
    }

    for (const auto vibratoDepth : { 20.0, 40.0 })
        for (const auto flex : { 0.0, 100.0 })
            for (const auto retune : { 0.0, 100.0 })
            {
                const auto contour = [vibratoDepth] (int sample)
                {
                    return ContourPoint { vibratoDepth * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate), true };
                };
                const auto frames = runContour (64, 48000, retune, contour, flex);
                auto correctionRange = 0.0;
                for (const auto& frame : frames)
                    if (frame.data.hasValidCorrection)
                        correctionRange = std::max (correctionRange, std::abs (frame.data.correctionCents));
                std::cout << "flex_vibrato | depth_cents=" << vibratoDepth << " flex_percent=" << flex
                          << " retune_ms=" << retune << " correction_peak_cents=" << correctionRange
                          << " target_switches=" << targetSwitchCount (frames) << '\n';
                require (targetSwitchCount (frames) == 0, "flex vibrato changed target");
            }

    const auto shortNote = [] (int sample)
    {
        return ContourPoint { sample < 4800 ? 40.0 : std::numeric_limits<double>::quiet_NaN(), sample < 4800 };
    };
    const auto shortBase = runContour (64, 10000, 0.0, shortNote, 0.0, 0.0);
    const auto shortHuman = runContour (64, 10000, 0.0, shortNote, 0.0, 100.0);
    const auto shortDifference = std::abs (validFrameAtOrAfter (shortBase, 4096).correctedPitchCents
                                           - validFrameAtOrAfter (shortHuman, 4096).correctedPitchCents);
    std::cout << "humanize_short_note | duration_ms=100 difference_cents=" << shortDifference << '\n';
    require (shortDifference <= 1.0e-12, "humanize changed a short note");

    // The target remains A throughout. A small 5 Hz residual establishes
    // valid vibrato before the centre moves 40 cents after half a second.
    // Humanize may relax this sustained periodic gesture, but not a drift.
    const auto sustainedError = [] (int sample)
    {
        const auto vibrato = 8.0 * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate);
        return ContourPoint { (sample < 24000 ? 0.0 : 40.0) + vibrato, true };
    };
    for (const auto retune : { 0.0, 20.0, 50.0 })
    {
        const auto base = runContour (64, 60000, retune, sustainedError, 0.0, 0.0);
        const auto human = runContour (64, 60000, retune, sustainedError, 0.0, 100.0);
        const auto probe = 24000 + analysisHop * 3;
        const auto baseCorrection = std::abs (validFrameAtOrAfter (base, probe).correctionCents);
        const auto humanCorrection = std::abs (validFrameAtOrAfter (human, probe).correctionCents);
        std::cout << "humanize_sustain | retune_ms=" << retune << " probe_ms="
                  << static_cast<double> (probe) * 1000.0 / sampleRate
                  << " base_correction_cents=" << baseCorrection
                  << " humanize100_correction_cents=" << humanCorrection << '\n';
        if (retune < 45.0)
            require (humanCorrection < baseCorrection, "humanize did not relax a fast-retune sustain");
        else
            require (std::abs (humanCorrection - baseCorrection) <= 1.0e-12,
                     "humanize changed an already-natural retune speed");
    }

    const auto targetChange = [] (int sample) { return ContourPoint { sample < 24000 ? 0.0 : 140.0, true }; };
    const auto targetBase = runContour (64, 60000, 50.0, targetChange, 0.0, 0.0);
    const auto targetHuman = runContour (64, 60000, 50.0, targetChange, 0.0, 100.0);
    const auto changeProbe = 24000 + analysisHop;
    const auto targetResetDifference = std::abs (validFrameAtOrAfter (targetBase, changeProbe).correctedPitchCents
                                                  - validFrameAtOrAfter (targetHuman, changeProbe).correctedPitchCents);
    std::cout << "humanize_target_reset | difference_cents=" << targetResetDifference << '\n';
    require (targetResetDifference <= 1.0e-12, "humanize was not reset by target change");

    const auto unvoicedReset = [] (int sample)
    {
        return ContourPoint { sample < 24000 ? 0.0 : 40.0,
                              sample < 24000 || sample >= 27000 };
    };
    const auto unvoicedBase = runContour (64, 60000, 50.0, unvoicedReset, 0.0, 0.0);
    const auto unvoicedHuman = runContour (64, 60000, 50.0, unvoicedReset, 0.0, 100.0);
    const auto returnProbe = 27000 + analysisHop;
    const auto unvoicedResetDifference = std::abs (validFrameAtOrAfter (unvoicedBase, returnProbe).correctedPitchCents
                                                    - validFrameAtOrAfter (unvoicedHuman, returnProbe).correctedPitchCents);
    std::cout << "humanize_unvoiced_reset | difference_cents=" << unvoicedResetDifference << '\n';
    require (unvoicedResetDifference <= 1.0e-12, "humanize was not reset by unvoiced state");

    // Centred vibrato must remain centred. Humanize changes only the sustain
    // retune time, never creates a DC pitch shift or removes the residual.
    const auto centredVibrato = [] (int sample)
    {
        return ContourPoint { 20.0 * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate), true };
    };
    const auto centredBase = runContour (64, 96000, 10.0, centredVibrato, 0.0, 0.0);
    const auto centredHuman = runContour (64, 96000, 10.0, centredVibrato, 0.0, 100.0);
    const auto centredBaseMean = meanCorrectedPitch (centredBase, 48000);
    const auto centredHumanMean = meanCorrectedPitch (centredHuman, 48000);
    const auto centredBaseDepth = centredOutputPeak (centredBase, 48000);
    const auto centredHumanDepth = centredOutputPeak (centredHuman, 48000);
    std::cout << "humanize_centred_vibrato | base_mean=" << centredBaseMean
              << " human_mean=" << centredHumanMean
              << " base_depth=" << centredBaseDepth
              << " human_depth=" << centredHumanDepth
              << " analysed_energy=" << meanVibratoEnergy (centredHuman, 48000) << '\n';
    require (std::abs (centredBaseMean) < 2.0 && std::abs (centredHumanMean) < 2.0,
             "Humanize drifted a centred vibrato");
    require (centredHumanDepth >= centredBaseDepth * 0.75,
             "Humanize removed valid centred vibrato");

    // A vibrato whose centre is sharp must be corrected at steady state; the
    // residual remains present instead of being classified as a 30-cent error.
    const auto offsetVibrato = [] (int sample)
    {
        return ContourPoint { 35.0 + 12.0 * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate), true };
    };
    const auto offsetHuman = runContour (64, 120000, 10.0, offsetVibrato, 0.0, 100.0);
    const auto offsetMean = meanCorrectedPitch (offsetHuman, 72000);
    const auto offsetDepth = centredOutputPeak (offsetHuman, 72000);
    std::cout << "humanize_offset_vibrato | output_mean=" << offsetMean
              << " output_depth=" << offsetDepth << '\n';
    require (std::abs (offsetMean) < 4.0, "Humanize failed to correct an offset vibrato centre");
    require (offsetDepth > 3.0, "Humanize removed offset vibrato residual");

    const auto wideVibrato = [] (int sample)
    {
        return ContourPoint { 40.0 * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate), true };
    };
    const auto wideHuman = runContour (64, 96000, 10.0, wideVibrato, 0.0, 100.0);
    std::cout << "humanize_wide_vibrato | target_switches=" << targetSwitchCount (wideHuman)
              << " output_depth=" << centredOutputPeak (wideHuman, 48000)
              << " analysed_energy=" << meanVibratoEnergy (wideHuman, 48000) << '\n';
    require (targetSwitchCount (wideHuman) == 0, "wide vibrato caused a Humanize target switch");

    // Slow non-periodic drift has no 4-8 Hz signature. Humanize therefore
    // must not relax its correction or turn it into a retained vibrato.
    const auto pitchDrift = [] (int sample)
    {
        return ContourPoint { 25.0 * std::sin (2.0 * std::numbers::pi * 0.7 * sample / sampleRate), true };
    };
    const auto driftBase = runContour (64, 96000, 10.0, pitchDrift, 0.0, 0.0);
    const auto driftHuman = runContour (64, 96000, 10.0, pitchDrift, 0.0, 100.0);
    auto driftDifference = 0.0;
    for (size_t index = 0; index < driftBase.size(); ++index)
        if (driftBase[index].data.hasValidCorrection && driftHuman[index].data.hasValidCorrection)
            driftDifference = std::max (driftDifference, std::abs (driftBase[index].data.correctedPitchCents
                                                                    - driftHuman[index].data.correctedPitchCents));
    std::cout << "humanize_nonperiodic_drift | max_difference=" << driftDifference << '\n';
    require (driftDifference < 0.5, "Humanize relaxed non-periodic pitch drift");

    const auto semitone = [] (int sample) { return ContourPoint { sample < 12000 ? 0.0 : 100.0, true }; };
    const auto octave = [] (int sample) { return ContourPoint { sample < 12000 ? 0.0 : 1200.0, true }; };
    const auto vibrato = [] (int sample)
    {
        return ContourPoint { 35.0 * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate), true };
    };
    const auto midpoint = [] (int sample)
    {
        const auto cycle = (sample / analysisHop) % 4;
        return ContourPoint { cycle < 2 ? 49.0 : 51.0, true };
    };
    const auto voicedUnvoiced = [] (int sample)
    {
        return ContourPoint { 50.0, sample < 10000 || sample >= 16000 };
    };

    const auto semitoneFrames = runContour (128, 30000, 20.0, semitone);
    const auto octaveFrames = runContour (128, 30000, 20.0, octave);
    const auto vibratoFrames = runContour (128, 30000, 20.0, vibrato);
    const auto midpointFrames = runContour (128, 30000, 20.0, midpoint);
    const auto voicedUnvoicedFrames = runContour (128, 30000, 20.0, voicedUnvoiced);
    std::cout << "transitions | semitone_switches=" << targetSwitchCount (semitoneFrames)
              << " octave_switches=" << targetSwitchCount (octaveFrames)
              << " vibrato_switches=" << targetSwitchCount (vibratoFrames)
              << " midpoint_switches=" << targetSwitchCount (midpointFrames)
              << " voiced_unvoiced_finite=" << hasOnlyFiniteValidOutputs (voicedUnvoicedFrames) << '\n';
    require (targetSwitchCount (semitoneFrames) == 1, "semitone target did not switch exactly once");
    require (targetSwitchCount (octaveFrames) == 1, "octave onset did not switch exactly once");
    require (targetSwitchCount (vibratoFrames) == 0, "vibrato switched target");
    require (targetSwitchCount (midpointFrames) == 0, "midpoint hysteresis chattered");
    require (hasOnlyFiniteValidOutputs (voicedUnvoicedFrames), "voiced boundary produced discontinuity or NaN");
    for (const auto& frame : voicedUnvoicedFrames)
        if (frame.sample >= 10240 && frame.sample < 15872)
            require (! frame.data.hasValidCorrection, "unvoiced frame published a correction");

    // This candidate is calibrated only for the hard-lock reference tuple.
    // Its offline switch must leave all four controls untouched elsewhere.
    const auto fourKnobBaseline = runContour (64, 30000, 20.0, vibrato, 50.0, 75.0, 12.0);
    const auto fourKnobDiagnostic = runContour (64, 30000, 20.0, vibrato,
                                                50.0, 75.0, 12.0, true);
    const auto fourKnobDifference = maximumBlockDifference (fourKnobBaseline, fourKnobDiagnostic);
    std::cout << "rapid_return_four_knob_isolation | max_cents_difference="
              << fourKnobDifference << '\n';
    require (fourKnobDifference == 0.0,
             "rapid-return diagnostic changed a non-hard-lock four-knob trajectory");

    const auto shortSourceMovement = [] (int sample)
    {
        return ContourPoint { sample < 12000 ? 20.0 : 40.0, true };
    };
    const auto slowRetuneProduction = runContour (64, 24000, 300.0, shortSourceMovement);
    const auto slowRetuneReferenceDisabled = runContour (64, 24000, 300.0, shortSourceMovement,
                                                         0.0, 0.0, 0.0, false, 0.0);
    const auto slowReferenceDifference = maximumBlockDifference (slowRetuneReferenceDisabled,
                                                                 slowRetuneProduction);
    std::cout << "slow_retune_input_reference | max_cents_difference="
              << slowReferenceDifference << '\n';
    require (slowReferenceDifference > 1.0,
             "production slow-Retune input reference did not alter its calibrated tuple");
    require (hasOnlyFiniteValidOutputs (slowRetuneProduction),
             "production slow-Retune input reference produced a non-finite value");

    struct InteractionSetting
    {
        const char* name;
        double flex;
        double humanize;
        double naturalVibrato;
    };
    constexpr std::array<InteractionSetting, 4> interactionSettings {{
        { "flex", 50.0, 0.0, 0.0 },
        { "humanize", 0.0, 75.0, 0.0 },
        { "natural_vibrato", 0.0, 0.0, 12.0 },
        { "all", 50.0, 75.0, 12.0 },
    }};
    for (const auto& setting : interactionSettings)
    {
        const auto production = runContour (64, 24000, 300.0, shortSourceMovement,
                                            setting.flex, setting.humanize, setting.naturalVibrato);
        const auto referenceDisabled = runContour (64, 24000, 300.0, shortSourceMovement,
                                                   setting.flex, setting.humanize,
                                                   setting.naturalVibrato, false, 0.0);
        const auto difference = maximumBlockDifference (production, referenceDisabled);
        std::cout << "slow_retune_four_knob_isolation | active_control=" << setting.name
                  << " max_cents_difference=" << difference << '\n';
        require (difference == 0.0,
                 "slow-Retune production law leaked into an unverified four-knob tuple");
    }

    const auto reference = runContour (64, 30000, 50.0, semitone);
    for (const auto blockSize : { 64, 128, 256, 512, 1024 })
    {
        const auto compared = runContour (blockSize, 30000, 50.0, semitone);
        const auto difference = maximumBlockDifference (reference, compared);
        std::cout << "block_invariance | block=" << blockSize << " max_cents_difference=" << difference << '\n';
        require (difference <= 1.0e-12, "trajectory depended on host block size");
    }

    const auto flexReference = runContour (64, 30000, 50.0, vibrato, 75.0);
    for (const auto blockSize : { 64, 1024 })
    {
        const auto compared = runContour (blockSize, 30000, 50.0, vibrato, 75.0);
        const auto difference = maximumBlockDifference (flexReference, compared);
        std::cout << "flex_block_invariance | flex_percent=75 block=" << blockSize
                  << " max_cents_difference=" << difference << '\n';
        require (difference <= 1.0e-12, "flex trajectory depended on host block size");
    }

    // Artist exposes Natural Vibrato as a signed -12..+12 control. The first
    // half second is ignored so both one-pole estimators have reached their
    // steady behaviour.
    const auto flatInput = [] (int) { return ContourPoint { 40.0, true }; };
    for (const auto naturalVibrato : { -12.0, 0.0, 12.0 })
    {
        const auto flatFrames = runContour (64, 48000, 0.0, flatInput, 0.0, 0.0, naturalVibrato);
        const auto flatPeak = centredOutputPeak (flatFrames, 24000);
        std::cout << "natural_vibrato_flat | value_percent=" << naturalVibrato
                  << " output_peak_cents=" << flatPeak << '\n';
        require (std::isfinite (flatPeak) && flatPeak <= 1.0e-9,
                 "natural vibrato created modulation on a flat input");
    }

    for (const auto inputDepth : { 10.0, 20.0, 40.0 })
        for (const auto retune : { 0.0, 50.0 })
            for (const auto flex : { 0.0, 100.0 })
            {
                const auto vibratoContour = [inputDepth] (int sample)
                {
                    return ContourPoint { inputDepth * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate), true };
                };
                const auto reduced = runContour (64, 96000, retune, vibratoContour, flex, 0.0, -12.0);
                const auto neutral = runContour (64, 96000, retune, vibratoContour, flex, 0.0, 0.0);
                const auto increased = runContour (64, 96000, retune, vibratoContour, flex, 0.0, 12.0);
                const auto reducedPeak = centredOutputPeak (reduced, 24000);
                const auto neutralPeak = centredOutputPeak (neutral, 24000);
                const auto increasedPeak = centredOutputPeak (increased, 24000);
                std::cout << "natural_vibrato | input_depth_cents=" << inputDepth
                          << " retune_ms=" << retune << " flex_percent=" << flex
                          << " reduced_peak_cents=" << reducedPeak
                          << " neutral_peak_cents=" << neutralPeak
                          << " increased_peak_cents=" << increasedPeak
                          << " target_switches=" << targetSwitchCount (neutral) << '\n';
                require (std::isfinite (reducedPeak) && std::isfinite (neutralPeak) && std::isfinite (increasedPeak),
                         "natural vibrato produced a non-finite output");
                const auto neutralIsHardTune = retune == 0.0 && flex == 0.0;
                if (! neutralIsHardTune)
                {
                    require (reducedPeak < neutralPeak * 0.95,
                             "negative natural vibrato did not reduce an existing residual");
                    require (increasedPeak > neutralPeak * 1.05,
                             "positive natural vibrato did not increase an existing residual");
                }
                // With no Flex or Retune trajectory moving underneath it, the
                // measured output modulation is the renderer-facing residual
                // itself and must obey the 80-cent safety clamp.
                if (retune == 0.0 && flex == 0.0)
                    require (increasedPeak <= 80.0 + 1.0e-9,
                             "natural vibrato exceeded its depth safety limit");
                require (targetSwitchCount (reduced) == 0 && targetSwitchCount (neutral) == 0
                         && targetSwitchCount (increased) == 0,
                         "natural vibrato changed the target note");
            }

    const auto naturalReference = runContour (64, 60000, 50.0, vibrato, 100.0, 0.0, 12.0);
    for (const auto blockSize : { 64, 1024 })
    {
        const auto compared = runContour (blockSize, 60000, 50.0, vibrato, 100.0, 0.0, 12.0);
        const auto difference = maximumBlockDifference (naturalReference, compared);
        std::cout << "natural_vibrato_block_invariance | block=" << blockSize
                  << " max_cents_difference=" << difference << '\n';
        require (difference <= 1.0e-12, "natural vibrato depended on host block size");
    }
}
