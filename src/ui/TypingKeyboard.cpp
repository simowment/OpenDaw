#include "TypingKeyboard.h"
#include "engine/EditManager.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTextEdit>
#include <algorithm>

namespace OpenDaw {

namespace {

const QString kEnabledKey = QStringLiteral("input/typingKeyboard");
const QString kOctaveKey = QStringLiteral("input/typingKeyboardOctave");

// Semitone offset from the octave base for each physical key.
#ifdef Q_OS_WIN
// PC set-1 scan codes (what QKeyEvent::nativeScanCode reports on Windows):
// physical positions, independent of the keyboard layout.
const QHash<quint32, int>& scanMap()
{
    static const QHash<quint32, int> m{
        // lower row: Z S X D C V G B H N J M ,
        {0x2C, 0}, {0x1F, 1}, {0x2D, 2}, {0x20, 3}, {0x2E, 4}, {0x2F, 5}, {0x22, 6},
        {0x30, 7}, {0x23, 8}, {0x31, 9}, {0x24, 10}, {0x32, 11}, {0x33, 12},
        // upper row: Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P
        {0x10, 12}, {0x03, 13}, {0x11, 14}, {0x04, 15}, {0x12, 16}, {0x13, 17}, {0x06, 18},
        {0x14, 19}, {0x07, 20}, {0x15, 21}, {0x08, 22}, {0x16, 23}, {0x17, 24}, {0x0A, 25},
        {0x18, 26}, {0x0B, 27}, {0x19, 28},
    };
    return m;
}
#endif

// Fallback by key (QWERTY positions) where scan codes are not usable.
const QHash<int, int>& keyMap()
{
    static const QHash<int, int> m{
        {Qt::Key_Z, 0}, {Qt::Key_S, 1}, {Qt::Key_X, 2}, {Qt::Key_D, 3}, {Qt::Key_C, 4},
        {Qt::Key_V, 5}, {Qt::Key_G, 6}, {Qt::Key_B, 7}, {Qt::Key_H, 8}, {Qt::Key_N, 9},
        {Qt::Key_J, 10}, {Qt::Key_M, 11}, {Qt::Key_Comma, 12},
        {Qt::Key_Q, 12}, {Qt::Key_2, 13}, {Qt::Key_W, 14}, {Qt::Key_3, 15}, {Qt::Key_E, 16},
        {Qt::Key_R, 17}, {Qt::Key_5, 18}, {Qt::Key_T, 19}, {Qt::Key_6, 20}, {Qt::Key_Y, 21},
        {Qt::Key_7, 22}, {Qt::Key_U, 23}, {Qt::Key_I, 24}, {Qt::Key_9, 25}, {Qt::Key_O, 26},
        {Qt::Key_0, 27}, {Qt::Key_P, 28},
    };
    return m;
}

quint32 physicalKey(const QKeyEvent* e)
{
#ifdef Q_OS_WIN
    if (e->nativeScanCode() != 0) return e->nativeScanCode();
#endif
    return quint32(e->key()) | 0x80000000u;
}

} // namespace

TypingKeyboard::TypingKeyboard(EditManager& editMgr, QObject* parent)
    : QObject(parent), editMgr_(editMgr)
{
    QSettings s;
    enabled_ = s.value(kEnabledKey, true).toBool();
    octaveBase_ = std::clamp(s.value(kOctaveKey, 48).toInt(), 0, 108);
    qApp->installEventFilter(this);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
        if (st != Qt::ApplicationActive) allNotesOff();
    });
    connect(&editMgr_, &EditManager::aboutToChangeEdit, this, [this]() { allNotesOff(); target_ = {}; });
}

TypingKeyboard::~TypingKeyboard()
{
    allNotesOff();
    if (qApp) qApp->removeEventFilter(this);
}

void TypingKeyboard::setEnabled(bool on)
{
    if (enabled_ == on) return;
    if (!on) allNotesOff();
    enabled_ = on;
    QSettings().setValue(kEnabledKey, on);
    emit enabledChanged(on);
}

void TypingKeyboard::setTarget(te::EditItemID trackId)
{
    if (target_ == trackId) return;
    allNotesOff();
    target_ = trackId;
}

void TypingKeyboard::setOctaveBase(int note)
{
    note = std::clamp(note, 0, 108);
    if (note == octaveBase_) return;
    allNotesOff();
    octaveBase_ = note;
    QSettings().setValue(kOctaveKey, note);
    emit octaveChanged(note);
}

te::AudioTrack* TypingKeyboard::targetTrack() const
{
    if (!editMgr_.edit()) return nullptr;
    if (target_.isValid())
        if (auto* t = editMgr_.findAudioTrack(target_))
            return t;
    for (auto* t : editMgr_.getAudioTracksInDisplayOrder())
        if (t && editMgr_.isMidiTrack(t) && !editMgr_.isBusTrack(t))
            return t;
    return nullptr;
}

void TypingKeyboard::send(te::AudioTrack* t, int note, bool on)
{
    if (!t) return;
    const auto msg = on ? juce::MidiMessage::noteOn(1, note, juce::uint8(100))
                        : juce::MidiMessage::noteOff(1, note);
    t->injectLiveMidiMessage(msg, 0);
    emit notePlayed(note, on);
}

void TypingKeyboard::allNotesOff()
{
    if (held_.isEmpty()) return;
    if (editMgr_.edit())
        if (auto* t = editMgr_.findAudioTrack(heldTrack_))
            for (int note : std::as_const(held_))
                send(t, note, false);
    held_.clear();
}

int TypingKeyboard::noteForKey(const QKeyEvent* e) const
{
    int offset = -1;
#ifdef Q_OS_WIN
    if (e->nativeScanCode() != 0) {
        // Extended keys (arrows, numpad Enter...) share scan codes with others.
        if (e->nativeModifiers() & 0x01000000u) return -1;
        offset = scanMap().value(e->nativeScanCode(), -1);
    } else
#endif
    {
        offset = keyMap().value(e->key(), -1);
    }
    if (offset < 0) return -1;
    const int note = octaveBase_ + offset;
    return (note >= 0 && note <= 127) ? note : -1;
}

bool TypingKeyboard::isOctaveKey(const QKeyEvent* e, int* direction)
{
    if (!(e->modifiers() & Qt::KeypadModifier)) return false;
    if (e->key() == Qt::Key_Plus)  { *direction = 1;  return true; }
    if (e->key() == Qt::Key_Minus) { *direction = -1; return true; }
    return false;
}

bool TypingKeyboard::shouldHandle(const QKeyEvent* e) const
{
    if (!enabled_ || !editMgr_.edit()) return false;
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
    auto* focus = QApplication::focusWidget();
    if (!focus) return QApplication::activeWindow() != nullptr;
    if (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus)
        || qobject_cast<QPlainTextEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus))
        return false;
    if (auto* combo = qobject_cast<QComboBox*>(focus); combo && combo->isEditable()) return false;
    // The piano roll has its own musical typing (with step input); let it work.
    for (auto* w = focus; w; w = w->parentWidget())
        if (w->inherits("OpenDaw::NoteGrid")) return false;
    return true;
}

bool TypingKeyboard::eventFilter(QObject* watched, QEvent* event)
{
    const auto type = event->type();
    if (type != QEvent::KeyPress && type != QEvent::KeyRelease && type != QEvent::ShortcutOverride)
        return QObject::eventFilter(watched, event);
    // The filter sees each key event once per receiver up the chain; act only
    // on the widget it was sent to.
    if (!watched->isWidgetType() || watched != QApplication::focusWidget()
        && !(QApplication::focusWidget() == nullptr && watched == QApplication::activeWindow()))
        return QObject::eventFilter(watched, event);

    auto* e = static_cast<QKeyEvent*>(event);
    const quint32 phys = physicalKey(e);

    // Releases always end a held note, even if focus moved meanwhile.
    if (type == QEvent::KeyRelease && !e->isAutoRepeat() && held_.contains(phys)) {
        const int note = held_.take(phys);
        if (auto* t = editMgr_.edit() ? editMgr_.findAudioTrack(heldTrack_) : nullptr)
            send(t, note, false);
        return true;
    }

    if (!shouldHandle(e)) return QObject::eventFilter(watched, event);

    int dir = 0;
    if (isOctaveKey(e, &dir)) {
        if (type == QEvent::ShortcutOverride) { e->accept(); return true; }
        if (type == QEvent::KeyPress) setOctaveBase(octaveBase_ + 12 * dir);
        return true;
    }

    const int note = noteForKey(e);
    if (note < 0) return QObject::eventFilter(watched, event);

    if (type == QEvent::ShortcutOverride) {
        // Claim the key so single-key shortcuts (S = split, R = record...) don't fire.
        e->accept();
        return true;
    }
    if (type == QEvent::KeyPress) {
        if (e->isAutoRepeat() || held_.contains(phys)) return true;
        auto* t = targetTrack();
        if (!t) return true;
        if (!held_.isEmpty() && heldTrack_ != t->itemID) allNotesOff();
        heldTrack_ = t->itemID;
        held_.insert(phys, note);
        send(t, note, true);
        return true;
    }
    return true;   // auto-repeat releases of a mapped key
}

} // namespace OpenDaw
