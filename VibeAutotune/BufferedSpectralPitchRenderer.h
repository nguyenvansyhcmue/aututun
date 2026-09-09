#pragma once

#include "EventOwnedSpectralFrameQueue.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

// Diagnostic-only phase-vocoder foundation.  Unlike the production black-box
// renderer, its output frames are explicitly owned by target events.  DSP
// integration is deliberately kept separate from the queue policy so a
// future target commit can re-render only unheard frame payloads.
class BufferedSpectralPitchRenderer
{
public:
    static constexpr int frameSize = 1024;
    static constexpr int hopSize = 128;

    void prepare (double newSampleRate, int maximumBlock)
    {
        sampleRate = newSampleRate;
        fft = std::make_unique<juce::dsp::FFT> (10);
        window.resize (frameSize);
        inputHistory.assign (frameSize, 0.0f);
        fftBuffer.assign (2 * frameSize, 0.0f);
        mappedSpectrum.assign (2 * frameSize, 0.0f);
        outputOla.assign (frameSize * 4 + maximumBlock, 0.0f);
        outputWeight.assign (outputOla.size(), 0.0f);
        previousInputPhase.assign (frameSize / 2 + 1, 0.0);
        outputPhase.assign (frameSize / 2 + 1, 0.0);
        rebuildPhase.assign (frameSize / 2 + 1, 0.0);
        for (int n = 0; n < frameSize; ++n)
            window[static_cast<size_t> (n)] = static_cast<float> (0.5 - 0.5 * std::cos (
                2.0 * std::numbers::pi * static_cast<double> (n) / static_cast<double> (frameSize - 1)));
        reset();
    }

    void reset() noexcept
    {
        std::fill (inputHistory.begin(), inputHistory.end(), 0.0f);
        std::fill (fftBuffer.begin(), fftBuffer.end(), 0.0f);
        std::fill (mappedSpectrum.begin(), mappedSpectrum.end(), 0.0f);
        std::fill (outputOla.begin(), outputOla.end(), 0.0f);
        std::fill (outputWeight.begin(), outputWeight.end(), 0.0f);
        std::fill (previousInputPhase.begin(), previousInputPhase.end(), 0.0);
        std::fill (outputPhase.begin(), outputPhase.end(), 0.0);
        std::fill (rebuildPhase.begin(), rebuildPhase.end(), 0.0);
        queue.clear();
        inputWrite = outputRead = samplesSinceFrame = 0;
        absoluteSample = targetEvent = 0;
    }

    int latencySamples() const noexcept { return frameSize; }
    EventOwnedSpectralFrameQueue& frameQueue() noexcept { return queue; }

    // The first diagnostic stage preserves the analysis/synthesis timeline
    // and queue ownership. Phase remapping is added only after this path is
    // proven click-free on the real vocal fixture.
    void process (const float* input, float* output, int samples, double ratio,
                  uint64_t newTargetEvent) noexcept
    {
        if (input == nullptr || output == nullptr || samples <= 0 || fft == nullptr) return;
        const auto safeRatio = std::clamp (ratio, 0.90, 1.10);
        if (newTargetEvent != targetEvent)
        {
            replacementAudibleSample = absoluteSample + latencySamples();
            rebuildFutureSequence (newTargetEvent, safeRatio);
            targetEvent = newTargetEvent;
        }
        for (int n = 0; n < samples; ++n, ++absoluteSample)
        {
            queue.retireBefore (absoluteSample);
            inputHistory[static_cast<size_t> (inputWrite)] = input[n];
            inputWrite = (inputWrite + 1) % frameSize;
            ++samplesSinceFrame;
            if (samplesSinceFrame >= hopSize)
            {
                samplesSinceFrame = 0;
                scheduleAnalysisFrame (safeRatio);
            }
            const auto index = outputRead % static_cast<int> (outputOla.size());
            output[n] = outputWeight[static_cast<size_t> (index)] > 1.0e-6f
                      ? outputOla[static_cast<size_t> (index)] / outputWeight[static_cast<size_t> (index)] : 0.0f;
            outputOla[static_cast<size_t> (index)] = 0.0f;
            outputWeight[static_cast<size_t> (index)] = 0.0f;
            ++outputRead;
        }
    }

private:
    void scheduleAnalysisFrame (double ratio) noexcept
    {
        const auto synthesisStart = absoluteSample + latencySamples();
        auto* scheduledFrame = queue.pushFrame (synthesisStart, targetEvent, ratio);
        if (scheduledFrame == nullptr) return;
        std::fill (fftBuffer.begin(), fftBuffer.end(), 0.0f);
        for (int n = 0; n < frameSize; ++n)
        {
            const auto sourceIndex = (inputWrite + n) % frameSize;
            scheduledFrame->analysisPayload[static_cast<size_t> (n)] = inputHistory[static_cast<size_t> (sourceIndex)];
            fftBuffer[static_cast<size_t> (n)] = scheduledFrame->analysisPayload[static_cast<size_t> (n)]
                                               * window[static_cast<size_t> (n)];
        }
        fft->performRealOnlyForwardTransform (fftBuffer.data());
        for (int sourceBin = 0; sourceBin <= frameSize / 2; ++sourceBin)
        {
            const auto real = fftBuffer[static_cast<size_t> (2 * sourceBin)];
            const auto imaginary = fftBuffer[static_cast<size_t> (2 * sourceBin + 1)];
            const auto phase = std::atan2 (imaginary, real);
            const auto expectedAdvance = 2.0 * std::numbers::pi * sourceBin * hopSize / frameSize;
            auto phaseDeviation = phase - previousInputPhase[static_cast<size_t> (sourceBin)] - expectedAdvance;
            phaseDeviation = std::remainder (phaseDeviation, 2.0 * std::numbers::pi);
            previousInputPhase[static_cast<size_t> (sourceBin)] = phase;
            scheduledFrame->analysisReal[static_cast<size_t> (sourceBin)] = real;
            scheduledFrame->analysisImaginary[static_cast<size_t> (sourceBin)] = imaginary;
            scheduledFrame->inputPhaseAdvance[static_cast<size_t> (sourceBin)] = static_cast<float> (expectedAdvance + phaseDeviation);
        }
        synthesiseFrame (*scheduledFrame, outputPhase);
        for (int n = 0; n < frameSize; ++n)
        {
            const auto value = scheduledFrame->synthesisPayload[static_cast<size_t> (n)];
            const auto destination = static_cast<int> ((synthesisStart + static_cast<uint64_t> (n)) % outputOla.size());
            outputOla[static_cast<size_t> (destination)] += value;
            outputWeight[static_cast<size_t> (destination)] += window[static_cast<size_t> (n)] * window[static_cast<size_t> (n)];
        }
    }

    void writeFramePayload (EventOwnedSpectralFrameQueue::Frame& frame, bool add) noexcept
    {
        for (int n = 0; n < frameSize; ++n)
        {
            const auto frameSample = frame.synthesisSample + static_cast<uint64_t> (n);
            if (frameSample < replacementAudibleSample)
                continue;
            const auto payload = frame.synthesisPayload[static_cast<size_t> (n)];
            const auto destination = static_cast<int> (frameSample % outputOla.size());
            const auto sign = add ? 1.0f : -1.0f;
            outputOla[static_cast<size_t> (destination)] += sign * payload;
            outputWeight[static_cast<size_t> (destination)] += sign * window[static_cast<size_t> (n)] * window[static_cast<size_t> (n)];
        }
    }

    void synthesiseFrame (EventOwnedSpectralFrameQueue::Frame& frame, std::vector<double>& phaseState) noexcept
    {
        constexpr auto lastBin = frameSize / 2;
        const auto ratio = std::clamp (frame.pitchRatio, 0.90, 1.10);
        for (int bin = 0; bin <= lastBin; ++bin)
            frame.phaseBefore[static_cast<size_t> (bin)] = static_cast<float> (phaseState[static_cast<size_t> (bin)]);
        std::fill (mappedSpectrum.begin(), mappedSpectrum.end(), 0.0f);
        for (int outputBin = 0; outputBin <= lastBin; ++outputBin)
        {
            const auto sourceBin = static_cast<double> (outputBin) / ratio;
            const auto lower = static_cast<int> (std::floor (sourceBin));
            if (lower < 0 || lower >= lastBin) continue;
            const auto upper = lower + 1;
            const auto fraction = sourceBin - lower;
            const auto real = frame.analysisReal[static_cast<size_t> (lower)]
                            + fraction * (frame.analysisReal[static_cast<size_t> (upper)] - frame.analysisReal[static_cast<size_t> (lower)]);
            const auto imaginary = frame.analysisImaginary[static_cast<size_t> (lower)]
                                 + fraction * (frame.analysisImaginary[static_cast<size_t> (upper)] - frame.analysisImaginary[static_cast<size_t> (lower)]);
            const auto advance = frame.inputPhaseAdvance[static_cast<size_t> (lower)]
                               + fraction * (frame.inputPhaseAdvance[static_cast<size_t> (upper)] - frame.inputPhaseAdvance[static_cast<size_t> (lower)]);
            phaseState[static_cast<size_t> (outputBin)] += advance * ratio;
            const auto magnitude = std::hypot (real, imaginary);
            mappedSpectrum[static_cast<size_t> (2 * outputBin)] = static_cast<float> (magnitude * std::cos (phaseState[static_cast<size_t> (outputBin)]));
            mappedSpectrum[static_cast<size_t> (2 * outputBin + 1)] = static_cast<float> (magnitude * std::sin (phaseState[static_cast<size_t> (outputBin)]));
        }
        fft->performRealOnlyInverseTransform (mappedSpectrum.data());
        for (int bin = 0; bin <= lastBin; ++bin)
            frame.phaseAfter[static_cast<size_t> (bin)] = static_cast<float> (phaseState[static_cast<size_t> (bin)]);
        for (int n = 0; n < frameSize; ++n)
            frame.synthesisPayload[static_cast<size_t> (n)] = mappedSpectrum[static_cast<size_t> (n)] * window[static_cast<size_t> (n)];
    }

    void rebuildFutureSequence (uint64_t newEvent, double newRatio) noexcept
    {
        auto& frames = queue.framesForRebuild();
        auto haveAnchor = false;
        uint64_t lastStart = 0;
        for (int pass = 0; pass < EventOwnedSpectralFrameQueue::capacity; ++pass)
        {
            EventOwnedSpectralFrameQueue::Frame* next = nullptr;
            for (auto& frame : frames)
                if (frame.active && frame.synthesisSample >= lastStart
                    && (next == nullptr || frame.synthesisSample < next->synthesisSample)
                    && (pass == 0 || frame.synthesisSample > lastStart))
                    next = &frame;
            if (next == nullptr) break;
            lastStart = next->synthesisSample;
            if (next->synthesisSample < replacementAudibleSample)
            {
                for (int bin = 0; bin <= frameSize / 2; ++bin)
                    rebuildPhase[static_cast<size_t> (bin)] = next->phaseAfter[static_cast<size_t> (bin)];
                haveAnchor = true;
                continue;
            }
            if (! haveAnchor)
            {
                for (int bin = 0; bin <= frameSize / 2; ++bin)
                    rebuildPhase[static_cast<size_t> (bin)] = next->phaseBefore[static_cast<size_t> (bin)];
                haveAnchor = true;
            }
            writeFramePayload (*next, false);
            next->targetEventId = newEvent;
            next->pitchRatio = newRatio;
            synthesiseFrame (*next, rebuildPhase);
            writeFramePayload (*next, true);
        }
        if (haveAnchor)
            outputPhase = rebuildPhase;
    }

    double sampleRate = 48000.0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, inputHistory, fftBuffer, mappedSpectrum, outputOla, outputWeight;
    std::vector<double> previousInputPhase, outputPhase, rebuildPhase;
    EventOwnedSpectralFrameQueue queue;
    int inputWrite = 0, outputRead = 0, samplesSinceFrame = 0;
    uint64_t absoluteSample = 0, targetEvent = 0;
    uint64_t replacementAudibleSample = 0;
};
