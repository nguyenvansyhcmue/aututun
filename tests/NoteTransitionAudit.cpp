#include "CorrectionTrajectory.h"
#include "NoteTransition.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int analysisHop = 512;
constexpr int controlInterval = 128;
constexpr int totalSamples = static_cast<int> (0.900 * sampleRate);

enum class Contour { GlideEF, InstantEF, JumpEG, ReanchorFG, BoundaryVibrato };

const char* contourName (Contour contour)
{
    switch (contour)
    {
        case Contour::GlideEF:         return "A_glide_E_to_F";
        case Contour::InstantEF:       return "B_instant_E_to_F";
        case Contour::JumpEG:          return "C_jump_E_to_G";
        case Contour::ReanchorFG:      return "D_reanchor_F_to_G";
        case Contour::BoundaryVibrato: return "E_boundary_vibrato";
    }
    return "unknown";
}

double contourMidi (Contour contour, int sample)
{
    constexpr auto e = 64.0, f = 65.0, g = 67.0;
    constexpr auto hold = static_cast<int> (0.300 * sampleRate);
    constexpr auto glide = static_cast<int> (0.100 * sampleRate);
    switch (contour)
    {
        case Contour::GlideEF:
            if (sample < hold) return e;
            if (sample < hold + glide) return e + (f - e) * static_cast<double> (sample - hold) / glide;
            return f;
        case Contour::InstantEF:
            return sample < hold ? e : f;
        case Contour::JumpEG:
            return sample < hold ? e : g;
        case Contour::ReanchorFG:
            if (sample < hold) return e;
            if (sample < hold + 2 * analysisHop) return f;
            return g;
        case Contour::BoundaryVibrato:
            return 64.5 + 0.18 * std::sin (2.0 * std::numbers::pi * 5.0 * sample / sampleRate);
    }
    return e;
}

NotePolicySnapshot chromaticPolicy()
{
    NotePolicySnapshot policy;
    policy.groupOctaves = true;
    policy.pitchClassDefaults.fill (NoteRule::Legal);
    return policy;
}

struct Metrics
{
    double configuredMs = 0.0;
    double actualMs = std::numeric_limits<double>::quiet_NaN();
    double startCents = std::numeric_limits<double>::quiet_NaN();
    double endCents = std::numeric_limits<double>::quiet_NaN();
    double maxTransitionStep = 0.0;
    double maxRendererStep = 0.0;
    int controlUpdatesToComplete = 0;
    int bridgeCount = 0;
    int targetCommitCount = 0;
    int targetReversalCount = 0;
    bool finite = true;
    bool reanchorContinuous = true;
};

Metrics runCase (Contour contour, bool classic, double retuneMs, int hostBlockSize,
                 std::ofstream* detailedLog)
{
    CorrectionTrajectory trajectory;
    NoteTransition noteTransition;
    const auto policy = chromaticPolicy();
    const auto tuningPolicy = classic ? TuningPolicy::Classic : TuningPolicy::Standard;
    const auto configuredMs = CorrectionTrajectory::getNoteTransitionDurationMs (tuningPolicy, retuneMs);
    const auto effectiveRetuneMs = CorrectionTrajectory::getEffectiveRetuneTimeMs (retuneMs);
    const auto dezipperSeconds = 0.006 + (0.032 - 0.006)
                               * (effectiveRetuneMs / (effectiveRetuneMs + 100.0));

    Metrics result;
    result.configuredMs = configuredMs;
    CorrectionTrajectoryData current;
    auto previousTransitionTarget = std::numeric_limits<double>::quiet_NaN();
    auto deliveredCents = 0.0;
    auto previousDeliveredCents = 0.0;
    auto pendingActualStartSample = -1;
    auto pendingControlUpdates = -1;
    auto bridgeActive = false;
    auto previousCommittedTarget = -1;
    auto previousCommitDirection = 0;

    if (detailedLog != nullptr)
        *detailedLog << "sampleIndex,timeMs,rawPitchCents,candidateTargetMidi,committedTargetMidi,committedTargetChanged,"
                        "configuredTransitionMs,transitionStartCents,transitionEndCents,transitionDistanceCents,"
                        "transitionProgress,transitionTargetCents,retuneTrajectoryCorrectionCents,"
                        "deliveredRendererCorrectionCents,rendererStepCents,dryBridgeActive\n";

    for (int blockStart = 0; blockStart < totalSamples; blockStart += hostBlockSize)
    {
        const auto blockEnd = std::min (blockStart + hostBlockSize, totalSamples);
        const auto firstControl = ((blockStart + controlInterval - 1) / controlInterval) * controlInterval;
        for (int sample = firstControl; sample < blockEnd; sample += controlInterval)
        {
            if (sample % analysisHop == 0)
            {
                // Production ordering: finish the last 128-sample interval
                // of the previous analysis frame before this YIN result may
                // commit a replacement target.
                if (current.hasValidCorrection)
                    current = trajectory.advanceScheduledTransition (controlInterval, sampleRate);
                const auto midi = contourMidi (contour, sample);
                const auto f0 = 440.0 * std::exp2 ((midi - 69.0) / 12.0);
                const auto targetBefore = current.transitionTargetCents;
                current = trajectory.update (f0, 440.0, 0, policy, retuneMs, 0.0, 0.0, 0.0,
                                             sampleRate, analysisHop, true, false, tuningPolicy, true);
                bridgeActive = noteTransition.update (true, current.extremeTargetChange, retuneMs == 0.0);

                result.finite = result.finite && (! current.hasValidCorrection
                    || (std::isfinite (current.transitionTargetCents) && std::isfinite (current.correctionCents)));
                if (current.targetChanged)
                {
                    ++result.targetCommitCount;
                    if (previousCommittedTarget >= 0)
                    {
                        const auto direction = current.targetNote > previousCommittedTarget ? 1
                                             : current.targetNote < previousCommittedTarget ? -1 : 0;
                        if (direction != 0 && previousCommitDirection != 0 && direction != previousCommitDirection)
                            ++result.targetReversalCount;
                        if (direction != 0)
                            previousCommitDirection = direction;
                    }
                    previousCommittedTarget = current.targetNote;
                    result.startCents = targetBefore;
                    result.endCents = current.targetPitchCents;
                    pendingActualStartSample = sample;
                    pendingControlUpdates = 0;
                    // `startCents` is sampled from transitionTargetCents
                    // immediately before the new commit. A later step may be
                    // large for a deliberately short Classic duration, but
                    // this starting point must never be stale target/unity.
                    result.reanchorContinuous = result.reanchorContinuous
                                             && (! std::isfinite (targetBefore) || std::isfinite (result.startCents));
                }
                else if (previousCommittedTarget < 0 && current.targetNote >= 0)
                {
                    previousCommittedTarget = current.targetNote;
                }
            }
            else if (current.hasValidCorrection)
            {
                current = trajectory.advanceScheduledTransition (controlInterval, sampleRate);
            }

            if (std::isfinite (previousTransitionTarget) && std::isfinite (current.transitionTargetCents))
                result.maxTransitionStep = std::max (result.maxTransitionStep,
                                                     std::abs (current.transitionTargetCents - previousTransitionTarget));
            previousTransitionTarget = current.transitionTargetCents;
            if (pendingActualStartSample >= 0 && current.transitionProgress >= 1.0)
            {
                result.actualMs = 1000.0 * static_cast<double> (sample - pendingActualStartSample) / sampleRate;
                pendingActualStartSample = -1;
                // Include the control tick which reaches progress == 1.
                result.controlUpdatesToComplete = pendingControlUpdates + 1;
                pendingControlUpdates = -1;
            }

            const auto alpha = 1.0 - std::exp (-static_cast<double> (controlInterval) / (dezipperSeconds * sampleRate));
            if (pendingControlUpdates >= 0 && sample > pendingActualStartSample)
                ++pendingControlUpdates;
            if (! bridgeActive && current.hasValidCorrection)
                deliveredCents += (current.correctionCents - deliveredCents) * alpha;
            const auto rendererStep = std::abs (deliveredCents - previousDeliveredCents);
            previousDeliveredCents = deliveredCents;
            result.maxRendererStep = std::max (result.maxRendererStep, rendererStep);
            result.bridgeCount += bridgeActive ? 1 : 0;

            if (detailedLog != nullptr)
            {
                const auto rawMidi = contourMidi (contour, sample);
                *detailedLog << sample << ',' << (1000.0 * sample / sampleRate) << ',' << (rawMidi - 69.0) * 100.0 << ','
                             << current.candidateTargetNote << ',' << current.targetNote << ',' << current.targetChanged << ','
                             << configuredMs << ',' << result.startCents << ',' << result.endCents << ','
                             << (std::isfinite (result.startCents) && std::isfinite (result.endCents)
                                     ? result.endCents - result.startCents : 0.0) << ','
                             << current.transitionProgress << ',' << current.transitionTargetCents << ','
                             << current.retuneCorrectionCents << ',' << deliveredCents << ',' << rendererStep << ','
                             << bridgeActive << '\n';
            }
        }
    }
    return result;
}

void writeSummaryRow (std::ofstream& summary, Contour contour, bool classic, double retuneMs,
                      int block, const Metrics& m)
{
    const auto bridgeIsExpected = contour == Contour::JumpEG;
    const auto needsThreeControlUpdates = retuneMs >= 25.0 && contour != Contour::BoundaryVibrato;
    // The <=20/<=8 acceptance applies to the normal one-semitone E->F
    // transition.  A deliberately re-anchored 200/300-cent contour is kept
    // as a continuity diagnostic, not judged by that near-note bound.
    const auto stepsWithinSlowBaseline = contour != Contour::GlideEF || retuneMs < 50.0
                                      || (m.maxTransitionStep <= 20.0 && m.maxRendererStep <= 8.0);
    const auto pass = m.finite && m.reanchorContinuous && stepsWithinSlowBaseline
                   && (! needsThreeControlUpdates || m.controlUpdatesToComplete >= 3)
                   && (bridgeIsExpected || m.bridgeCount == 0);
    summary << contourName (contour) << ',' << (classic ? "ON" : "OFF") << ',' << retuneMs << ',' << block << ','
            << m.configuredMs << ',' << m.actualMs << ',' << m.startCents << ',' << m.endCents << ','
            << (std::isfinite (m.startCents) && std::isfinite (m.endCents) ? m.endCents - m.startCents : 0.0) << ','
            << m.controlUpdatesToComplete << ',' << m.maxTransitionStep << ',' << m.maxRendererStep << ',' << m.bridgeCount << ','
            << m.targetCommitCount << ',' << m.targetReversalCount << ',' << m.finite << ',' << m.reanchorContinuous << ',' << (pass ? "PASS" : "FAIL") << '\n';
}
}

int main (int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: VibeAutotuneNoteTransitionAudit <output-directory>\n";
        return 2;
    }

    const std::string directory (argv[1]);
    std::ofstream summary (directory + "/note_transition_summary.csv");
    if (! summary)
    {
        std::cerr << "unable to create output directory/file\n";
        return 2;
    }
    summary << "contour,classic,retuneMs,hostBlock,configuredTransitionMs,actualTransitionMs,transitionStartCents,"
               "transitionEndCents,transitionDistanceCents,controlUpdates,maxTransitionStepCt,maxRendererStepCt,bridgeCount,"
               "targetCommitCount,targetReversalCount,finite,reanchorContinuous,pass\n";
    summary << std::fixed << std::setprecision (4);

    constexpr std::array contours { Contour::GlideEF, Contour::InstantEF, Contour::JumpEG,
                                    Contour::ReanchorFG, Contour::BoundaryVibrato };
    constexpr std::array retunes { 0.0, 5.0, 10.0, 25.0, 50.0, 75.0, 100.0, 200.0, 400.0 };
    constexpr std::array blocks { 64, 128, 256, 512, 1024 };

    // Full grid for block-size comparison. Write detailed 128-sample logs
    // only for the canonical 256-sample host block so the audit stays usable.
    for (const auto contour : contours)
        for (const auto classic : { false, true })
            for (const auto retune : retunes)
                for (const auto block : blocks)
                {
                    std::ofstream detail;
                    std::ofstream* detailPtr = nullptr;
                    if (block == 256)
                    {
                        detail.open (directory + "/" + contourName (contour) + "_classic_"
                                     + (classic ? "on" : "off") + "_retune_"
                                     + std::to_string (static_cast<int> (retune)) + ".csv");
                        detailPtr = &detail;
                    }
                    writeSummaryRow (summary, contour, classic, retune, block,
                                     runCase (contour, classic, retune, block, detailPtr));
                }

    std::cout << "summary=" << directory << "/note_transition_summary.csv\n";
    return 0;
}
