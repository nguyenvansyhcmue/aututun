#include <signalsmith-stretch/signalsmith-stretch.h>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;

double measureF0 (const std::vector<float>& audio, int start, int end)
{
    auto first = 0.0, last = 0.0;
    auto count = 0;
    for (int sample = start + 1; sample < end; ++sample)
    {
        const auto before = static_cast<double> (audio[static_cast<size_t> (sample - 1)]);
        const auto after = static_cast<double> (audio[static_cast<size_t> (sample)]);
        if (before <= 0.0 && after > 0.0)
        {
            const auto crossing = static_cast<double> (sample - 1) - before / (after - before);
            if (count == 0) first = crossing;
            last = crossing;
            ++count;
        }
    }
    return count > 1 ? sampleRate * static_cast<double> (count - 1) / (last - first) : 0.0;
}

double run (int fftSize, int interval, double requestedCents)
{
    constexpr auto blockSize = 256;
    constexpr auto sourceF0 = 220.0;
    signalsmith::stretch::SignalsmithStretch<float> renderer;
    renderer.configure (1, fftSize, interval, true);
    renderer.setTransposeFactor (1.0f);
    renderer.reset();

    std::vector<float> input (blockSize), output (blockSize), rendered;
    rendered.reserve (static_cast<size_t> (sampleRate * 4));
    const auto ratio = std::exp2 (requestedCents / 1200.0);
    auto phase = 0.0;
    for (int start = 0; start < static_cast<int> (sampleRate * 4); start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            input[static_cast<size_t> (sample)] = static_cast<float> (0.3 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * sourceF0 / sampleRate;
        }
        const float* in[] { input.data() };
        float* out[] { output.data() };
        renderer.setTransposeFactor (static_cast<float> (ratio));
        struct Input { const float* const* channels; const float* operator[] (int index) const { return channels[index]; } } inputBuffers { in };
        struct Output { float* const* channels; float* operator[] (int index) const { return channels[index]; } } outputBuffers { out };
        renderer.process (inputBuffers, blockSize, outputBuffers, blockSize);
        rendered.insert (rendered.end(), output.begin(), output.end());
    }

    const auto latency = renderer.inputLatency() + renderer.outputLatency();
    const auto f0 = measureF0 (rendered, latency + static_cast<int> (sampleRate),
                                latency + static_cast<int> (sampleRate * 3));
    return 1200.0 * std::log2 (f0 / sourceF0);
}
}

int main()
{
    for (const auto [fftSize, interval] : { std::pair { 1024, 128 }, std::pair { 1024, 256 },
                                             std::pair { 2048, 128 }, std::pair { 2048, 256 } })
    {
        signalsmith::stretch::SignalsmithStretch<float> latencyProbe;
        latencyProbe.configure (1, fftSize, interval, true);
        std::fprintf (stderr, "signalsmith_latency | fft=%d interval=%d input=%d output=%d total=%d\n",
                      fftSize, interval, latencyProbe.inputLatency(), latencyProbe.outputLatency(),
                      latencyProbe.inputLatency() + latencyProbe.outputLatency());
        for (const auto requested : { -60.0, -40.0, -30.0, -20.0, 20.0, 40.0 })
        {
            const auto measured = run (fftSize, interval, requested);
            std::fprintf (stderr, "signalsmith_calibration | fft=%d interval=%d requested_ct=%7.3f measured_ct=%7.3f error_ct=%7.3f\n",
                          fftSize, interval, requested, measured, measured - requested);
        }
    }
}
