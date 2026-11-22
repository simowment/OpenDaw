#pragma once

#include "AudioEngine.h"
#include <QObject>
#include <QStringList>
#include <QThread>
#include <atomic>

namespace OpenDaw {

// How long the helper may spend on one plugin file before it is killed.
// Generous on purpose: a VST2 shell (WaveShell) instantiates every sub-plugin,
// and a licence or authorisation dialog waits for the user.
inline constexpr int kDefaultPluginScanTimeoutMs = 180000;

// What one scan run works from. Built on the main thread by PluginScanner,
// then handed to the worker, which never touches the live plugin list.
struct PluginScanOptions {
    juce::File helper;                          // OpenDawPluginScanner, or invalid for in-process
    juce::Array<juce::PluginDescription> known; // the list as it was when the scan started
    QStringList blacklist;                      // files never to scan
    bool fullRescan = false;                    // rescan files already in the list
    juce::File inProgressFile;                  // dead-man's pedal for in-process scans
    const std::atomic<bool>* stop = nullptr;    // set to cancel between (or during) files
    int helperTimeoutMs = kDefaultPluginScanTimeoutMs; // per-file limit in the helper
    // When non-empty, scanned instead of the default VST3/VST folders and the
    // bundled plugins (tests use this to scan a folder of their own).
    juce::FileSearchPath searchPathOverride;
};

// Runs on PluginScanner's worker thread. Out of process: one helper run per
// file, so a plugin that crashes or hangs only takes the helper down.
class PluginScanWorker : public QObject {
    Q_OBJECT
public:
    explicit PluginScanWorker(PluginScanOptions options);

public slots:
    void doScan();

signals:
    void scanProgress(const QString& pluginName, int current, int total);
    void pluginFound(const juce::PluginDescription& desc);
    // blacklist: the file crashed the scanner and should be skipped from now on.
    // A file that only ran out of time is reported with blacklist = false, so
    // the next scan tries it again (slow shells, first-run licence dialogs).
    void pluginFailed(const QString& file, const QString& reason, bool blacklist);
    void scanFinished();

private:
    bool stopRequested() const;
    void scanInProcess(juce::AudioPluginFormat& format, const juce::String& file,
                       juce::OwnedArray<juce::PluginDescription>& found);

    PluginScanOptions options_;
};

class PluginScanner : public QObject {
    Q_OBJECT

public:
    explicit PluginScanner(AudioEngine& engine, QObject* parent = nullptr);
    ~PluginScanner() override;

    // Scans the default VST3/VST folders and the bundled plugins. Files already
    // in the list (and unchanged) are skipped unless fullRescan is set;
    // blacklisted files are always skipped.
    void startScan(bool fullRescan = false);
    // Stops after the current file, killing a running helper. scanFinished follows.
    void requestStop();
    bool isScanning() const { return scanning_; }
    juce::KnownPluginList& getPluginList();

    void loadCachedList();
    void saveCachedList();
    juce::File getCacheFile() const;

    // Files that crashed the scanner; never scanned again until cleared.
    QStringList blacklist() const { return blacklist_; }
    // Forgets every blacklisted file (ours and the KnownPluginList's) and saves
    // both the blacklist and the plugin cache. Follow with startScan(true) to
    // scan them again.
    void clearBlacklist();
    juce::File getBlacklistFile() const;

    // True when the helper executable was found, so scans can't crash the DAW.
    bool isOutOfProcess() const;

    // The OpenDawPluginScanner executable beside the program, or invalid.
    static juce::File helperExecutable();

    static constexpr int kHelperTimeoutMs = kDefaultPluginScanTimeoutMs;

    enum class HelperOutcome {
        Scanned,    // the helper finished; found holds what it saw (maybe nothing)
        Crashed,    // the plugin crashed the helper: blacklist it
        TimedOut,   // still running after timeoutMs; killed, but not blacklisted
        Failed,     // the helper reported an error that isn't the plugin's fault
        NotStarted, // the helper couldn't be run at all
        Cancelled,  // *stop was set; the helper was killed
    };

    // Scans one file ("VST3" or "VST" format) in the helper, killing it after
    // timeoutMs or as soon as *stop becomes true. On anything but Scanned,
    // *error says why. Blocks the calling thread; usable from any thread.
    static HelperOutcome scanFileWithHelper(const juce::File& helper, const juce::String& formatName,
                                            const juce::String& file,
                                            juce::OwnedArray<juce::PluginDescription>& found,
                                            QString* error = nullptr,
                                            const std::atomic<bool>* stop = nullptr,
                                            int timeoutMs = kHelperTimeoutMs);

signals:
    void scanProgress(const QString& pluginName, int current, int total);
    void scanFinished();
    void pluginFailed(const QString& file, const QString& reason);

private:
    void loadBlacklist();
    void saveBlacklist() const;
    void addToBlacklist(const QString& file);
    juce::File getInProgressFile() const;

    AudioEngine& audioEngine_;
    QThread workerThread_;
    bool scanning_ = false;
    std::atomic<bool> stopRequested_ { false };
    QStringList blacklist_;
};

} // namespace OpenDaw
