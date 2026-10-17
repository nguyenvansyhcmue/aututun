cmake_minimum_required(VERSION 3.22)

project(VibeAutotune VERSION 1.0.8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG origin/master
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(VibeAutotune
    COMPANY_NAME "VibeAudio"
    PLUGIN_MANUFACTURER_CODE VbAu
    PLUGIN_CODE Atun
    FORMATS VST3 AU
    PRODUCT_NAME "VibeAutotune"
)

target_sources(VibeAutotune PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
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
)

juce_generate_juce_header(VibeAutotune)
