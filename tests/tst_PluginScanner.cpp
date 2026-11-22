// Plugin scanner: crash isolation through the OpenDawPluginScanner helper and
// blacklist persistence. Small and tolerant on purpose.
//
// PluginScanner keeps its cache and blacklist under JUCE's
// userApplicationDataDirectory/OpenDaw, which cannot be redirected. Tests
// that would write there (a full scan, the blacklist) therefore run only on CI
// (env CI set, as GitHub Actions does) or when OPENDAW_TESTS_ALLOW_PLUGIN_STATE=1,
// and they restore the files they touch.

#include "TestHarness.h"

#include "engine/PluginScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

using namespace OpenDaw;
using OpenDaw::Test::harness;

namespace {

QString toQ(const juce::String& s) { return QString::fromStdString(s.toStdString()); }
juce::String toJ(const QString& s) { return juce::String(s.toUtf8().constData()); }

bool mayTouchPluginState()
{
    return qEnvironmentVariableIsSet("CI") || qEnvironmentVariableIntValue("OPENDAW_TESTS_ALLOW_PLUGIN_STATE") != 0;
}

// The helper beside the test executable, beside the app, or wherever
// PluginScanner itself would look.
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
    // Where the build puts the helpers ($<TARGET_FILE_DIR:OpenDaw>).
    if (const QFileInfo built(QDir(QString::fromUtf8(OPENDAW_TOOLS_DIR)).filePath(exe)); built.isFile())
        return juce::File(toJ(QDir::toNativeSeparators(built.absoluteFilePath())));
#endif
    const QDir here(QCoreApplication::applicationDirPath());
    for (const QString& rel : {QString(), QStringLiteral("OpenDaw_artefacts/Release/"),
                               QStringLiteral("OpenDaw_artefacts/Debug/"),
                               QStringLiteral("OpenDaw_artefacts/RelWithDebInfo/"),
                               QStringLiteral("OpenDaw_artefacts/"),
                               QStringLiteral("OpenDawPluginScanner_artefacts/Release/"),
                               QStringLiteral("OpenDawPluginScanner_artefacts/Debug/")}) {
        const QFileInfo fi(here.filePath(rel + exe));
        if (fi.isFile())
            return juce::File(toJ(QDir::toNativeSeparators(fi.absoluteFilePath())));
    }
    return {};
}

// Restores a file to what it held when constructed (or deletes it).
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

class TestPluginScanner : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }

    void basics()
    {
        auto& scanner = harness().pluginScanner();
        QVERIFY(!scanner.isScanning());
        QCOMPARE(scanner.isOutOfProcess(), PluginScanner::helperExecutable().existsAsFile());
        QVERIFY(scanner.getBlacklistFile().getParentDirectory() == scanner.getCacheFile().getParentDirectory());
        // Readable without side effects.
        const QStringList bl = scanner.blacklist();
        Q_UNUSED(bl);
    }

    void helperMissingIsReported()
    {
        juce::OwnedArray<juce::PluginDescription> found;
        QString error;
        const auto outcome = PluginScanner::scanFileWithHelper(
            juce::File(toJ(QDir::toNativeSeparators(dir_.filePath("no-such-helper")))), "VST3",
            toJ(QDir::toNativeSeparators(dir_.filePath("x.vst3"))), found, &error, nullptr, 5000);
        QVERIFY(outcome == PluginScanner::HelperOutcome::NotStarted);
        QVERIFY(!error.isEmpty());
        QCOMPARE(found.size(), 0);
    }

    void garbagePluginDoesNotCrashUs_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("bundle");
        QTest::newRow("vst3 file") << QStringLiteral("VST3") << false;
        QTest::newRow("vst3 bundle") << QStringLiteral("VST3") << true;
#ifdef Q_OS_WIN
        QTest::newRow("vst2 dll") << QStringLiteral("VST") << false;
#endif
    }

    void garbagePluginDoesNotCrashUs()
    {
        QFETCH(QString, format);
        QFETCH(bool, bundle);

        const juce::File helper = findHelper();
        if (!helper.existsAsFile())
            QSKIP("OpenDawPluginScanner helper not found next to the test executable");

        // Random bytes dressed up as a plugin.
        const QString sub = harness().makeTempSubdir(QStringLiteral("fake-plugins"));
        const QString name = format == QLatin1String("VST") ? QStringLiteral("crash.dll") : QStringLiteral("crash.vst3");
        const QString pluginPath = QDir(sub).filePath(name);
        QString binaryPath = pluginPath;
        if (bundle) {
            // The VST3 bundle layout: <name>.vst3/Contents/<arch>/<binary>.
#if defined(Q_OS_WIN)
            const QString inner = QStringLiteral("Contents/x86_64-win/crash.vst3");
#elif defined(Q_OS_MAC)
            const QString inner = QStringLiteral("Contents/MacOS/crash");
#else
            const QString inner = QStringLiteral("Contents/x86_64-linux/crash.so");
#endif
            binaryPath = QDir(pluginPath).filePath(inner);
            QVERIFY(QDir().mkpath(QFileInfo(binaryPath).absolutePath()));
        }
        {
            QFile f(binaryPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QByteArray junk(64 * 1024, Qt::Uninitialized);
            auto* rng = QRandomGenerator::global();
            for (auto& c : junk)
                c = char(rng->bounded(256));
            // Start with a PE/ELF-looking magic so loaders actually try it.
#if defined(Q_OS_WIN)
            junk[0] = 'M'; junk[1] = 'Z';
#else
            junk[0] = 0x7f; junk[1] = 'E'; junk[2] = 'L'; junk[3] = 'F';
#endif
            f.write(junk);
        }

        juce::OwnedArray<juce::PluginDescription> found;
        QString error;
        const auto outcome = PluginScanner::scanFileWithHelper(
            helper, toJ(format), toJ(QDir::toNativeSeparators(pluginPath)), found, &error, nullptr, 30000);

        // We are still here: whatever the helper did, it did it in its own process.
        QVERIFY2(outcome != PluginScanner::HelperOutcome::NotStarted, qPrintable(error));
        QVERIFY(outcome != PluginScanner::HelperOutcome::Cancelled);
        QCOMPARE(found.size(), 0);
        if (outcome != PluginScanner::HelperOutcome::Scanned)
            QVERIFY(!error.isEmpty());
    }

    void blacklistPersistence()
    {
        if (!mayTouchPluginState())
            QSKIP("writes the per-user plugin blacklist; runs on CI or with OPENDAW_TESTS_ALLOW_PLUGIN_STATE=1");

        auto& scanner = harness().pluginScanner();
        const juce::File blFile = scanner.getBlacklistFile();
        const juce::File pedal = blFile.getSiblingFile("plugin-scan-in-progress.txt");
        FileRestorer keepBlacklist(blFile);
        FileRestorer keepPedal(pedal);
        FileRestorer keepCache(scanner.getCacheFile());

        // A scan that "crashed" on this file leaves it in the in-progress
        // file; the next PluginScanner blacklists it and saves the list.
        const QString victim = QDir::toNativeSeparators(dir_.filePath("crashed-last-time.vst3"));
        blFile.getParentDirectory().createDirectory();
        QVERIFY(pedal.replaceWithText(toJ(victim)));

        {
            PluginScanner second(harness().audioEngine());
            QVERIFY2(second.blacklist().contains(victim, Qt::CaseInsensitive), qPrintable(second.blacklist().join(", ")));
            QVERIFY(!pedal.existsAsFile());
            QVERIFY(blFile.existsAsFile());
            QVERIFY(toQ(blFile.loadFileAsString()).contains(victim, Qt::CaseInsensitive));
        }
        {
            PluginScanner third(harness().audioEngine());
            QVERIFY(third.blacklist().contains(victim, Qt::CaseInsensitive)); // loaded from disk
            third.clearBlacklist();
            QVERIFY(third.blacklist().isEmpty());
        }
        {
            PluginScanner fourth(harness().audioEngine());
            QVERIFY(fourth.blacklist().isEmpty());
        }
    }

    void scanCompletes()
    {
        if (!mayTouchPluginState())
            QSKIP("rewrites the per-user plugin cache; runs on CI or with OPENDAW_TESTS_ALLOW_PLUGIN_STATE=1");

        auto& scanner = harness().pluginScanner();
        FileRestorer keepCache(scanner.getCacheFile());
        FileRestorer keepBlacklist(scanner.getBlacklistFile());

        QSignalSpy finished(&scanner, &PluginScanner::scanFinished);
        QSignalSpy failed(&scanner, &PluginScanner::pluginFailed);
        scanner.startScan(false);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 300000);
        QVERIFY(!scanner.isScanning());
        for (const auto& args : failed)
            qInfo() << "plugin failed:" << args.value(0).toString() << args.value(1).toString();

        // A second scan right after also finishes (nothing new to scan).
        scanner.startScan(false);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 2, 300000);
    }
};

OD_REGISTER_TEST(TestPluginScanner)
#include "tst_PluginScanner.moc"
