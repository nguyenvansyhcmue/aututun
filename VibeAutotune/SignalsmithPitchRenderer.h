#pragma once

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Fixed-latency spectral pitch renderer used for voiced correction.  It owns
// no note selection: the existing trajectory remains the sole control source.
class SignalsmithPitchRenderer
{
public:
    void prepare (double newSampleRate, int maximumBlock)
    {
        sampleRate = newSampleRate;
        stretch.configure (1, 1024, 128, true);
        stretch.setFormantBase (static_cast<float> (200.0 / sampleRate));
        stretch.reset();
        dryDelay.assign (static_cast<size_t> (latencySamples() + maximumBlock + 1), 0.0f);
        dryWrite = 0;
        wetGate = 0.0f;
        // This is a lifecycle gate, not a musical envelope or a noise gate.
        // It only prevents an instantaneous spectral-wet -> delayed-dry
        // splice when voicing certification changes at a control boundary.
        wetGateAttack = onePoleCoefficient (0.0015);
        wetGateRelease = onePoleCoefficient (0.0040);
    }
    void reset()
    {
        stretch.reset();
        std::fill (dryDelay.begin(), dryDelay.end(), 0.0f);
        dryWrite = 0;
        wetGate = 0.0f;
    }
    int latencySamples() const noexcept { return stretch.inputLatency() + stretch.outputLatency(); }
    void process (const float* source, float* destination, int samples, double ratio, bool voiced) noexcept
    {
        if (source == nullptr || destination == nullptr || samples <= 0 || dryDelay.empty()) return;
        const auto safeRatio = static_cast<float> (std::clamp (ratio, 0.90, 1.10));
        stretch.setTransposeFactor (safeRatio);
        struct Input { const float* p; const float* operator[] (int) const { return p; } } input { source };
        struct Output { float* p; float* operator[] (int) const { return p; } } output { destination };
        stretch.process (input, samples, output, samples);
        for (int n = 0; n < samples; ++n)
        {
            dryDelay[dryWrite] = source[n];
            const auto read = (dryWrite + dryDelay.size() - static_cast<size_t> (latencySamples())) % dryDelay.size();
            const auto dry = dryDelay[read];
            // A direct wet/dry switch can create a one-sample click because
            // the spectral output and its latency-aligned dry counterpart
            // are not sample-identical around a pitch event.  Smooth only
            // that renderer lifecycle edge.  The 1.5/4 ms time constants
            // are shorter than a vocal note transition, so this cannot turn
            // a discrete target note into a glide.
            const auto targetWet = voiced ? 1.0f : 0.0f;
            const auto coefficient = targetWet > wetGate ? wetGateAttack : wetGateRelease;
            wetGate += (targetWet - wetGate) * coefficient;
            destination[n] = dry + (destination[n] - dry) * wetGate;
            dryWrite = (dryWrite + 1) % dryDelay.size();
        }
    }
private:
    float onePoleCoefficient (double seconds) const noexcept
    {
        return static_cast<float> (1.0 - std::exp (-1.0 / std::max (1.0, seconds * sampleRate)));
    }

    double sampleRate = 48000.0;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::vector<float> dryDelay;
    size_t dryWrite = 0;
    float wetGate = 0.0f;
    float wetGateAttack = 1.0f;
    float wetGateRelease = 1.0f;
};
