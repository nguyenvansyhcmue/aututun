#include "CausalTDPSOLARenderer.h"

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

namespace
{
double estimateF0 (const std::vector<float>& samples, int begin, int end, double sampleRate)
{
    auto crossings = 0;
    for (int i = begin + 1; i < end; ++i)
        if (samples[static_cast<size_t> (i - 1)] <= 0.0f && samples[static_cast<size_t> (i)] > 0.0f) ++crossings;
    return crossings * sampleRate / static_cast<double> (end - begin);
}

void require (bool condition, const char* message)
{
    if (! condition) { std::cerr << "FAILED: " << message << '\n'; std::exit (1); }
}

double centsError (double actual, double expected) { return 1200.0 * std::log2 (actual / expected); }

double sinusoidMagnitude (const std::vector<float>& samples, int begin, int end,
                          double frequency, double sampleRate)
{
    double real = 0.0, imaginary = 0.0;
    const auto increment = 2.0 * std::numbers::pi * frequency / sampleRate;
    for (int i = begin; i < end; ++i)
    {
        const auto phase = increment * static_cast<double> (i - begin);
        const auto position = static_cast<double> (i - begin) / static_cast<double> (std::max (1, end - begin - 1));
        const auto window = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * position);
        real += window * static_cast<double> (samples[static_cast<size_t> (i)]) * std::cos (phase);
        imaginary -= window * static_cast<double> (samples[static_cast<size_t> (i)]) * std::sin (phase);
    }
    return 2.0 * std::sqrt (real * real + imaginary * imaginary)
         / static_cast<double> (std::max (1, end - begin));
}

// Measures whether a transition contains two simultaneously audible
// fundamentals.  A pure frequency estimate cannot detect this: it reports
// only the stronger note and hides the quieter, trailing note that listeners
// perceive as a second voice.
double trailingNoteDb (const std::vector<float>& samples, int begin, int end,
                       double oldFrequency, double newFrequency, double sampleRate)
{
    const auto oldMagnitude = sinusoidMagnitude (samples, begin, end, oldFrequency, sampleRate);
    const auto newMagnitude = sinusoidMagnitude (samples, begin, end, newFrequency, sampleRate);
    return 20.0 * std::log10 (std::max (oldMagnitude, 1.0e-12)
                              / std::max (newMagnitude, 1.0e-12));
}

void reportRapidReturnForensics()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 64;
    constexpr int latency = 256;
    constexpr int segmentSamples = 4608; // 96 ms: still a rapid return, but enough for an honest F0 measurement.
    constexpr double sourceF0 = 220.0;
    constexpr double bRatio = 0.8908987181; // A -> G, then immediately return A.
    constexpr int total = segmentSamples * 5;

    CausalTDPSOLARenderer renderer;
    renderer.prepare (sampleRate, block, latency);
    std::vector<float> input (block), output (block), rendered;
    rendered.reserve (total);
    auto phase = 0.0;
    for (int start = 0; start < total; start += block)
    {
        const auto segment = start / segmentSamples;
        const auto ratio = segment == 2 ? bRatio : 1.0;
        const auto targetHz = sourceF0 * ratio;
        for (int n = 0; n < block; ++n)
        {
            // A small harmonic stack makes the probe closer to a vowel while
            // retaining known fundamental frequencies for the measurement.
            input[static_cast<size_t> (n)] = static_cast<float> (
                0.42 * std::sin (phase) + 0.18 * std::sin (2.0 * phase)
              + 0.07 * std::sin (3.0 * phase));
            phase += 2.0 * std::numbers::pi * sourceF0 / sampleRate;
        }
        renderer.process (input.data(), output.data(), block, sourceF0, ratio, true, false, targetHz);
        rendered.insert (rendered.end(), output.begin(), output.end());
    }

    // Inspect the final 24 ms of B, then the first 24 ms after B->A.  The
    // second value is the direct double-voice metric: old B must be well
    // below the returned A, without relying on a subjective waveform view.
    constexpr int probe = 2304;
    const auto bEndAtOutput = segmentSamples * 3 + latency;
    const auto returnBegin = bEndAtOutput;
    const auto bTailDb = trailingNoteDb (rendered, bEndAtOutput - probe, bEndAtOutput,
                                         sourceF0, sourceF0 * bRatio, sampleRate);
    const auto returnTailDb = trailingNoteDb (rendered, returnBegin, returnBegin + probe,
                                              sourceF0 * bRatio, sourceF0, sampleRate);
    std::cout << "renderer_rapid_return"
              << " b_tail_hz=" << estimateF0 (rendered, bEndAtOutput - probe, bEndAtOutput, sampleRate)
              << " b_tail_a_over_b_db=" << bTailDb
              << " return_b_over_a_db=" << returnTailDb << '\n';
}

void reportHarmonicProfile (const char* name, double ratio)
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 64;
    constexpr int total = static_cast<int> (sampleRate * 3.0);
    constexpr double sourceF0 = sampleRate / 132.0;
    CausalTDPSOLARenderer renderer;
    renderer.prepare (sampleRate, block, 256);
    std::vector<float> input (block), output (block), rendered;
    rendered.reserve (total);
    auto phase = 0.0;
    for (int start = 0; start < total; start += block)
    {
        for (int i = 0; i < block; ++i)
        {
            input[static_cast<size_t> (i)] = static_cast<float> (
                0.58 * std::sin (phase) + 0.26 * std::sin (2.0 * phase)
              + 0.11 * std::sin (3.0 * phase) + 0.05 * std::sin (4.0 * phase));
            phase += 2.0 * std::numbers::pi * sourceF0 / sampleRate;
        }
        renderer.process (input.data(), output.data(), block, sourceF0, ratio, true);
        rendered.insert (rendered.end(), output.begin(), output.end());
    }

    constexpr int begin = 48000, end = 96000;
    const auto outputF0 = sourceF0 * ratio;
    const auto fundamental = sinusoidMagnitude (rendered, begin, end, outputF0, sampleRate);
    std::cout << "renderer_harmonics | case=" << name << " ratio=" << ratio;
    for (int harmonic = 2; harmonic <= 4; ++harmonic)
    {
        const auto magnitude = sinusoidMagnitude (rendered, begin, end, outputF0 * harmonic, sampleRate);
        std::cout << " h" << harmonic << "_db="
                  << 20.0 * std::log10 (std::max (magnitude, 1.0e-12) / std::max (fundamental, 1.0e-12));
    }
    std::cout << '\n';
}

void reportHarmonicProfileWithControlJitter()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 128;
    constexpr int total = static_cast<int> (sampleRate * 3.0);
    constexpr double sourceF0 = sampleRate / 132.0;
    constexpr double targetF0 = 369.994423;
    CausalTDPSOLARenderer renderer;
    renderer.prepare (sampleRate, block, 256);
    std::vector<float> input (block), output (block), rendered;
    rendered.reserve (total);
    auto phase = 0.0;
    for (int start = 0; start < total; start += block)
    {
        // This mirrors the measured tracker behaviour on the held probe:
        // F0 is refreshed in short control blocks with roughly +/- 1 Hz
        // variation, while ratio is recomputed from that imperfect value.
        const auto controlF0 = sourceF0 - 0.64 + 0.96 * std::sin (2.0 * std::numbers::pi
                                                           * static_cast<double> (start) / 2048.0);
        const auto controlRatio = targetF0 / controlF0;
        for (int i = 0; i < block; ++i)
        {
            input[static_cast<size_t> (i)] = static_cast<float> (
                0.58 * std::sin (phase) + 0.26 * std::sin (2.0 * phase)
              + 0.11 * std::sin (3.0 * phase) + 0.05 * std::sin (4.0 * phase));
            phase += 2.0 * std::numbers::pi * sourceF0 / sampleRate;
        }
        renderer.process (input.data(), output.data(), block, controlF0, controlRatio, true, false, targetF0);
        rendered.insert (rendered.end(), output.begin(), output.end());
    }
    constexpr int begin = 48000, end = 96000;
    const auto fundamental = sinusoidMagnitude (rendered, begin, end, targetF0, sampleRate);
    std::cout << "renderer_harmonics | case=plus_30_cents_control_bias_and_jitter";
    for (int harmonic = 2; harmonic <= 4; ++harmonic)
    {
        const auto magnitude = sinusoidMagnitude (rendered, begin, end, targetF0 * harmonic, sampleRate);
        std::cout << " h" << harmonic << "_db="
                  << 20.0 * std::log10 (std::max (magnitude, 1.0e-12) / std::max (fundamental, 1.0e-12));
    }
    std::cout << '\n';
}
}

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 64;
    constexpr int total = static_cast<int> (sampleRate * 3.0);
    // These two cases isolate renderer resynthesis from YIN, target
    // selection, and host timing. The source is exactly periodic with an
    // exact 132-sample period, so a loss of H2/H3/H4 can only originate in
    // grain selection or OLA geometry.
    reportHarmonicProfile ("identity", 1.0);
    reportHarmonicProfile ("plus_30_cents", std::exp2 (30.0 / 1200.0));
    reportHarmonicProfileWithControlJitter();
    reportRapidReturnForensics();
    for (const auto ratio : { 0.96, 1.0, 1.04 })
    {
        CausalTDPSOLARenderer renderer;
        renderer.prepare (sampleRate, block, 256);
        std::vector<float> input (block), output (block), rendered;
        rendered.reserve (total);
        double phase = 0.0;
        auto maximumStep = 0.0;
        for (int start = 0; start < total; start += block)
        {
            for (int i = 0; i < block; ++i)
            {
                input[static_cast<size_t> (i)] = static_cast<float> (0.30 * std::sin (phase));
                phase += 2.0 * std::numbers::pi * 220.0 / sampleRate;
            }
            renderer.process (input.data(), output.data(), block, 220.0, ratio, true);
            for (const auto sample : output)
            {
                if (! rendered.empty()) maximumStep = std::max (maximumStep, std::abs (static_cast<double> (sample - rendered.back())));
                rendered.push_back (sample);
            }
        }
        constexpr int begin = 48000, end = 96000;
        const auto f0 = estimateF0 (rendered, begin, end, sampleRate);
        const auto expected = 220.0 * ratio;
        std::cout << "causal_tdpsola | latency=" << renderer.latencySamples() << " ratio=" << ratio
                  << " f0=" << f0 << " max_step=" << maximumStep << '\n';
        if (! std::isfinite (f0) || std::abs (1200.0 * std::log2 (f0 / expected)) > 20.0 || maximumStep > 0.10)
            return 1;
    }

    CausalTDPSOLARenderer dynamicRenderer;
    dynamicRenderer.prepare (sampleRate, block, 256);
    std::vector<float> input (block), output (block);
    double phase = 0.0;
    auto previous = 0.0f;
    auto dynamicMaximumStep = 0.0;
    for (int start = 0; start < total; start += block)
    {
        const auto time = static_cast<double> (start) / sampleRate;
        const auto vibrato = 35.0 * std::sin (2.0 * std::numbers::pi * 6.0 * time);
        const auto f0 = 220.0 * std::exp2 (vibrato / 1200.0);
        const auto ratio = 1.0 + 0.04 * std::sin (2.0 * std::numbers::pi * 2.0 * time);
        for (int i = 0; i < block; ++i)
        {
            input[static_cast<size_t> (i)] = static_cast<float> (0.30 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * f0 / sampleRate;
        }
        dynamicRenderer.process (input.data(), output.data(), block, f0, ratio, true);
        for (const auto sample : output)
        {
            dynamicMaximumStep = std::max (dynamicMaximumStep, std::abs (static_cast<double> (sample - previous)));
            previous = sample;
            if (! std::isfinite (sample)) return 1;
        }
    }
    std::cout << "causal_tdpsola_dynamic | max_step=" << dynamicMaximumStep << '\n';
    if (dynamicMaximumStep > 0.10) return 1;

    // Deterministic renderer conformance: direct F0/ratio controls, with no
    // tracker, target selector or trajectory in the path.
    auto conformancePass = true;
    for (const auto cents : { 0.0, 50.0, 100.0, -50.0, -60.0, -70.0, -80.0,
                              -90.0, -95.0, -99.0, -100.0, -101.0, -110.0 })
    {
        CausalTDPSOLARenderer renderer;
        renderer.prepare (sampleRate, block, 256);
        std::vector<float> all, in (block), out (block);
        all.reserve (static_cast<size_t> (sampleRate * 1.5));
        auto phase = 0.0;
        const auto ratio = std::exp2 (cents / 1200.0);
        for (int start = 0; start < static_cast<int> (sampleRate * 1.5); start += block)
        {
            for (int i = 0; i < block; ++i) { in[i] = static_cast<float> (0.30 * std::sin (phase)); phase += 2.0 * std::numbers::pi * 220.0 / sampleRate; }
            renderer.process (in.data(), out.data(), block, 220.0, ratio, true);
            all.insert (all.end(), out.begin(), out.end());
        }
        const auto measured = estimateF0 (all, 48000, 72000, sampleRate);
        const auto error = centsError (measured, 220.0 * ratio);
        std::cout << "renderer_static | cents=" << cents << " measured_hz=" << measured << " error_cents=" << error << '\n';
        conformancePass = conformancePass && std::abs (error) < 20.0;
    }

    CausalTDPSOLARenderer stepRenderer;
    stepRenderer.prepare (sampleRate, block, 256);
    constexpr std::array<double, 13> stepCents {{ 0.0, 20.0, 0.0, 50.0, 0.0, 80.0, 0.0, -20.0, 0.0, -50.0, 0.0, -80.0, 0.0 }};
    std::vector<float> stepAll, stepIn (block), stepOut (block);
    stepAll.reserve (static_cast<size_t> (sampleRate * 6.5));
    auto stepPhase = 0.0;
    std::array<CausalTDPSOLARenderer::DiagnosticState, stepCents.size()> stepStates {};
    std::array<bool, stepCents.size()> stepArtifactVeto {};
    for (int start = 0; start < static_cast<int> (sampleRate * 6.5); start += block)
    {
        const auto segment = std::min (static_cast<int> (stepCents.size()) - 1, start / static_cast<int> (sampleRate * 0.5));
        const auto ratio = std::exp2 (stepCents[static_cast<size_t> (segment)] / 1200.0);
        for (int i = 0; i < block; ++i) { stepIn[i] = static_cast<float> (0.30 * std::sin (stepPhase)); stepPhase += 2.0 * std::numbers::pi * 220.0 / sampleRate; }
        stepRenderer.process (stepIn.data(), stepOut.data(), block, 220.0, ratio, true);
        stepStates[static_cast<size_t> (segment)] = stepRenderer.getDiagnosticState();
        stepArtifactVeto[static_cast<size_t> (segment)] = stepArtifactVeto[static_cast<size_t> (segment)]
                                                       || stepRenderer.getDiagnosticState().artifactDryVeto;
        stepAll.insert (stepAll.end(), stepOut.begin(), stepOut.end());
    }
    for (int segment = 0; segment < static_cast<int> (stepCents.size()); ++segment)
    {
        const auto begin = static_cast<int> ((segment * 0.5 + 0.25) * sampleRate);
        const auto f0 = estimateF0 (stepAll, begin, begin + 9600, sampleRate);
        const auto error = centsError (f0, 220.0 * std::exp2 (stepCents[static_cast<size_t> (segment)] / 1200.0));
        const auto& stepState = stepStates[static_cast<size_t> (segment)];
        std::cout << "renderer_step | segment=" << segment << " cents=" << stepCents[static_cast<size_t> (segment)]
                  << " measured_hz=" << f0 << " error_cents=" << error
                  << " requested_ratio=" << stepState.requestedRatio
                  << " active_ratio=" << stepState.activeRatio
                  << " wet_mix=" << stepState.wetMix
                  << " marks=" << stepState.queuedMarks
                  << " artifact_veto=" << stepArtifactVeto[static_cast<size_t> (segment)] << '\n';
        conformancePass = conformancePass && std::abs (error) < 25.0;
    }

    CausalTDPSOLARenderer resetRenderer;
    resetRenderer.prepare (sampleRate, block, 256);
    std::vector<float> resetAll, resetIn (block), resetOut (block);
    resetAll.reserve (static_cast<size_t> (sampleRate * 1.5));
    auto resetPhase = 0.0; auto sawReset = false; auto sawReentry = false;
    for (int start = 0; start < static_cast<int> (sampleRate * 1.5); start += block)
    {
        const auto cents = start < 24000 ? 80.0 : -80.0;
        const auto ratio = std::exp2 (cents / 1200.0);
        for (int i = 0; i < block; ++i) { resetIn[i] = static_cast<float> (0.30 * std::sin (resetPhase)); resetPhase += 2.0 * std::numbers::pi * 220.0 / sampleRate; }
        resetRenderer.process (resetIn.data(), resetOut.data(), block, 220.0, ratio, true);
        // Ignore normal start-up priming: only a reversal-triggered reset or
        // dry hold would violate the continuity contract.
        if (start >= 24000)
        {
            sawReset = sawReset || resetRenderer.getDiagnosticState().latticeReset;
            sawReentry = sawReentry || resetRenderer.getDiagnosticState().reentryHoldActive;
        }
        resetAll.insert (resetAll.end(), resetOut.begin(), resetOut.end());
    }
    const auto afterResetF0 = estimateF0 (resetAll, 48000, 67200, sampleRate);
    const auto afterResetError = centsError (afterResetF0, 220.0 * std::exp2 (-80.0 / 1200.0));
    // A signed correction reversal is not a loss of voiced control.  The
    // renderer must preserve its source/synthesis phase and let queued OLA
    // tails finish; resetting here was the click-producing behaviour this
    // test originally guarded against.  Safety re-entry is reserved for an
    // actual dry/invalid interval, which is covered by the live-path tests.
    std::cout << "renderer_reversal_continuity | reset=" << sawReset << " reentry=" << sawReentry
              << " measured_hz=" << afterResetF0 << " error_cents=" << afterResetError << '\n';
    conformancePass = conformancePass && ! sawReset && ! sawReentry && std::abs (afterResetError) < 25.0;
    std::cout << "TD_PSOLA_RENDERER: " << (conformancePass ? "PASS" : "FAIL") << '\n';
    return conformancePass ? 0 : 1;
}
