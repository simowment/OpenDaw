#include "ChannelRow.h"
#include "StepRow.h"
#include "ui/controls/RotaryKnob.h"
#include "utils/IconFont.h"
#include "utils/ThemeManager.h"
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QDir>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace OpenDaw {

// ── ChannelLed ──────────────────────────────────────────────────────────────

ChannelLed::ChannelLed(QWidget* parent)
    : QWidget(parent)
{
    setAccessibleName("Channel Mute");
    setToolTip("Mute: click to switch the channel off or on");
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
}

void ChannelLed::setLit(bool lit)
{
    if (lit == lit_) return;
    lit_ = lit;
    update();
}

void ChannelLed::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
    if (lit_) {
        QRadialGradient glow(r.center(), r.width() * 0.7);
        glow.setColorAt(0.0, theme.stepPlaying.lighter(140));
        glow.setColorAt(1.0, theme.stepPlaying);
        p.setBrush(glow);
    } else {
        p.setBrush(theme.controlTrack);
    }
    p.setPen(QPen(theme.separator, 1.0));
    p.drawEllipse(r);
}

void ChannelLed::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && onClicked) {
        onClicked();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// ── ChannelNameButton ───────────────────────────────────────────────────────

namespace {

QString kindTip(ChannelSoundKind kind)
{
    switch (kind) {
    case ChannelSoundKind::Sample:
        return QStringLiteral("Click to hear the sample, double-click for the piano roll, "
                              "right-click to replace the sound and more. Drop an audio file "
                              "here to replace it.");
    case ChannelSoundKind::Plugin:
        return QStringLiteral("Click to hear the channel, double-click to open the plugin, "
                              "right-click to replace the sound and more.");
    case ChannelSoundKind::BuiltIn:
        return QStringLiteral("Click to hear the channel, double-click for the piano roll, "
                              "right-click to replace the sound and more.");
    case ChannelSoundKind::None:
        break;
    }
    return QStringLiteral("This channel has no sound yet: right-click > Replace sound, or drop "
                          "an audio file here.");
}

} // namespace

ChannelNameButton::ChannelNameButton(QWidget* parent)
    : QWidget(parent)
{
    setAccessibleName("Channel Name");
    setToolTip(kindTip(kind_));
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
}

void ChannelNameButton::setText(const QString& text)
{
    if (text == text_) return;
    text_ = text;
    setAccessibleDescription(text);
    update();
}

void ChannelNameButton::setKind(ChannelSoundKind kind)
{
    if (kind == kind_) return;
    kind_ = kind;
    update();
}

void ChannelNameButton::setColour(const QColor& colour)
{
    colour_ = colour;
    update();
}

void ChannelNameButton::setSelected(bool selected)
{
    if (selected == selected_) return;
    selected_ = selected;
    update();
}

void ChannelNameButton::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double radius = std::clamp(theme.cornerRadius, 0, 4);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QColor bg = selected_ ? theme.channelButton.lighter(118) : theme.channelButton;
    if (hover_ && !selected_) bg = bg.lighter(108);
    p.setPen(QPen(selected_ ? theme.accent : theme.separator, selected_ ? 1.5 : 1.0));
    p.setBrush(bg);
    p.drawRoundedRect(r, radius, radius);

    // Channel colour tab on the left.
    if (colour_.isValid()) {
        p.setPen(Qt::NoPen);
        p.setBrush(colour_);
        p.drawRect(QRectF(r.left() + 2.0, r.top() + 3.0, 3.0, r.height() - 6.0));
    }

    const QColor textColour = selected_ ? ThemeManager::readableTextOn(bg) : theme.text;

    // The sound-source badge.
    constexpr int kBadge = 14;
    const QRect badge(8, (height() - kBadge) / 2, kBadge, kBadge);
    QColor glyph = textColour;
    glyph.setAlpha(selected_ ? 235 : 165);
    p.setPen(glyph);
    switch (kind_) {
    case ChannelSoundKind::Sample:
        p.setFont(icons::fontAudio(13));
        p.drawText(badge, Qt::AlignCenter, QString(icons::fa::Waveform));
        break;
    case ChannelSoundKind::Plugin:
        p.setFont(icons::fontAudio(13));
        p.drawText(badge, Qt::AlignCenter, QString(icons::fa::Midiplug));
        break;
    case ChannelSoundKind::BuiltIn:
        p.setFont(icons::materialIcons(13));
        p.drawText(badge, Qt::AlignCenter, QString(icons::mi::Piano));
        break;
    case ChannelSoundKind::None: {
        QFont q = font();
        q.setPixelSize(11);
        q.setBold(true);
        p.setFont(q);
        p.setPen(QPen(theme.textDim, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(badge).adjusted(1.0, 1.0, -1.0, -1.0));
        p.drawText(badge, Qt::AlignCenter, QStringLiteral("?"));
        break;
    }
    }

    QFont f = font();
    f.setPixelSize(11);
    f.setBold(selected_);
    p.setFont(f);
    p.setPen(kind_ == ChannelSoundKind::None ? theme.textDim : textColour);
    const QRect textRect = rect().adjusted(badge.right() + 5, 0, -4, 0);
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(f).elidedText(text_, Qt::ElideRight, textRect.width()));
}

void ChannelNameButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (onClicked) onClicked();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ChannelNameButton::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (onDoubleClicked) onDoubleClicked();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void ChannelNameButton::contextMenuEvent(QContextMenuEvent* event)
{
    if (onContextMenu) onContextMenu(event->globalPos());
    event->accept();
}

void ChannelNameButton::enterEvent(QEnterEvent* event)
{
    hover_ = true;
    update();
    QWidget::enterEvent(event);
}

void ChannelNameButton::leaveEvent(QEvent* event)
{
    hover_ = false;
    update();
    QWidget::leaveEvent(event);
}

// ── ChannelRow ──────────────────────────────────────────────────────────────

ChannelRow::ChannelRow(EditManager* editMgr, PatternManager* patterns, te::AudioTrack* track,
                       QWidget* parent)
    : QWidget(parent), editMgr_(editMgr), patterns_(patterns),
      trackId_(track ? track->itemID : te::EditItemID())
{
    setAccessibleName("Channel");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* left = new QWidget(this);
    left->setFixedSize(kLeftWidth, kHeight);
    auto* leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(4, 0, 8, 0);
    leftLayout->setSpacing(0);

    led_ = new ChannelLed(left);
    leftLayout->addWidget(led_, 0, Qt::AlignVCenter);
    leftLayout->addSpacing(6);

    panKnob_ = new RotaryKnob(left);
    panKnob_->setAccessibleName("Channel Pan");
    panKnob_->setRange(-1.0, 1.0);
    panKnob_->setFixedSize(24, 24);
    leftLayout->addWidget(panKnob_, 0, Qt::AlignVCenter);
    leftLayout->addSpacing(2);

    volumeKnob_ = new RotaryKnob(left);
    volumeKnob_->setAccessibleName("Channel Volume");
    volumeKnob_->setRange(0.0, 1.0);
    volumeKnob_->setFixedSize(24, 24);
    leftLayout->addWidget(volumeKnob_, 0, Qt::AlignVCenter);
    leftLayout->addSpacing(6);

    nameBtn_ = new ChannelNameButton(left);
    leftLayout->addWidget(nameBtn_, 0, Qt::AlignVCenter);
    layout->addWidget(left);

    steps_ = new StepRow(this);
    layout->addWidget(steps_, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    led_->onClicked = [this]() {
        auto* t = this->track();
        if (!t || !editMgr_) return;
        t->setMute(!t->isMuted(false));
        led_->setLit(!t->isMuted(false));
        // The mixer and the track headers show mute too.
        editMgr_->notifyEdited();
    };
    nameBtn_->onClicked = [this]() {
        // Like FL: clicking a channel plays its sound.
        if (kind_ != ChannelSoundKind::None) previewSound();
        emit selectRequested(trackId_);
    };
    nameBtn_->onDoubleClicked = [this]() { emit openRequested(trackId_); };
    nameBtn_->onContextMenu = [this](const QPoint& pos) { emit menuRequested(trackId_, pos); };

    connect(panKnob_, &RotaryKnob::valueChanged, this, [this](double v) {
        auto* t = this->track();
        if (!t) return;
        if (auto* vp = t->getVolumePlugin()) vp->setPan(float(v));
        panKnob_->setToolTip(std::abs(v) < 0.005 ? QString("Pan: centre")
                             : QString("Pan: %1% %2").arg(int(std::round(std::abs(v) * 100)))
                                   .arg(v < 0 ? "left" : "right"));
    });
    connect(volumeKnob_, &RotaryKnob::valueChanged, this, [this](double v) {
        auto* t = this->track();
        if (!t) return;
        if (auto* vp = t->getVolumePlugin()) vp->setSliderPos(float(v));
        const float db = te::volumeFaderPositionToDB(float(v));
        volumeKnob_->setToolTip(db <= -99.0f ? QString("Volume: -inf dB")
                                             : QString("Volume: %1 dB").arg(db, 0, 'f', 1));
    });
    connect(steps_, &StepRow::stepsEdited, this, [this](const QVector<PatternStep>& steps) {
        emit stepsEdited(trackId_, steps);
    });

    refreshControls();
}

te::AudioTrack* ChannelRow::track() const
{
    return editMgr_ && trackId_.isValid() ? editMgr_->findAudioTrack(trackId_) : nullptr;
}

void ChannelRow::refreshControls()
{
    auto* t = track();
    if (!t) return;
    const QColor colour = editMgr_->getTrackColour(t);
    const QString name = channelsound::displayName(*editMgr_, patterns_, t);
    nameBtn_->setText(name);
    nameBtn_->setColour(colour);
    samplePath_ = patterns_ ? patterns_->channelSamplePath(trackId_) : QString();
    kind_ = channelsound::kind(*editMgr_, patterns_, t);
    nameBtn_->setKind(kind_);

    // Tooltip: what the channel plays, then how to use the button.
    QString source;
    switch (kind_) {
    case ChannelSoundKind::Sample:
        source = QString("Sample: %1").arg(QDir::toNativeSeparators(samplePath_));
        break;
    case ChannelSoundKind::BuiltIn:
        source = QString("Built-in instrument: %1").arg(channelsound::soundName(*editMgr_, patterns_, t));
        break;
    case ChannelSoundKind::Plugin:
        source = QString("Plugin: %1").arg(channelsound::soundName(*editMgr_, patterns_, t));
        break;
    case ChannelSoundKind::None:
        source = QString("No sound");
        break;
    }
    nameBtn_->setToolTip(QString("<b>%1</b><br>%2<br><br>%3")
                             .arg(name.toHtmlEscaped(), source.toHtmlEscaped(),
                                  kindTip(kind_).toHtmlEscaped()));
    steps_->setAccentColour(colour.darker(115));
    led_->setLit(!t->isMuted(false));

    if (auto* vp = t->getVolumePlugin()) {
        {
            QSignalBlocker block(panKnob_);
            panKnob_->setValue(vp->getPan());
        }
        {
            QSignalBlocker block(volumeKnob_);
            volumeKnob_->setValue(vp->getSliderPos());
        }
        const float db = vp->getVolumeDb();
        volumeKnob_->setToolTip(db <= -99.0f ? QString("Volume: -inf dB")
                                             : QString("Volume: %1 dB").arg(db, 0, 'f', 1));
        const double pan = vp->getPan();
        panKnob_->setToolTip(std::abs(pan) < 0.005 ? QString("Pan: centre")
                             : QString("Pan: %1% %2").arg(int(std::round(std::abs(pan) * 100)))
                                   .arg(pan < 0 ? "left" : "right"));
    }
}

void ChannelRow::setSelected(bool selected)
{
    if (selected == selected_) return;
    selected_ = selected;
    nameBtn_->setSelected(selected);
    update();
}

void ChannelRow::setDropHighlight(bool on)
{
    if (on == dropHighlight_) return;
    dropHighlight_ = on;
    update();
}

void ChannelRow::previewSound()
{
    auto* t = track();
    if (!t || !editMgr_ || !editMgr_->edit()) return;
    const int note = std::clamp(patterns_ ? patterns_->channelRootNote(trackId_) : 60, 0, 127);
    t->injectLiveMidiMessage(juce::MidiMessage::noteOn(1, note, juce::uint8(100)), 0);
    // The note-off looks the track up again by id: it may be gone by then.
    // Context: the EditManager, so it is still sent if this row is rebuilt.
    auto* mgr = editMgr_;
    const auto id = trackId_;
    QTimer::singleShot(kPreviewMs, mgr, [mgr, id, note]() {
        if (!mgr->edit()) return;
        if (auto* track = mgr->findAudioTrack(id))
            track->injectLiveMidiMessage(juce::MidiMessage::noteOff(1, note), 0);
    });
}

void ChannelRow::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    if (selected_)
        p.fillRect(rect(), QColor(theme.accent.red(), theme.accent.green(), theme.accent.blue(), 22));
    if (dropHighlight_) {
        p.fillRect(rect(), QColor(theme.accent.red(), theme.accent.green(), theme.accent.blue(), 40));
        p.setPen(QPen(theme.accent, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0));
    }
    p.fillRect(QRect(0, height() - 1, width(), 1), theme.separator);
}

} // namespace OpenDaw
