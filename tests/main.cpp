// OpenDawTests: one process, one JUCE/Tracktion initialisation, every test
// class run in turn with QTest::qExec.
//
//   OpenDawTests                       run everything
//   OpenDawTests --class TestPatterns  run one class (repeatable)
//   OpenDawTests --list-classes        print the class names
//   any other argument is passed to QTest (e.g. -v2, a test function name)

#include "TestHarness.h"

#include <QApplication>
#include <QStringList>
#include <QtTest>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    // Headless by default (CI, ctest); an explicit setting wins.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    // Split our own options from the ones QTest understands.
    QStringList onlyClasses;
    bool listOnly = false;
    std::vector<char*> qtestArgs;
    qtestArgs.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a == "--class" && i + 1 < argc) {
            onlyClasses << QString::fromLocal8Bit(argv[++i]);
        } else if (a == "--list-classes") {
            listOnly = true;
        } else {
            qtestArgs.push_back(argv[i]);
        }
    }
    if (const QByteArray env = qgetenv("OPENDAW_TEST_CLASS"); !env.isEmpty())
        onlyClasses << QString::fromLocal8Bit(env).split(',', Qt::SkipEmptyParts);

    // QApplication (not QGuiApplication) so widget-level logic such as the
    // typing keyboard's event filter can be tested; still headless (offscreen).
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenDaw-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("OpenDawTests"));

    if (listOnly) {
        for (const auto& t : OpenDaw::Test::registeredTests())
            std::printf("%s\n", t.name);
        return 0;
    }

    OpenDaw::Test::TestHarness harness;
    QString error;
    if (!harness.init(&error)) {
        std::fprintf(stderr, "OpenDawTests: harness initialisation failed: %s\n", qPrintable(error));
        return 2;
    }

    int failedClasses = 0;
    int ran = 0;
    for (const auto& t : OpenDaw::Test::registeredTests()) {
        if (!onlyClasses.isEmpty() && !onlyClasses.contains(QString::fromLatin1(t.name)))
            continue;
        ++ran;
        harness.resetProject();
        std::unique_ptr<QObject> test = t.create();
        const int rc = QTest::qExec(test.get(), int(qtestArgs.size()), qtestArgs.data());
        if (rc != 0) {
            ++failedClasses;
            std::fprintf(stderr, "OpenDawTests: %s had %d failure(s)\n", t.name, rc);
        }
    }

    if (ran == 0) {
        std::fprintf(stderr, "OpenDawTests: no test class matched\n");
        return 3;
    }

    harness.shutdown();
    if (failedClasses > 0)
        std::fprintf(stderr, "OpenDawTests: %d of %d test classes failed\n", failedClasses, ran);
    else
        std::printf("OpenDawTests: all %d test classes passed\n", ran);
    return failedClasses == 0 ? 0 : 1;
}
