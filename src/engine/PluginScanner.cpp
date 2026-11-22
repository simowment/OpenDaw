#include "PluginScanner.h"
#include "instruments/BundledPlugins.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <QDebug>
#include <QElapsedTimer>
#include <QProcess>
#include <vector>

namespace OpenDaw {

namespace {

QString toQString(const juce::String& s)
{
    return QString::fromUtf8(s.toRawUTF8());
}

juce::String toJuceString(const QString& s)
{
    return juce::String::fromUTF8(s.toUtf8().constData());
}

// A path as a File, or an invalid File for anything that isn't an absolute
// path (juce::File asserts on relative ones).
juce::File asFile(const juce::String& path)
{
    return juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
}

// True if candidate is path itself or lies inside it (a VST3 bundle's binary).
bool isSameOrInside(const juce::String& candidate, const juce::File& path)
{
    const auto f = asFile(candidate);
    return f != juce::File() && (f == path || f.isAChildOf(path));
}

QString describeExitCode(int code)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(code), 8, 16, QLatin1Char('0'));
}

} // namespace

// ── PluginScanWorker ─────────────────────────────────────────────────────────

PluginScanWorker::PluginScanWorker(PluginScanOptions options)
    : options_(std::move(options))
{
}

bool PluginScanWorker::stopRequested() const
{
    return options_.stop != nullptr && options_.stop->load();
}

void PluginScanWorker::scanInProcess(juce::AudioPluginFormat& format, const juce::String& file,
                                     juce::OwnedArray<juce::PluginDescription>& found)
{
    // If the plugin takes the whole program down, the next start finds this
    // file and blacklists the plugin (PluginScanner's constructor).
    const auto pedal = options_.inProgressFile;
    if (pedal != juce::File()) {
        pedal.getParentDirectory().createDirectory();
        pedal.replaceWithText(file);
    }

    try {
        format.findAllTypesForFile(found, file);
    } catch (...) {
        qWarning() << "[PluginScanner] plugin threw during scan:" << toQString(file);
        found.clear();
    }

    if (pedal != juce::File())
        pedal.deleteFile();
}

void PluginScanWorker::doScan()
{
    juce::VST3PluginFormat vst3;
#if JUCE_PLUGINHOST_VST
    juce::VSTPluginFormat  vst2;
#endif

    juce::AudioPluginFormat* formats[] = {
        &vst3,
#if JUCE_PLUGINHOST_VST
        &vst2,
#endif
    };

    juce::Array<juce::File> blacklisted;
    for (const auto& path : options_.blacklist)
        if (auto f = asFile(toJuceString(path)); f != juce::File())
            blacklisted.add(f);

    auto isBlacklisted = [&blacklisted](const juce::String& file) {
        const auto f = asFile(file);
        if (f == juce::File()) return false;
        for (const auto& b : blacklisted)
            if (f == b || f.isAChildOf(b)) return true;
        return false;
    };

    auto isKnown = [this](const juce::String& file, juce::AudioPluginFormat& format) {
        const auto path = asFile(file);
        if (path == juce::File()) return false;
        const auto formatName = format.getName();
        for (const auto& d : options_.known)
            if (d.pluginFormatName == formatName && isSameOrInside(d.fileOrIdentifier, path)
                && ! format.pluginNeedsRescanning(d))
                return true;
        return false;
    };

    struct Candidate {
        juce::String             file;
        juce::AudioPluginFormat* format;
    };
    std::vector<Candidate> candidates;

    for (auto* format : formats) {
        auto paths = options_.searchPathOverride;
        if (paths.getNumPaths() == 0) {
            paths = format->getDefaultLocationsToSearch();
            if (format == &vst3 && BundledPlugins::folder() != juce::File())
                paths.add(BundledPlugins::folder());
        }

        juce::StringArray seen;
        for (const auto& file : format->searchPathsForPlugins(paths, true, true)) {
            if (seen.contains(file)) continue;
            seen.add(file);
            if (isBlacklisted(file)) continue;
            if (! options_.fullRescan && isKnown(file, *format)) continue;
            candidates.push_back({ file, format });
        }
    }

    bool outOfProcess = options_.helper.existsAsFile();
    if (! outOfProcess)
        qWarning() << "[PluginScanner] OpenDawPluginScanner not found beside the program;"
                      " scanning plugins in-process (a crashing plugin will crash OpenDaw)";

    const int total = static_cast<int>(candidates.size());
    for (int i = 0; i < total; ++i) {
        if (stopRequested()) break;

        const auto& candidate = candidates[static_cast<size_t>(i)];
        const auto fileName = toQString(asFile(candidate.file).getFileNameWithoutExtension());
        emit scanProgress(fileName.isEmpty() ? toQString(candidate.file) : fileName, i, total);

        juce::OwnedArray<juce::PluginDescription> found;
        bool scanned = false;

        if (outOfProcess) {
            QString error;
            const auto outcome = PluginScanner::scanFileWithHelper(
                options_.helper, candidate.format->getName(), candidate.file,
                found, &error, options_.stop, options_.helperTimeoutMs);

            switch (outcome) {
            case PluginScanner::HelperOutcome::Scanned:
                scanned = true;
                break;
            case PluginScanner::HelperOutcome::Crashed:
                emit pluginFailed(toQString(candidate.file), error, true);
                break;
            case PluginScanner::HelperOutcome::TimedOut:
                // Slow is not broken: report it, but try again next scan.
            case PluginScanner::HelperOutcome::Failed:
                emit pluginFailed(toQString(candidate.file), error, false);
                break;
            case PluginScanner::HelperOutcome::Cancelled:
                break;
            case PluginScanner::HelperOutcome::NotStarted:
                qWarning() << "[PluginScanner]" << error
                           << "- scanning the remaining plugins in-process";
                outOfProcess = false;
                break;
            }
            if (outcome == PluginScanner::HelperOutcome::Cancelled) break;
        }

        if (! outOfProcess && ! scanned) {
            found.clear();
            scanInProcess(*candidate.format, candidate.file, found);
            scanned = true;
        }

        for (auto* desc : found)
            emit pluginFound(*desc);

        const auto shownName = found.isEmpty() ? fileName : toQString(found.getFirst()->name);
        emit scanProgress(shownName, i + 1, total);
    }

    emit scanFinished();
}

// ── PluginScanner ────────────────────────────────────────────────────────────

PluginScanner::PluginScanner(AudioEngine& engine, QObject* parent)
    : QObject(parent), audioEngine_(engine)
{
    loadCachedList();
    loadBlacklist();

    // An in-process scan that never finished: the plugin it was on took the
    // program down, so never scan it again.
    const auto pedal = getInProgressFile();
    if (pedal.existsAsFile()) {
        const auto file = pedal.loadFileAsString().trim();
        pedal.deleteFile();
        if (file.isNotEmpty()) {
            qWarning() << "[PluginScanner] blacklisting plugin that crashed the last scan:"
                       << toQString(file);
            addToBlacklist(toQString(file));
        }
    }
}

PluginScanner::~PluginScanner()
{
    requestStop();
    workerThread_.quit();
    workerThread_.wait();
}

void PluginScanner::startScan(bool fullRescan)
{
    if (scanning_)
        return;

    if (workerThread_.isRunning()) {
        workerThread_.quit();
        workerThread_.wait();
    }

    scanning_ = true;
    stopRequested_ = false;

    PluginScanOptions options;
    options.helper = helperExecutable();
    options.known = getPluginList().getTypes();
    options.blacklist = blacklist_;
    for (const auto& f : getPluginList().getBlacklistedFiles())
        options.blacklist << toQString(f);
    options.fullRescan = fullRescan;
    options.inProgressFile = getInProgressFile();
    options.stop = &stopRequested_;

    auto* worker = new PluginScanWorker(std::move(options));
    worker->moveToThread(&workerThread_);

    connect(&workerThread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &PluginScanWorker::scanProgress,
            this, &PluginScanner::scanProgress, Qt::QueuedConnection);
    // Runs on the worker thread and hands a copy of the description to the
    // main thread, where the list is changed (keeps it in order with the
    // queued signals below).
    connect(worker, &PluginScanWorker::pluginFound, this,
            [this](const juce::PluginDescription& desc) {
                QMetaObject::invokeMethod(this, [this, desc]() {
                    getPluginList().addType(desc);
                }, Qt::QueuedConnection);
            }, Qt::DirectConnection);
    connect(worker, &PluginScanWorker::pluginFailed, this,
            [this](const QString& file, const QString& reason, bool blacklist) {
                qWarning() << "[PluginScanner] plugin failed to scan:" << file << "-" << reason;
                if (blacklist)
                    addToBlacklist(file);
                emit pluginFailed(file, reason);
            }, Qt::QueuedConnection);
    connect(worker, &PluginScanWorker::scanFinished, this, [this]() {
        scanning_ = false;
        saveCachedList();
        emit scanFinished();
    }, Qt::QueuedConnection);

    workerThread_.start();
    QMetaObject::invokeMethod(worker, &PluginScanWorker::doScan, Qt::QueuedConnection);
}

void PluginScanner::requestStop()
{
    stopRequested_ = true;
}

juce::KnownPluginList& PluginScanner::getPluginList()
{
    return audioEngine_.engine().getPluginManager().knownPluginList;
}

juce::File PluginScanner::getCacheFile() const
{
    auto appData = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory);
    return appData.getChildFile("OpenDaw").getChildFile("plugin-cache.xml");
}

juce::File PluginScanner::getBlacklistFile() const
{
    return getCacheFile().getSiblingFile("plugin-blacklist.txt");
}

juce::File PluginScanner::getInProgressFile() const
{
    return getCacheFile().getSiblingFile("plugin-scan-in-progress.txt");
}

void PluginScanner::loadCachedList()
{
    auto cacheFile = getCacheFile();
    if (!cacheFile.existsAsFile()) return;

    auto xml = juce::XmlDocument::parse(cacheFile);
    if (!xml) {
        qWarning() << "[PluginScanner] corrupt plugin cache, deleting";
        cacheFile.deleteFile();
        return;
    }
    getPluginList().recreateFromXml(*xml);

    auto& kpl = getPluginList();
    for (int i = kpl.getNumTypes() - 1; i >= 0; --i) {
        auto desc = kpl.getTypes()[i];
        if (!juce::File(desc.fileOrIdentifier).exists())
            kpl.removeType(desc);
    }
}

void PluginScanner::saveCachedList()
{
    auto cacheFile = getCacheFile();
    cacheFile.getParentDirectory().createDirectory();

    if (auto xml = getPluginList().createXml()) {
        xml->writeTo(cacheFile);
    }
}

void PluginScanner::loadBlacklist()
{
    blacklist_.clear();
    const auto file = getBlacklistFile();
    if (!file.existsAsFile()) return;

    juce::StringArray lines;
    file.readLines(lines);
    for (const auto& line : lines) {
        const auto path = toQString(line.trim());
        if (!path.isEmpty() && !blacklist_.contains(path, Qt::CaseInsensitive))
            blacklist_ << path;
    }
}

void PluginScanner::saveBlacklist() const
{
    const auto file = getBlacklistFile();
    if (blacklist_.isEmpty()) {
        file.deleteFile();
        return;
    }
    file.getParentDirectory().createDirectory();
    file.replaceWithText(toJuceString(blacklist_.join(QLatin1Char('\n')) + QLatin1Char('\n')));
}

void PluginScanner::addToBlacklist(const QString& file)
{
    if (file.isEmpty() || blacklist_.contains(file, Qt::CaseInsensitive))
        return;
    blacklist_ << file;
    saveBlacklist();
}

void PluginScanner::clearBlacklist()
{
    blacklist_.clear();
    // Also drops the KnownPluginList's own blacklisted entries, which live in
    // plugin-cache.xml: save it too, or they come back on the next start.
    getPluginList().clearBlacklistedFiles();
    saveBlacklist();
    saveCachedList();
}

bool PluginScanner::isOutOfProcess() const
{
    return helperExecutable().existsAsFile();
}

juce::File PluginScanner::helperExecutable()
{
#if JUCE_WINDOWS
    const juce::String name = "OpenDawPluginScanner.exe";
#else
    const juce::String name = "OpenDawPluginScanner";
#endif
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                            .getParentDirectory();

    juce::Array<juce::File> candidates;
    candidates.add(exeDir.getChildFile(name));
#if JUCE_MAC
    // Inside the app bundle: Contents/MacOS, beside the main binary.
    candidates.add(juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                       .getChildFile("Contents").getChildFile("MacOS").getChildFile(name));
    candidates.add(exeDir.getParentDirectory().getChildFile("MacOS").getChildFile(name));
#endif

    for (const auto& f : candidates)
        if (f.existsAsFile()) return f;
    return {};
}

PluginScanner::HelperOutcome PluginScanner::scanFileWithHelper(
    const juce::File& helper, const juce::String& formatName, const juce::String& file,
    juce::OwnedArray<juce::PluginDescription>& found, QString* error,
    const std::atomic<bool>* stop, int timeoutMs)
{
    auto setError = [error](const QString& message) {
        if (error) *error = message;
    };

    if (!helper.existsAsFile()) {
        setError(QStringLiteral("the plugin scanner (OpenDawPluginScanner) is missing"));
        return HelperOutcome::NotStarted;
    }

    // A unique name: the worker and BundledPlugins may both be scanning.
    const auto result = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("opendaw-plugin-scan-" + juce::Uuid().toString() + ".xml");

    QProcess process;
    process.setProgram(toQString(helper.getFullPathName()));
    process.setArguments({ QStringLiteral("--format"), toQString(formatName),
                           QStringLiteral("--file"), toQString(file),
                           QStringLiteral("--out"), toQString(result.getFullPathName()) });
    process.setWorkingDirectory(toQString(helper.getParentDirectory().getFullPathName()));
    // Plugins can be chatty; unread pipes would fill up and stall the helper.
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());

    process.start();
    if (!process.waitForStarted(10000)) {
        setError(QStringLiteral("could not start the plugin scanner: ") + process.errorString());
        process.kill();
        process.waitForFinished(2000);
        result.deleteFile();
        return HelperOutcome::NotStarted;
    }

    QElapsedTimer timer;
    timer.start();
    bool timedOut = false;
    bool cancelled = false;
    while (process.state() != QProcess::NotRunning) {
        if (stop != nullptr && stop->load()) {
            cancelled = true;
            break;
        }
        if (timer.elapsed() >= timeoutMs) {
            timedOut = true;
            break;
        }
        process.waitForFinished(200);
    }
    if (cancelled || timedOut) {
        process.kill();
        process.waitForFinished(5000);
    }

    auto finish = [&result](HelperOutcome outcome) {
        result.deleteFile();
        return outcome;
    };

    if (cancelled) {
        setError(QStringLiteral("scan cancelled"));
        return finish(HelperOutcome::Cancelled);
    }
    if (timedOut) {
        setError(QStringLiteral("the plugin did not finish scanning within %1 s; it will be tried again on the next scan")
                     .arg(timeoutMs / 1000));
        return finish(HelperOutcome::TimedOut);
    }
    if (process.exitStatus() == QProcess::CrashExit) {
        setError(QStringLiteral("the plugin crashed the scanner (exit code %1)")
                     .arg(describeExitCode(process.exitCode())));
        return finish(HelperOutcome::Crashed);
    }

    const int exitCode = process.exitCode();
    if (exitCode != 0 && exitCode != 1 && exitCode != 2) {
        setError(QStringLiteral("the plugin brought the scanner down (exit code %1)")
                     .arg(describeExitCode(exitCode)));
        return finish(HelperOutcome::Crashed);
    }

    std::unique_ptr<juce::XmlElement> xml;
    if (result.existsAsFile())
        xml = juce::parseXML(result);

    if (exitCode == 1) {
        const auto message = xml ? xml->getStringAttribute("error") : juce::String();
        setError(message.isNotEmpty() ? toQString(message)
                                      : QStringLiteral("the plugin scanner reported an error"));
        return finish(HelperOutcome::Failed);
    }

    // Exit 0 or 2 must come with a result; without one, the plugin most
    // likely ended the process itself.
    if (!xml || !xml->hasTagName("SCAN") || !xml->getBoolAttribute("ok")) {
        setError(QStringLiteral("the plugin scanner exited without a result"));
        return finish(HelperOutcome::Crashed);
    }

    for (auto* child : xml->getChildIterator()) {
        juce::PluginDescription desc;
        if (desc.loadFromXml(*child))
            found.add(new juce::PluginDescription(desc));
    }
    return finish(HelperOutcome::Scanned);
}

} // namespace OpenDaw
