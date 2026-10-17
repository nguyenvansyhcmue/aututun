#include "PluginProcessor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

class ControlHandoffTestAccess
{
public:
    static void push (VibeAutotuneProcessor& processor, uint64_t timestamp,
                      double f0, double ratio, bool valid)
    {
        processor.pushPsolaControlFrame (timestamp, f0, ratio, 0.0, valid);
    }

    static auto at (const VibeAutotuneProcessor& processor, uint64_t timestamp)
    {
        return processor.getRendererControlFrame (timestamp);
    }
};

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int tick = 128;

void require (bool condition, const char* message)
{
    if (! condition) { std::cerr << "FAILED: " << message << '\n'; std::exit (1); }
}

NotePolicySnapshot a4Only()
{
    NotePolicySnapshot policy;
    policy.pitchClassDefaults.fill (NoteRule::IllegalNearest);
    policy.hasMidiNoteOverride[69] = true;
    policy.midiNoteOverrides[69] = NoteRule::Legal;
    policy.groupOctaves = false;
    return policy;
}
}

int main()
{
    VibeAutotuneProcessor processor;
    CorrectionTrajectory trajectory;
    const auto policy = a4Only();
    uint64_t timestamp = 0;
    auto checked = 0;

    // A4 is the sole legal target, so these source positions create the
    // requested +20, +70, -40 and 0-cent trajectory corrections exactly.
    for (const auto expectedCents : { 20.0, 70.0, -40.0, 0.0 })
    {
        const auto sourceCents = -expectedCents;
        const auto f0 = 440.0 * std::exp2 (sourceCents / 1200.0);
        for (int observation = 0; observation < 4; ++observation)
        {
            const auto data = trajectory.update (f0, 440.0, 0, policy, 0.0, 0.0, 0.0, 0.0,
                                                 sampleRate, 512, true, false,
                                                 TuningPolicy::Standard, true);
            if (! data.hasValidCorrection) continue;
            auto scheduled = data;
            for (int frame = 0; frame < 4; ++frame)
            {
                if (frame != 0) scheduled = trajectory.advanceScheduledTransition (tick, sampleRate);
                ControlHandoffTestAccess::push (processor, timestamp, f0, scheduled.pitchRatio,
                                                scheduled.hasValidCorrection);
                const auto control = ControlHandoffTestAccess::at (processor, timestamp);
                const auto error = std::abs (1200.0 * std::log2 (control.pitchRatio / scheduled.pitchRatio));
                require (control.valid == scheduled.hasValidCorrection && error < 1.0e-9,
                         "PsolaControlFrame did not preserve its source-timestamp trajectory ratio");
                ++checked;
                timestamp += tick;
            }
        }
        std::cout << "control_handoff | expected_cents=" << expectedCents << " checked_frames=" << checked << '\n';
    }
    require (checked > 0, "no valid trajectory frame was produced");
    std::cout << "CONTROL_HANDOFF: PASS | checked_frames=" << checked << '\n';
}
