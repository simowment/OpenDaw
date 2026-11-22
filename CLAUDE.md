# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**OpenDaw** — a free, open-source DAW for Windows. Qt 6 handles all GUI; Tracktion Engine (built on JUCE) handles all audio. The binary is produced at `build\OpenDaw_artefacts\Debug\OpenDaw.exe`.

## Build Commands

All commands must run from a **Visual Studio Developer Command Prompt** (or wrap with `vcvarsall.bat`):

**Configure:**
```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`" x64 && c:\qt\Tools\CMake_64\bin\cmake.exe -B build -G Ninja -DCMAKE_PREFIX_PATH=c:/qt/6.10.2/msvc2022_64 -DCMAKE_TOOLCHAIN_FILE=c:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_MAKE_PROGRAM=c:/qt/Tools/Ninja/ninja.exe"
```

**Build:**
```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`" x64 && c:\qt\Tools\CMake_64\bin\cmake.exe --build build"
```

**Run:**
```powershell
$env:PATH = "c:\qt\6.10.2\msvc2022_64\bin;" + $env:PATH
.\build\OpenDaw_artefacts\Debug\OpenDaw.exe
```

Or simply `.\build.ps1` (finds Visual Studio/Build Tools via vswhere, Qt in `C:\Qt\6.10.2\msvc2022_64` or `$env:QT_DIR`, vcpkg in `C:\vcpkg` or `$env:VCPKG_ROOT`); `.\build.ps1 -Test` also runs the tests.

First build takes 2-4 minutes (compiling JUCE/Tracktion). Incremental rebuilds take ~10-15 seconds.

**Tests:** `ctest --test-dir build --output-on-failure` (or `build\OpenDawTests.exe --class TestPatterns -o -,txt`). Qt Test based integration suite in `tests/`, linked against the `OpenDawCore` library; runs headless (`QT_QPA_PLATFORM=offscreen`) with no audio device. See `tests/README.md`.

## Architecture

### Framework Bridge

Two frameworks coexist via `JuceQtBridge` (`src/app/JuceQtBridge.h`): a `QTimer` fires every 5ms to pump JUCE's `MessageManager` from inside Qt's event loop. JUCE's audio processing runs on its own real-time thread, independent of both GUI event loops. This pump must be running before any Tracktion Engine calls.

### Ownership Chain

```
main.cpp
  └─ OpenDawApplication          owns all singletons
       ├─ JuceQtBridge           JUCE message pump
       ├─ AudioEngine            wraps te::Engine
       ├─ EditManager            wraps te::Edit (the project document)
       ├─ PluginScanner          VST/VST3 scan, out of process via OpenDawPluginScanner.exe
       ├─ PatternManager         patterns (channel rack / step sequencer / playlist placements)
       ├─ PatternPlayer          plays the current pattern in Pattern mode (live MIDI)
       ├─ RenderService          offline region render, stems, audio analysis (agent tools)
       ├─ AiToolExecutor + CommandGateway + LocalApiServer   external agent API (MCP)
       └─ MainWindow             QMainWindow with dock layout
```

### Build targets

- `OpenDawCore` (static lib): engine, AI tools, agent API — everything except widgets. JUCE/Tracktion modules are compiled here once; other targets use `opendaw_use_core()` which re-exports the module include dirs and macro set (never link JUCE modules publicly — they'd compile twice).
- `OpenDaw` (GUI app), `OpenDawPluginScanner` (JUCE console helper that loads one plugin per run), `opendaw-mcp` (Qt console MCP stdio bridge), `OpenDawTests`. Helpers land next to `OpenDaw.exe`.

### Agent API (MCP)

`AiToolExecutor` holds every tool (the in-app chat and external agents share it). Modules add tools through `AiToolExtensionRegistry` providers (`src/ai/AiToolExtensions.h`), registered in `OpenDawApplication::initialize()` before any executor exists. Tool inputs are validated against their JSON schema (`AiSchemaValidator`, unknown properties rejected); errors carry `AiErrorCode::*`; ids are `track:<EditItemID>`, `clip:<id>`, `plugin:<id>`, `pattern:<n>` (`src/ai/AiIds.h`). Mutating tools each get one undo transaction; `run_batch` runs many atomically and rolls back via the undo history — so **any project change a tool makes must be undoable** (pass the Edit's UndoManager; for `AutomatableParameter` use the undoable `setParamUndoable` helper, since `setParameter` alone records nothing). `LocalApiServer` serves MCP JSON-RPC over a per-user local socket on the GUI thread; `opendaw-mcp` relays stdio to it. See `docs/AGENT_API.md`.

### Pattern workflow

Channels are the MIDI tracks; a pattern stores step rows per channel in the Edit state (`OpenDaw_PATTERNS`, written through the UndoManager); placing a pattern creates one tagged MIDI clip per used channel (`OpenDaw_patternId`/`OpenDaw_placementId`), regenerated whenever the pattern changes. UI: `src/ui/channelrack/`, pattern selector + PAT/SONG toggle in `TransportBar`.

`OpenDawApplication` is the single place to access engine/edit globals — never create additional `te::Engine` instances.

### Tracktion Engine Integration

- Namespace alias: `namespace te = tracktion::engine;` (defined in `AudioEngine.h`, used throughout)
- The `te::Edit` object is the project data model (tracks, clips, tempo, effects). It is owned by `EditManager`.
- `EditManager` exposes Qt signals (`editChanged`, `tracksChanged`, `transportStateChanged`) so Qt widgets can react to engine-side changes.
- Audio clips in Tracktion are positioned by **beat**, not seconds. `addAudioClipToTrack` takes a `double startBeat`.
- Level metering: each track needs a `te::LevelMeterPlugin` inserted into its plugin chain. `EditManager::ensureLevelMetersOnAllTracks()` handles this. `ChannelStrip` polls `te::LevelMeasurer::Client` via a `QTimer`.
- The project file format is `.tracktionedit` (Tracktion's native XML format).

### UI Layout

`MainWindow` uses Qt dock widgets:
- **Central widget**: `TimelineView` — the "Playlist" (arrangement with automation lanes)
- **Bottom dock** (tabbed): `MixerView`, `PianoRollEditor`, `AudioClipEditor`, `RoutingView`, `SheetMusicView`
- **Left dock**: `FileBrowserPanel` (full height)
- **Channel Rack dock**: `ChannelRackView` (step sequencer for the current pattern)
- **Right dock** (tabbed): `EffectChainWidget` + `AiChatWidget`
- F5 Playlist, F6 Channel Rack, F7 Piano Roll, F8 Browser, F9 Mixer; Ctrl+P places the current pattern
- **Top toolbar**: `TransportBar`
- **Dialogs**: `ExportDialog`, `RecoveryDialog`, `AudioSettingsDialog`

### Timeline Coordinates

`TimelineView` uses `QGraphicsScene`/`QGraphicsView` for the clip area. The coordinate system is pixels, converted to/from beats via `pixelsPerBeat_` (default 40.0 px/beat). `GridSnapper` handles snap-to-grid logic for all drag operations.

### Naming & Namespace

All project code lives in the `OpenDaw` namespace. The `DONT_SET_USING_JUCE_NAMESPACE=1` compile definition prevents JUCE from polluting the global namespace — always qualify JUCE types with `juce::` or use the `te::` alias.

## Key Files

| File | Purpose |
|------|---------|
| `src/app/OpenDawApplication.h` | Top-level app object; entry point for accessing engine/edit |
| `src/engine/EditManager.h` | Project document API; Qt signals for track/transport changes |
| `src/engine/AudioEngine.h` | Wraps `te::Engine`; device management |
| `src/app/JuceQtBridge.h` | QTimer-based JUCE message pump |
| `src/ui/timeline/GridSnapper.h` | Beat/pixel conversion and snap logic |
| `CMakeLists.txt` | Single build file; all sources listed here |

## External Dependencies

### FFmpeg (video thumbnails + MP3 export)
FFmpeg C libraries are linked at build time for video frame extraction and MP3 encoding.

**Windows (vcpkg):**
```powershell
git clone --depth 1 https://github.com/microsoft/vcpkg.git c:\vcpkg
c:\vcpkg\bootstrap-vcpkg.bat
c:\vcpkg\vcpkg install ffmpeg[core,avformat,avcodec,swscale,swresample,mp3lame]:x64-windows
```
Pass `-DCMAKE_TOOLCHAIN_FILE=c:/vcpkg/scripts/buildsystems/vcpkg.cmake` to CMake configure.

**macOS:** `brew install ffmpeg`
**Linux:** `sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev`

### Video Track
A single video file can be attached per Edit (stored via `Edit::setVideoFile`/`Edit::getVideoFile`). The `VideoTrackLane` widget renders frame thumbnails synced to the timeline's beat/pixel coordinate system. `VideoThumbnailCache` wraps FFmpeg's C API for async frame extraction with LRU caching.

## Submodules

- `libs/JUCE` — pinned to commit `7c89e11` (required by Tracktion Engine)
- `libs/tracktion_engine` — audio engine

After cloning without `--recurse-submodules`, run:
```powershell
git submodule update --init --depth 1
cd libs/JUCE && git checkout 7c89e11f6b7316c369f3d3f22227c60e816e738b
```
