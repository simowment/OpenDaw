cmake_minimum_required(VERSION 3.22)
project(OpenDaw VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Version from git tag ─────────────────────────────────────────────────────
# Runs early so the parsed version is available for JUCE's exe metadata.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OpenDaw_GIT_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_RESULT)
    if(NOT GIT_DESCRIBE_RESULT EQUAL 0)
        set(OpenDaw_GIT_RAW "dev")
    endif()
else()
    set(OpenDaw_GIT_RAW "dev")
endif()

# Strip leading "v" for display (v1.0.003 -> 1.0.003)
string(REGEX REPLACE "^v" "" OpenDaw_DISPLAY_VERSION "${OpenDaw_GIT_RAW}")

# Extract numeric major.minor.patch for Windows exe metadata
string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ver_match "${OpenDaw_GIT_RAW}")
if(_ver_match)
    math(EXPR _major "${CMAKE_MATCH_1}")
    math(EXPR _minor "${CMAKE_MATCH_2}")
    math(EXPR _patch "${CMAKE_MATCH_3}")
    set(OpenDaw_NUMERIC_VERSION "${_major}.${_minor}.${_patch}")
else()
    set(OpenDaw_NUMERIC_VERSION "0.0.0")
endif()

message(STATUS "OpenDaw display version : ${OpenDaw_DISPLAY_VERSION}")
message(STATUS "OpenDaw numeric version : ${OpenDaw_NUMERIC_VERSION}")

# ── Qt6 ──────────────────────────────────────────────────────────────────────
set(CMAKE_PREFIX_PATH "c:/qt/6.10.2/msvc2022_64" CACHE STRING "Qt prefix path")
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent PrintSupport)
qt_standard_project_setup()

# ── FFmpeg ────────────────────────────────────────────────────────────────────
# On Windows: vcpkg provides FindFFMPEG.cmake via toolchain file.
# On Linux/macOS: system packages provide pkg-config .pc files.
if(WIN32)
    find_package(FFMPEG REQUIRED)
    set(FFMPEG_LINK_TARGET ${FFMPEG_LIBRARIES})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    set(FFMPEG_LINK_TARGET PkgConfig::FFMPEG)
endif()

# ── JUCE ─────────────────────────────────────────────────────────────────────
set(JUCE_MODULES_ONLY OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/JUCE)

# ── Tracktion Engine (detects JUCE automatically) ────────────────────────────
set(TE_ADD_EXAMPLES OFF CACHE BOOL "" FORCE)
# ── Local patches to the Tracktion submodule ────────────────────────────────
# Applied in place at configure time (idempotent: skipped once applied), so
# the submodule stays pinned to its upstream commit.
foreach(_patch ${CMAKE_SOURCE_DIR}/patches/tracktion_engine-live-midi-without-clips.patch)
    execute_process(COMMAND git apply --reverse --check "${_patch}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libs/tracktion_engine
        RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already EQUAL 0)
        execute_process(COMMAND git apply "${_patch}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libs/tracktion_engine
            RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "Could not apply ${_patch} to libs/tracktion_engine")
        endif()
        message(STATUS "Applied ${_patch}")
    endif()
endforeach()

add_subdirectory(libs/tracktion_engine)

# ── Compile definitions shared by everything that compiles engine code ──────
set(OPENDAW_DEFS
    JUCE_PLUGINHOST_VST=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    DONT_SET_USING_JUCE_NAMESPACE=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1)

# ── Core library: engine, AI tools and the agent API (no widgets) ────────────
# Linked by the app, the tests and anything else that drives the engine. The
# JUCE/Tracktion module code is compiled once, here.
set(OpenDawCore_SOURCES
    src/app/JuceQtBridge.h          src/app/JuceQtBridge.cpp
    src/engine/AudioEngine.h        src/engine/AudioEngine.cpp
    src/engine/LoudnessMeter.h      src/engine/LoudnessMeter.cpp
    src/engine/LooperPlugin.h       src/engine/LooperPlugin.cpp
    src/engine/LooperEngine.h       src/engine/LooperEngine.cpp
    src/engine/instruments/SoundFontInstrument.h src/engine/instruments/SoundFontInstrument.cpp
    src/engine/instruments/BundledPlugins.h src/engine/instruments/BundledPlugins.cpp
    src/engine/EditManager.h        src/engine/EditManager.cpp
    src/engine/PluginScanner.h      src/engine/PluginScanner.cpp
    src/engine/PatternManager.h     src/engine/PatternManager.cpp
    src/engine/AudioFileAnalyzer.h  src/engine/AudioFileAnalyzer.cpp
    src/utils/WaveformCache.h       src/utils/WaveformCache.cpp
    src/utils/ThemeManager.h        src/utils/ThemeManager.cpp
    src/utils/FFmpegUtils.h         src/utils/FFmpegUtils.cpp
    src/ai/AiTypes.h
    src/ai/AiIds.h
    src/ai/AiToolExtensions.h
    src/ai/AiSchemaValidator.h     src/ai/AiSchemaValidator.cpp
    src/ai/AiAudioAnalysis.h      src/ai/AiAudioAnalysis.cpp
    src/ai/AiToolDefs.h            src/ai/AiToolDefs.cpp
    src/ai/AiToolExecutor.h        src/ai/AiToolExecutor.cpp
    src/ai/AiService.h             src/ai/AiService.cpp
    src/ai/AiChatStore.h           src/ai/AiChatStore.cpp
    src/ai/AiHandSplitter.h        src/ai/AiHandSplitter.cpp
    src/api/RenderService.h        src/api/RenderService.cpp
    src/api/PatternTools.h         src/api/PatternTools.cpp
    src/api/CommandGateway.h       src/api/CommandGateway.cpp
    src/api/LocalApiServer.h       src/api/LocalApiServer.cpp
)

add_library(OpenDawCore STATIC ${OpenDawCore_SOURCES})
target_compile_definitions(OpenDawCore
    PUBLIC  ${OPENDAW_DEFS} OpenDaw_VERSION="${OpenDaw_DISPLAY_VERSION}"
    PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_include_directories(OpenDawCore PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/libs/vst2sdk
    ${CMAKE_SOURCE_DIR}/libs/tinysoundfont ${FFMPEG_INCLUDE_DIRS})
target_link_directories(OpenDawCore PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(OpenDawCore
    PUBLIC
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent
        ${FFMPEG_LINK_TARGET}
        $<$<PLATFORM_ID:Windows>:winmm>
    PRIVATE
        tracktion::tracktion_core
        tracktion::tracktion_engine
        tracktion::tracktion_graph
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_events
        juce::juce_recommended_warning_flags)
if(MSVC)
    target_compile_options(OpenDawCore PRIVATE /EHsc /bigobj)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(OpenDawCore PRIVATE -Wno-unknown-pragmas)
endif()

# Consumers see the module headers and the exact macro set the modules were
# compiled with, without compiling the modules again (JUCE's own pattern for
# plugin wrappers: linking the modules publicly would duplicate them).
function(opendaw_use_core tgt)
    target_link_libraries(${tgt} PRIVATE OpenDawCore)
    target_include_directories(${tgt} PRIVATE
        $<TARGET_GENEX_EVAL:OpenDawCore,$<TARGET_PROPERTY:OpenDawCore,INCLUDE_DIRECTORIES>>)
    target_compile_definitions(${tgt} PRIVATE
        $<TARGET_GENEX_EVAL:OpenDawCore,$<TARGET_PROPERTY:OpenDawCore,COMPILE_DEFINITIONS>>)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /EHsc /bigobj)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(${tgt} PRIVATE -Wno-unknown-pragmas)
    endif()
endfunction()

# ── GUI sources ──────────────────────────────────────────────────────────────
set(OpenDaw_SOURCES
    src/main.cpp
    src/app/OpenDawApplication.h    src/app/OpenDawApplication.cpp
    src/ui/MainWindow.h             src/ui/MainWindow.cpp
    src/ui/TypingKeyboard.h         src/ui/TypingKeyboard.cpp
    src/ui/timeline/TimelineView.h  src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimeRuler.h     src/ui/timeline/TimeRuler.cpp
    src/ui/timeline/TrackLane.h     src/ui/timeline/TrackLane.cpp
    src/ui/timeline/ClipItem.h      src/ui/timeline/ClipItem.cpp
    src/ui/timeline/GridSnapper.h   src/ui/timeline/GridSnapper.cpp
    src/ui/timeline/TrackHeaderWidget.h src/ui/timeline/TrackHeaderWidget.cpp
    src/ui/timeline/EnvelopeUtils.h
    src/ui/timeline/AutomationLaneItem.h  src/ui/timeline/AutomationLaneItem.cpp
    src/ui/timeline/AutomationPointItem.h src/ui/timeline/AutomationPointItem.cpp
    src/ui/timeline/AutomationLaneHeader.h src/ui/timeline/AutomationLaneHeader.cpp
    src/ui/timeline/TakeLanes.h src/ui/timeline/TakeLanes.cpp
    src/ui/timeline/RecordingPreview.h src/ui/timeline/RecordingPreview.cpp
    src/ui/timeline/EditTool.h src/ui/timeline/EditTool.cpp
    src/ui/timeline/MarkerTempoLane.h     src/ui/timeline/MarkerTempoLane.cpp
    src/ui/timeline/FolderHeaderWidget.h  src/ui/timeline/FolderHeaderWidget.cpp
    src/ui/mixer/MixerView.h        src/ui/mixer/MixerView.cpp
    src/ui/mixer/ChannelStrip.h     src/ui/mixer/ChannelStrip.cpp
    src/ui/mixer/SendsPanel.h       src/ui/mixer/SendsPanel.cpp
    src/ui/mixer/LoudnessPanel.h    src/ui/mixer/LoudnessPanel.cpp
    src/ui/looper/LooperWindow.h    src/ui/looper/LooperWindow.cpp
    src/ui/transport/TransportBar.h src/ui/transport/TransportBar.cpp
    src/ui/controls/RotaryKnob.h    src/ui/controls/RotaryKnob.cpp
    src/ui/controls/VolumeFader.h   src/ui/controls/VolumeFader.cpp
    src/ui/controls/LevelMeter.h    src/ui/controls/LevelMeter.cpp
    src/ui/controls/WaveformWidget.h src/ui/controls/WaveformWidget.cpp
    src/ui/dialogs/ExportDialog.h        src/ui/dialogs/ExportDialog.cpp
    src/ui/dialogs/RecoveryDialog.h      src/ui/dialogs/RecoveryDialog.cpp
    src/ui/dialogs/AudioSettingsDialog.h src/ui/dialogs/AudioSettingsDialog.cpp
    src/ui/effects/EffectChainWidget.h   src/ui/effects/EffectChainWidget.cpp
    src/ui/effects/DelayEffectWidget.h   src/ui/effects/DelayEffectWidget.cpp
    src/ui/effects/EffectSelectorDialog.h src/ui/effects/EffectSelectorDialog.cpp
    src/ui/effects/VstSelectorDialog.h    src/ui/effects/VstSelectorDialog.cpp
    src/ui/effects/InstrumentMenu.h       src/ui/effects/InstrumentMenu.cpp
    src/ui/effects/PluginEditorWindow.h   src/ui/effects/PluginEditorWindow.cpp
    src/ui/audioclip/WaveformSelection.h
    src/ui/audioclip/AudioFileUndoManager.h  src/ui/audioclip/AudioFileUndoManager.cpp
    src/ui/audioclip/AudioFileOperations.h   src/ui/audioclip/AudioFileOperations.cpp
    src/ui/audioclip/AudioClipEditor.h       src/ui/audioclip/AudioClipEditor.cpp
    src/ui/audioclip/AudioWaveformView.h     src/ui/audioclip/AudioWaveformView.cpp
    src/ui/pianoroll/PianoRollEditor.h    src/ui/pianoroll/PianoRollEditor.cpp
    src/ui/pianoroll/PianoKeyboard.h      src/ui/pianoroll/PianoKeyboard.cpp
    src/ui/pianoroll/NoteGrid.h           src/ui/pianoroll/NoteGrid.cpp
    src/ui/pianoroll/NoteItem.h           src/ui/pianoroll/NoteItem.cpp
    src/ui/pianoroll/VelocityLane.h       src/ui/pianoroll/VelocityLane.cpp
    src/ui/pianoroll/CcLane.h             src/ui/pianoroll/CcLane.cpp
    src/ui/pianoroll/ChannelColors.h
    src/ui/pianoroll/PianoRollRuler.h     src/ui/pianoroll/PianoRollRuler.cpp
    src/ui/routing/RoutingView.h          src/ui/routing/RoutingView.cpp
    src/ui/routing/RoutingMatrixView.h    src/ui/routing/RoutingMatrixView.cpp
    src/ui/routing/RoutingNode.h          src/ui/routing/RoutingNode.cpp
    src/ui/routing/CableItem.h            src/ui/routing/CableItem.cpp
    src/ui/sheetmusic/SheetMusicView.h    src/ui/sheetmusic/SheetMusicView.cpp
    src/ui/sheetmusic/NotationModel.h     src/ui/sheetmusic/NotationModel.cpp
    src/ui/sheetmusic/ScoreScene.h        src/ui/sheetmusic/ScoreScene.cpp
    src/ui/browser/FileBrowserPanel.h    src/ui/browser/FileBrowserPanel.cpp
    src/ui/SplashScreen.h           src/ui/SplashScreen.cpp
    src/ui/channelrack/ChannelRackView.h src/ui/channelrack/ChannelRackView.cpp
    src/ui/channelrack/ChannelRow.h      src/ui/channelrack/ChannelRow.cpp
    src/ui/channelrack/StepRow.h         src/ui/channelrack/StepRow.cpp
    src/ui/channelrack/ChannelSound.h    src/ui/channelrack/ChannelSound.cpp
    src/ui/channelrack/SoundPicker.h     src/ui/channelrack/SoundPicker.cpp
    src/ui/channelrack/SampleBrowserDialog.h src/ui/channelrack/SampleBrowserDialog.cpp
    src/ui/channelrack/SamplePreviewPlayer.h src/ui/channelrack/SamplePreviewPlayer.cpp
    src/ui/toolbar/PatternSelector.h     src/ui/toolbar/PatternSelector.cpp
    src/ui/toolbar/DockTitleBar.h        src/ui/toolbar/DockTitleBar.cpp
    src/ai/AiChatWidget.h          src/ai/AiChatWidget.cpp
    src/ai/AiQuickPrompt.h         src/ai/AiQuickPrompt.cpp
    src/utils/IconFont.h
    src/video/VideoThumbnailCache.h src/video/VideoThumbnailCache.cpp
    src/ui/timeline/VideoTrackLane.h src/ui/timeline/VideoTrackLane.cpp
    src/ui/video/VideoPlayerWidget.h src/ui/video/VideoPlayerWidget.cpp
)

# ── Create the OpenDaw JUCE GUI App target ───────────────────────────────────
juce_add_gui_app(OpenDaw
    VERSION "${OpenDaw_NUMERIC_VERSION}"
    PRODUCT_NAME "OpenDaw"
    COMPANY_NAME "OpenDaw"
    BUNDLE_ID "com.OpenDaw.app"
    ICON_BIG "${CMAKE_SOURCE_DIR}/resources/icon.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OpenDaw needs microphone access for audio recording")

target_sources(OpenDaw PRIVATE ${OpenDaw_SOURCES})
opendaw_use_core(OpenDaw)

# ── Windows application icon ─────────────────────────────────────────────────
# JUCE's generated RC expects icon.ico next to OpenDaw_resources.rc
if(WIN32)
    file(COPY "${CMAKE_SOURCE_DIR}/resources/OpenDaw.ico"
         DESTINATION "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/JuceLibraryCode")
    file(RENAME
         "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/JuceLibraryCode/OpenDaw.ico"
         "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/JuceLibraryCode/icon.ico")
endif()
qt_add_resources(OpenDaw "OpenDaw_resources" PREFIX "/" BASE resources FILES
    resources/splash.png
    resources/icon.png
    resources/fontaudio.ttf
    resources/MaterialSymbolsOutlined.ttf
    resources/mixerStripBG.png
    resources/Bravura.otf)

# ── Built-in instrument sounds ───────────────────────────────────────────────
# GeneralUser GS v2.0.3 by S. Christian Collins, a free General MIDI SoundFont
# (see its LICENSE, shipped beside it). Downloaded at configure time rather
# than kept in git, and copied next to the program after each build.
set(OPENDAW_SOUNDFONT_DIR "${CMAKE_BINARY_DIR}/soundfonts")
set(OPENDAW_SOUNDFONT_URL
    "https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60")
file(DOWNLOAD "${OPENDAW_SOUNDFONT_URL}/GeneralUser-GS.sf2"
     "${OPENDAW_SOUNDFONT_DIR}/GeneralUser-GS.sf2"
     EXPECTED_HASH SHA256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe)
file(DOWNLOAD "${OPENDAW_SOUNDFONT_URL}/documentation/LICENSE.txt"
     "${OPENDAW_SOUNDFONT_DIR}/GeneralUser-GS-LICENSE.txt"
     EXPECTED_HASH SHA256=7b32efefdf95ce38a043799f0659853ddc00fbaa14d8c50f0aca16b9b8b405be)
if(APPLE)
    set(OPENDAW_SOUNDFONT_DEST "$<TARGET_BUNDLE_CONTENT_DIR:OpenDaw>/Resources/soundfonts")
else()
    set(OPENDAW_SOUNDFONT_DEST "$<TARGET_FILE_DIR:OpenDaw>/soundfonts")
endif()
add_custom_command(TARGET OpenDaw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENDAW_SOUNDFONT_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENDAW_SOUNDFONT_DIR}/GeneralUser-GS.sf2"
            "${OPENDAW_SOUNDFONT_DIR}/GeneralUser-GS-LICENSE.txt"
            "${OPENDAW_SOUNDFONT_DEST}"
    COMMENT "Copying the built-in instrument sounds")

# ── Bundled synth: Surge XT ──────────────────────────────────────────────────
# Surge XT 1.3.4 by the Surge Synth Team (GPL-3.0, source at
# https://github.com/surge-synthesizer/surge/tree/release_xt_1.3.4): the
# official VST3 plus its factory content (patches, wavetables, skins), fetched
# at configure time and copied beside the program after each build. Optional:
# if a download fails the build carries on without it.
option(OPENDAW_BUNDLE_SURGE_XT "Ship the Surge XT synth with OpenDaw" ON)
if(OPENDAW_BUNDLE_SURGE_XT)
    set(SURGE_XT_VERSION "1.3.4")
    set(SURGE_XT_URL "https://github.com/surge-synthesizer/releases-xt/releases/download/${SURGE_XT_VERSION}")
    set(SURGE_XT_DIR "${CMAKE_BINARY_DIR}/surge-xt")
    if(WIN32)
        set(SURGE_XT_PLUGINS "surge-xt-win64-${SURGE_XT_VERSION}-pluginsonly.zip")
        set(SURGE_XT_PLUGINS_SHA256 564e162c560af07ad4ed47fe1bfcd827cf97a575de30d06c48249aad2e7c35e6)
        set(SURGE_XT_VST3_PATTERN "Surge XT.vst3")
    elseif(APPLE)
        set(SURGE_XT_PLUGINS "surge-xt-macos-${SURGE_XT_VERSION}-pluginsonly.zip")
        set(SURGE_XT_PLUGINS_SHA256 37d8d809693adc86587b7bc75a2384fc4e905f259aa9b9a21777749d36fbf8aa)
        set(SURGE_XT_VST3_PATTERN "Surge XT.vst3")
    else()
        set(SURGE_XT_PLUGINS "surge-xt-linux-${SURGE_XT_VERSION}-pluginsonly.tar.gz")
        set(SURGE_XT_PLUGINS_SHA256 dd431b75f5fa197c4bffa6ca27ca46970f0a94c834119bb1db7decdeec4c28db)
        set(SURGE_XT_VST3_PATTERN "./Surge XT.vst3")
    endif()

    # Downloads `url` to `file` unless it's already there; checks its SHA256.
    function(opendaw_fetch url file sha256 ok)
        if(EXISTS "${file}")
            file(SHA256 "${file}" have)
            if(have STREQUAL sha256)
                set(${ok} TRUE PARENT_SCOPE)
                return()
            endif()
        endif()
        message(STATUS "Downloading ${url}")
        file(DOWNLOAD "${url}" "${file}" STATUS status)
        list(GET status 0 code)
        if(code EQUAL 0)
            file(SHA256 "${file}" have)
        endif()
        if(code EQUAL 0 AND have STREQUAL sha256)
            set(${ok} TRUE PARENT_SCOPE)
        else()
            file(REMOVE "${file}")
            message(WARNING "Couldn't download ${url} (${status}); building without Surge XT")
            set(${ok} FALSE PARENT_SCOPE)
        endif()
    endfunction()

    set(SURGE_XT_STAGE "${SURGE_XT_DIR}/stage-${SURGE_XT_VERSION}")
    if(NOT EXISTS "${SURGE_XT_STAGE}/.complete")
        opendaw_fetch("${SURGE_XT_URL}/${SURGE_XT_PLUGINS}" "${SURGE_XT_DIR}/${SURGE_XT_PLUGINS}"
                      ${SURGE_XT_PLUGINS_SHA256} plugins_ok)
        opendaw_fetch("${SURGE_XT_URL}/surge-xt-portable-content-${SURGE_XT_VERSION}.tar.gz"
                      "${SURGE_XT_DIR}/surge-xt-portable-content-${SURGE_XT_VERSION}.tar.gz"
                      7ed24822eab69f73101d8558222979b1bceb4882a780292bb7087309b7b101bd content_ok)
        opendaw_fetch("https://raw.githubusercontent.com/surge-synthesizer/surge/release_xt_${SURGE_XT_VERSION}/LICENSE"
                      "${SURGE_XT_DIR}/LICENSE"
                      d4c0fe570b3750d909691ce5f494c9cd11ee047bbbedda9a5d75cc548e4df06b license_ok)
        if(plugins_ok AND content_ok AND license_ok)
            message(STATUS "Unpacking Surge XT ${SURGE_XT_VERSION}")
            file(REMOVE_RECURSE "${SURGE_XT_STAGE}")
            file(ARCHIVE_EXTRACT INPUT "${SURGE_XT_DIR}/${SURGE_XT_PLUGINS}"
                 DESTINATION "${SURGE_XT_STAGE}" PATTERNS "${SURGE_XT_VST3_PATTERN}")
            # The factory content only; the third-party patch and wavetable
            # packs would add another 340 MB.
            set(data "./Surge Synth Team/SurgeXTData")
            file(ARCHIVE_EXTRACT INPUT "${SURGE_XT_DIR}/surge-xt-portable-content-${SURGE_XT_VERSION}.tar.gz"
                 DESTINATION "${SURGE_XT_DIR}/content"
                 PATTERNS "${data}/patches_factory" "${data}/wavetables" "${data}/skins"
                          "${data}/tuning_library" "${data}/fx_presets" "${data}/modulator_presets")
            file(RENAME "${SURGE_XT_DIR}/content/Surge Synth Team/SurgeXTData" "${SURGE_XT_STAGE}/SurgeXTData")
            file(REMOVE_RECURSE "${SURGE_XT_DIR}/content")
            file(COPY_FILE "${SURGE_XT_DIR}/LICENSE" "${SURGE_XT_STAGE}/Surge XT LICENSE.txt")
            file(TOUCH "${SURGE_XT_STAGE}/.complete")
        endif()
    endif()

    if(EXISTS "${SURGE_XT_STAGE}/.complete")
        # Windows and Linux: the plugin and its data side by side (Surge finds a
        # SurgeXTData folder beside it). macOS: the plugin in the bundle's
        # PlugIns and its data in Resources (OpenDaw points Surge at it).
        if(APPLE)
            set(surge_plugin_dest "$<TARGET_BUNDLE_CONTENT_DIR:OpenDaw>/PlugIns")
            set(surge_data_dest "$<TARGET_BUNDLE_CONTENT_DIR:OpenDaw>/Resources")
        else()
            set(surge_plugin_dest "$<TARGET_FILE_DIR:OpenDaw>/plugins")
            set(surge_data_dest "$<TARGET_FILE_DIR:OpenDaw>/plugins")
        endif()
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
            set(copy_dir copy_directory_if_different)
        else()
            set(copy_dir copy_directory)
        endif()
        add_custom_command(TARGET OpenDaw POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E ${copy_dir}
                    "${SURGE_XT_STAGE}/Surge XT.vst3" "${surge_plugin_dest}/Surge XT.vst3"
            COMMAND ${CMAKE_COMMAND} -E ${copy_dir}
                    "${SURGE_XT_STAGE}/SurgeXTData" "${surge_data_dest}/SurgeXTData"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SURGE_XT_STAGE}/Surge XT LICENSE.txt" "${surge_data_dest}/Surge XT LICENSE.txt"
            COMMENT "Copying the Surge XT synth")
    endif()
endif()
target_link_libraries(OpenDaw PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    juce::juce_recommended_warning_flags)


# ── Helper executables, placed next to OpenDaw so they ship with it ──────────
set(OPENDAW_TOOLS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/$<CONFIG>")
if(APPLE)
    set(OPENDAW_TOOLS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/OpenDaw_artefacts/$<CONFIG>/OpenDaw.app/Contents/MacOS")
endif()

# Out-of-process plugin scanner: loads one plugin binary per run, so a plugin
# that crashes while being scanned takes down this helper, not the DAW.
juce_add_console_app(OpenDawPluginScanner PRODUCT_NAME "OpenDawPluginScanner")
target_sources(OpenDawPluginScanner PRIVATE src/tools/PluginScannerMain.cpp)
target_compile_definitions(OpenDawPluginScanner PRIVATE ${OPENDAW_DEFS} JUCE_STANDALONE_APPLICATION=1)
target_include_directories(OpenDawPluginScanner PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/libs/vst2sdk)
target_link_libraries(OpenDawPluginScanner PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_warning_flags)
set_target_properties(OpenDawPluginScanner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OPENDAW_TOOLS_OUTPUT_DIR}")
if(MSVC)
    target_compile_options(OpenDawPluginScanner PRIVATE /EHsc /bigobj)
endif()
add_dependencies(OpenDaw OpenDawPluginScanner)

# MCP stdio server for external agents (Claude Code, Codex, ...). Relays MCP
# JSON-RPC between stdin/stdout and the running OpenDaw over a local socket.
qt_add_executable(opendaw-mcp src/tools/McpBridgeMain.cpp)
target_link_libraries(opendaw-mcp PRIVATE Qt6::Core Qt6::Network)
target_include_directories(opendaw-mcp PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(opendaw-mcp PRIVATE OpenDaw_VERSION="${OpenDaw_DISPLAY_VERSION}")
set_target_properties(opendaw-mcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OPENDAW_TOOLS_OUTPUT_DIR}")
add_dependencies(OpenDaw opendaw-mcp)

# ── Tests ────────────────────────────────────────────────────────────────────
option(OPENDAW_BUILD_TESTS "Build the OpenDaw integration tests" ON)
if(OPENDAW_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    file(GLOB OPENDAW_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.cpp ${CMAKE_SOURCE_DIR}/tests/*.h)
    qt_add_executable(OpenDawTests ${OPENDAW_TEST_SOURCES}
        # Widget-free Channel Rack logic (tst_ChannelRackFl).
        src/ui/channelrack/ChannelSound.h src/ui/channelrack/ChannelSound.cpp
        # Typing keyboard (tst_TypingKeyboard; needs a QApplication).
        src/ui/TypingKeyboard.h src/ui/TypingKeyboard.cpp)
    opendaw_use_core(OpenDawTests)
    target_include_directories(OpenDawTests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(OpenDawTests PRIVATE
        OPENDAW_TOOLS_DIR="$<TARGET_FILE_DIR:OpenDaw>"
        OPENDAW_SOUNDFONT_DIR="${OPENDAW_SOUNDFONT_DIR}")
    target_link_libraries(OpenDawTests PRIVATE Qt6::Test Qt6::Gui Qt6::Widgets)
    add_dependencies(OpenDawTests OpenDawPluginScanner opendaw-mcp)
    add_test(NAME OpenDawTests COMMAND OpenDawTests)
    set_tests_properties(OpenDawTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)
    if(WIN32)
        get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
        set_tests_properties(OpenDawTests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${_qt_bin};PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenDaw>")
    endif()
endif()
