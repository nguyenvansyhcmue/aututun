#include "PluginProcessor.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int defaultBlockSize = 1024;
// Kept short so the complete five-block-size matrix is practical in CI while
// still containing multiple fixed-hop analyses after frame warm-up.
constexpr double durationSeconds = 0.5;

struct Measurement
{
    double expectedHz = std::numeric_limits<double>::quiet_NaN();
    VibeAutotuneProcessor::PitchTrackingData tracking;
};

struct Summary
{
    int frames = 0;
    int voicedFrames = 0;
    int validFrequencyFrames = 0;
    double meanAbsoluteCents = std::numeric_limits<double>::quiet_NaN();
    double maximumAbsoluteCents = std::numeric_limits<double>::quiet_NaN();
    int octaveErrors = 0;
    int firstVoicedSample = -1;
    int newNoteCaptureSamples = -1;
    int meanCpuMicroseconds = 0;
    int worstCpuMicroseconds = 0;
};

template <typename Signal>
Summary runCase (const std::string& name, Signal&& signal, bool hasGroundTruth,
                 int blockSize = defaultBlockSize, int inputType = 1)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    processor.setPitchAnalysisProfilingEnabled (true);
    processor.parameters.getParameter ("inputType")->setValueNotifyingHost (static_cast<float> (juce::jlimit (0, 3, inputType)) / 3.0f);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const auto sampleCount = static_cast<int> (durationSeconds * sampleRate);
    std::vector<Measurement> measurements;
    measurements.reserve (static_cast<size_t> ((sampleCount + blockSize - 1) / blockSize));
    long long totalCpuMicroseconds = 0;
    int worstCpuMicroseconds = 0;

    for (int start = 0; start < sampleCount; start += blockSize)
    {
        for (int index = 0; index < blockSize; ++index)
        {
            const auto absoluteSample = start + index;
            const auto sample = absoluteSample < sampleCount ? signal (absoluteSample) : 0.0f;
            buffer.setSample (0, index, sample);
            buffer.setSample (1, index, sample);
        }

        const auto began = std::chrono::steady_clock::now();
        processor.processBlock (buffer, midi);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds> (
            std::chrono::steady_clock::now() - began).count();
        totalCpuMicroseconds += elapsed;
        worstCpuMicroseconds = std::max (worstCpuMicroseconds, static_cast<int> (elapsed));

        const auto frameEnd = std::min (start + blockSize, sampleCount);
        measurements.push_back ({ signal.expectedFrequency (frameEnd - 1), processor.getPitchTrackingData() });
    }

    Summary summary;
    summary.frames = static_cast<int> (measurements.size());
    summary.meanCpuMicroseconds = summary.frames == 0 ? 0 : static_cast<int> (totalCpuMicroseconds / summary.frames);
    summary.worstCpuMicroseconds = worstCpuMicroseconds;
    double centsSum = 0.0;
    double maximumCents = 0.0;

    for (const auto& measurement : measurements)
    {
        if (measurement.tracking.voiced)
        {
            ++summary.voicedFrames;
            if (summary.firstVoicedSample < 0)
                summary.firstVoicedSample = static_cast<int> (measurement.tracking.timestampSamples);
        }

        if (hasGroundTruth && measurement.tracking.voiced
            && std::isfinite (measurement.expectedHz)
            && std::isfinite (measurement.tracking.stableF0Hz)
            && measurement.expectedHz > 0.0 && measurement.tracking.stableF0Hz > 0.0)
        {
            const auto cents = std::abs (1200.0 * std::log2 (measurement.tracking.stableF0Hz / measurement.expectedHz));
            centsSum += cents;
            maximumCents = std::max (maximumCents, cents);
            ++summary.validFrequencyFrames;
            if (cents >= 600.0)
                ++summary.octaveErrors;
        }
    }

    if (summary.validFrequencyFrames > 0)
    {
        summary.meanAbsoluteCents = centsSum / static_cast<double> (summary.validFrequencyFrames);
        summary.maximumAbsoluteCents = maximumCents;
    }

    if constexpr (requires { signal.transitionSample(); })
    {
        const auto transitionSample = signal.transitionSample();
        const auto targetMidi = static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (signal.expectedFrequency (transitionSample) / 440.0)));
        for (const auto& measurement : measurements)
        {
            if (measurement.tracking.timestampSamples < static_cast<uint64_t> (transitionSample)
                || ! measurement.tracking.voiced
                || ! std::isfinite (measurement.tracking.stableF0Hz))
                continue;

            const auto measuredMidi = static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (measurement.tracking.stableF0Hz / 440.0)));
            if (measuredMidi == targetMidi)
            {
                summary.newNoteCaptureSamples = static_cast<int> (measurement.tracking.timestampSamples) - transitionSample;
                break;
            }
        }
    }

    std::cout << name
              << " | block=" << blockSize
              << " input_type=" << inputType
              << " | frames=" << summary.frames
              << " voiced=" << summary.voicedFrames
              << " valid_f0=" << summary.validFrequencyFrames
              << " mean_abs_cents=" << summary.meanAbsoluteCents
              << " max_abs_cents=" << summary.maximumAbsoluteCents
              << " octave_errors=" << summary.octaveErrors
              << " first_voiced_end_sample=" << summary.firstVoicedSample
              << " new_note_capture_samples=" << summary.newNoteCaptureSamples
              << " analysis_latency_samples=" << (measurements.empty() ? 0 : measurements.back().tracking.analysisLatencySamples)
              << " cpu_us_avg=" << summary.meanCpuMicroseconds
              << " cpu_us_worst=" << summary.worstCpuMicroseconds
              << '\n';
    const auto performance = processor.getPitchAnalysisPerformanceData();
    if (performance.analysisCount > 0)
        std::cout << "  stages_us | fifo=" << performance.fifoFramePreparationMicroseconds
                  << " difference=" << performance.differenceFunctionMicroseconds
                  << " cmndf=" << performance.cmndfMicroseconds
                  << " candidates=" << performance.candidateSearchMicroseconds
                  << " parabolic=" << performance.parabolicInterpolationMicroseconds
                  << " autocorrelation=" << performance.normalizedAutocorrelationMicroseconds
                  << " voicing=" << performance.voicingFeaturesMicroseconds
                  << " octave=" << performance.octaveScoringMicroseconds
                  << " smoothing=" << performance.smoothingMicroseconds
                  << " total=" << performance.totalMicroseconds << '\n';
    return summary;
}

struct FixedSine
{
    double frequency;
    float operator() (int sample) const { return 0.5f * std::sin (2.0 * std::numbers::pi * frequency * sample / sampleRate); }
    double expectedFrequency (int) const { return frequency; }
};

struct SweepSine
{
    float operator() (int sample) const
    {
        const auto time = static_cast<double> (sample) / sampleRate;
        const auto slope = std::log (440.0 / 110.0) / durationSeconds;
        return 0.5f * std::sin (2.0 * std::numbers::pi * 110.0 * (std::exp (slope * time) - 1.0) / slope);
    }
    double expectedFrequency (int sample) const
    {
        return 110.0 * std::exp (std::log (440.0 / 110.0) * (static_cast<double> (sample) / sampleRate) / durationSeconds);
    }
};

struct HarmonicVoiced
{
    float operator() (int sample) const
    {
        const auto phase = 2.0 * std::numbers::pi * 220.0 * sample / sampleRate;
        return static_cast<float> (0.34 * std::sin (phase) + 0.13 * std::sin (phase * 2.0) + 0.07 * std::sin (phase * 3.0));
    }
    double expectedFrequency (int) const { return 220.0; }
};

struct Vibrato
{
    float operator() (int sample)
    {
        const auto frequency = expectedFrequency (sample);
        phase += 2.0 * std::numbers::pi * frequency / sampleRate;
        return static_cast<float> (0.5 * std::sin (phase));
    }
    double expectedFrequency (int sample) const
    {
        const auto time = static_cast<double> (sample) / sampleRate;
        return 220.0 * std::pow (2.0, 35.0 * std::sin (2.0 * std::numbers::pi * 5.0 * time) / 1200.0);
    }

    double phase = 0.0;
};

struct NoteChange
{
    double secondFrequency = 440.0;

    float operator() (int sample)
    {
        const auto frequency = expectedFrequency (sample);
        phase += 2.0 * std::numbers::pi * frequency / sampleRate;
        return static_cast<float> (0.5 * std::sin (phase));
    }
    double expectedFrequency (int sample) const { return sample < transitionSample() ? 220.0 : secondFrequency; }
    int transitionSample() const { return static_cast<int> (durationSeconds * sampleRate * 0.5); }

    double phase = 0.0;
};

struct Silence
{
    float operator() (int) const { return 0.0f; }
    double expectedFrequency (int) const { return std::numeric_limits<double>::quiet_NaN(); }
};

struct Noise
{
    explicit Noise (float newAmplitude = 0.3f) : amplitude (newAmplitude) {}

    float operator() (int) 
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float> (static_cast<int32_t> (state) / 2147483648.0) * amplitude;
    }
    double expectedFrequency (int) const { return std::numeric_limits<double>::quiet_NaN(); }

    float amplitude = 0.3f;
    uint32_t state = 0x3c6ef372u;
};

struct NoisySine
{
    explicit NoisySine (float newNoiseAmplitude) : noiseAmplitude (newNoiseAmplitude) {}

    float operator() (int sample)
    {
        noiseState = noiseState * 1664525u + 1013904223u;
        const auto noise = static_cast<float> (static_cast<int32_t> (noiseState) / 2147483648.0) * noiseAmplitude;
        return FixedSine { 220.0 } (sample) + noise;
    }
    double expectedFrequency (int) const { return 220.0; }

    float noiseAmplitude;
    uint32_t noiseState = 0x17b7d1e5u;
};

struct HarmonicSignal
{
    HarmonicSignal (double newFundamental, double newFundamentalGain, double newSecondGain)
        : fundamental (newFundamental), fundamentalGain (newFundamentalGain), secondGain (newSecondGain) {}

    float operator() (int sample) const
    {
        const auto phase = 2.0 * std::numbers::pi * fundamental * sample / sampleRate;
        return static_cast<float> (fundamentalGain * std::sin (phase) + secondGain * std::sin (phase * 2.0));
    }
    double expectedFrequency (int) const { return fundamental; }

    double fundamental, fundamentalGain, secondGain;
};

struct ConsonantProxy
{
    float operator() (int sample) const
    {
        const auto phase = sample % 4800;
        if (phase < 720)
            return Noise {} (sample);
        if (phase < 780)
            return sample % 2 == 0 ? 0.5f : -0.5f;
        return 0.0f;
    }
    double expectedFrequency (int) const { return std::numeric_limits<double>::quiet_NaN(); }
};
}

int main()
{
    std::cout << std::fixed << std::setprecision (3);
    std::cout << "pitch_tracking_baseline sample_rate=" << sampleRate << '\n';

    for (const auto blockSize : { 64, 128, 256, 512, 1024 })
    {
        runCase ("sine_fixed_220hz", FixedSine { 220.0 }, true, blockSize);
        runCase ("sine_sweep_110_to_440hz", SweepSine {}, true, blockSize);
        runCase ("vibrato_220hz_5hz_35cents", Vibrato {}, true, blockSize);
        runCase ("semitone_change_220_to_233_082hz", NoteChange { 233.0818808 }, true, blockSize);
        runCase ("octave_change_220_to_440hz", NoteChange { 440.0 }, true, blockSize);
        runCase ("silence", Silence {}, false, blockSize);
        runCase ("white_noise", Noise { 0.30f }, false, blockSize);
        runCase ("s_t_k_consonant_proxy", ConsonantProxy {}, false, blockSize);
        runCase ("noisy_sine_low_snr", NoisySine { 0.20f }, true, blockSize);
    }

    for (const auto blockSize : { 64, 128, 256, 512, 1024 })
    {
        runCase ("low_sine_55hz", FixedSine { 55.0 }, true, blockSize, 3);
        runCase ("low_sine_65hz", FixedSine { 65.0 }, true, blockSize, 3);
        runCase ("low_sine_80hz", FixedSine { 80.0 }, true, blockSize, 3);
        runCase ("low_sine_100hz", FixedSine { 100.0 }, true, blockSize, 3);
    }

    runCase ("harmonic_missing_fundamental_220hz", HarmonicSignal { 220.0, 0.0, 0.5 }, true);
    runCase ("harmonic_weak_fundamental_220hz", HarmonicSignal { 220.0, 0.03, 0.5 }, true);
    runCase ("second_harmonic_stronger_220hz", HarmonicSignal { 220.0, 0.12, 0.50 }, true);

}
