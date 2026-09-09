#pragma once

#include <cstdint>

// Sits after target commitment and before renderer delivery. It deliberately
// does not modify the chosen target or CorrectionTrajectory: Retune remains
// the only control that determines how pitch follows that target.
class NoteTransition
{
public:
    void reset() noexcept
    {
        dryBridgeFramesRemaining = 0;
        bridgeCount = 0;
    }

    bool update (bool correctionVoiced, bool extremeTargetChange, bool hardTune) noexcept
    {
        if (! correctionVoiced)
        {
            dryBridgeFramesRemaining = 0;
            return false;
        }

        // CorrectionTrajectory already rejects/limits anomalous targets and
        // carries every committed target through a continuous S-curve.  A
        // one-frame dry bridge here interrupted that path precisely on an
        // octave false positive, causing an audible wet/dry click.  Keep the
        // renderer continuous; an extreme target is telemetry, not a reason
        // to splice audio paths.
        (void) extremeTargetChange;
        (void) hardTune;

        if (dryBridgeFramesRemaining <= 0)
            return false;

        --dryBridgeFramesRemaining;
        return true;
    }

    uint64_t getBridgeCount() const noexcept { return bridgeCount; }

private:
    int dryBridgeFramesRemaining = 0;
    uint64_t bridgeCount = 0;
};
