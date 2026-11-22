// TypingKeyboard: computer keys play the selected channel. Real QKeyEvents
// with Windows scan codes (as an AZERTY keyboard sends them) are delivered to
// a focused offscreen widget through QApplication, so the app-wide event
// filter, shortcut blocking and focus rules are exercised as in the app.

#include "TestHarness.h"
#include "engine/EditManager.h"
#include "ui/TypingKeyboard.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>

using namespace OpenDaw;
using OpenDaw::Test::harness;

namespace {

// Physical key -> (scan code, the Qt key an AZERTY layout reports for it).
struct PhysKey { quint32 scan; Qt::Key azertyKey; };
const PhysKey kLowerC{0x2C, Qt::Key_W};   // bottom-left letter (W on AZERTY, Z on QWERTY)
const PhysKey kLowerCs{0x1F, Qt::Key_S};
const PhysKey kUpperC{0x10, Qt::Key_A};   // top-left letter (A on AZERTY, Q on QWERTY)
const PhysKey kRecordR{0x13, Qt::Key_R};  // F of the upper row; R is also the Record shortcut

void sendKey(QWidget* to, QEvent::Type type, const PhysKey& k, Qt::KeyboardModifiers mods = Qt::NoModifier,
             bool autoRepeat = false)
{
    QKeyEvent ev(type, k.azertyKey, mods, k.scan, 0, 0, QString(), autoRepeat);
    QApplication::sendEvent(to, &ev);
}

} // namespace

class TestTypingKeyboard : public QObject {
    Q_OBJECT

    std::unique_ptr<QWidget> window_;
    QWidget* focusTarget_ = nullptr;
    QLineEdit* edit_ = nullptr;
    QAction* recordShortcut_ = nullptr;
    int recordShortcutHits_ = 0;

private slots:
    void initTestCase()
    {
        QSettings().remove("input/typingKeyboard");
        QSettings().remove("input/typingKeyboardOctave");
        window_ = std::make_unique<QWidget>();
        auto* lay = new QVBoxLayout(window_.get());
        focusTarget_ = new QWidget(window_.get());
        focusTarget_->setFocusPolicy(Qt::StrongFocus);
        edit_ = new QLineEdit(window_.get());
        lay->addWidget(focusTarget_);
        lay->addWidget(edit_);
        // A single-key window shortcut like the app's R = Record.
        recordShortcut_ = new QAction(window_.get());
        recordShortcut_->setShortcut(QKeySequence(Qt::Key_R));
        recordShortcut_->setShortcutContext(Qt::WindowShortcut);
        window_->addAction(recordShortcut_);
        connect(recordShortcut_, &QAction::triggered, this, [this]() { ++recordShortcutHits_; });
        window_->show();
        window_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));
    }

    void init()
    {
        harness().resetProject();
        focusTarget_->setFocus();
        QTRY_VERIFY(focusTarget_->hasFocus());
        recordShortcutHits_ = 0;
    }

    void physicalKeysPlayTheTarget()
    {
        auto* track = harness().editManager().addMidiTrack();
        QVERIFY(track);
        TypingKeyboard kb(harness().editManager());
        kb.setTarget(track->itemID);
        QVERIFY(kb.isEnabled());   // on by default, like FL
        QSignalSpy played(&kb, &TypingKeyboard::notePlayed);

        sendKey(focusTarget_, QEvent::KeyPress, kLowerC);
        sendKey(focusTarget_, QEvent::KeyPress, kLowerC, Qt::NoModifier, true);   // auto-repeat: ignored
        sendKey(focusTarget_, QEvent::KeyPress, kLowerCs);
        sendKey(focusTarget_, QEvent::KeyPress, kUpperC);
        QCOMPARE(played.count(), 3);
        QCOMPARE(played.at(0).at(0).toInt(), 48);   // lower row C (C3)
        QCOMPARE(played.at(1).at(0).toInt(), 49);
        QCOMPARE(played.at(2).at(0).toInt(), 60);   // upper row C (C4)
        for (int i = 0; i < 3; ++i) QVERIFY(played.at(i).at(1).toBool());

        sendKey(focusTarget_, QEvent::KeyRelease, kLowerC);
        sendKey(focusTarget_, QEvent::KeyRelease, kLowerCs);
        sendKey(focusTarget_, QEvent::KeyRelease, kUpperC);
        QCOMPARE(played.count(), 6);
        for (int i = 3; i < 6; ++i) QVERIFY(!played.at(i).at(1).toBool());
    }

    void mappedKeysBeatSingleKeyShortcuts()
    {
        auto* track = harness().editManager().addMidiTrack();
        TypingKeyboard kb(harness().editManager());
        kb.setTarget(track->itemID);
        QSignalSpy played(&kb, &TypingKeyboard::notePlayed);
        QTest::keyClick(focusTarget_, Qt::Key_R);   // goes through the shortcut system
        // QTest::keyClick has no scan code: the fallback map still covers R.
        QCOMPARE(recordShortcutHits_, 0);
        QVERIFY(played.count() >= 1);
    }

    void textFieldsAndModifiersAreLeftAlone()
    {
        auto* track = harness().editManager().addMidiTrack();
        TypingKeyboard kb(harness().editManager());
        kb.setTarget(track->itemID);
        QSignalSpy played(&kb, &TypingKeyboard::notePlayed);

        sendKey(focusTarget_, QEvent::KeyPress, kLowerC, Qt::ControlModifier);   // Ctrl+key: a shortcut
        QCOMPARE(played.count(), 0);

        edit_->setFocus();
        QTRY_VERIFY(edit_->hasFocus());
        sendKey(edit_, QEvent::KeyPress, kLowerC);
        sendKey(edit_, QEvent::KeyRelease, kLowerC);
        QCOMPARE(played.count(), 0);
    }

    void disabledDoesNothingAndReleasesHeldNotes()
    {
        auto* track = harness().editManager().addMidiTrack();
        TypingKeyboard kb(harness().editManager());
        kb.setTarget(track->itemID);
        QSignalSpy played(&kb, &TypingKeyboard::notePlayed);
        sendKey(focusTarget_, QEvent::KeyPress, kLowerC);
        QCOMPARE(played.count(), 1);
        kb.setEnabled(false);                       // the held note is released
        QCOMPARE(played.count(), 2);
        QVERIFY(!played.at(1).at(1).toBool());
        sendKey(focusTarget_, QEvent::KeyPress, kUpperC);
        QCOMPARE(played.count(), 2);
        kb.setEnabled(true);
    }

    void numpadShiftsTheOctave()
    {
        auto* track = harness().editManager().addMidiTrack();
        TypingKeyboard kb(harness().editManager());
        kb.setTarget(track->itemID);
        QSignalSpy played(&kb, &TypingKeyboard::notePlayed);
        QKeyEvent plus(QEvent::KeyPress, Qt::Key_Plus, Qt::KeypadModifier, 0x4E, 0, 0);
        QApplication::sendEvent(focusTarget_, &plus);
        QCOMPARE(kb.octaveBase(), 60);
        sendKey(focusTarget_, QEvent::KeyPress, kLowerC);
        QCOMPARE(played.last().at(0).toInt(), 60);
        sendKey(focusTarget_, QEvent::KeyRelease, kLowerC);
        kb.setOctaveBase(48);
    }

    void cleanupTestCase()
    {
        window_.reset();
        QSettings().remove("input/typingKeyboard");
        QSettings().remove("input/typingKeyboardOctave");
    }
};

OD_REGISTER_TEST(TestTypingKeyboard)
#include "tst_TypingKeyboard.moc"
