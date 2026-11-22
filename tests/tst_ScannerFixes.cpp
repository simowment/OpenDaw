// Plugin scanner fixes: a plugin that only runs out of time is reported but
// never blacklisted, and clearBlacklist() also cleans the plugin cache.
//
// The timeout is forced with a 0 ms limit, so the real helper is killed right
// after it starts, before it can load anything: no slow plugin is needed.

#include "TestHarness.h"

#include "engine/PluginScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

using namespace OpenDaw;
using OpenDaw::Test::harness;

namespace {

juce::String toJ(const QString& s) { return juce::String(s.toUtf8().constData()); }
juce::File toFile(const QString& path) { return juce::File(toJ(QDir::toNativeSeparators(path))); }

bool mayTouchPluginState()
{
    return qEnvironmentVariableIsSet("CI") || qEnvironmentVariableIntValue("OPENDAW_TESTS_ALLOW_PLUGIN_STATE") != 0;
}

juce::File findHelper()
{
    if (auto h = PluginScanner::helperExecutable(); h.existsAsFile())
        return h;
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("OpenDawPluginScanner.exe");
#else
    const QString exe = QStringLiteral("OpenDawPluginScanner");
#endif
#ifdef OPENDAW_TOOLS_DIR
    if (const QFileInfo built(QDir(QString::fromUtf8(OPENDAW_TOOLS_DIR)).filePath(exe)); built.isFile())
        return toFile(built.absoluteFilePath());
#endif
    const QFileInfo beside(QDir(QCoreApplication::applicationDirPath()).filePath(exe));
    return beside.isFile() ? toFile(beside.absoluteFilePath()) : juce::File();
}

// A VST3 bundle holding junk: <dir>/<name>.vst3/Contents/<arch>/<binary>.
QString makeFakeVst3(const QString& dir, const QString& name)
{
    const QString bundle = QDir(dir).filePath(name + QStringLiteral(".vst3"));
#if defined(Q_OS_WIN)
    const QString inner = QStringLiteral("Contents/x86_64-win/") + name + QStringLiteral(".vst3");
#elif defined(Q_OS_MAC)
    const QString inner = QStringLiteral("Contents/MacOS/") + name;
#else
    const QString inner = QStringLiteral("Contents/x86_64-linux/") + name + QStringLiteral(".so");
#endif
    const QString binary = QDir(bundle).filePath(inner);
    if (!QDir().mkpath(QFileInfo(binary).absolutePath()))
        return {};
    QFile f(binary);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(QByteArray(4096, 'x'));
    return QDir::toNativeSeparators(bundle);
}

class FileRestorer {
public:
    explicit FileRestorer(const juce::File& f) : file_(f)
    {
        if (file_.existsAsFile()) {
            juce::MemoryBlock mb;
            if (file_.loadFileAsData(mb))
                saved_ = mb;
        }
    }
    ~FileRestorer()
    {
        if (saved_)
            file_.replaceWithData(saved_->getData(), saved_->getSize());
        else
            file_.deleteFile();
    }

private:
    juce::File file_;
    std::optional<juce::MemoryBlock> saved_;
};

} // namespace

class TestScannerFixes : public QObject {
    Q_OBJECT

private slots:
    void defaultTimeoutIsMinutes()
    {
        QVERIFY(PluginScanner::kHelperTimeoutMs >= 180000);
        QCOMPARE(PluginScanOptions{}.helperTimeoutMs, PluginScanner::kHelperTimeoutMs);
    }

    void helperTimeoutIsItsOwnOutcome()
    {
        const juce::File helper = findHelper();
        if (!helper.existsAsFile())
            QSKIP("OpenDawPluginScanner helper not found");

        const QString dir = harness().makeTempSubdir(QStringLiteral("scan-timeout"));
        const QString plugin = makeFakeVst3(dir, QStringLiteral("slow"));
        QVERIFY(!plugin.isEmpty());

        juce::OwnedArray<juce::PluginDescription> found;
        QString error;
        const auto outcome = PluginScanner::scanFileWithHelper(helper, "VST3", toJ(plugin), found, &error,
                                                               nullptr, /*timeoutMs*/ 0);
        QVERIFY2(outcome == PluginScanner::HelperOutcome::TimedOut, qPrintable(error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(found.size(), 0);
    }

    void timedOutPluginIsNotBlacklisted()
    {
        const juce::File helper = findHelper();
        if (!helper.existsAsFile())
            QSKIP("OpenDawPluginScanner helper not found");

        const QString dir = harness().makeTempSubdir(QStringLiteral("scan-worker"));
        const QString plugin = makeFakeVst3(dir, QStringLiteral("slow"));
        QVERIFY(!plugin.isEmpty());

        PluginScanOptions options;
        options.helper = helper;
        options.fullRescan = true;
        options.helperTimeoutMs = 0; // every file "takes too long"
        options.searchPathOverride = juce::FileSearchPath(toJ(QDir::toNativeSeparators(dir)));

        PluginScanWorker worker(std::move(options));
        QSignalSpy failed(&worker, &PluginScanWorker::pluginFailed);
        QSignalSpy finished(&worker, &PluginScanWorker::scanFinished);
        worker.doScan(); // synchronous on this thread

        QCOMPARE(finished.count(), 1);
        bool sawPlugin = false;
        for (const auto& args : failed) {
            const QString file = args.value(0).toString();
            if (toFile(file) == toFile(plugin) || toFile(file).isAChildOf(toFile(plugin)))
                sawPlugin = true;
            QVERIFY2(!args.value(2).toBool(),
                     qPrintable(QStringLiteral("timed-out file was marked for the blacklist: ") + file));
            QVERIFY(!args.value(1).toString().isEmpty());
        }
        QVERIFY2(sawPlugin, "the fake plugin was not scanned (or not reported as timed out)");
    }

    void clearBlacklistCleansTheCache()
    {
        if (!mayTouchPluginState())
            QSKIP("writes the per-user plugin cache and blacklist; runs on CI or with OPENDAW_TESTS_ALLOW_PLUGIN_STATE=1");

        auto& scanner = harness().pluginScanner();
        FileRestorer keepCache(scanner.getCacheFile());
        FileRestorer keepBlacklist(scanner.getBlacklistFile());

        const QString dir = harness().makeTempSubdir(QStringLiteral("scan-blacklist"));
        const juce::String victim = toJ(QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("bad.vst3"))));

        auto& list = scanner.getPluginList();
        const auto before = list.getBlacklistedFiles();
        list.addToBlacklist(victim);
        scanner.saveCachedList();
        QVERIFY(scanner.getCacheFile().loadFileAsString().contains(victim));

        scanner.clearBlacklist();
        QVERIFY(scanner.blacklist().isEmpty());
        QVERIFY(list.getBlacklistedFiles().isEmpty());
        // Written back, so the entry does not return on the next start.
        QVERIFY(!scanner.getCacheFile().loadFileAsString().contains(victim));

        for (const auto& f : before) // leave the shared list as we found it
            list.addToBlacklist(f);
    }
};

OD_REGISTER_TEST(TestScannerFixes)
#include "tst_ScannerFixes.moc"
