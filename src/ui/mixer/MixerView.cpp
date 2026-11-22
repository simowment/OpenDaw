#include "MixerView.h"
#include "LoudnessPanel.h"
#include "utils/ThemeManager.h"
#include <QFrame>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <unordered_map>

namespace OpenDaw {
namespace {
ChannelStrip* findParentStrip(QWidget* w)
{
    while (w) {
        if (auto* strip = qobject_cast<ChannelStrip*>(w))
            return strip;
        w = w->parentWidget();
    }
    return nullptr;
}

void installFilterRecursive(QWidget* widget, QObject* filter)
{
    widget->installEventFilter(filter);
    for (auto* child : widget->children()) {
        if (auto* cw = qobject_cast<QWidget*>(child))
            installFilterRecursive(cw, filter);
    }
}
}

MixerView::MixerView(EditManager* editMgr, QWidget* parent)
    : QWidget(parent), editMgr_(editMgr)
{
    setAccessibleName("Mixer View");
    auto& theme = ThemeManager::instance().current();

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    setAutoFillBackground(true);
    QPalette viewPal;
    viewPal.setColor(QPalette::Window, theme.background);
    setPalette(viewPal);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameStyle(QFrame::NoFrame);
    scrollArea_->setStyleSheet(
        QString("QScrollArea { background: %1; border: none; }")
            .arg(theme.background.name()));

    stripContainer_ = new QWidget();
    stripContainer_->setAcceptDrops(true);
    stripContainer_->installEventFilter(this);
    stripContainer_->setAutoFillBackground(true);
    stripContainer_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    QPalette contPal;
    contPal.setColor(QPalette::Window, theme.background);
    stripContainer_->setPalette(contPal);
    stripLayout_ = new QHBoxLayout(stripContainer_);
    stripLayout_->setContentsMargins(4, 4, 4, 4);
    stripLayout_->setSpacing(3);
    stripLayout_->setAlignment(Qt::AlignLeft);
    stripContainer_->installEventFilter(this);
    scrollArea_->viewport()->installEventFilter(this);

    scrollArea_->setWidget(stripContainer_);
    mainLayout->addWidget(scrollArea_, 1);

    // Master strip on the right
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet(QString("color: %1;").arg(theme.separator.name()));
    mainLayout->addWidget(sep);

    masterStrip_ = ChannelStrip::createMasterStrip(editMgr_, this);
    installFilterRecursive(masterStrip_, this);
    mainLayout->addWidget(masterStrip_);

    auto* sep2 = new QFrame(this);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setStyleSheet(QString("color: %1;").arg(theme.separator.name()));
    mainLayout->addWidget(sep2);
    mainLayout->addWidget(new LoudnessPanel(editMgr_, this));

    connect(editMgr_, &EditManager::aboutToChangeEdit, this, [this]() {
        for (auto* s : strips_) {
            stripLayout_->removeWidget(s);
            delete s;
        }
        strips_.clear();
        routing_ = {};
        editStamp_.detach();
        shownStamp_ = 0;
    });

    connect(editMgr_, &EditManager::tracksChanged,
            this, &MixerView::rebuildStrips);
    connect(editMgr_, &EditManager::routingChanged,
            this, &MixerView::rebuildStrips, Qt::QueuedConnection);
    connect(editMgr_, &EditManager::editChanged,
            this, &MixerView::refreshStrips);

    rebuildStrips();
}

void MixerView::retireStrip(ChannelStrip* s)
{
    // Only cut strip -> MixerView connections. A bare s->disconnect() also drops
    // the destroyed() connection Qt's accessibility cache relies on, leaving a
    // dangling accessible interface that crashes when UI Automation is active.
    disconnect(s, nullptr, this, nullptr);
    // The strip lives until deleteLater runs, so also stop it reacting to
    // edit signals (its track may be gone). detach() drops the track and the
    // meter at once: from here on nothing in the strip touches either.
    QObject::disconnect(editMgr_, nullptr, s, nullptr);
    s->detach();
    stripLayout_->removeWidget(s);
    s->hide();
    s->deleteLater();
}

bool MixerView::stripsMatchTracks(const juce::Array<te::AudioTrack*>& tracks) const
{
    if (static_cast<int>(strips_.size()) != tracks.size())
        return false;
    for (int i = 0; i < tracks.size(); ++i) {
        const auto* s = strips_[size_t(i)];
        auto* t = tracks[i];
        if (!s->trackAlive() || s->track() != t || s->trackId() != t->itemID.getRawID()
            || s->kind() != trackWidgetKind(*editMgr_, t))
            return false;
    }
    return true;
}

void MixerView::rebuildStrips()
{
    juce::Array<te::AudioTrack*> tracks;
    if (editMgr_->edit())
        tracks = editMgr_->getAudioTracksInDisplayOrder();

    std::unordered_map<uint64_t, te::AudioTrack*> live;
    live.reserve(size_t(tracks.size()));
    for (auto* t : tracks)
        live.emplace(t->itemID.getRawID(), t);

    // Retire first: a strip whose track is gone (or was replaced by another
    // object, e.g. by an undo, or changed kind) stops before anything below
    // can reach it. Pointers are only compared here, never dereferenced.
    std::unordered_map<uint64_t, ChannelStrip*> kept;
    for (auto* s : strips_) {
        auto it = live.find(s->trackId());
        const bool keep = it != live.end()
            && s->trackAlive() && s->track() == it->second
            && s->kind() == trackWidgetKind(*editMgr_, it->second)
            && kept.find(s->trackId()) == kept.end();
        if (keep)
            kept.emplace(s->trackId(), s);
        else
            retireStrip(s);
    }
    strips_.clear();

    // By pointer only: the selected track may be the one just deleted.
    if (selectedTrack_ && !tracks.contains(selectedTrack_))
        selectedTrack_ = nullptr;

    // One device and bus query for every strip.
    routing_ = editMgr_->edit() ? TrackRoutingChoices::query(*editMgr_) : TrackRoutingChoices{};

    std::vector<ChannelStrip*> ordered;
    ordered.reserve(size_t(tracks.size()));
    for (auto* track : tracks) {
        ChannelStrip* strip = nullptr;
        auto it = kept.find(track->itemID.getRawID());
        if (it != kept.end()) {
            strip = it->second;
            strip->refresh(&routing_);
        } else {
            strip = new ChannelStrip(track, editMgr_, stripContainer_, &routing_);
            connect(strip, &ChannelStrip::instrumentSelectRequested,
                    this, &MixerView::instrumentSelectRequested);
            strip->refresh();   // arm, mute and solo as saved
        }
        strip->setSelected(track == selectedTrack_);
        ordered.push_back(strip);
    }

    // Put the strips in display order, touching the layout only if it changed.
    bool sameOrder = stripLayout_->count() == static_cast<int>(ordered.size());
    for (int i = 0; sameOrder && i < static_cast<int>(ordered.size()); ++i)
        sameOrder = stripLayout_->itemAt(i) && stripLayout_->itemAt(i)->widget() == ordered[size_t(i)];
    if (!sameOrder) {
        for (auto* s : ordered)
            stripLayout_->removeWidget(s);
        for (auto* s : ordered)
            stripLayout_->addWidget(s);
    }
    strips_ = std::move(ordered);

    shownStamp_ = editStamp_.sync(editMgr_->edit());
}

void MixerView::refreshStrips()
{
    juce::Array<te::AudioTrack*> tracks;
    if (editMgr_->edit())
        tracks = editMgr_->getAudioTracksInDisplayOrder();

    // A track went (or came) without tracksChanged: rebuild rather than let
    // a strip read a deleted track.
    if (!stripsMatchTracks(tracks)) {
        rebuildStrips();
        return;
    }

    // tracksChanged just rebuilt the strips for this very change.
    const auto stamp = editStamp_.sync(editMgr_->edit());
    if (stamp == shownStamp_)
        return;

    // Combos keep the lists of the last rebuild (routingChanged and
    // tracksChanged rebuild them) and only reselect here.
    for (auto* strip : strips_)
        strip->refresh();
    shownStamp_ = stamp;
}

void MixerView::setSelectedTrack(te::AudioTrack* track)
{
    selectedTrack_ = track;
    for (auto* strip : strips_)
        strip->setSelected(strip->track() == selectedTrack_);
    if (masterStrip_)
        masterStrip_->setSelected(false);
}

void MixerView::setMasterSelected()
{
    selectedTrack_ = nullptr;
    for (auto* strip : strips_)
        strip->setSelected(false);
    if (masterStrip_)
        masterStrip_->setSelected(true);
}

bool MixerView::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() != Qt::LeftButton)
            return QWidget::eventFilter(watched, event);

        auto* watchedWidget = qobject_cast<QWidget*>(watched);
        if (masterStrip_ && watchedWidget
            && (watchedWidget == masterStrip_ || findParentStrip(watchedWidget) == masterStrip_)) {
            if (!masterStrip_->isSelected()) {
                setMasterSelected();
                emit masterSelected();
            }
            return QWidget::eventFilter(watched, event);
        }

        if (watched == stripContainer_ || watched == scrollArea_->viewport()) {
            QPoint containerPos = mouseEvent->pos();
            if (watched == scrollArea_->viewport())
                containerPos = stripContainer_->mapFrom(scrollArea_->viewport(), mouseEvent->pos());

            auto* clickedWidget = stripContainer_->childAt(containerPos);
            auto* clickedStrip = findParentStrip(clickedWidget);

            if (clickedStrip && clickedStrip->track()) {
                emit trackSelected(clickedStrip->track());
            } else {
                emit trackSelected(nullptr);
            }
        }
    }

    if (watched == stripContainer_) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat("application/x-opendaw-track-reorder")) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasFormat("application/x-opendaw-track-reorder")) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            if (de->mimeData()->hasFormat("application/x-opendaw-track-reorder") && editMgr_ && editMgr_->edit()) {
                int srcIdx = de->mimeData()->data("application/x-opendaw-track-reorder").toInt();
                int dropX = de->position().toPoint().x();
                auto tracks = editMgr_->getAudioTracks();
                int destIdx = 0;
                int accX = 0;
                for (int i = 0; i < static_cast<int>(strips_.size()); ++i) {
                    accX += strips_[size_t(i)]->width();
                    if (dropX < accX) { destIdx = i; break; }
                    destIdx = i + 1;
                }
                destIdx = std::clamp(destIdx, 0, tracks.size() - 1);
                if (srcIdx != destIdx && srcIdx >= 0 && srcIdx < tracks.size()) {
                    auto* srcTrack = tracks[srcIdx];
                    te::AudioTrack* precedingTrack = nullptr;
                    if (destIdx > 0 && destIdx < tracks.size())
                        precedingTrack = tracks[destIdx > srcIdx ? destIdx : destIdx - 1];
                    editMgr_->edit()->moveTrack(
                        te::Track::Ptr(srcTrack),
                        te::TrackInsertPoint(nullptr, precedingTrack));
                    emit editMgr_->tracksChanged();
                }
                de->acceptProposedAction();
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace OpenDaw
