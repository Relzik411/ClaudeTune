cmake_minimum_required(VERSION 3.22)

project(ClaudeTune VERSION 1.9.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if CLAUDETUNE_JUCE_DIR is given, otherwise fetch it.
# ---------------------------------------------------------------------------
set(CLAUDETUNE_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(CLAUDETUNE_JUCE_DIR)
    add_subdirectory(${CLAUDETUNE_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
if(APPLE)
    set(CLAUDETUNE_FORMATS VST3 AU Standalone)
elseif(WIN32)
    set(CLAUDETUNE_FORMATS VST3 Standalone)
else()
    set(CLAUDETUNE_FORMATS VST3)
endif()

option(CLAUDETUNE_COPY_AFTER_BUILD "Install the plug-in into the user plug-in folders after building" OFF)

juce_add_plugin(ClaudeTune
    COMPANY_NAME "ClaudeTune"
    BUNDLE_ID "com.claudetune.ClaudeTune"
    PLUGIN_MANUFACTURER_CODE Cltn
    PLUGIN_CODE CTun
    FORMATS ${CLAUDETUNE_FORMATS}
    PRODUCT_NAME "ClaudeTune"
    DESCRIPTION "Real-time vocal pitch correction"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Fx Pitch Shift
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    COPY_PLUGIN_AFTER_BUILD ${CLAUDETUNE_COPY_AFTER_BUILD})

target_sources(ClaudeTune PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UI/NoteEditor.cpp
    Source/Engine/TuneEngine.cpp
    Source/Edit/PitchAnalysis.cpp
    Source/Edit/NoteSegmenter.cpp
    Source/Edit/EditModel.cpp
    Source/Edit/Renderer.cpp
    Source/Edit/EditSession.cpp
    Source/SessionCodec.cpp)

target_include_directories(ClaudeTune PRIVATE Source)

target_compile_definitions(ClaudeTune PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ClaudeTune
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Engine unit tests (no JUCE needed)
# ---------------------------------------------------------------------------
option(CLAUDETUNE_BUILD_TESTS "Build the engine test program" ON)
if(CLAUDETUNE_BUILD_TESTS)
    add_executable(ClaudeTuneEngineTests Tests/EngineTests.cpp Source/Engine/TuneEngine.cpp)
    target_include_directories(ClaudeTuneEngineTests PRIVATE Source)

    add_executable(ClaudeTuneEditTests
        Tests/EditTests.cpp
        Source/Edit/PitchAnalysis.cpp
        Source/Edit/NoteSegmenter.cpp
        Source/Edit/EditModel.cpp
        Source/Edit/Renderer.cpp)
    target_include_directories(ClaudeTuneEditTests PRIVATE Source Tests)

    add_executable(ClaudeTuneSessionTests
        Tests/SessionTests.cpp
        Source/Edit/PitchAnalysis.cpp
        Source/Edit/NoteSegmenter.cpp
        Source/Edit/EditModel.cpp
        Source/Edit/Renderer.cpp
        Source/Edit/EditSession.cpp)
    target_include_directories(ClaudeTuneSessionTests PRIVATE Source Tests)
    find_package(Threads REQUIRED)
    target_link_libraries(ClaudeTuneSessionTests PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Headless UI snapshot tool (development only)
# ---------------------------------------------------------------------------
option(CLAUDETUNE_BUILD_SNAPSHOT "Build the UI snapshot tool" OFF)
if(CLAUDETUNE_BUILD_SNAPSHOT)
    juce_add_console_app(ClaudeTuneSnapshot PRODUCT_NAME "ClaudeTuneSnapshot")
    target_sources(ClaudeTuneSnapshot PRIVATE
        Tools/UISnapshot.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/UI/NoteEditor.cpp
        Source/Engine/TuneEngine.cpp
        Source/Edit/PitchAnalysis.cpp
        Source/Edit/NoteSegmenter.cpp
        Source/Edit/EditModel.cpp
        Source/Edit/Renderer.cpp
        Source/Edit/EditSession.cpp
        Source/SessionCodec.cpp)
    target_include_directories(ClaudeTuneSnapshot PRIVATE Source Tests)
    target_compile_definitions(ClaudeTuneSnapshot PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="ClaudeTune" JucePlugin_IsSynth=0)
    target_link_libraries(ClaudeTuneSnapshot PRIVATE
        juce::juce_audio_utils juce::juce_recommended_config_flags)

    juce_add_console_app(ClaudeTunePluginTest PRODUCT_NAME "ClaudeTunePluginTest")
    target_sources(ClaudeTunePluginTest PRIVATE
        Tools/PluginTest.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/UI/NoteEditor.cpp
        Source/Engine/TuneEngine.cpp
        Source/Edit/PitchAnalysis.cpp
        Source/Edit/NoteSegmenter.cpp
        Source/Edit/EditModel.cpp
        Source/Edit/Renderer.cpp
        Source/Edit/EditSession.cpp
        Source/SessionCodec.cpp)
    target_include_directories(ClaudeTunePluginTest PRIVATE Source Tests)
    target_compile_definitions(ClaudeTunePluginTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="ClaudeTune" JucePlugin_IsSynth=0)
    target_link_libraries(ClaudeTunePluginTest PRIVATE
        juce::juce_audio_utils juce::juce_recommended_config_flags)

    juce_add_console_app(ClaudeTuneEditorTest PRODUCT_NAME "ClaudeTuneEditorTest")
    target_sources(ClaudeTuneEditorTest PRIVATE
        Tools/EditorTest.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/UI/NoteEditor.cpp
        Source/Engine/TuneEngine.cpp
        Source/Edit/PitchAnalysis.cpp
        Source/Edit/NoteSegmenter.cpp
        Source/Edit/EditModel.cpp
        Source/Edit/Renderer.cpp
        Source/Edit/EditSession.cpp
        Source/SessionCodec.cpp)
    target_include_directories(ClaudeTuneEditorTest PRIVATE Source Tests)
    target_compile_definitions(ClaudeTuneEditorTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="ClaudeTune" JucePlugin_IsSynth=0)
    target_link_libraries(ClaudeTuneEditorTest PRIVATE
        juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()

# ---------------------------------------------------------------------------
# VST3 host test (development only): loads the built plug-in and exercises it
# ---------------------------------------------------------------------------
option(CLAUDETUNE_BUILD_HOSTTEST "Build the VST3 host test tool" OFF)
if(CLAUDETUNE_BUILD_HOSTTEST)
    juce_add_console_app(ClaudeTuneHostTest PRODUCT_NAME "ClaudeTuneHostTest")
    target_sources(ClaudeTuneHostTest PRIVATE Tools/HostTest.cpp)
    target_compile_definitions(ClaudeTuneHostTest PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ClaudeTuneHostTest PRIVATE
        juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)
endif()
