cmake_minimum_required(VERSION 3.22)

project(VibeAutotuneProj VERSION 1.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Official Signalsmith sources, pinned for reproducible production builds.
# Both projects are header-only and MIT licensed; no runtime is packaged.
FetchContent_Declare(signalsmith_linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG 5668673560146a9cfe38c25315071e3fd68c8317
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(signalsmith_linear)

FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 57b93f4e9206a089a45387eaa39bdc9f310d3308
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(signalsmith_stretch)

add_subdirectory(JUCE_SDK)

juce_add_plugin(VibeAutotune
    COMPANY_NAME "VibeAudio"
    PLUGIN_MANUFACTURER_CODE VbAu
    PLUGIN_CODE Atun
    FORMATS VST3 AU
    PRODUCT_NAME "VibeAutotune"
)

target_sources(VibeAutotune PRIVATE
    VibeAutotune/PluginEditor.cpp
    VibeAutotune/PluginProcessor.cpp
)

target_compile_definitions(VibeAutotune PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(VibeAutotune PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    signalsmith-stretch
)

juce_generate_juce_header(VibeAutotune)

# Offline measurement only. It feeds generated signals through the existing
# processor and never changes the plug-in's real-time audio path.
add_executable(VibeAutotunePitchBaseline
    tests/PitchTrackingBaseline.cpp
)

target_link_libraries(VibeAutotunePitchBaseline PRIVATE
    VibeAutotune
    juce::juce_audio_utils
    juce::juce_audio_processors
    signalsmith-stretch
)

target_include_directories(VibeAutotunePitchBaseline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneCorrectionTrajectoryTest
    tests/CorrectionTrajectoryTest.cpp
)
target_include_directories(VibeAutotuneCorrectionTrajectoryTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneLiveControlPipelineTest
    tests/LiveControlPipelineTest.cpp
)
target_include_directories(VibeAutotuneLiveControlPipelineTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotunePitchContourExport
    tests/PitchContourExport.cpp
)
target_include_directories(VibeAutotunePitchContourExport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotunePitchContourExport PRIVATE
    VibeAutotune
    juce::juce_audio_formats
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneIndependentPitchContour
    tests/IndependentPitchContour.cpp
)
target_link_libraries(VibeAutotuneIndependentPitchContour PRIVATE
    juce::juce_audio_formats
)

add_executable(VibeAutotuneTDPSOLATest
    tests/TDPSOLARendererTest.cpp
)
target_include_directories(VibeAutotuneTDPSOLATest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneCausalTDPSOLATest
    tests/CausalTDPSOLATest.cpp
)

add_executable(VibeAutotuneRendererStepResponse
    tests/RendererStepResponse.cpp
)
target_include_directories(VibeAutotuneRendererStepResponse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneControlHandoffTest
    tests/ControlHandoffTest.cpp
)
target_include_directories(VibeAutotuneControlHandoffTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneControlHandoffTest PRIVATE
    VibeAutotune
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneCausalLiveWavTest
    tests/CausalLiveWavTest.cpp
)
target_include_directories(VibeAutotuneCausalLiveWavTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneCausalLiveWavTest PRIVATE
    juce::juce_audio_formats
)
target_include_directories(VibeAutotuneCausalTDPSOLATest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneSignalsmithRendererTest
    tests/SignalsmithPitchRendererTest.cpp
)
target_include_directories(VibeAutotuneSignalsmithRendererTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneSignalsmithRendererTest PRIVATE
    signalsmith-stretch
)

add_executable(VibeAutotuneEventOwnedSpectralFrameQueueTest
    tests/EventOwnedSpectralFrameQueueTest.cpp
)
target_include_directories(VibeAutotuneEventOwnedSpectralFrameQueueTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneBufferedSpectralPitchRendererTest
    tests/BufferedSpectralPitchRendererTest.cpp
)
target_include_directories(VibeAutotuneBufferedSpectralPitchRendererTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneBufferedSpectralPitchRendererTest PRIVATE
    juce::juce_dsp
)

add_executable(VibeAutotuneSignalsmithCalibrationTest
    tests/SignalsmithCalibrationTest.cpp
)
target_include_directories(VibeAutotuneSignalsmithCalibrationTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneSignalsmithCalibrationTest PRIVATE
    signalsmith-stretch
)

add_executable(VibeAutotuneEndToEndTest
    tests/EndToEndPipelineTest.cpp
)
target_include_directories(VibeAutotuneEndToEndTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneEndToEndTest PRIVATE
    VibeAutotune
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneRetunePolarityTest
    tests/RetunePolarityTest.cpp
)
target_include_directories(VibeAutotuneRetunePolarityTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneRetunePolarityTest PRIVATE
    VibeAutotune
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotunePitchDisplayTest
    tests/PitchDisplayTest.cpp
)
target_include_directories(VibeAutotunePitchDisplayTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotunePitchDisplayTest PRIVATE
    VibeAutotune
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneNotePolicyTest
    tests/NotePolicyTest.cpp
)
target_include_directories(VibeAutotuneNotePolicyTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneNotePolicyTest PRIVATE
    VibeAutotune
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneKeyScaleLivePathTest
    tests/KeyScaleLivePathTest.cpp
)
target_include_directories(VibeAutotuneKeyScaleLivePathTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneKeyScaleLivePathTest PRIVATE
    VibeAutotune
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneVocalDiagnostic
    tests/VocalPipelineDiagnostic.cpp
)
target_include_directories(VibeAutotuneVocalDiagnostic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneVocalDiagnostic PRIVATE
    VibeAutotune
    juce::juce_audio_formats
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneVocalHumanizeWavTest
    tests/VocalHumanizeWavTest.cpp
)
target_include_directories(VibeAutotuneVocalHumanizeWavTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneVocalHumanizeWavTest PRIVATE
    VibeAutotune
    juce::juce_audio_formats
    juce::juce_audio_processors
    signalsmith-stretch
)

add_executable(VibeAutotuneVocalControlWavTest
    tests/VocalControlWavTest.cpp
)

# Deterministic voiced-vowel fixtures for black-box Auto-Tune Artist control
# calibration.  The executable only writes input WAVs; it never changes DSP.
add_executable(VibeAutotuneArtistControlStimuli
    tests/ArtistControlStimuli.cpp
)
target_link_libraries(VibeAutotuneArtistControlStimuli PRIVATE
    juce::juce_audio_formats
)

# A local inspector for the user's installed Auto-Tune Artist AU. It is used
# before attempting automated black-box calibration renders.
add_executable(VibeAutotuneArtistAUInspector
    tests/AutoTuneArtistAUInspector.cpp
)
target_link_libraries(VibeAutotuneArtistAUInspector PRIVATE
    juce::juce_audio_processors
    "-framework CoreAudioKit"
)
target_compile_definitions(VibeAutotuneArtistAUInspector PRIVATE
    JUCE_PLUGINHOST_AU=1
)

# Offline batch host for the user's locally installed/licensed Auto-Tune
# Artist AU. It renders calibration fixtures only; no plugin code is copied.
add_executable(VibeAutotuneArtistBatchRender
    tests/AutoTuneArtistBatchRender.cpp
)
target_link_libraries(VibeAutotuneArtistBatchRender PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_formats
    "-framework CoreAudioKit"
)
target_compile_definitions(VibeAutotuneArtistBatchRender PRIVATE
    JUCE_PLUGINHOST_AU=1
)

# Native AUv2 offline host. Unlike the JUCE wrapper above, this drives the
# installed Artist component through AudioUnitRender with a real sample clock.
# It must reproduce the known Logic bounce before it is allowed to batch-test
# the controls.
add_executable(VibeAutotuneArtistNativeAURender
    tests/AutoTuneArtistNativeAURender.cpp
)
target_link_libraries(VibeAutotuneArtistNativeAURender PRIVATE
    juce::juce_audio_formats
    "-framework AudioToolbox"
    "-framework AudioUnit"
    "-framework CoreAudio"
)

add_executable(VibeAutotuneSignalChainDiagnostic
    tests/SignalChainDiagnostic.cpp
)
target_include_directories(VibeAutotuneSignalChainDiagnostic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneSignalChainDiagnostic PRIVATE
    VibeAutotune
    juce::juce_audio_formats
    juce::juce_audio_processors
    signalsmith-stretch
)

# Read-only audit of production signal-chain traces. It makes transition
# ownership/takeover timing a repeatable multi-fixture measurement before any
# renderer experiment is allowed to change OLA behaviour.
add_executable(VibeAutotuneEventHandoffAudit
    tests/EventHandoffAudit.cpp
)

# Shadow-only feasibility measurement for the researched target-aware cycle
# scheduler direction.  It cannot affect the plug-in audio path.
add_executable(VibeAutotuneTargetAwareCycleSchedulerAudit
    tests/TargetAwareCycleSchedulerAudit.cpp
)

add_executable(VibeAutotuneTargetEventOwnershipAudit
    tests/TargetEventOwnershipAudit.cpp
)

# Offline golden-reference scorer. This does not participate in the plug-in
# build; it makes renderer iterations reproducible against a target WAV.
add_executable(VibeAutotuneTargetReferenceMatch
    tests/TargetReferenceMatch.cpp
)
target_link_libraries(VibeAutotuneTargetReferenceMatch PRIVATE
    juce::juce_audio_formats
    juce::juce_dsp
)

add_executable(VibeAutotuneArtistContourRendererDiagnostic
    tests/ArtistContourRendererDiagnostic.cpp
)
target_include_directories(VibeAutotuneArtistContourRendererDiagnostic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneArtistContourRendererDiagnostic PRIVATE
    juce::juce_audio_formats
)

add_executable(VibeAutotuneSignalsmithArtistContourDiagnostic
    tests/SignalsmithArtistContourDiagnostic.cpp
)
target_link_libraries(VibeAutotuneSignalsmithArtistContourDiagnostic PRIVATE
    juce::juce_audio_formats
    signalsmith-stretch
)

add_executable(VibeAutotuneNoteTransitionAudit
    tests/NoteTransitionAudit.cpp
)
target_include_directories(VibeAutotuneNoteTransitionAudit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)

add_executable(VibeAutotuneTuningModePolicyTest
    tests/TuningModePolicyTest.cpp
)
target_include_directories(VibeAutotuneTuningModePolicyTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_include_directories(VibeAutotuneVocalControlWavTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/VibeAutotune
)
target_link_libraries(VibeAutotuneVocalControlWavTest PRIVATE
    VibeAutotune
    juce::juce_audio_formats
    juce::juce_audio_processors
    signalsmith-stretch
)
