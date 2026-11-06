cmake_minimum_required(VERSION 3.24)

if(APPLE AND (NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL ""))
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

# A fixed deployment floor avoids inheriting the build machine's macOS version.
# macOS 11 is the first native Apple Silicon release; both slices use this floor.
if(APPLE AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum supported macOS version" FORCE)
endif()

project(Synthax1
    VERSION 0.1.0
    DESCRIPTION "Synthax1 lab instrument"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SYNTHAX1_ENABLE_TESTS "Build synthax1 tests" ON)
option(SYNTHAX1_ENABLE_COPY_AFTER_BUILD "Copy plugin bundles after build" OFF)
option(SYNTHAX1_ENABLE_ASAN "Build with AddressSanitizer enabled" OFF)
option(SYNTHAX1_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer enabled" OFF)
set(SYNTHAX1_JUCE_GIT_TAG "8.0.13" CACHE STRING "JUCE git tag used when SYNTHAX1_JUCE_PATH is not set")
set(SYNTHAX1_JUCE_PATH "" CACHE PATH "Optional local JUCE checkout")

if(SYNTHAX1_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(SYNTHAX1_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

if(SYNTHAX1_JUCE_PATH)
    add_subdirectory("${SYNTHAX1_JUCE_PATH}" juce)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${SYNTHAX1_JUCE_GIT_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(SYNTHAX1_COMMON_SOURCES
    src/dsp/Arpeggiator.cpp
    src/dsp/Arpeggiator.h
    src/dsp/Envelope.cpp
    src/dsp/Envelope.h
    src/dsp/Filter.cpp
    src/dsp/Filter.h
    src/dsp/Lfo.cpp
    src/dsp/Lfo.h
    src/dsp/OscillatorStack.cpp
    src/dsp/OscillatorStack.h
    src/dsp/Ramp.cpp
    src/dsp/Ramp.h
    src/dsp/SynthEngine.cpp
    src/dsp/SynthEngine.h
    src/dsp/SynthParameters.h
    src/dsp/fx/FxChain.cpp
    src/dsp/fx/FxChain.h
    src/midi/MidiControllerMap.cpp
    src/midi/MidiControllerMap.h
    src/modulation/ModulationRouteModel.cpp
    src/modulation/ModulationRouteModel.h
    src/plugin/ParameterRegistry.cpp
    src/plugin/ParameterRegistry.h
    src/presets/PresetLibrary.cpp
    src/presets/PresetLibrary.h
    src/presets/PresetManager.cpp
    src/presets/PresetManager.h
    src/presets/PresetValidator.cpp
    src/presets/PresetValidator.h
    src/legacy/LegacyFields.inc
    src/legacy/LegacyFormat.cpp
    src/legacy/LegacyInit.inc
    src/legacy/LegacyFormat.h
    src/legacy/LegacyImport.cpp
    src/legacy/LegacyImport.h
    src/legacy/LegacyPatchMapper.cpp
    src/legacy/LegacyPatchMapper.h
    src/voice/Voice.cpp
    src/voice/Voice.h
    src/voice/VoiceAllocator.cpp
    src/voice/VoiceAllocator.h)

juce_add_plugin(Synthax1Plugin
    COMPANY_NAME "BandPass Records"
    BUNDLE_ID "com.bandpassrecords.synthax1"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${SYNTHAX1_ENABLE_COPY_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE BpRc
    PLUGIN_CODE Synx
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Synthax1")

target_sources(Synthax1Plugin
    PRIVATE
        ${SYNTHAX1_COMMON_SOURCES}
        src/plugin/PluginEditor.cpp
        src/plugin/PluginEditor.h
        src/plugin/PresetBrowser.cpp
        src/plugin/PresetBrowser.h
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginProcessor.h)

if(APPLE)
    foreach(SYNTHAX1_PLUGIN_FORMAT_TARGET IN ITEMS Synthax1Plugin_AU Synthax1Plugin_VST3 Synthax1Plugin_Standalone)
        if(TARGET ${SYNTHAX1_PLUGIN_FORMAT_TARGET})
            juce_add_bundle_resources_directory(${SYNTHAX1_PLUGIN_FORMAT_TARGET} "${CMAKE_CURRENT_SOURCE_DIR}/presets/factory")
        endif()
    endforeach()
else()
    # Windows and Linux build no AU. Factory presets go inside the VST3 bundle
    # (Synthax1.vst3/Contents/Resources/factory) and next to the standalone executable,
    # where bundleFactoryPresetDirectory() looks for them.
    add_custom_command(TARGET Synthax1Plugin_VST3 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/presets/factory"
                "$<TARGET_FILE_DIR:Synthax1Plugin_VST3>/../Resources/factory"
        VERBATIM)
    add_custom_command(TARGET Synthax1Plugin_Standalone POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/presets/factory"
                "$<TARGET_FILE_DIR:Synthax1Plugin_Standalone>/factory"
        VERBATIM)
endif()

target_compile_definitions(Synthax1Plugin
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")

target_link_libraries(Synthax1Plugin
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

juce_generate_juce_header(Synthax1Plugin)

add_executable(Synthax1Render
    ${SYNTHAX1_COMMON_SOURCES}
    src/validation/SynthRender.cpp)

target_link_libraries(Synthax1Render
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

target_compile_definitions(Synthax1Render
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
        SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")

# Offline host that renders the installed the reference synth for docs/SOUND_PARITY.md. A developer
# tool: it hosts VST3 plug-ins, so that is enabled for this target only.
option(SYNTHAX1_BUILD_REFERENCE_HOST "Build the the reference synth reference render host" ON)
if(SYNTHAX1_BUILD_REFERENCE_HOST)
    juce_add_console_app(LegacyRenderHost PRODUCT_NAME "LegacyRenderHost")
    target_sources(LegacyRenderHost
        PRIVATE
            tools/legacy_host/LegacyRenderHost.cpp
            src/legacy/LegacyFormat.cpp
            src/legacy/LegacyFormat.h)
    target_compile_definitions(LegacyRenderHost
        PRIVATE
            JUCE_PLUGINHOST_VST3=1
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
    target_link_libraries(LegacyRenderHost
        PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_processors
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

if(SYNTHAX1_ENABLE_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(Synthax1NativeProcessorTest tests/smoke/NativeProcessorTest.cpp)
    target_include_directories(Synthax1NativeProcessorTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            $<TARGET_PROPERTY:Synthax1Plugin,JUCE_GENERATED_SOURCES_DIRECTORY>)
    target_link_libraries(Synthax1NativeProcessorTest
        PRIVATE
            Synthax1Plugin
            juce::juce_audio_utils
            juce::juce_dsp)
    add_test(NAME Synthax1NativeProcessorTest COMMAND Synthax1NativeProcessorTest)
    set_tests_properties(Synthax1NativeProcessorTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Synthax1ReferenceComparisonTest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/ReferenceComparisonTest.py")
    set_tests_properties(Synthax1ReferenceComparisonTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(Synthax1SmokeTest
        ${SYNTHAX1_COMMON_SOURCES}
        tests/smoke/SynthSmokeTest.cpp)

    target_link_libraries(Synthax1SmokeTest
        PRIVATE
            juce::juce_core
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    target_compile_definitions(Synthax1SmokeTest
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
            SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")

    add_test(NAME Synthax1SmokeTest COMMAND Synthax1SmokeTest)
    set_tests_properties(Synthax1SmokeTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(Synthax1ContractTest
        ${SYNTHAX1_COMMON_SOURCES}
        tests/smoke/SynthContractTest.cpp)

    target_link_libraries(Synthax1ContractTest
        PRIVATE
            juce::juce_core
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    target_compile_definitions(Synthax1ContractTest
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
            SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")

    add_test(NAME Synthax1ContractTest COMMAND Synthax1ContractTest)
    set_tests_properties(Synthax1ContractTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(Synthax1VoiceCoreTest
        ${SYNTHAX1_COMMON_SOURCES}
        tests/smoke/SynthVoiceCoreTest.cpp)

    target_link_libraries(Synthax1VoiceCoreTest
        PRIVATE
            juce::juce_core
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    target_compile_definitions(Synthax1VoiceCoreTest
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
            SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")

    add_test(NAME Synthax1VoiceCoreTest COMMAND Synthax1VoiceCoreTest)
    set_tests_properties(Synthax1VoiceCoreTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(Synthax1DspCoreTest
        ${SYNTHAX1_COMMON_SOURCES}
        tests/smoke/SynthDspCoreTest.cpp)

    target_link_libraries(Synthax1DspCoreTest
        PRIVATE
            juce::juce_core
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    target_compile_definitions(Synthax1DspCoreTest
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
            SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")

    add_test(NAME Synthax1DspCoreTest COMMAND Synthax1DspCoreTest)
    set_tests_properties(Synthax1DspCoreTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    # The reference synth interop. Format/mapper tests are JUCE-free; pass a folder of real
    # .fxp/.fxb files with -DSYNTHAX1_LEGACY_SAMPLES=<dir> to round-trip them too.
    set(SYNTHAX1_LEGACY_SAMPLES "" CACHE PATH "Optional folder of the reference synth .fxp/.fxb files for round-trip tests")
    add_executable(LegacyFormatTest src/legacy/LegacyFormat.cpp tests/legacy/LegacyFormatTest.cpp)
    add_test(NAME LegacyFormatTest COMMAND LegacyFormatTest ${SYNTHAX1_LEGACY_SAMPLES})
    add_executable(LegacyMapperTest src/legacy/LegacyFormat.cpp src/legacy/LegacyPatchMapper.cpp tests/legacy/LegacyMapperTest.cpp)
    add_test(NAME LegacyMapperTest COMMAND LegacyMapperTest)

    add_executable(LegacyImportTest ${SYNTHAX1_COMMON_SOURCES} tests/legacy/LegacyImportTest.cpp)
    target_link_libraries(LegacyImportTest
        PRIVATE
            juce::juce_core
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    target_compile_definitions(LegacyImportTest
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1
            SYNTHAX1_PROJECT_VERSION="${PROJECT_VERSION}")
    add_test(NAME LegacyImportTest COMMAND LegacyImportTest ${SYNTHAX1_LEGACY_SAMPLES})

    add_test(NAME Synthax1RenderCoreSuite
        COMMAND Synthax1Render --suite core --output-dir "${CMAKE_CURRENT_BINARY_DIR}/reports/ctest-core")
    set_tests_properties(Synthax1RenderCoreSuite PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Synthax1RandomizeRenderTest
        COMMAND Synthax1Render --randomize-test --output "${CMAKE_CURRENT_BINARY_DIR}/reports/ctest-randomize.json")
    set_tests_properties(Synthax1RandomizeRenderTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Synthax1ModulationRouteRenderTest
        COMMAND Synthax1Render --modulation-route-render-test --output "${CMAKE_CURRENT_BINARY_DIR}/reports/ctest-modulation-route-render.json")
    set_tests_properties(Synthax1ModulationRouteRenderTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Synthax1OfflineRealtimeCompareTest
        COMMAND Synthax1Render --offline-realtime-compare-test --output "${CMAKE_CURRENT_BINARY_DIR}/reports/ctest-offline-realtime-compare.json")
    set_tests_properties(Synthax1OfflineRealtimeCompareTest PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Synthax1PatchRecreationSuite
        COMMAND Synthax1Render --suite patch-recreation --output-dir "${CMAKE_CURRENT_BINARY_DIR}/reports/ctest-patch-recreation")
    set_tests_properties(Synthax1PatchRecreationSuite PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
