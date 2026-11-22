#pragma once

// Helpers that keep the mixer and the playlist headers cheap to refresh:
//
//  - TrackRoutingChoices: the input sources and output destinations queried
//    from EditManager ONCE per rebuild, then handed to every ChannelStrip /
//    TrackHeaderWidget (each widget used to enumerate the audio and MIDI
//    devices itself, which made adding a track O(tracks * devices)).
//  - trackWidgetKind(): what decides a track widget's fixed structure (a MIDI
//    track has an instrument row, an audio track a takes button). A widget
//    is reused only while its track keeps the same kind.
//  - EditStateStamp: a change counter over the Edit's ValueTree, so a view
//    that has just rebuilt for tracksChanged can skip the identical rebuild
//    for the editChanged that follows it.
//
// No widget headers here: the tests (Qt Gui only) use these helpers too.

#include "engine/EditManager.h"
#include <QList>
#include <QSignalBlocker>
#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <utility>

namespace OpenDaw {

// Sorts of track widget. A change of kind means the widget is recreated.
enum class TrackWidgetKind { Audio, Midi, Bus };

inline TrackWidgetKind trackWidgetKind(const EditManager& em, te::AudioTrack* track)
{
    if (!track) return TrackWidgetKind::Audio;
    if (em.isMidiTrack(track)) return TrackWidgetKind::Midi;
    if (em.isBusTrack(track)) return TrackWidgetKind::Bus;
    return TrackWidgetKind::Audio;
}

struct TrackRoutingChoices {
    struct Bus {
        te::AudioTrack* track = nullptr;
        EditManager::OutputChoice choice;
    };

    QList<InputSource> audioInputs;
    QList<InputSource> midiInputs;
    QVector<Bus> buses;                         // every bus track, in edit order
    QList<EditManager::OutputChoice> devices;   // direct outputs (not Master's device)

    // One device enumeration and one bus scan for the whole rebuild.
    static TrackRoutingChoices query(const EditManager& em)
    {
        TrackRoutingChoices r;
        r.audioInputs = em.getAvailableInputSources();
        r.midiInputs = em.getAvailableMidiInputSources();
        for (auto* bus : em.getBusTracks())
            r.buses.append(Bus{ bus, EditManager::OutputChoice{
                QString::fromStdString(bus->getName().toStdString()),
                "track:" + QString::number(bus->itemID.getRawID()) } });
        for (const auto& dev : em.getAudioOutputDevices()) {
            if (dev.isMain) continue;
            r.devices.append(EditManager::OutputChoice{
                "Direct: " + dev.name,
                "device:" + QString::fromStdString(dev.id.toStdString()) });
        }
        return r;
    }

    // The same list, in the same order, as EditManager::getOutputChoices(track).
    QList<EditManager::OutputChoice> outputChoicesFor(const EditManager& em,
                                                      te::AudioTrack* track) const
    {
        QList<EditManager::OutputChoice> choices;
        choices.append(EditManager::OutputChoice{ "Master", "master" });
        for (const auto& bus : buses) {
            if (track && (bus.track == track || em.wouldCreateCycle(track, bus.track)))
                continue;
            choices.append(bus.choice);
        }
        for (const auto& dev : devices)
            choices.append(dev);
        choices.append(EditManager::OutputChoice{ "No Output", "none" });
        return choices;
    }

    // (label, data) items of a track's input combo: "No Input", the MIDI
    // inputs for a MIDI track ("midi:<device>"), then the audio inputs.
    QVector<std::pair<QString, QString>> inputItemsFor(bool isMidi) const
    {
        QVector<std::pair<QString, QString>> items;
        items.append({ "No Input", QString() });
        if (isMidi)
            for (const auto& src : midiInputs)
                items.append({ src.displayName,
                               "midi:" + QString::fromStdString(src.deviceName.toStdString()) });
        for (const auto& src : audioInputs)
            items.append({ src.displayName,
                           QString::fromStdString(src.deviceName.toStdString()) });
        return items;
    }
};

inline QVector<std::pair<QString, QString>> toComboItems(const QList<EditManager::OutputChoice>& choices)
{
    QVector<std::pair<QString, QString>> items;
    items.reserve(choices.size());
    for (const auto& c : choices)
        items.append({ c.label, c.key });
    return items;
}

// Replaces a combo box's items only when they differ (clearing and refilling
// a combo is what made every refresh slow), without emitting index changes.
// A template so this header needs no QtWidgets include.
template <typename Combo>
void setComboItemsIfChanged(Combo* combo, const QVector<std::pair<QString, QString>>& items)
{
    if (!combo) return;
    bool same = combo->count() == items.size();
    for (int i = 0; same && i < items.size(); ++i)
        same = combo->itemText(i) == items[i].first
            && combo->itemData(i).toString() == items[i].second;
    if (same) return;
    QSignalBlocker block(combo);
    combo->clear();
    for (const auto& [label, data] : items)
        combo->addItem(label, data);
}

// Selects the item whose data is `data` (or `fallback`), else the first one.
template <typename Combo>
void selectComboData(Combo* combo, const QString& data, const QString& fallback = QString())
{
    if (!combo) return;
    int idx = combo->findData(data);
    if (idx < 0 && !fallback.isEmpty())
        idx = combo->findData(fallback);
    if (idx < 0) idx = 0;
    if (combo->currentIndex() == idx) return;
    QSignalBlocker block(combo);
    combo->setCurrentIndex(idx);
}

// Counts changes to an Edit's state tree (transport moves excluded). Two
// equal readings mean nothing in the project document changed in between.
// sync() follows the current Edit: a different Edit counts as a change.
class EditStateStamp : private juce::ValueTree::Listener {
public:
    EditStateStamp() = default;
    ~EditStateStamp() override { detach(); }
    EditStateStamp(const EditStateStamp&) = delete;
    EditStateStamp& operator=(const EditStateStamp&) = delete;

    uint64_t sync(te::Edit* edit)
    {
        if (!edit) {
            if (state_.isValid()) detach();
            return count_.load();
        }
        if (!(state_ == edit->state)) {
            detach();
            state_ = edit->state;
            state_.addListener(this);
        }
        return count_.load();
    }

    // Stop listening (the Edit is about to go). Counts as a change.
    void detach()
    {
        if (state_.isValid())
            state_.removeListener(this);
        state_ = {};
        bump();
    }

    uint64_t current() const { return count_.load(); }

private:
    void bump() { count_.fetch_add(1, std::memory_order_relaxed); }
    static bool isTransport(const juce::ValueTree& t) { return t.hasType(te::IDs::TRANSPORT); }

    void valueTreePropertyChanged(juce::ValueTree& t, const juce::Identifier& id) override
    {
        // The playhead and Tracktion's own "last changed" time show nothing.
        if (isTransport(t) || id == te::IDs::lastSignificantChange) return;
        bump();
    }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { bump(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { bump(); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { bump(); }
    void valueTreeParentChanged(juce::ValueTree&) override {}
    void valueTreeRedirected(juce::ValueTree&) override { bump(); }

    juce::ValueTree state_;
    std::atomic<uint64_t> count_ { 1 };
};

} // namespace OpenDaw
