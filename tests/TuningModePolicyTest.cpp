#include "CorrectionTrajectory.h"
#include "NoteTransition.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int hopSamples = 512;

NotePolicySnapshot chromaticPolicy()
{
    NotePolicySnapshot policy;
    for (int note = 0; note <= 127; ++note)
    policy.pitchClassDefaults.fill (NoteRule::Legal);
    return policy;
}

double frequencyForMidi (double midi)
{
    return 440.0 * std::exp2 ((midi - 69.0) / 12.0);
}

CorrectionTrajectoryData update (CorrectionTrajectory& trajectory, double midi,
                                 double retuneMs, double flex, TuningPolicy policy)
{
    return trajectory.update (frequencyForMidi (midi), 440.0, 0, chromaticPolicy(),
                              retuneMs, flex, 0.0, 0.0,
                              sampleRate, hopSamples, true, false, policy);
}

void require (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit (1);
    }
}

CorrectionTrajectoryData settleAt (CorrectionTrajectory& trajectory, double midi,
                                    double retuneMs, double flex, TuningPolicy policy, int frames = 8)
{
    CorrectionTrajectoryData result;
    for (int frame = 0; frame < frames; ++frame)
        result = update (trajectory, midi, retuneMs, flex, policy);
    return result;
}
}

int main()
{
    std::cout << std::fixed << std::setprecision (3);

    // Standard policy retains Flex; Classic must ignore it while retaining
    // the saved parameter value for a later switch back to Classic OFF.
    CorrectionTrajectory formantFlex0;
    CorrectionTrajectory formantFlex100;
    const auto formant0 = settleAt (formantFlex0, 64.35, 25.0, 0.0, TuningPolicy::Standard);
    const auto formant100 = settleAt (formantFlex100, 64.35, 25.0, 100.0, TuningPolicy::Standard);
    const auto formantDifference = std::abs (formant0.correctionCents - formant100.correctionCents);
    std::cout << "formant_flex_difference_cents=" << formantDifference << '\n';
    require (formantDifference > 0.5, "Classic OFF policy did not apply Flex-Tune");

    CorrectionTrajectory classicFlex0;
    CorrectionTrajectory classicFlex100;
    const auto classic0 = settleAt (classicFlex0, 64.35, 25.0, 0.0, TuningPolicy::Classic);
    const auto classic100 = settleAt (classicFlex100, 64.35, 25.0, 100.0, TuningPolicy::Classic);
    const auto classicDifference = std::abs (classic0.correctionCents - classic100.correctionCents);
    std::cout << "classic_flex_difference_cents=" << classicDifference << '\n';
    require (classicDifference < 1.0e-9, "Classic mode did not bypass Flex-Tune");

    // FormantEnabled is intentionally absent from CorrectionTrajectory's
    // interface: it is downstream renderer state. Thus either formant value
    // must leave the target trajectory and Flex weight identical.
    CorrectionTrajectory formantOnTrajectory;
    CorrectionTrajectory formantOffTrajectory;
    const auto formantOn = settleAt (formantOnTrajectory, 64.35, 25.0, 50.0, TuningPolicy::Standard);
    const auto formantOff = settleAt (formantOffTrajectory, 64.35, 25.0, 50.0, TuningPolicy::Standard);
    const auto formantTrajectoryDifference = std::max (std::abs (formantOn.transitionTargetCents - formantOff.transitionTargetCents),
                                                        std::abs (formantOn.flexCorrectionGain - formantOff.flexCorrectionGain));
    std::cout << "formant_on_off_trajectory_difference=" << formantTrajectoryDifference << '\n';
    require (formantTrajectoryDifference < 1.0e-12,
             "Formant preservation unexpectedly affected target or Flex trajectory");

    // E -> F at Retune 25 ms: the policies commit the same note, but Classic
    // OFF intentionally keeps a short continuous target transition while Classic
    // reaches its shorter destination policy first.
    CorrectionTrajectory formantTransition;
    CorrectionTrajectory classicTransition;
    settleAt (formantTransition, 64.0, 25.0, 0.0, TuningPolicy::Standard);
    settleAt (classicTransition, 64.0, 25.0, 0.0, TuningPolicy::Classic);
    CorrectionTrajectoryData formantChange, classicChange;
    auto formantCommitted = false;
    auto classicCommitted = false;
    for (int frame = 0; frame < 6; ++frame)
    {
        const auto formantFrame = update (formantTransition, 65.0, 25.0, 0.0, TuningPolicy::Standard);
        const auto classicFrame = update (classicTransition, 65.0, 25.0, 0.0, TuningPolicy::Classic);
        if (formantFrame.targetChanged) { formantChange = formantFrame; formantCommitted = true; }
        if (classicFrame.targetChanged) { classicChange = classicFrame; classicCommitted = true; }
        if (formantCommitted && classicCommitted)
            break;
    }
    std::cout << "E_to_F | formant_target=" << formantChange.targetNote
              << " formant_progress=" << formantChange.transitionProgress
              << " classic_target=" << classicChange.targetNote
              << " classic_progress=" << classicChange.transitionProgress << '\n';
    require (formantCommitted && classicCommitted && formantChange.targetNote == 65 && classicChange.targetNote == 65,
             "modes committed a different E-to-F target");
    require (formantChange.transitionActive && formantChange.transitionProgress > 0.0
             && formantChange.transitionProgress < 1.0,
             "Classic OFF policy did not create a continuous target transition");
    require (! classicChange.transitionActive && classicChange.transitionProgress == 1.0,
             "Classic ON policy was not shorter at Retune 25 ms");

    // A second committed target during an in-flight Formant transition must
    // continue from the in-flight value, never from stale E or unity.
    CorrectionTrajectory reanchor;
    settleAt (reanchor, 64.0, 100.0, 0.0, TuningPolicy::Standard);
    CorrectionTrajectoryData firstChange;
    auto firstCommitted = false;
    for (int frame = 0; frame < 6; ++frame)
    {
        const auto frameData = update (reanchor, 65.0, 100.0, 0.0, TuningPolicy::Standard);
        if (frameData.targetChanged)
        {
            firstChange = frameData;
            firstCommitted = true;
            break;
        }
    }
    require (firstCommitted && firstChange.transitionActive, "expected an in-flight transition before re-anchor");
    const auto beforeSecondCommit = firstChange.transitionTargetCents;
    CorrectionTrajectoryData secondChange;
    auto secondCommitted = false;
    for (int frame = 0; frame < 6; ++frame)
    {
        const auto frameData = update (reanchor, 66.0, 100.0, 0.0, TuningPolicy::Standard);
        if (frameData.targetChanged)
        {
            secondChange = frameData;
            secondCommitted = true;
            break;
        }
    }
    std::cout << "reanchor | before=" << beforeSecondCommit
              << " after=" << secondChange.transitionTargetCents
              << " progress=" << secondChange.transitionProgress << '\n';
    require (secondCommitted && secondChange.targetNote == 66 && secondChange.transitionTargetCents >= beforeSecondCommit,
             "second transition did not continue from the in-flight target");

    // Classic hard-tune may be musically near-instant, but renderer control
    // remains protected by its existing dezipper. This policy layer itself
    // never uses a dry bridge for an ordinary note change.
    NoteTransition bridge;
    const auto ordinaryBridge = bridge.update (true, false, true);
    std::cout << "ordinary_target_bridge=" << ordinaryBridge << '\n';
    require (! ordinaryBridge && bridge.getBridgeCount() == 0,
             "ordinary target change incorrectly entered dry bridge");

    const auto formantHardMs = CorrectionTrajectory::getNoteTransitionDurationMs (TuningPolicy::Standard, 0.0);
    const auto classicHardMs = CorrectionTrajectory::getNoteTransitionDurationMs (TuningPolicy::Classic, 0.0);
    std::cout << "hard_tune_transition_ms | formant=" << formantHardMs
              << " classic=" << classicHardMs << '\n';
    require (classicHardMs < formantHardMs, "Classic hard-tune policy is not faster");

    std::cout << "PASS\n";
    return 0;
}
