#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int benchmarkSamples = 480000; // 10 s: common multiple of all tested host blocks and covers retime stress.
constexpr int warmupRuns = 1;
constexpr int measuredRuns = 5;
constexpr int retimeHeavyStart = 275000;
constexpr int retimeHeavyEnd = 278500;

struct Configuration
{
    const char* name;
    int quantum;
    bool retiming;
    bool useProductionDefaults = false;
    // Diagnostic only: select causal TD-PSOLA after prepareToPlay(), which
    // otherwise restores the shipped spectral renderer.
    bool useCausalTdpsolaCandidate = false;
};
struct Summary
{
    double meanUs = 0.0, medianUs = 0.0, p95Us = 0.0, p99Us = 0.0, maximumUs = 0.0, stddevUs = 0.0;
    double normalMaximumUs = 0.0, heavyMaximumUs = 0.0;
    int latency = 0, plannerPeak = 0, pendingPeak = 0;
    uint64_t sourceQuantums = 0;
    uint64_t epochRetimes = 0, retimedSamples = 0, maxRetime = 0;
    bool safetyClean = true;
};

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void configureFixtureParameters (VibeAutotuneProcessor& processor)
{
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "retuneSpeed", 9.0f);
    setParameter (processor, "flexTune", 0.0f);
    setParameter (processor, "humanize", 0.0f);
    setParameter (processor, "naturalVibrato", 0.0f);
    setParameter (processor, "voicedColour", 0.0f);
    setParameter (processor, "inputType", 1.0f); // Alto
    setParameter (processor, "key", 6.0f);       // F#
    setParameter (processor, "scale", 2.0f);     // Natural minor
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);
    setParameter (processor, "tracking", 50.0f);
}

std::vector<float> loadMono (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate != sampleRate || reader->lengthInSamples < benchmarkSamples)
        throw std::runtime_error ("fixture must be a >=10 second 48 kHz WAV");
    juce::AudioBuffer<float> buffer (1, benchmarkSamples);
    reader->read (&buffer, 0, benchmarkSamples, 0, true, false);
    return { buffer.getReadPointer (0), buffer.getReadPointer (0) + benchmarkSamples };
}

double percentile (std::vector<double> values, double p)
{
    std::sort (values.begin(), values.end());
    const auto index = static_cast<size_t> (std::ceil (p * values.size())) - 1;
    return values[std::min (index, values.size() - 1)];
}

bool cleanSafety (const VibeAutotuneProcessor& processor)
{
    const auto scheduler = processor.getSourceSchedulerDiagnostics();
    return processor.getRendererRetimeDeadlineMissCount() == 0
        && processor.getRendererTicketIssuedPastDeadlineCount() == 0
        && processor.getRendererTicketMutationAfterIssueCount() == 0
        && processor.getRendererPendingDeadlineMissCount() == 0
        && processor.getRendererLateCandidateCreationCount() == 0
        && processor.getRendererLateOlaWriteCount() == 0
        && processor.getRendererEpochOrderViolationCount() == 0
        && processor.getRendererPendingQueueOverflowCount() == 0
        && scheduler.sourceDropCount == 0 && scheduler.sourceDuplicateCount == 0
        && scheduler.schedulerGapCount == 0 && scheduler.schedulerOverlapCount == 0;
}

Summary run (const Configuration& configuration, int hostBlock, const std::vector<float>& input)
{
    std::vector<double> callbackUs;
    callbackUs.reserve (static_cast<size_t> (measuredRuns * benchmarkSamples / hostBlock));
    Summary result;

    for (int repetition = 0; repetition < warmupRuns + measuredRuns; ++repetition)
    {
        if (configuration.useProductionDefaults)
        {
            ::unsetenv ("VIBE_SOURCE_SCHEDULER_QUANTUM");
            ::unsetenv ("VIBE_RETIMABLE_EPOCHS");
        }
        else
        {
            ::setenv ("VIBE_SOURCE_SCHEDULER_QUANTUM", configuration.quantum == 32 ? "32" : "64", 1);
            ::setenv ("VIBE_RETIMABLE_EPOCHS", configuration.retiming ? "1" : "0", 1);
        }
        VibeAutotuneProcessor processor;
        if (! configuration.useProductionDefaults)
        {
            CausalTDPSOLARenderer::DiagnosticAblation ablation;
            ablation.retimableEpochs = configuration.retiming;
            processor.setRendererDiagnosticAblation (ablation);
        }
        processor.prepareToPlay (sampleRate, hostBlock);
        if (configuration.useCausalTdpsolaCandidate)
        {
            CausalTDPSOLARenderer::DiagnosticAblation candidate;
            candidate.phaseContinuousRenderer = false;
            candidate.retimableEpochs = configuration.retiming;
            processor.setRendererDiagnosticAblation (candidate);
        }
        configureFixtureParameters (processor);
        // This timer mode is deliberately separate from the callback timing
        // benchmark. It records component attribution without changing DSP
        // decisions; normal timing runs leave it off.
        const auto profile = std::getenv ("VIBE_BENCHMARK_PROFILE") != nullptr;
        processor.setPitchAnalysisProfilingEnabled (profile);

        juce::AudioBuffer<float> buffer (2, hostBlock);
        juce::MidiBuffer midi;
        auto normalMax = 0.0, heavyMax = 0.0;
        for (int start = 0; start < benchmarkSamples; start += hostBlock)
        {
            const auto* source = input.data() + start;
            buffer.copyFrom (0, 0, source, hostBlock);
            buffer.copyFrom (1, 0, source, hostBlock);
            const auto begin = std::chrono::steady_clock::now();
            processor.processBlock (buffer, midi);
            const auto elapsed = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - begin).count();
            const auto heavy = start < retimeHeavyEnd && start + hostBlock > retimeHeavyStart;
            if (repetition >= warmupRuns)
            {
                callbackUs.push_back (elapsed);
                if (heavy) heavyMax = std::max (heavyMax, elapsed);
                else normalMax = std::max (normalMax, elapsed);
            }
        }
        if (repetition >= warmupRuns)
        {
            result.normalMaximumUs = std::max (result.normalMaximumUs, normalMax);
            result.heavyMaximumUs = std::max (result.heavyMaximumUs, heavyMax);
        }
        result.latency = processor.getLatencySamples();
        result.plannerPeak = std::max (result.plannerPeak, processor.getRendererRetimableEpochPeakOccupancy());
        result.pendingPeak = std::max (result.pendingPeak, processor.getRendererPendingQueuePeakOccupancy());
        result.epochRetimes = std::max (result.epochRetimes, processor.getRendererEpochRetimeCount());
        result.retimedSamples = std::max (result.retimedSamples, processor.getRendererRetimedSamplesTotal());
        result.maxRetime = std::max (result.maxRetime, processor.getRendererMaxSingleEpochRetime());
        result.sourceQuantums = std::max (result.sourceQuantums,
                                          processor.getSourceSchedulerDiagnostics().quantumsScheduled);
        result.safetyClean = result.safetyClean && cleanSafety (processor) && result.latency == 1152;
        if (profile && repetition == warmupRuns)
        {
            const auto p = processor.getPitchAnalysisPerformanceData();
            std::cout << "yin_profile analyses=" << p.analysisCount
                      << " total_us=" << p.totalMicroseconds
                      << " fifo_us=" << p.fifoFramePreparationMicroseconds
                      << " difference_us=" << p.differenceFunctionMicroseconds
                      << " cmndf_us=" << p.cmndfMicroseconds
                      << " candidate_us=" << p.candidateSearchMicroseconds
                      << " autocorrelation_us=" << p.normalizedAutocorrelationMicroseconds
                      << " parabolic_us=" << p.parabolicInterpolationMicroseconds
                      << " voicing_us=" << p.voicingFeaturesMicroseconds
                      << " octave_us=" << p.octaveScoringMicroseconds
                      << " smoothing_us=" << p.smoothingMicroseconds << '\n';
        }
    }

    result.meanUs = std::accumulate (callbackUs.begin(), callbackUs.end(), 0.0) / callbackUs.size();
    result.medianUs = percentile (callbackUs, 0.50);
    result.p95Us = percentile (callbackUs, 0.95);
    result.p99Us = percentile (callbackUs, 0.99);
    result.maximumUs = *std::max_element (callbackUs.begin(), callbackUs.end());
    auto squareSum = 0.0;
    for (const auto value : callbackUs) squareSum += (value - result.meanUs) * (value - result.meanUs);
    result.stddevUs = std::sqrt (squareSum / callbackUs.size());
    return result;
}

void print (const Configuration& c, int block, const Summary& s)
{
    const auto deadlineUs = 1000000.0 * static_cast<double> (block) / sampleRate;
    std::cout << std::fixed << std::setprecision (3)
              << "benchmark config=" << c.name << " host=" << block
              << " mean_us=" << s.meanUs << " median_us=" << s.medianUs
              << " p95_us=" << s.p95Us << " p99_us=" << s.p99Us << " max_us=" << s.maximumUs << " stddev_us=" << s.stddevUs
              << " deadline_us=" << deadlineUs << " mean_pct=" << 100.0 * s.meanUs / deadlineUs
              << " p95_pct=" << 100.0 * s.p95Us / deadlineUs << " p99_pct=" << 100.0 * s.p99Us / deadlineUs << " max_pct=" << 100.0 * s.maximumUs / deadlineUs
              << " max_headroom_us=" << deadlineUs - s.maximumUs
              << " normal_max_us=" << s.normalMaximumUs << " retime_heavy_max_us=" << s.heavyMaximumUs
              << " latency=" << s.latency << " planner_peak=" << s.plannerPeak << " pending_peak=" << s.pendingPeak
              << " source_quantums=" << s.sourceQuantums
              << " retime_count=" << s.epochRetimes << " retimed_samples=" << s.retimedSamples
              << " max_retime=" << s.maxRetime << " safety=" << (s.safetyClean ? "PASS" : "FAIL") << '\n';
}
}

int main (int argc, char** argv)
{
    std::cout.setf (std::ios::unitbuf);
    if (argc != 2 && argc != 4)
    {
        std::cerr << "usage: VibeAutotuneRealtimeSchedulerBenchmark <48k-vocal-fixture.wav> [config-index(0..4) host-block]\n";
        return 2;
    }
    try
    {
        const auto input = loadMono (juce::File (argv[1]));
        const Configuration configurations[] { { "A_64_off", 64, false }, { "B_32_off", 32, false },
                                               { "C_32_on", 32, true }, { "D_production_default", 0, false, true },
                                               { "E_causal_tdpsola_candidate", 0, false, true, true } };
        const int hostBlocks[] { 32, 64, 128, 256, 100 };
        std::cout << "realtime_benchmark sample_rate=48000 samples=" << benchmarkSamples
                  << " warmup_runs=" << warmupRuns << " measured_runs=" << measuredRuns
                  << " diagnostics=disabled fixture=" << argv[1] << '\n';
        if (argc == 4)
        {
            const auto index = std::atoi (argv[2]);
            const auto hostBlock = std::atoi (argv[3]);
            if (index < 0 || index >= 5 || hostBlock <= 0 || benchmarkSamples % hostBlock != 0)
                throw std::runtime_error ("configuration index or host block is invalid");
            print (configurations[index], hostBlock, run (configurations[index], hostBlock, input));
        }
        else
            for (const auto& configuration : configurations)
                for (const auto hostBlock : hostBlocks)
                    print (configuration, hostBlock, run (configuration, hostBlock, input));
        ::unsetenv ("VIBE_SOURCE_SCHEDULER_QUANTUM");
        ::unsetenv ("VIBE_RETIMABLE_EPOCHS");
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
