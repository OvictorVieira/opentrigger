cmake_minimum_required(VERSION 3.22)
project(OpenTrigger VERSION 0.1.0 LANGUAGES C CXX)
set(OT_VERSION_LABEL "0.1.0-beta.1" CACHE STRING "Release label used by the installers")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

include(FetchContent)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(juce)

if(WIN32)
    # no system zlib on Windows: build the reference one
    FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.1 GIT_SHALLOW ON)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
else()
    find_package(ZLIB REQUIRED)
endif()

# Format + engine, no GUI: shared by the plugin and the tests.
add_library(ot_core STATIC src/third_party/monocypher/monocypher.c src/Crypt.cpp src/Otci.cpp src/Pack.cpp src/Engine.cpp src/Library.cpp src/Draft.cpp src/SmartTrigger.cpp)
target_include_directories(ot_core PUBLIC src)
target_link_libraries(ot_core PUBLIC
    juce::juce_core juce::juce_cryptography juce::juce_audio_basics juce::juce_audio_formats juce::juce_graphics
    ZLIB::ZLIB
    PRIVATE juce::juce_recommended_config_flags)
target_compile_definitions(ot_core PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)

# AAX (Pro Tools): needs Avid's AAX SDK (free developer account + license) and PACE/Avid signing, neither of which can be
# bundled. Configure with  -DOT_AAX_SDK_PATH=/path/to/aax-sdk  to add the AAX format.
set(OT_FORMATS VST3 AU Standalone)
if(OT_AAX_SDK_PATH)
    juce_set_aax_sdk_path("${OT_AAX_SDK_PATH}")
    list(APPEND OT_FORMATS AAX)
endif()

juce_add_plugin(OpenTrigger
    COMPANY_NAME "OpenTrigger"
    PLUGIN_MANUFACTURER_CODE Otrg
    PLUGIN_CODE Otr1
    FORMATS ${OT_FORMATS}
    AAX_IDENTIFIER com.opentrigger.OpenTrigger
    PRODUCT_NAME "OpenTrigger"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx)
target_sources(OpenTrigger PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp src/EditorPanel.cpp src/UiKit.cpp src/SlotUi.cpp src/SmartUi.cpp)
target_link_libraries(OpenTrigger PRIVATE ot_core
    juce::juce_audio_utils juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_definitions(OpenTrigger PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0)

# The same plug-in with all 8 stereo outputs (16 channels) on from the start, so a host maps them to the track by itself.
# Hosts such as REAPER list a multi-output instrument as "(16 out)" and route every output to the track on insert, which they
# do not do for an effect, so this one is declared an instrument (it still reads the track audio on its input).
# It is a separate plug-in in the host's list ("OpenTrigger 16out"); projects, presets and settings are interchangeable.
juce_add_plugin(OpenTrigger16
    COMPANY_NAME "OpenTrigger"
    PLUGIN_MANUFACTURER_CODE Otrg
    PLUGIN_CODE Otr2
    FORMATS VST3 AU
    PRODUCT_NAME "OpenTrigger 16out"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    VST3_CATEGORIES Instrument)
target_sources(OpenTrigger16 PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp src/EditorPanel.cpp src/UiKit.cpp src/SlotUi.cpp src/SmartUi.cpp)
target_link_libraries(OpenTrigger16 PRIVATE ot_core
    juce::juce_audio_utils juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_definitions(OpenTrigger16 PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0 OT_ALL_OUTPUTS=1)

juce_add_console_app(ot_pack PRODUCT_NAME "ot_pack")
target_sources(ot_pack PRIVATE tools/PackCli.cpp)
target_link_libraries(ot_pack PRIVATE ot_core juce::juce_recommended_config_flags)

juce_add_console_app(ot_tests PRODUCT_NAME "ot_tests")
target_sources(ot_tests PRIVATE tests/Tests.cpp)
target_link_libraries(ot_tests PRIVATE ot_core juce::juce_recommended_config_flags)

juce_add_console_app(ot_loadcheck PRODUCT_NAME "ot_loadcheck")
target_sources(ot_loadcheck PRIVATE tests/LoadCheck.cpp)
target_link_libraries(ot_loadcheck PRIVATE ot_core juce::juce_recommended_config_flags)


juce_add_console_app(ot_latency PRODUCT_NAME "ot_latency")
target_sources(ot_latency PRIVATE tests/Latency.cpp src/PluginProcessor.cpp src/PluginEditor.cpp src/EditorPanel.cpp src/UiKit.cpp src/SlotUi.cpp src/SmartUi.cpp)
target_link_libraries(ot_latency PRIVATE ot_core juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra juce::juce_recommended_config_flags)

juce_add_console_app(ot_bench PRODUCT_NAME "ot_bench")
target_sources(ot_bench PRIVATE tests/Bench.cpp src/PluginProcessor.cpp src/PluginEditor.cpp src/EditorPanel.cpp src/UiKit.cpp src/SlotUi.cpp src/SmartUi.cpp)
target_link_libraries(ot_bench PRIVATE ot_core juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra juce::juce_recommended_config_flags)

juce_add_console_app(ot_guitest PRODUCT_NAME "ot_guitest")
target_sources(ot_guitest PRIVATE tests/GuiTest.cpp src/PluginProcessor.cpp src/PluginEditor.cpp src/EditorPanel.cpp src/UiKit.cpp src/SlotUi.cpp src/SmartUi.cpp)
target_link_libraries(ot_guitest PRIVATE ot_core juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra juce::juce_recommended_config_flags)

juce_add_console_app(ot_hosttest PRODUCT_NAME "ot_hosttest")
target_sources(ot_hosttest PRIVATE tests/HostTest.cpp)
target_compile_definitions(ot_hosttest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1)
target_link_libraries(ot_hosttest PRIVATE ot_core juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)
if(APPLE)
    target_link_libraries(ot_hosttest PRIVATE "-framework AudioUnit" "-framework CoreAudioKit" "-framework AudioToolbox")
endif()
