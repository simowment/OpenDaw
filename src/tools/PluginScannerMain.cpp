// OpenDawPluginScanner: scans one plugin file in a separate process, so a
// plugin that crashes or hangs while being scanned can't take the DAW down.
//
//   OpenDawPluginScanner --format VST3|VST --file <path> --out <result.xml>
//
// Writes <SCAN ok="1" format=".." file=".."> holding one PluginDescription
// XML element per type found, or <SCAN ok="0" error=".."> on failure.
// Exit codes: 0 = types found, 2 = no types in the file, 1 = error.
// Anything else (or a crash) means the plugin brought the scanner down.
//
// JUCE only: no Qt, no Tracktion, no FFmpeg.

#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include <memory>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <shellapi.h>
 #if defined(_MSC_VER)
  #include <crtdbg.h>
  #include <cstdlib>
  #pragma comment(lib, "shell32.lib")
 #endif
#endif

namespace {

constexpr int kExitFound   = 0;
constexpr int kExitError   = 1;
constexpr int kExitNoTypes = 2;

// The arguments as Unicode. On Windows, argv is in the ANSI code page, which
// can't hold every path, so read the wide command line instead.
juce::StringArray commandLineArguments(int argc, char* argv[])
{
    juce::StringArray args;
#if JUCE_WINDOWS
    juce::ignoreUnused(argc, argv);
    int count = 0;
    if (auto** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i)
            args.add(juce::String(static_cast<const wchar_t*>(wide[i])));
        LocalFree(wide);
    }
#else
    for (int i = 1; i < argc; ++i)
        args.add(juce::String(juce::CharPointer_UTF8(argv[i])));
#endif
    return args;
}

juce::String argumentValue(const juce::StringArray& args, const juce::String& name)
{
    const int index = args.indexOf(name);
    return (index >= 0 && index + 1 < args.size()) ? args[index + 1] : juce::String();
}

// The --out file, or an invalid File if it's missing or not absolute.
juce::File outputFile(const juce::StringArray& args)
{
    const auto path = argumentValue(args, "--out");
    return (path.isNotEmpty() && juce::File::isAbsolutePath(path)) ? juce::File(path) : juce::File();
}

// Keeps a crash from popping up a dialog that would leave the scan waiting
// on the timeout (Windows error boxes, CRT abort and debug-assert reports).
void silenceCrashDialogs()
{
#if JUCE_WINDOWS
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
 #if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
 #endif
#endif
}

bool writeResult(const juce::File& out, const juce::XmlElement& xml)
{
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    return xml.writeTo(out);
}

int fail(const juce::File& out, const juce::String& message)
{
    std::fprintf(stderr, "OpenDawPluginScanner: %s\n", message.toRawUTF8());
    if (out != juce::File()) {
        juce::XmlElement xml("SCAN");
        xml.setAttribute("ok", 0);
        xml.setAttribute("error", message);
        writeResult(out, xml);
    }
    std::fflush(stderr);
    return kExitError;
}

std::unique_ptr<juce::AudioPluginFormat> createFormat(const juce::String& name)
{
#if JUCE_PLUGINHOST_VST3
    if (name.equalsIgnoreCase(juce::VST3PluginFormat::getFormatName()))
        return std::make_unique<juce::VST3PluginFormat>();
#endif
#if JUCE_PLUGINHOST_VST
    if (name.equalsIgnoreCase(juce::VSTPluginFormat::getFormatName()) || name.equalsIgnoreCase("VST2"))
        return std::make_unique<juce::VSTPluginFormat>();
#endif
    juce::ignoreUnused(name);
    return nullptr;
}

int runScan(const juce::StringArray& args)
{
    const auto formatName = argumentValue(args, "--format");
    const auto filePath   = argumentValue(args, "--file");
    const auto out        = outputFile(args);

    if (out == juce::File())
        return fail({}, "usage: OpenDawPluginScanner --format VST3|VST --file <path> --out <result.xml>");

    if (filePath.isEmpty())
        return fail(out, "missing --file");

    auto format = createFormat(formatName);
    if (format == nullptr)
        return fail(out, "unsupported plugin format: " + formatName);

    // The plugin's own code runs here, on the message thread, which is where
    // JUCE expects plugin factories to be queried.
    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile(found, filePath);

    juce::XmlElement xml("SCAN");
    xml.setAttribute("ok", 1);
    xml.setAttribute("format", format->getName());
    xml.setAttribute("file", filePath);
    for (auto* desc : found)
        if (auto child = desc->createXml())
            xml.addChildElement(child.release());

    if (! writeResult(out, xml))
        return fail(out, "could not write " + out.getFullPathName());

    std::fflush(stdout);
    return found.isEmpty() ? kExitNoTypes : kExitFound;
}

} // namespace

int main(int argc, char* argv[])
{
    silenceCrashDialogs();

    const auto args = commandLineArguments(argc, argv);

    int result = kExitError;
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        try {
            result = runScan(args);
        } catch (const std::exception& e) {
            result = fail(outputFile(args), juce::String("exception: ") + e.what());
        } catch (...) {
            result = fail(outputFile(args), "unknown exception");
        }
    }
    return result;
}
