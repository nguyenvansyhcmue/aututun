#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace
{
constexpr int fftOrder = 11;
constexpr int fftSize = 1 << fftOrder;
constexpr int hopSize = 512;
constexpr double silenceFloorDb = -55.0;

struct AudioFile
{
    double sampleRate = 0.0;
    std::vector<float> mono;
};

struct Metrics
{
    int alignmentSamples = 0;
    int frames = 0;
    double waveformNrmse = 0.0;
    double envelopeRmseDb = 0.0;
    double spectralDistanceDb = 0.0;
    double spectralTiltDistanceDb = 0.0;
    double candidateMaximumStep = 0.0;
    double targetMaximumStep = 0.0;
};

bool readMono (const juce::File& file, AudioFile& result)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    const auto samples = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> decoded (juce::jmax (1, static_cast<int> (reader->numChannels)), samples);
    if (! reader->read (&decoded, 0, samples, 0, true, true))
        return false;

    result.sampleRate = reader->sampleRate;
    result.mono.resize (static_cast<size_t> (samples));
    for (int sample = 0; sample < samples; ++sample)
    {
        auto sum = 0.0f;
        for (int channel = 0; channel < decoded.getNumChannels(); ++channel)
            sum += decoded.getSample (channel, sample);
        result.mono[static_cast<size_t> (sample)] = sum / static_cast<float> (decoded.getNumChannels());
    }
    return true;
}

double rmsDb (const float* samples, int count)
{
    auto sum = 0.0;
    for (int i = 0; i < count; ++i)
        sum += static_cast<double> (samples[i]) * samples[i];
    return 20.0 * std::log10 (std::sqrt (sum / static_cast<double> (count)) + 1.0e-12);
}

double maximumStep (const std::vector<float>& samples)
{
    auto maximum = 0.0;
    for (size_t i = 1; i < samples.size(); ++i)
        maximum = std::max (maximum, std::abs (static_cast<double> (samples[i] - samples[i - 1])));
    return maximum;
}

std::array<double, 4> logBandEnergies (const float* samples, double sampleRate,
                                       juce::dsp::FFT& fft, std::vector<float>& scratch)
{
    std::fill (scratch.begin(), scratch.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
    {
        const auto phase = static_cast<double> (i) / static_cast<double> (fftSize - 1);
        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * phase);
        scratch[static_cast<size_t> (i)] = static_cast<float> (samples[i] * window);
    }
    fft.performRealOnlyForwardTransform (scratch.data());

    constexpr std::array<double, 5> edges { 80.0, 400.0, 2000.0, 6000.0, 12000.0 };
    std::array<double, 4> energy {};
    std::array<int, 4> bins {};
    for (int bin = 1; bin < fftSize / 2; ++bin)
    {
        const auto frequency = sampleRate * static_cast<double> (bin) / static_cast<double> (fftSize);
        for (int band = 0; band < 4; ++band)
        {
            if (frequency >= edges[static_cast<size_t> (band)] && frequency < edges[static_cast<size_t> (band + 1)])
            {
                const auto real = scratch[static_cast<size_t> (2 * bin)];
                const auto imaginary = scratch[static_cast<size_t> (2 * bin + 1)];
                energy[static_cast<size_t> (band)] += static_cast<double> (real) * real + static_cast<double> (imaginary) * imaginary;
                ++bins[static_cast<size_t> (band)];
                break;
            }
        }
    }
    for (int band = 0; band < 4; ++band)
        energy[static_cast<size_t> (band)] = 10.0 * std::log10 (energy[static_cast<size_t> (band)] / std::max (1, bins[static_cast<size_t> (band)]) + 1.0e-20);
    return energy;
}

Metrics compare (const AudioFile& target, const AudioFile& candidate, int alignmentSamples,
                 std::ostream* frameCsv = nullptr)
{
    Metrics metrics;
    metrics.targetMaximumStep = maximumStep (target.mono);
    metrics.candidateMaximumStep = maximumStep (candidate.mono);
    metrics.alignmentSamples = alignmentSamples;
    const auto first = std::max (0, -metrics.alignmentSamples);
    const auto last = std::min (static_cast<int> (target.mono.size()),
                                static_cast<int> (candidate.mono.size()) - metrics.alignmentSamples);
    if (last - first < fftSize)
        return metrics;

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> targetScratch (static_cast<size_t> (2 * fftSize));
    std::vector<float> candidateScratch (static_cast<size_t> (2 * fftSize));
    auto waveformError = 0.0;
    auto waveformTargetEnergy = 0.0;
    auto envelopeError = 0.0;
    auto spectrumError = 0.0;
    auto tiltError = 0.0;

    if (frameCsv != nullptr)
        *frameCsv << "timeMs,targetRmsDb,candidateRmsDb,waveformNrmse,band80_400DbDiff,band400_2000DbDiff,band2000_6000DbDiff,band6000_12000DbDiff,spectralRmseDb,targetMaxStep,targetMaxStepOffset,candidateMaxStep,candidateMaxStepOffset\n";

    for (int start = first; start + fftSize <= last; start += hopSize)
    {
        const auto* targetFrame = target.mono.data() + start;
        const auto* candidateFrame = candidate.mono.data() + start + metrics.alignmentSamples;
        const auto targetRms = rmsDb (targetFrame, fftSize);
        const auto candidateRms = rmsDb (candidateFrame, fftSize);
        if (std::max (targetRms, candidateRms) < silenceFloorDb)
            continue;

        for (int i = 0; i < fftSize; ++i)
        {
            const auto difference = static_cast<double> (targetFrame[i] - candidateFrame[i]);
            waveformError += difference * difference;
            waveformTargetEnergy += static_cast<double> (targetFrame[i]) * targetFrame[i];
        }
        envelopeError += std::pow (targetRms - candidateRms, 2.0);
        const auto targetBands = logBandEnergies (targetFrame, target.sampleRate, fft, targetScratch);
        const auto candidateBands = logBandEnergies (candidateFrame, candidate.sampleRate, fft, candidateScratch);
        auto frameSpectrumError = 0.0;
        for (int band = 0; band < 4; ++band)
        {
            const auto difference = targetBands[static_cast<size_t> (band)] - candidateBands[static_cast<size_t> (band)];
            spectrumError += difference * difference;
            frameSpectrumError += difference * difference;
        }
        const auto targetTilt = targetBands[3] - targetBands[0];
        const auto candidateTilt = candidateBands[3] - candidateBands[0];
        tiltError += std::pow (targetTilt - candidateTilt, 2.0);
        if (frameCsv != nullptr)
        {
            auto frameError = 0.0, frameEnergy = 0.0;
            auto targetStep = 0.0, candidateStep = 0.0;
            auto targetStepOffset = 0, candidateStepOffset = 0;
            for (int i = 1; i < fftSize; ++i)
            {
                const auto difference = static_cast<double> (targetFrame[i] - candidateFrame[i]);
                frameError += difference * difference;
                frameEnergy += static_cast<double> (targetFrame[i]) * targetFrame[i];
                const auto nextTargetStep = std::abs (static_cast<double> (targetFrame[i] - targetFrame[i - 1]));
                const auto nextCandidateStep = std::abs (static_cast<double> (candidateFrame[i] - candidateFrame[i - 1]));
                if (nextTargetStep > targetStep) { targetStep = nextTargetStep; targetStepOffset = i; }
                if (nextCandidateStep > candidateStep) { candidateStep = nextCandidateStep; candidateStepOffset = i; }
            }
            *frameCsv << (1000.0 * static_cast<double> (start) / target.sampleRate) << ','
                      << targetRms << ',' << candidateRms << ',' << std::sqrt (frameError / std::max (frameEnergy, 1.0e-20));
            for (int band = 0; band < 4; ++band)
                *frameCsv << ',' << (candidateBands[static_cast<size_t> (band)] - targetBands[static_cast<size_t> (band)]);
            *frameCsv << ',' << std::sqrt (frameSpectrumError / 4.0) << ',' << targetStep << ',' << targetStepOffset
                      << ',' << candidateStep << ',' << candidateStepOffset << '\n';
        }
        ++metrics.frames;
    }

    if (metrics.frames > 0)
    {
        metrics.waveformNrmse = std::sqrt (waveformError / std::max (waveformTargetEnergy, 1.0e-20));
        metrics.envelopeRmseDb = std::sqrt (envelopeError / metrics.frames);
        metrics.spectralDistanceDb = std::sqrt (spectrumError / (4.0 * metrics.frames));
        metrics.spectralTiltDistanceDb = std::sqrt (tiltError / metrics.frames);
    }
    return metrics;
}
}

int main (int argc, char* argv[])
{
    if (argc != 3 && argc != 4 && argc != 5)
    {
        std::cerr << "usage: VibeAutotuneTargetReferenceMatch <target-wav> <candidate-wav> [candidate-alignment-samples] [frame-csv]\n";
        return 2;
    }
    AudioFile target, candidate;
    if (! readMono (juce::File (argv[1]), target) || ! readMono (juce::File (argv[2]), candidate))
    {
        std::cerr << "unable to decode input wav\n";
        return 3;
    }
    if (std::abs (target.sampleRate - candidate.sampleRate) > 0.1)
    {
        std::cerr << "sample-rate mismatch\n";
        return 4;
    }
    // For production comparisons this value is intentionally supplied from
    // measured pipeline latency, rather than estimated from periodic vocals.
    const auto alignmentSamples = argc >= 4 ? std::atoi (argv[3]) : 0;
    std::ofstream frameCsv;
    if (argc == 5)
        frameCsv.open (argv[4]);
    const auto metrics = compare (target, candidate, alignmentSamples,
                                  frameCsv.is_open() ? &frameCsv : nullptr);
    std::cout << "frames=" << metrics.frames
              << " alignment_samples=" << metrics.alignmentSamples
              << " waveform_nrmse=" << metrics.waveformNrmse
              << " envelope_rmse_db=" << metrics.envelopeRmseDb
              << " spectral_rmse_db=" << metrics.spectralDistanceDb
              << " spectral_tilt_rmse_db=" << metrics.spectralTiltDistanceDb
              << " target_max_step=" << metrics.targetMaximumStep
              << " candidate_max_step=" << metrics.candidateMaximumStep << '\n';
    return metrics.frames > 0 ? 0 : 5;
}
