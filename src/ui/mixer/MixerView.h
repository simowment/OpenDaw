#pragma once

#include "ChannelStrip.h"
#include "engine/EditManager.h"
#include "ui/ViewRefreshSupport.h"
#include <QWidget>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QEvent>
#include <vector>

namespace OpenDaw {

class MixerView : public QWidget {
    Q_OBJECT

public:
    explicit MixerView(EditManager* editMgr, QWidget* parent = nullptr);

    QSize sizeHint() const override { return {800, 320}; }

public slots:
    // Brings the strips in line with the Edit's tracks: keeps the strips of
    // tracks that still exist, retires those whose track went, creates
    // strips for new tracks only, then puts them in display order.
    void rebuildStrips();
    // editChanged: re-reads the strips (cheap). Falls back to rebuildStrips()
    // when a strip's track is gone, so no strip touches a deleted track.
    void refreshStrips();
    void setSelectedTrack(te::AudioTrack* track);
    void setMasterSelected();

signals:
    void effectInsertRequested(te::AudioTrack* track, int slotIndex);
    void instrumentSelectRequested(te::AudioTrack* track);
    void trackSelected(te::AudioTrack* track);
    void masterSelected();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    // Disconnects, detaches (drops its track) and schedules deletion.
    void retireStrip(ChannelStrip* strip);
    // True when every strip still matches a live track (same id, object, kind).
    bool stripsMatchTracks(const juce::Array<te::AudioTrack*>& tracks) const;

    EditManager* editMgr_;
    QScrollArea* scrollArea_;
    QWidget* stripContainer_;
    QHBoxLayout* stripLayout_;
    ChannelStrip* masterStrip_ = nullptr;
    std::vector<ChannelStrip*> strips_;
    te::AudioTrack* selectedTrack_ = nullptr;
    // Device/bus lists from the last rebuild, for editChanged refreshes.
    TrackRoutingChoices routing_;
    // Edit change count the strips last showed; an equal count on
    // editChanged means nothing to re-read.
    EditStateStamp editStamp_;
    uint64_t shownStamp_ = 0;
};

} // namespace OpenDaw
