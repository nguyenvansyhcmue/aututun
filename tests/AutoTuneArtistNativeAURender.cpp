#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr UInt32 channels = 1;
constexpr UInt32 framesPerSlice = 512;
constexpr AudioUnitParameterID inputTypeParameter = 0;
constexpr AudioUnitParameterID keyParameter = 1;
constexpr AudioUnitParameterID scaleParameter = 2;
constexpr AudioUnitParameterID detuneParameter = 3;
constexpr AudioUnitParameterID formantParameter = 5;
constexpr AudioUnitParameterID retuneParameter = 6;
constexpr AudioUnitParameterID humanizeParameter = 7;
constexpr AudioUnitParameterID naturalVibratoParameter = 8;
constexpr AudioUnitParameterID trackingParameter = 20;
constexpr AudioUnitParameterID stereoReferenceParameter = 21;
constexpr AudioUnitParameterID classicParameter = 22;
constexpr AudioUnitParameterID flexTuneParameter = 27;
constexpr AudioUnitParameterID transposeParameter = 34;

struct InputSource
{
    const std::vector<float>* samples = nullptr; // mono
    int64_t nextFrame = 0;
    int64_t frameCount = 0;
};

const char* statusText (OSStatus status)
{
    static thread_local char text[5] {};
    const auto code = static_cast<UInt32> (status);
    for (int i = 0; i < 4; ++i)
        text[i] = static_cast<char> ((code >> ((3 - i) * 8)) & 0xff);
    for (const auto c : text)
        if (c < 32 || c > 126)
            return "non-fourcc OSStatus";
    return text;
}

bool require (OSStatus status, const char* operation)
{
    if (status == noErr)
        return true;
    std::cerr << operation << " failed: " << status << " (" << statusText (status) << ")\n";
    return false;
}

OSStatus supplyInput (void* reference, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                      UInt32 requestedFrames, AudioBufferList* buffers)
{
    auto& source = *static_cast<InputSource*> (reference);
    const auto available = std::max<int64_t> (0, source.frameCount - source.nextFrame);
    const auto copiedFrames = static_cast<UInt32> (std::min<int64_t> (available, requestedFrames));

    for (UInt32 bufferIndex = 0; bufferIndex < buffers->mNumberBuffers; ++bufferIndex)
    {
        auto& buffer = buffers->mBuffers[bufferIndex];
        auto* destination = static_cast<float*> (buffer.mData);
        if (destination == nullptr)
            continue;

        // Artist requests an interleaved stereo input in this host. Keep the
        // fallback correct if a future component asks for separate buffers.
        if (buffers->mNumberBuffers == 1 && buffer.mNumberChannels == channels)
        {
            std::memcpy (destination, source.samples->data() + source.nextFrame * channels,
                         copiedFrames * channels * sizeof (float));
            std::fill (destination + copiedFrames * channels, destination + requestedFrames * channels, 0.0f);
        }
        else
        {
            const auto channel = std::min<UInt32> (bufferIndex, channels - 1);
            for (UInt32 frame = 0; frame < copiedFrames; ++frame)
                destination[frame] = (*source.samples)[(source.nextFrame + frame) * channels + channel];
            std::fill (destination + copiedFrames, destination + requestedFrames, 0.0f);
        }
        buffer.mDataByteSize = requestedFrames * buffer.mNumberChannels * sizeof (float);
    }
    source.nextFrame += copiedFrames;
    return noErr;
}

AudioStreamBasicDescription makeStereoFloatFormat()
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    // Logic's calibration track is mono. Keeping the AU mono also avoids its
    // stereo-reference path influencing what should be a one-voice test.
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof (float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof (float);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    return format;
}

bool setNormalisedParameter (AudioUnit unit, AudioUnitParameterID id, AudioUnitParameterValue value, const char* label)
{
    AudioUnitParameterInfo info {};
    UInt32 size = sizeof (info);
    if (! require (AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id,
                                         &info, &size), label))
        return false;
    std::cout << "parameter," << label << ",range=" << info.minValue << ".." << info.maxValue
              << ",set=" << value << '\n';
    if (! require (AudioUnitSetParameter (unit, id, kAudioUnitScope_Global, 0, value, 0), label))
        return false;
    AudioUnitParameterValue actual = 0.0f;
    if (! require (AudioUnitGetParameter (unit, id, kAudioUnitScope_Global, 0, &actual), label))
        return false;
    AudioUnitParameterStringFromValue display { id, &actual, nullptr };
    UInt32 displaySize = sizeof (display);
    if (AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0,
                              &display, &displaySize) == noErr && display.outString != nullptr)
    {
        std::cout << "parameter_actual," << label << "," << juce::String::fromCFString (display.outString) << '\n';
        CFRelease (display.outString);
    }
    return true;
}

bool setParameterText (AudioUnit unit, AudioUnitParameterID id, const char* text, const char* label)
{
    const auto string = CFStringCreateWithCString (kCFAllocatorDefault, text, kCFStringEncodingUTF8);
    AudioUnitParameterValueFromString conversion { id, string, 0.0f };
    UInt32 conversionSize = sizeof (conversion);
    const auto parsed = AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterValueFromString,
                                               kAudioUnitScope_Global, 0, &conversion, &conversionSize);
    CFRelease (string);
    if (! require (parsed, label))
        return false;
    std::cout << "parameter," << label << ",text=" << text << ",normalised=" << conversion.outValue << '\n';
    return require (AudioUnitSetParameter (unit, id, kAudioUnitScope_Global, 0, conversion.outValue, 0), label);
}

bool configureReferencePreset (AudioUnit unit, float retuneMs)
{
    // Artist's AU interface exposes the complete 0..1 host-normalised range.
    // The three enum indices below come from the inspector; the rest use the
    // component's own parser so e.g. “50” becomes its exact internal value.
    return setNormalisedParameter (unit, inputTypeParameter, 1.0f / 4.0f, "Input Type")
        && setNormalisedParameter (unit, keyParameter, 6.0f / 11.0f, "Key")
        && setNormalisedParameter (unit, scaleParameter, 1.0f / 27.0f, "Scale")
        && setParameterText (unit, detuneParameter, "0", "Detune")
        && setParameterText (unit, formantParameter, "Off", "Formant Correction")
        && setParameterText (unit, retuneParameter, juce::String (retuneMs, 3).toRawUTF8(), "Retune Speed")
        && setParameterText (unit, humanizeParameter, "0", "Humanize")
        && setParameterText (unit, naturalVibratoParameter, "0", "Natural Vibrato")
        && setParameterText (unit, trackingParameter, "50", "Tracking")
        && setParameterText (unit, stereoReferenceParameter, "Left", "Stereo Reference")
        && setParameterText (unit, classicParameter, "0", "Use Classic Mode DSP")
        && setParameterText (unit, flexTuneParameter, "0", "Flex-Tune")
        && setParameterText (unit, transposeParameter, "0", "Transpose");
}

bool loadStereoInput (juce::AudioFormatManager& formats, const juce::File& file, std::vector<float>& destination)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate != sampleRate)
    {
        std::cerr << "expected a 48 kHz WAV: " << file.getFullPathName() << '\n';
        return false;
    }
    juce::AudioBuffer<float> source (1, static_cast<int> (reader->lengthInSamples));
    reader->read (&source, 0, source.getNumSamples(), 0, true, true);
    destination.resize (static_cast<size_t> (source.getNumSamples()) * channels);
    for (int frame = 0; frame < source.getNumSamples(); ++frame)
        destination[static_cast<size_t> (frame)] = source.getSample (0, frame);
    return true;
}

bool writeMonoWav (const juce::File& file, const std::vector<float>& interleaved)
{
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        stream != nullptr ? wav.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0) : nullptr);
    if (writer == nullptr)
    {
        std::cerr << "cannot write " << file.getFullPathName() << '\n';
        return false;
    }
    stream.release();
    juce::AudioBuffer<float> mono (1, static_cast<int> (interleaved.size() / channels));
    for (int frame = 0; frame < mono.getNumSamples(); ++frame)
        // The calibration fixture and Artist's Logic track are mono. Use the
        // first rendered channel, rather than summing two independently
        // processed channels (which can create a false subharmonic).
        mono.setSample (0, frame, interleaved[static_cast<size_t> (frame) * channels]);
    return writer->writeFromAudioSampleBuffer (mono, 0, mono.getNumSamples());
}
}

int main (int argc, char* argv[])
{
    const auto root = juce::File::getCurrentWorkingDirectory();
    const auto input = argc >= 2 ? juce::File (argv[1])
        : root.getChildFile ("benchmark/autotune-artist-colour-probe/fsharp-minor/vowel_fs4_flat30_minus12dbfs.wav");
    const auto output = argc >= 3 ? juce::File (argv[2])
        : root.getChildFile ("build/native-artist-au-validation.wav");
    const auto retuneMs = argc >= 4 ? juce::String (argv[3]).getFloatValue() : 0.0f;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<float> inputSamples;
    if (! loadStereoInput (formats, input, inputSamples))
        return 1;

    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_MusicEffect;
    description.componentSubType = static_cast<OSType> ('ATxL');
    description.componentManufacturer = static_cast<OSType> ('VST ');
    auto component = AudioComponentFindNext (nullptr, &description);
    if (component == nullptr)
    {
        std::cerr << "Auto-Tune Artist AU is not installed/visible to AudioComponentFindNext\n";
        return 2;
    }

    AudioUnit unit = nullptr;
    if (! require (AudioComponentInstanceNew (component, &unit), "AudioComponentInstanceNew"))
        return 3;
    const auto dispose = juce::ScopeGuard ([&] { AudioComponentInstanceDispose (unit); });

    const auto format = makeStereoFloatFormat();
    if (! require (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                         &format, sizeof (format)), "set input format")
        || ! require (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                            &format, sizeof (format)), "set output format")
        || ! require (AudioUnitSetProperty (unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                            &framesPerSlice, sizeof (framesPerSlice)), "set maximum frames per slice"))
        return 4;

    AudioStreamBasicDescription confirmedFormat {};
    UInt32 confirmedFormatSize = sizeof (confirmedFormat);
    if (! require (AudioUnitGetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                         &confirmedFormat, &confirmedFormatSize), "read output format"))
        return 4;
    std::cout << "output_format,sample_rate=" << confirmedFormat.mSampleRate
              << ",channels=" << confirmedFormat.mChannelsPerFrame
              << ",bytes_per_frame=" << confirmedFormat.mBytesPerFrame << '\n';

    InputSource source { &inputSamples, 0, static_cast<int64_t> (inputSamples.size() / channels) };
    AURenderCallbackStruct callback { supplyInput, &source };
    if (! require (AudioUnitSetProperty (unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                         &callback, sizeof (callback)), "set input callback")
        || ! configureReferencePreset (unit, retuneMs)
        || ! require (AudioUnitInitialize (unit), "AudioUnitInitialize"))
        return 5;
    const auto uninitialize = juce::ScopeGuard ([&] { AudioUnitUninitialize (unit); });

    const auto outputFrames = static_cast<size_t> (source.frameCount);
    std::vector<float> outputLeft (outputFrames, 0.0f);
    AudioBufferList outputBuffers {};
    outputBuffers.mNumberBuffers = 1;
    outputBuffers.mBuffers[0].mNumberChannels = 1;

    AudioTimeStamp time {};
    time.mFlags = kAudioTimeStampSampleTimeValid;
    for (int64_t frame = 0; frame < source.frameCount; frame += framesPerSlice)
    {
        const auto count = static_cast<UInt32> (std::min<int64_t> (framesPerSlice, source.frameCount - frame));
        outputBuffers.mBuffers[0].mData = outputLeft.data() + frame;
        outputBuffers.mBuffers[0].mDataByteSize = count * sizeof (float);
        time.mSampleTime = static_cast<Float64> (frame);
        AudioUnitRenderActionFlags flags = 0;
        if (! require (AudioUnitRender (unit, &flags, &time, 0, count, &outputBuffers), "AudioUnitRender"))
            return 6;
    }
    std::vector<float> outputSamples (inputSamples.size(), 0.0f);
    for (size_t frame = 0; frame < outputFrames; ++frame)
        outputSamples[frame * channels] = outputLeft[frame];
    if (! writeMonoWav (output, outputSamples))
        return 7;
    std::cout << "rendered," << output.getFullPathName() << ",retune_ms=" << retuneMs << '\n';
    return 0;
}
