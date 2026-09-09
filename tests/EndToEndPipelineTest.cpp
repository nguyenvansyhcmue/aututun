#include "PluginProcessor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <numeric>
#include <set>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

double estimateF0 (const std::vector<float>& audio, int start, int end)
{
    if (end - start < 32) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> crossings;
    for (int sample = start + 1; sample < end; ++sample)
        if (audio[static_cast<size_t> (sample - 1)] <= 0.0f && audio[static_cast<size_t> (sample)] > 0.0f)
        {
            const auto previous = static_cast<double> (audio[static_cast<size_t> (sample - 1)]);
            const auto current = static_cast<double> (audio[static_cast<size_t> (sample)]);
            crossings.push_back (static_cast<double> (sample - 1) - previous / (current - previous));
        }
    if (crossings.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return sampleRate * static_cast<double> (crossings.size() - 1) / (crossings.back() - crossings.front());
}

struct Metrics
{
    double outputF0 = std::numeric_limits<double>::quiet_NaN();
    double rms = 0.0, peak = 0.0, maxStep = 0.0, boundaryMaxStep = 0.0, averageUs = 0.0, worstUs = 0.0;
    double response63Ms = -1.0, response90Ms = -1.0, response99Ms = -1.0;
    bool finite = true, clipped = false;
    int latency = 0;
};

void require (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit (1);
    }
}

double median (std::vector<double> values)
{
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort (values.begin(), values.end());
    return values[values.size() / 2];
}

void configureRetuneOnly (VibeAutotuneProcessor& processor, float retuneMs)
{
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "inputType", 1.0f);
    setParameter (processor, "key", 0.0f);
    setParameter (processor, "scale", 0.0f);
    setParameter (processor, "tracking", 50.0f);
    setParameter (processor, "retuneSpeed", retuneMs);
    setParameter (processor, "flexTune", 0.0f);
    setParameter (processor, "humanize", 0.0f);
    setParameter (processor, "naturalVibrato", 0.0f);
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);
}

void configureFlexOnly (VibeAutotuneProcessor& processor, float flexPercent)
{
    configureRetuneOnly (processor, 25.0f);
    setParameter (processor, "flexTune", flexPercent);
}

Metrics runStatic (int blockSize, float retuneMs, double inputF0, double seconds = 2.0)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    configureRetuneOnly (processor, retuneMs);

    const auto length = static_cast<int> (seconds * sampleRate);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (static_cast<size_t> (length));
    auto totalUs = 0.0, worstUs = 0.0;
    auto boundaryMaxStep = 0.0;
    for (int start = 0; start < length; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto time = static_cast<double> (start + sample) / sampleRate;
            const auto value = static_cast<float> (0.28 * std::sin (2.0 * std::numbers::pi * inputF0 * time)
                                                   + 0.10 * std::sin (4.0 * std::numbers::pi * inputF0 * time));
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
        }
        const auto begin = std::chrono::steady_clock::now();
        processor.processBlock (buffer, midi);
        const auto elapsed = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - begin).count();
        totalUs += elapsed;
        worstUs = std::max (worstUs, elapsed);
        if (! output.empty())
            boundaryMaxStep = std::max (boundaryMaxStep,
                                        std::abs (static_cast<double> (buffer.getSample (0, 0) - output.back())));
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    const auto first = processor.getLatencySamples() + static_cast<int> (sampleRate);
    auto energy = 0.0, peak = 0.0, maxStepWithinBlock = 0.0;
    auto finite = true;
    for (int sample = first + 1; sample < static_cast<int> (output.size()); ++sample)
    {
        const auto value = output[static_cast<size_t> (sample)];
        finite = finite && std::isfinite (value);
        energy += value * value;
        peak = std::max (peak, std::abs (static_cast<double> (value)));
        maxStepWithinBlock = std::max (maxStepWithinBlock, std::abs (static_cast<double> (value - output[static_cast<size_t> (sample - 1)])));
    }
    const auto count = static_cast<double> (output.size() - first - 1);
    const auto inputCents = 1200.0 * std::log2 (inputF0 / 220.0);
    auto response63 = -1.0, response90 = -1.0, response99 = -1.0;
    for (int windowStart = processor.getLatencySamples(); windowStart + 2048 <= static_cast<int> (output.size()); windowStart += 512)
    {
        const auto f0 = estimateF0 (output, windowStart, windowStart + 2048);
        if (! std::isfinite (f0)) continue;
        const auto outputCents = 1200.0 * std::log2 (f0 / 220.0);
        const auto progress = inputCents != 0.0 ? (inputCents - outputCents) / inputCents : 0.0;
        const auto timeMs = 1000.0 * static_cast<double> (windowStart - processor.getLatencySamples()) / sampleRate;
        if (response63 < 0.0 && progress >= 0.63) response63 = timeMs;
        if (response90 < 0.0 && progress >= 0.90) response90 = timeMs;
        if (response99 < 0.0 && progress >= 0.99) response99 = timeMs;
    }
    std::vector<double> steadyF0;
    for (int windowStart = first; windowStart + 2048 <= static_cast<int> (output.size()); windowStart += 512)
    {
        const auto f0 = estimateF0 (output, windowStart, windowStart + 2048);
        if (std::isfinite (f0)) steadyF0.push_back (f0);
    }
    return { median (std::move (steadyF0)), std::sqrt (energy / count), peak,
             maxStepWithinBlock, boundaryMaxStep,
             totalUs / static_cast<double> (length / blockSize), worstUs, response63, response90, response99,
             finite, peak >= 1.0, processor.getLatencySamples() };
}

void printStatic (int block, float retuneMs, double inputF0)
{
    const auto result = runStatic (block, retuneMs, inputF0);
    const auto target = 220.0;
    const auto cents = 1200.0 * std::log2 (result.outputF0 / target);
    std::cout << "e2e_static | block=" << block << " retune_ms=" << retuneMs << " input_hz=" << inputF0
              << " target_hz=" << target << " output_hz=" << result.outputF0 << " cents=" << cents
              << " latency=" << result.latency << " rms=" << result.rms << " peak=" << result.peak
              << " max_step=" << result.maxStep << " boundary_step=" << result.boundaryMaxStep
              << " clipped=" << result.clipped << " avg_us=" << result.averageUs << " worst_us=" << result.worstUs
              << " t63_ms=" << result.response63Ms << " t90_ms=" << result.response90Ms << " t99_ms=" << result.response99Ms
              << " finite=" << result.finite << '\n';
}

struct TransitionMetrics
{
    double beforeF0 = std::numeric_limits<double>::quiet_NaN();
    double afterF0 = std::numeric_limits<double>::quiet_NaN();
    double settleMs = std::numeric_limits<double>::quiet_NaN();
    double boundaryMaxStep = 0.0;
    bool finite = true;
    bool clipped = false;
};

TransitionMetrics runSuddenNoteChange (int blockSize, float retuneMs)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    configureRetuneOnly (processor, retuneMs);

    constexpr auto beforeTarget = 220.0;
    constexpr auto afterTarget = 233.0818808;
    constexpr auto transitionSample = static_cast<int> (sampleRate);
    constexpr auto totalSamples = static_cast<int> (sampleRate * 3.0);
    const auto detuneRatio = std::exp2 (40.0 / 1200.0);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (totalSamples);
    auto boundaryMaxStep = 0.0;
    auto phase = 0.0;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto absolute = start + sample;
            const auto frequency = (absolute < transitionSample ? beforeTarget : afterTarget) * detuneRatio;
            const auto value = static_cast<float> (0.28 * std::sin (phase) + 0.10 * std::sin (phase * 2.0));
            phase += 2.0 * std::numbers::pi * frequency / sampleRate;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
        }
        processor.processBlock (buffer, midi);
        if (! output.empty())
            boundaryMaxStep = std::max (boundaryMaxStep,
                                        std::abs (static_cast<double> (buffer.getSample (0, 0) - output.back())));
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    const auto latency = processor.getLatencySamples();
    const auto beforeF0 = estimateF0 (output, latency + 24000, latency + 40000);
    const auto afterF0 = estimateF0 (output, latency + transitionSample + 24000, latency + transitionSample + 40000);
    const auto settlingStart = latency + transitionSample;
    auto settleMs = std::numeric_limits<double>::quiet_NaN();
    // A short 512-sample F0 estimate finds when the processed output has
    // reached 90% of the one-semitone destination. This exposes any hidden
    // "slow-motion" tail after the host-reported renderer latency.
    const auto settleThreshold = beforeTarget * std::exp2 (0.9 / 12.0);
    for (int windowStart = settlingStart; windowStart + 512 <= static_cast<int> (output.size()); windowStart += 128)
    {
        const auto f0 = estimateF0 (output, windowStart, windowStart + 512);
        if (std::isfinite (f0) && f0 >= settleThreshold)
        {
            settleMs = 1000.0 * static_cast<double> (windowStart - settlingStart) / sampleRate;
            break;
        }
    }
    auto finite = true;
    auto clipped = false;
    for (const auto value : output)
    {
        finite = finite && std::isfinite (value);
        clipped = clipped || std::abs (value) >= 1.0f;
    }
    return { beforeF0, afterF0, settleMs, boundaryMaxStep, finite, clipped };
}

void verifyBypassDelay (int blockSize)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    setParameter (processor, "bypass", 1.0f);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    const auto totalSamples = processor.getLatencySamples() + blockSize * 3;
    output.reserve (totalSamples);

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        buffer.clear();
        if (start == 0)
        {
            buffer.setSample (0, 0, 0.5f);
            buffer.setSample (1, 0, 0.5f);
        }
        processor.processBlock (buffer, midi);
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    auto maximumIndex = 0;
    for (int sample = 1; sample < static_cast<int> (output.size()); ++sample)
        if (std::abs (output[static_cast<size_t> (sample)]) > std::abs (output[static_cast<size_t> (maximumIndex)]))
            maximumIndex = sample;

    std::cout << "bypass_delay | block=" << blockSize << " reported_samples=" << processor.getLatencySamples()
              << " measured_peak_sample=" << maximumIndex << '\n';
    require (maximumIndex == processor.getLatencySamples(), "bypass dry path was not latency aligned");
}

struct FlexMetrics
{
    double outputCents = std::numeric_limits<double>::quiet_NaN();
    double averageCorrectionCents = 0.0;
    double outputVariance = 0.0;
    double boundaryMaxStep = 0.0;
    bool finite = true;
    bool clipped = false;
    std::set<std::string> targetNotes;
    VibeAutotuneProcessor::RendererDiscontinuityData rendererDiscontinuity;
};

FlexMetrics runFlexCase (int blockSize, float flexPercent, double deviationCents, bool oscillate)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    configureFlexOnly (processor, flexPercent);

    // Key/Scale defaults are synchronised by the first production callback.
    // Perform that sync before applying a manual note-map edit, exactly as a
    // UI user would after the plug-in has opened.
    juce::AudioBuffer<float> syncBuffer (2, blockSize);
    juce::MidiBuffer syncMidi;
    processor.processBlock (syncBuffer, syncMidi);

    // Keep A as the only enabled chromatic target so +/-70 cents exercises a
    // genuinely large correction rather than selecting the adjacent semitone.
    processor.setEnabledNoteMask (static_cast<uint16_t> (1u << 9));

    constexpr auto seconds = 3.0;
    const auto length = static_cast<int> (seconds * sampleRate);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (static_cast<size_t> (length));
    std::vector<double> appliedCorrections;
    std::set<std::string> targetNotes;
    auto phase = 0.0;
    auto boundaryMaxStep = 0.0;

    for (int start = 0; start < length; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto time = static_cast<double> (start + sample) / sampleRate;
            const auto cents = oscillate ? deviationCents * std::sin (2.0 * std::numbers::pi * 2.0 * time)
                                         : deviationCents;
            const auto frequency = 220.0 * std::exp2 (cents / 1200.0);
            const auto value = static_cast<float> (0.28 * std::sin (phase) + 0.10 * std::sin (2.0 * phase));
            phase += 2.0 * std::numbers::pi * frequency / sampleRate;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
        }

        processor.processBlock (buffer, midi);
        if (! output.empty())
            boundaryMaxStep = std::max (boundaryMaxStep,
                                        std::abs (static_cast<double> (buffer.getSample (0, 0) - output.back())));
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);

        const auto monitor = processor.getPitchMonitorData();
        if (monitor.voiced)
        {
            appliedCorrections.push_back (std::abs (static_cast<double> (monitor.correctionCents)));
            if (monitor.targetNote != "--")
                targetNotes.insert (monitor.targetNote.toStdString());
        }
    }

    std::vector<double> outputCents;
    const auto first = processor.getLatencySamples() + static_cast<int> (sampleRate);
    for (int windowStart = first; windowStart + 1024 <= static_cast<int> (output.size()); windowStart += 512)
    {
        const auto f0 = estimateF0 (output, windowStart, windowStart + 1024);
        if (std::isfinite (f0)) outputCents.push_back (1200.0 * std::log2 (f0 / 220.0));
    }

    auto mean = 0.0;
    for (const auto cents : outputCents) mean += cents;
    mean /= static_cast<double> (std::max<size_t> (1, outputCents.size()));
    auto variance = 0.0;
    for (const auto cents : outputCents) variance += (cents - mean) * (cents - mean);
    variance /= static_cast<double> (std::max<size_t> (1, outputCents.size()));

    auto finite = true;
    auto clipped = false;
    for (const auto sample : output)
    {
        finite = finite && std::isfinite (sample);
        clipped = clipped || std::abs (sample) >= 1.0f;
    }

    const auto correctionMean = appliedCorrections.empty() ? 0.0
                               : std::accumulate (appliedCorrections.begin(), appliedCorrections.end(), 0.0)
                                 / static_cast<double> (appliedCorrections.size());
    return { mean, correctionMean, variance, boundaryMaxStep, finite, clipped, std::move (targetNotes),
             processor.getRendererDiscontinuityData() };
}

enum class HumanizeScenario { shortNote, sustainedMotion, targetChange, unvoicedRestart };

struct HumanizeMetrics
{
    double attackCorrectionCents = 0.0;
    double sustainCorrectionCents = 0.0;
    double restartCorrectionCents = 0.0;
    double boundaryMaxStep = 0.0;
    bool sawUnvoiced = false;
    bool finite = true;
    bool clipped = false;
    std::set<std::string> targetNotes;
};

HumanizeMetrics runHumanizeCase (int blockSize, float humanizePercent, HumanizeScenario scenario,
                                  double shortDurationSeconds = 0.0)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    configureRetuneOnly (processor, 10.0f);
    setParameter (processor, "humanize", humanizePercent);

    if (scenario != HumanizeScenario::targetChange)
        processor.setEnabledNoteMask (static_cast<uint16_t> (1u << 9)); // A only: target must remain stable.

    constexpr auto totalSeconds = 1.5;
    const auto totalSamples = static_cast<int> (totalSeconds * sampleRate);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (static_cast<size_t> (totalSamples));
    std::vector<double> attack, sustain, restart;
    std::set<std::string> targetNotes;
    auto phase = 0.0;
    auto boundaryMaxStep = 0.0;
    auto sawUnvoiced = false;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto time = static_cast<double> (start + sample) / sampleRate;
            auto voiced = true;
            auto frequency = 220.0 * std::exp2 (40.0 / 1200.0);

            if (scenario == HumanizeScenario::shortNote)
                voiced = time < shortDurationSeconds;
            else if (scenario == HumanizeScenario::sustainedMotion && time >= 0.50)
                frequency = 220.0 * std::exp2 (10.0 / 1200.0);
            else if (scenario == HumanizeScenario::targetChange && time >= 0.60)
                frequency = 233.0818808 * std::exp2 (40.0 / 1200.0);
            else if (scenario == HumanizeScenario::unvoicedRestart)
            {
                voiced = time < 0.50 || time >= 0.90;
                if (time >= 0.90)
                    frequency = 220.0 * std::exp2 (10.0 / 1200.0);
            }

            const auto value = voiced ? static_cast<float> (0.28 * std::sin (phase) + 0.10 * std::sin (2.0 * phase)) : 0.0f;
            phase += 2.0 * std::numbers::pi * frequency / sampleRate;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
        }

        processor.processBlock (buffer, midi);
        if (! output.empty())
            boundaryMaxStep = std::max (boundaryMaxStep,
                                        std::abs (static_cast<double> (buffer.getSample (0, 0) - output.back())));
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);

        const auto time = static_cast<double> (start) / sampleRate;
        const auto monitor = processor.getPitchMonitorData();
        sawUnvoiced = sawUnvoiced || ((scenario == HumanizeScenario::unvoicedRestart || scenario == HumanizeScenario::shortNote)
                                      && time > 0.55 && ! monitor.voiced);
        if (monitor.voiced)
        {
            targetNotes.insert (monitor.targetNote.toStdString());
            const auto correction = std::abs (static_cast<double> (monitor.correctionCents));
            if (time >= 0.08 && time < 0.22) attack.push_back (correction);
            if (scenario == HumanizeScenario::sustainedMotion && time >= 0.55 && time < 0.75) sustain.push_back (correction);
            if (scenario == HumanizeScenario::unvoicedRestart && time >= 1.05 && time < 1.20) restart.push_back (correction);
            if (scenario == HumanizeScenario::targetChange && time >= 0.82 && time < 0.97) restart.push_back (correction);
        }
    }

    auto finite = true;
    auto clipped = false;
    for (const auto sample : output)
    {
        finite = finite && std::isfinite (sample);
        clipped = clipped || std::abs (sample) >= 1.0f;
    }
    const auto average = [] (const std::vector<double>& values)
    {
        return values.empty() ? 0.0 : std::accumulate (values.begin(), values.end(), 0.0) / static_cast<double> (values.size());
    };
    return { average (attack), average (sustain), average (restart), boundaryMaxStep,
             sawUnvoiced, finite, clipped, std::move (targetNotes) };
}

enum class VibratoScenario { flat, startsAfterAttack, targetChange, unvoicedRestart };

struct VibratoMetrics
{
    double depthCents = 0.0;
    double rateHz = 0.0;
    double centreCents = 0.0;
    double variance = 0.0;
    double boundaryMaxStep = 0.0;
    bool sawUnvoiced = false;
    bool finite = true;
    bool clipped = false;
    std::set<std::string> targetNotes;
};

VibratoMetrics runVibratoCase (int blockSize, float naturalVibratoPercent, double baseFrequency,
                                double depthCents, double rateHz, VibratoScenario scenario)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    configureRetuneOnly (processor, 25.0f);
    setParameter (processor, "naturalVibrato", naturalVibratoPercent);

    if (scenario != VibratoScenario::targetChange)
        processor.setEnabledNoteMask (static_cast<uint16_t> (1u << 9)); // A octave family, stable target class.

    constexpr auto totalSeconds = 2.0;
    const auto totalSamples = static_cast<int> (totalSeconds * sampleRate);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (static_cast<size_t> (totalSamples));
    std::set<std::string> targetNotes;
    auto phase = 0.0;
    auto boundaryMaxStep = 0.0;
    auto sawUnvoiced = false;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto time = static_cast<double> (start + sample) / sampleRate;
            auto voiced = true;
            auto frequency = baseFrequency;
            auto vibratoDepth = depthCents;
            if (scenario == VibratoScenario::startsAfterAttack && time < 0.30)
                vibratoDepth = 0.0;
            else if (scenario == VibratoScenario::targetChange && time >= 0.90)
                frequency = baseFrequency * std::exp2 (200.0 / 1200.0);
            else if (scenario == VibratoScenario::unvoicedRestart)
                voiced = time < 0.70 || time >= 1.05;

            if (voiced)
                frequency *= std::exp2 ((vibratoDepth * std::sin (2.0 * std::numbers::pi * rateHz * time)) / 1200.0);
            const auto value = voiced ? static_cast<float> (0.28 * std::sin (phase) + 0.10 * std::sin (2.0 * phase)) : 0.0f;
            phase += 2.0 * std::numbers::pi * frequency / sampleRate;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
        }

        processor.processBlock (buffer, midi);
        if (! output.empty())
            boundaryMaxStep = std::max (boundaryMaxStep,
                                        std::abs (static_cast<double> (buffer.getSample (0, 0) - output.back())));
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        const auto monitor = processor.getPitchMonitorData();
        const auto time = static_cast<double> (start) / sampleRate;
        sawUnvoiced = sawUnvoiced || (scenario == VibratoScenario::unvoicedRestart && time > 0.80 && time < 1.00 && ! monitor.voiced);
        if (monitor.voiced) targetNotes.insert (monitor.targetNote.toStdString());
    }

    std::vector<double> cents;
    std::vector<double> times;
    const auto first = processor.getLatencySamples() + static_cast<int> (0.75 * sampleRate);
    for (int windowStart = first; windowStart + 1024 <= static_cast<int> (output.size()); windowStart += 256)
    {
        const auto f0 = estimateF0 (output, windowStart, windowStart + 1024);
        if (std::isfinite (f0))
        {
            cents.push_back (1200.0 * std::log2 (f0 / baseFrequency));
            times.push_back (static_cast<double> (windowStart) / sampleRate);
        }
    }
    auto centre = 0.0;
    for (const auto value : cents) centre += value;
    centre /= static_cast<double> (std::max<size_t> (1, cents.size()));
    auto variance = 0.0;
    for (const auto value : cents) variance += (value - centre) * (value - centre);
    variance /= static_cast<double> (std::max<size_t> (1, cents.size()));
    const auto depth = std::sqrt (2.0 * variance); // sinusoidal peak depth estimate

    auto upwardCrossings = 0;
    for (size_t index = 1; index < cents.size(); ++index)
        if (cents[index - 1] <= centre && cents[index] > centre) ++upwardCrossings;
    const auto span = times.size() > 1 ? times.back() - times.front() : 0.0;
    const auto measuredRate = span > 0.0 ? static_cast<double> (upwardCrossings) / span : 0.0;

    auto finite = true;
    auto clipped = false;
    for (const auto sample : output)
    {
        finite = finite && std::isfinite (sample);
        clipped = clipped || std::abs (sample) >= 1.0f;
    }
    return { depth, measuredRate, centre, variance, boundaryMaxStep, sawUnvoiced,
             finite, clipped, std::move (targetNotes) };
}
}

int main()
{
    std::cout << std::fixed << std::setprecision (3);
    const auto plus40 = 220.0 * std::exp2 (40.0 / 1200.0);
    const auto minus40 = 220.0 * std::exp2 (-40.0 / 1200.0);
    for (const auto block : { 64, 256, 1024 })
    {
        verifyBypassDelay (block);
        std::array<Metrics, 3> positive {};
        std::array<Metrics, 3> negative {};
        constexpr std::array<float, 3> retunes {{ 0.0f, 25.0f, 100.0f }};
        for (size_t index = 0; index < retunes.size(); ++index)
        {
            positive[index] = runStatic (block, retunes[index], plus40);
            negative[index] = runStatic (block, retunes[index], minus40);
            printStatic (block, retunes[index], plus40);
            printStatic (block, retunes[index], minus40);
            for (const auto& result : { positive[index], negative[index] })
            {
                require (result.finite && ! result.clipped, "retune produced non-finite or clipped output");
                require (result.boundaryMaxStep < 0.10, "retune produced a block-boundary discontinuity");
            }
        }

        for (const auto* metrics : { &positive, &negative })
        {
            require ((*metrics)[0].response63Ms >= 0.0 && (*metrics)[1].response63Ms >= 0.0 && (*metrics)[2].response63Ms >= 0.0,
                     "retune response did not reach 63%");
            require ((*metrics)[0].response63Ms < (*metrics)[1].response63Ms,
                     "25 ms retune was not slower than 0 ms");
            require ((*metrics)[1].response99Ms < (*metrics)[2].response99Ms,
                     "100 ms retune was not slower than 25 ms");
        }

        for (const auto retune : retunes)
        {
            const auto transition = runSuddenNoteChange (block, retune);
            std::cout << "e2e_note_transition | block=" << block << " retune_ms=" << retune
                      << " before_hz=" << transition.beforeF0 << " after_hz=" << transition.afterF0
                      << " settle_ms=" << transition.settleMs
                      << " boundary_step=" << transition.boundaryMaxStep
                      << " finite=" << transition.finite << " clipped=" << transition.clipped << '\n';
            require (transition.finite && ! transition.clipped, "note transition produced non-finite or clipped output");
            require (transition.boundaryMaxStep < 0.10, "note transition clicked at a block boundary");
            require (std::isfinite (transition.beforeF0) && std::isfinite (transition.afterF0)
                     && transition.afterF0 > transition.beforeF0 * 1.03,
                     "note transition did not move in the target direction");
        }
    }

    // Phase 2: Flex-Tune only. Retune remains fixed at 25 ms and all other
    // pitch modifiers are disabled by configureFlexOnly().
    constexpr std::array<float, 4> flexValues {{ 0.0f, 30.0f, 60.0f, 100.0f }};
    for (const auto block : { 64, 256, 1024 })
    {
        for (const auto deviation : { 10.0, 20.0, 35.0 })
        {
            std::array<FlexMetrics, 4> results {};
            for (size_t index = 0; index < flexValues.size(); ++index)
            {
                results[index] = runFlexCase (block, flexValues[index], deviation, true);
                const auto& result = results[index];
                std::cout << "e2e_flex_small | block=" << block << " flex=" << flexValues[index]
                          << " deviation_cents=" << deviation << " output_cents=" << result.outputCents
                          << " correction_cents=" << result.averageCorrectionCents
                          << " variance=" << result.outputVariance << " targets=" << result.targetNotes.size() << " [";
                for (const auto& note : result.targetNotes) std::cout << note << ' ';
                std::cout << "]"
                          << " boundary_step=" << result.boundaryMaxStep << " finite=" << result.finite
                          << " clipped=" << result.clipped
                          << " renderer_step=" << result.rendererDiscontinuity.step
                          << " renderer_sample=" << result.rendererDiscontinuity.sampleIndex
                          << " renderer_f0=" << result.rendererDiscontinuity.sourceF0Hz
                          << " renderer_ratio=" << result.rendererDiscontinuity.pitchRatio
                          << " renderer_period=" << result.rendererDiscontinuity.synthesisPeriod
                          << " renderer_source_period=" << result.rendererDiscontinuity.sourcePeriod
                          << " renderer_source_period_jump=" << result.rendererDiscontinuity.sourcePeriodJump
                          << " renderer_tdpsola_step=" << result.rendererDiscontinuity.tdpsolaOutputStep
                          << " renderer_tdpsola_fallback=" << result.rendererDiscontinuity.tdpsolaFallback
                          << " renderer_wet=" << result.rendererDiscontinuity.dryWetMix
                          << " renderer_voiced=" << result.rendererDiscontinuity.voiced << '\n';
                require (result.finite && ! result.clipped && result.boundaryMaxStep < 0.10,
                         "Flex-Tune produced invalid or discontinuous audio");
                require (result.targetNotes.size() == 1, "Flex-Tune changed the target note");
            }
            require (results[0].averageCorrectionCents > results[3].averageCorrectionCents + 1.0,
                     "Flex 0 was not stronger than Flex 100 near the target");
            require (results[3].outputVariance > results[0].outputVariance * 1.20,
                     "Flex 100 did not retain more near-target motion than Flex 0");
            for (size_t index = 1; index < results.size(); ++index)
                require (results[index].targetNotes == results[0].targetNotes,
                         "target selection changed with Flex-Tune");
        }

        for (const auto deviation : { 70.0, -70.0 })
        {
            std::array<FlexMetrics, 4> results {};
            for (size_t index = 0; index < flexValues.size(); ++index)
            {
                results[index] = runFlexCase (block, flexValues[index], deviation, false);
                const auto& result = results[index];
                std::cout << "e2e_flex_large | block=" << block << " flex=" << flexValues[index]
                          << " deviation_cents=" << deviation << " output_cents=" << result.outputCents
                          << " correction_cents=" << result.averageCorrectionCents << " targets=" << result.targetNotes.size()
                          << " boundary_step=" << result.boundaryMaxStep << " finite=" << result.finite
                          << " clipped=" << result.clipped << '\n';
                require (result.finite && ! result.clipped && result.boundaryMaxStep < 0.10,
                         "large-error Flex-Tune produced invalid or discontinuous audio");
                require (result.averageCorrectionCents > 5.0,
                         "Flex-Tune treated a large deviation as bypass");
            }
            require (results[0].averageCorrectionCents > results[3].averageCorrectionCents + 1.0,
                     "Flex 0 was not the strongest large-error correction");
        }
    }

    // Phase 3: Humanize only. At 10 ms Retune, any Humanize effect must be
    // absent before sustain and must appear only after the 240 ms hold/ramp.
    constexpr std::array<float, 3> humanizeValues {{ 0.0f, 50.0f, 100.0f }};
    for (const auto block : { 64, 256, 1024 })
    {
        for (const auto duration : { 0.080, 0.150 })
        {
            std::array<HumanizeMetrics, 3> results {};
            for (size_t index = 0; index < humanizeValues.size(); ++index)
            {
                results[index] = runHumanizeCase (block, humanizeValues[index], HumanizeScenario::shortNote, duration);
                const auto& result = results[index];
                std::cout << "e2e_humanize_short | block=" << block << " humanize=" << humanizeValues[index]
                          << " duration_ms=" << duration * 1000.0 << " attack_correction=" << result.attackCorrectionCents
                          << " unvoiced=" << result.sawUnvoiced << " boundary_step=" << result.boundaryMaxStep
                          << " finite=" << result.finite << " clipped=" << result.clipped << '\n';
                require (result.finite && ! result.clipped && result.boundaryMaxStep < 0.10,
                         "Humanize short-note case produced invalid audio");
            }
            require (std::abs (results[0].attackCorrectionCents - results[2].attackCorrectionCents) < 3.0,
                     "Humanize altered a short-note attack");
        }

        std::array<HumanizeMetrics, 3> sustained {};
        for (size_t index = 0; index < humanizeValues.size(); ++index)
        {
            sustained[index] = runHumanizeCase (block, humanizeValues[index], HumanizeScenario::sustainedMotion);
            const auto& result = sustained[index];
            std::cout << "e2e_humanize_sustain | block=" << block << " humanize=" << humanizeValues[index]
                      << " attack_correction=" << result.attackCorrectionCents
                      << " post_240ms_correction=" << result.sustainCorrectionCents
                      << " boundary_step=" << result.boundaryMaxStep << " finite=" << result.finite
                      << " clipped=" << result.clipped << '\n';
            require (result.finite && ! result.clipped && result.boundaryMaxStep < 0.10,
                     "Humanize sustained-note case produced invalid audio");
        }
        require (std::abs (sustained[0].attackCorrectionCents - sustained[2].attackCorrectionCents) < 3.0,
                 "Humanize altered the long-note attack");
        require (sustained[2].sustainCorrectionCents > sustained[0].sustainCorrectionCents + 1.0,
                 "Humanize 100 did not slow the sustained correction response");
        require (sustained[2].sustainCorrectionCents > 5.0,
                 "Humanize 100 turned sustained correction into bypass");

        const auto targetZero = runHumanizeCase (block, 0.0f, HumanizeScenario::targetChange);
        const auto targetFull = runHumanizeCase (block, 100.0f, HumanizeScenario::targetChange);
        std::cout << "e2e_humanize_target_reset | block=" << block << " reset_attack_0=" << targetZero.restartCorrectionCents
                  << " reset_attack_100=" << targetFull.restartCorrectionCents << " targets=" << targetFull.targetNotes.size() << '\n';
        require (targetZero.finite && targetFull.finite && targetZero.targetNotes.size() >= 2 && targetFull.targetNotes.size() >= 2,
                 "target change was not observed or produced invalid audio");
        require (std::abs (targetZero.restartCorrectionCents - targetFull.restartCorrectionCents) < 4.0,
                 "Humanize did not reset after a target change");

        const auto restartZero = runHumanizeCase (block, 0.0f, HumanizeScenario::unvoicedRestart);
        const auto restartFull = runHumanizeCase (block, 100.0f, HumanizeScenario::unvoicedRestart);
        std::cout << "e2e_humanize_unvoiced_reset | block=" << block << " restart_0=" << restartZero.restartCorrectionCents
                  << " restart_100=" << restartFull.restartCorrectionCents << " unvoiced=" << restartFull.sawUnvoiced << '\n';
        require (restartZero.finite && restartFull.finite && restartZero.sawUnvoiced && restartFull.sawUnvoiced,
                 "unvoiced reset was not observed");
        require (std::abs (restartZero.restartCorrectionCents - restartFull.restartCorrectionCents) < 4.0,
                 "Humanize did not reset after unvoiced audio");
    }

    // Phase 4: Natural Vibrato only. The input contains no LFO in the flat
    // cases; every observed modulation must therefore remain negligible.
    constexpr std::array<float, 5> vibratoValues {{ -100.0f, -50.0f, 0.0f, 50.0f, 100.0f }};
    for (const auto block : { 64, 256, 1024 })
    {
        for (const auto baseFrequency : { 220.0, 440.0 })
        {
            std::array<VibratoMetrics, 5> flat {};
            for (size_t index = 0; index < vibratoValues.size(); ++index)
            {
                flat[index] = runVibratoCase (block, vibratoValues[index], baseFrequency, 0.0, 4.5, VibratoScenario::flat);
                const auto& result = flat[index];
                std::cout << "e2e_vibrato_flat | block=" << block << " natural=" << vibratoValues[index]
                          << " base_hz=" << baseFrequency << " depth=" << result.depthCents
                          << " centre=" << result.centreCents << " rate=" << result.rateHz
                          << " targets=" << result.targetNotes.size() << " finite=" << result.finite
                          << " clipped=" << result.clipped << '\n';
                require (result.finite && ! result.clipped && result.boundaryMaxStep < 0.10,
                         "flat Natural Vibrato case produced invalid audio");
                require (result.depthCents < 8.0, "Natural Vibrato generated modulation on a flat input");
            }
            for (size_t index = 1; index < flat.size(); ++index)
            {
                require (flat[index].targetNotes == flat[0].targetNotes,
                         "Natural Vibrato changed a flat-input target");
                require (std::abs (flat[index].centreCents - flat[0].centreCents) < 4.0,
                         "Natural Vibrato drifted the flat-input centre pitch");
            }
        }

        for (const auto depth : { 15.0, 35.0 })
        {
            for (const auto rate : { 4.5, 6.0 })
            {
                std::array<VibratoMetrics, 5> results {};
                for (size_t index = 0; index < vibratoValues.size(); ++index)
                {
                    results[index] = runVibratoCase (block, vibratoValues[index], 220.0, depth, rate,
                                                      VibratoScenario::startsAfterAttack);
                    const auto& result = results[index];
                    std::cout << "e2e_vibrato | block=" << block << " natural=" << vibratoValues[index]
                              << " input_depth=" << depth << " input_rate=" << rate << " output_depth=" << result.depthCents
                              << " output_rate=" << result.rateHz << " centre=" << result.centreCents
                              << " variance=" << result.variance << " targets=" << result.targetNotes.size()
                              << " finite=" << result.finite << " clipped=" << result.clipped << '\n';
                    require (result.finite && ! result.clipped && result.boundaryMaxStep < 0.10,
                             "Natural Vibrato case produced invalid audio");
                    require (result.targetNotes.size() == 1, "Natural Vibrato changed the target note");
                    require (std::abs (result.rateHz - rate) < 1.5, "Natural Vibrato changed the vibrato rate");
                }
                require (results[0].depthCents + 0.25 < results[2].depthCents,
                         "-100% Natural Vibrato did not reduce depth");
                require (results[4].depthCents > results[2].depthCents + 0.25,
                         "+100% Natural Vibrato did not increase depth");
                for (size_t index = 1; index < results.size(); ++index)
                {
                    require (results[index].targetNotes == results[0].targetNotes,
                             "Natural Vibrato changed target selection");
                    require (std::abs (results[index].centreCents - results[2].centreCents) < 5.0,
                             "Natural Vibrato drifted the vibrato centre pitch");
                }
            }
        }

        const auto changedTarget = runVibratoCase (block, 100.0f, 220.0, 15.0, 4.5, VibratoScenario::targetChange);
        const auto restarted = runVibratoCase (block, 100.0f, 220.0, 15.0, 4.5, VibratoScenario::unvoicedRestart);
        std::cout << "e2e_vibrato_resets | block=" << block << " target_count=" << changedTarget.targetNotes.size()
                  << " unvoiced=" << restarted.sawUnvoiced << " target_depth=" << changedTarget.depthCents
                  << " restart_depth=" << restarted.depthCents << '\n';
        require (changedTarget.finite && changedTarget.targetNotes.size() >= 2,
                 "Natural Vibrato target transition failed");
        require (restarted.finite && restarted.sawUnvoiced && restarted.depthCents > 0.25,
                 "Natural Vibrato did not restart cleanly after unvoiced audio");
    }
}
