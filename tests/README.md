# OpenDaw integration tests

`OpenDawTests` is one Qt Test executable that links `OpenDawCore` (engine, AI
tools and agent API, no widgets). It starts the engine once, headless and
without an audio device, then runs every test class in turn:

| Class | What it covers |
|---|---|
| `TestSchemaValidator` | `AiSchemaValidator`: types, integer vs fractional, required, unknown properties, enum, min/max, nested arrays/objects, oneOf |
| `TestToolExecutor` | tools/list completeness, stable `track:`/`clip:` ids, `NOT_FOUND` / `INVALID_ARGUMENT` errors, MIDI clip round trip, `run_batch` (success, atomic rollback, non-batchable tools), undo/redo |
| `TestMcpProtocol` | MCP over `CommandGateway` (initialize, notifications, ping, tools/list annotations, errors, JSON-RPC batches), `LocalApiServer` over a local socket (token auth, partial lines), and the `opendaw-mcp` stdio bridge when it was built |
| `TestPatterns` | pattern tools: steps, placements as linked MIDI clips, regeneration, undo, deleting a placement clip, save/reload |
| `TestRenderAnalysis` | `AudioFileAnalyzer` on synthesized WAVs (sine, silence, clipped square, inverted stereo), `render_region`, `export_stems` |
| `TestPluginScanner` | crash isolation through the `OpenDawPluginScanner` helper, blacklist persistence, a full scan |

## Build

Configure with `-DOPENDAW_BUILD_TESTS=ON` (and usually
`-DOPENDAW_BUNDLE_SURGE_XT=OFF`), then build the `OpenDawTests` target:

```powershell
# Windows, from a VS Developer prompt (see CLAUDE.md for the full configure line)
cmake -B build -G Ninja -DOPENDAW_BUILD_TESTS=ON -DOPENDAW_BUNDLE_SURGE_XT=OFF ...
cmake --build build --target OpenDawTests
```

```bash
# Linux / macOS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENDAW_BUILD_TESTS=ON -DOPENDAW_BUNDLE_SURGE_XT=OFF
cmake --build build
```

## Run

```bash
ctest --test-dir build --output-on-failure          # what CI runs
build/OpenDawTests                                    # everything, verbose output
build/OpenDawTests --list-classes
build/OpenDawTests --class TestPatterns               # one class (repeatable)
build/OpenDawTests --class TestToolExecutor runBatchAtomicRollback   # one function
OPENDAW_TEST_CLASS=TestSchemaValidator,TestMcpProtocol build/OpenDawTests
```

Any argument other than `--class` / `--list-classes` goes to QTest (`-v2`,
`-o file,junitxml`, a test function name, ...). The exit code is non-zero when
any class failed.

On Windows the Qt DLLs must be on `PATH`
(`$env:PATH = "c:\qt\6.10.2\msvc2022_64\bin;" + $env:PATH`), or run
`windeployqt` on `OpenDawTests.exe`.

`QT_QPA_PLATFORM` defaults to `offscreen`, so no display is needed.

## Optional parts

Some tests skip (QSKIP) when a piece is missing, instead of failing:

- **Built-in SoundFont** (`soundfonts/GeneralUser-GS.sf2` next to the
  executable, as the app's POST_BUILD step places it): without it,
  `render_region` still has to produce a file with analysis, but the
  "the render is not silent" checks are skipped.
- **`opendaw-mcp`**: looked up next to the test executable and in
  `OpenDaw_artefacts/<config>/`, or set `OPENDAW_MCP_BRIDGE=/path/to/opendaw-mcp`.
- **`OpenDawPluginScanner`**: looked up the same way.
- **Plugin cache / blacklist tests** write the per-user files under JUCE's
  application data folder (`%APPDATA%\OpenDaw`, `~/.config/OpenDaw`, ...),
  which PluginScanner cannot redirect. They run only on CI (`CI` set) or with
  `OPENDAW_TESTS_ALLOW_PLUGIN_STATE=1`, and restore the files afterwards.

## Isolation

- Organisation name `OpenDaw-tests` (separate QSettings) and
  `QStandardPaths::setTestModeEnabled(true)` (autosave folder and other app
  data go to the Qt test locations).
- Every file a test writes lives in a `QTemporaryDir`.
- The agent API listens on `opendaw-test-<pid>` and writes its discovery file
  to the temporary directory (`OPENDAW_AGENT_API_FILE`), so a running OpenDaw
  is not affected.
- No audio device is opened; rendering is offline.

## Writing a test

Add `tests/tst_Something.cpp` with a `QObject` test class, end it with

```cpp
OD_REGISTER_TEST(TestSomething)
#include "tst_Something.moc"
```

and list the file in the `OpenDawTests` sources in `CMakeLists.txt`. Reach the
engine through `OpenDaw::Test::harness()`; call tools with
`OD_CALL_OK(result, "tool_name", (QJsonObject{...}))` /
`OD_CALL_ERR(...)`. The project is reset (`newEdit()`) before each class;
call `harness().resetProject()` in `init()` for a fresh project per test.
Never block on `waitFor*()` for something the harness serves (the socket
server and JUCE run on this thread): use `QTRY_VERIFY_WITH_TIMEOUT` or
`QTest::qWait`.
