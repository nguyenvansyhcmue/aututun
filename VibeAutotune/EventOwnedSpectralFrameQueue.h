#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Scheduling core for the replacement spectral renderer.  It intentionally
// contains no DSP: ownership must be correct before a phase-vocoder is
// allowed to write into the output overlap-add buffer.
class EventOwnedSpectralFrameQueue
{
public:
    struct Frame
    {
        uint64_t synthesisSample = 0;
        uint64_t targetEventId = 0;
        double pitchRatio = 1.0;
        bool active = false;
        // Kept outside the audio output ring so an unheard frame can be
        // subtracted and rebuilt without touching audio already delivered.
        std::array<float, 1024> analysisPayload {};
        std::array<float, 1024> synthesisPayload {};
        // Enough information to rebuild a chronological future sequence
        // without estimating phase from rendered samples.
        std::array<float, 513> analysisReal {};
        std::array<float, 513> analysisImaginary {};
        std::array<float, 513> inputPhaseAdvance {};
        std::array<float, 513> phaseBefore {};
        std::array<float, 513> phaseAfter {};
    };

    static constexpr int capacity = 32;

    void clear() noexcept
    {
        for (auto& frame : frames) frame = {};
    }

    bool push (uint64_t synthesisSample, uint64_t targetEventId, double pitchRatio) noexcept
    {
        return pushFrame (synthesisSample, targetEventId, pitchRatio) != nullptr;
    }

    Frame* pushFrame (uint64_t synthesisSample, uint64_t targetEventId, double pitchRatio) noexcept
    {
        for (auto& frame : frames)
            if (! frame.active)
            {
                frame = {};
                frame.synthesisSample = synthesisSample;
                frame.targetEventId = targetEventId;
                frame.pitchRatio = pitchRatio;
                frame.active = true;
                return &frame;
            }
        return nullptr;
    }

    // Only frames whose first sample has not become audible may change
    // ownership. Frames already audible retain their original data and are
    // permitted to finish their window normally.
    int replaceUnheard (uint64_t audibleSample, uint64_t oldEventId,
                        uint64_t newEventId, double newPitchRatio) noexcept
    {
        auto replaced = 0;
        for (auto& frame : frames)
            if (frame.active && frame.targetEventId == oldEventId
                && frame.synthesisSample + 1024 > audibleSample)
            {
                frame.targetEventId = newEventId;
                frame.pitchRatio = newPitchRatio;
                ++replaced;
            }
        return replaced;
    }

    template <typename Rebuild>
    int replaceUnheard (uint64_t audibleSample, uint64_t oldEventId,
                        uint64_t newEventId, double newPitchRatio, Rebuild&& rebuild) noexcept
    {
        auto replaced = 0;
        for (auto& frame : frames)
            if (frame.active && frame.targetEventId == oldEventId
                && frame.synthesisSample + 1024 > audibleSample)
            {
                rebuild (frame, false);
                frame.targetEventId = newEventId;
                frame.pitchRatio = newPitchRatio;
                rebuild (frame, true);
                ++replaced;
            }
        return replaced;
    }

    int countOwnedBy (uint64_t eventId) const noexcept
    {
        return static_cast<int> (std::count_if (frames.begin(), frames.end(), [eventId] (const Frame& frame)
        {
            return frame.active && frame.targetEventId == eventId;
        }));
    }

    // A frame stays owned until its final overlap-add sample has been sent.
    // Retiring at `synthesisSample` loses the metadata while its 1024-sample
    // tail is still audible, which makes a following note unable to revoke
    // the old pitch contribution.
    void retireBefore (uint64_t audibleSample) noexcept
    {
        for (auto& frame : frames)
            if (frame.active && frame.synthesisSample + 1024 <= audibleSample)
                frame.active = false;
    }

    const std::array<Frame, capacity>& diagnosticFrames() const noexcept { return frames; }
    std::array<Frame, capacity>& framesForRebuild() noexcept { return frames; }

private:
    std::array<Frame, capacity> frames {};
};
