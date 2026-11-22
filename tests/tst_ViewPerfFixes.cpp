// The helpers behind the incremental mixer / playlist header rebuilds
// (src/ui/ViewRefreshSupport.h). The widgets themselves need a QApplication,
// which this headless suite (QGuiApplication) does not have, so these tests
// cover what the widgets rely on:
//  - the shared routing lists give exactly what EditManager gives each widget
//    when asked separately (so querying once per rebuild changes nothing);
//  - the edit change stamp stays put without changes, moves on any project
//    change and on a new Edit (so the editChanged that follows tracksChanged
//    can be skipped safely);
//  - combo items are only rewritten when they differ;
//  - the widget kind that decides whether a strip/header is reused.

#include "TestHarness.h"

#include "engine/EditManager.h"
#include "ui/ViewRefreshSupport.h"

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QtTest>

using namespace OpenDaw;
using OpenDaw::Test::harness;

namespace {

EditManager& em() { return harness().editManager(); }

QStringList keysOf(const QList<EditManager::OutputChoice>& choices)
{
    QStringList out;
    for (const auto& c : choices)
        out << c.label + QLatin1Char('=') + c.key;
    return out;
}

// Just enough of QComboBox for setComboItemsIfChanged / selectComboData.
class FakeCombo : public QObject {
public:
    int count() const { return int(items.size()); }
    QString itemText(int i) const { return items[i].first; }
    QVariant itemData(int i) const { return items[i].second; }
    void clear() { items.clear(); ++clears; index = -1; }
    void addItem(const QString& t, const QVariant& d)
    {
        items.append({ t, d.toString() });
        if (index < 0) index = 0;
    }
    int findData(const QVariant& d) const
    {
        for (int i = 0; i < items.size(); ++i)
            if (items[i].second == d.toString()) return i;
        return -1;
    }
    int currentIndex() const { return index; }
    void setCurrentIndex(int i) { index = i; ++indexSets; }

    QVector<std::pair<QString, QString>> items;
    int clears = 0;
    int indexSets = 0;
    int index = -1;
};

} // namespace

class TestViewPerfFixes : public QObject {
    Q_OBJECT

private slots:
    void init() { harness().resetProject(); }

    void sharedRoutingMatchesPerTrackQueries()
    {
        auto* bus = em().addBusTrack();
        QVERIFY(bus);
        auto* midi = em().addMidiTrack();
        QVERIFY(midi);
        auto tracks = em().getAudioTracks();
        QVERIFY(tracks.size() >= 3);
        // Route a track into the bus: the bus's own list must then leave out
        // nothing it would not (no self, no cycle).
        em().setTrackOutputToTrack(*tracks[0], *bus);

        const auto routing = TrackRoutingChoices::query(em());
        for (auto* t : em().getAudioTracks())
            QCOMPARE(keysOf(routing.outputChoicesFor(em(), t)), keysOf(em().getOutputChoices(t)));
        QCOMPARE(keysOf(routing.outputChoicesFor(em(), nullptr)),
                 keysOf(em().getOutputChoices(nullptr)));

        const auto audioItems = routing.inputItemsFor(false);
        QCOMPARE(audioItems.size(), 1 + em().getAvailableInputSources().size());
        QCOMPARE(audioItems.front().first, QStringLiteral("No Input"));
        const auto midiItems = routing.inputItemsFor(true);
        QCOMPARE(midiItems.size(), 1 + em().getAvailableInputSources().size()
                                     + em().getAvailableMidiInputSources().size());
        for (int i = 1; i <= em().getAvailableMidiInputSources().size(); ++i)
            QVERIFY(midiItems[i].second.startsWith(QStringLiteral("midi:")));
    }

    void widgetKindFollowsTrackType()
    {
        auto* audio = em().addAudioTrack();
        auto* midi = em().addMidiTrack();
        auto* bus = em().addBusTrack();
        QVERIFY(audio && midi && bus);
        QCOMPARE(int(trackWidgetKind(em(), audio)), int(TrackWidgetKind::Audio));
        QCOMPARE(int(trackWidgetKind(em(), midi)), int(TrackWidgetKind::Midi));
        QCOMPARE(int(trackWidgetKind(em(), bus)), int(TrackWidgetKind::Bus));
        QCOMPARE(int(trackWidgetKind(em(), nullptr)), int(TrackWidgetKind::Audio));
    }

    void editStampTracksProjectChanges()
    {
        EditStateStamp stamp;
        auto* edit = em().edit();
        QVERIFY(edit);

        const auto a = stamp.sync(edit);
        QCOMPARE(stamp.sync(edit), a);          // nothing changed
        harness().pump(20);
        QCOMPARE(stamp.sync(edit), a);          // idle time is no change

        // Moving the playhead is not a project change.
        edit->getTransport().setPosition(tracktion::TimePosition::fromSeconds(3.0));
        harness().pump(20);
        QCOMPARE(stamp.sync(edit), a);

        auto tracks = em().getAudioTracks();
        QVERIFY(!tracks.isEmpty());
        tracks[0]->setName("Renamed for stamp");
        const auto b = stamp.sync(edit);
        QVERIFY(b != a);

        em().addAudioTrack();
        const auto c = stamp.sync(edit);
        QVERIFY(c != b);

        em().removeTrack(em().getAudioTracks().getLast());
        QVERIFY(stamp.sync(edit) != c);

        // A new Edit always counts as a change, and so does detaching.
        const auto beforeNew = stamp.sync(edit);
        harness().resetProject();
        QVERIFY(stamp.sync(em().edit()) != beforeNew);
        const auto beforeDetach = stamp.current();
        stamp.detach();
        QVERIFY(stamp.current() != beforeDetach);
    }

    void comboItemsRewrittenOnlyWhenDifferent()
    {
        FakeCombo combo;
        const QVector<std::pair<QString, QString>> items {
            { "Master", "master" }, { "Bus", "track:7" }, { "No Output", "none" } };

        setComboItemsIfChanged(&combo, items);
        QCOMPARE(combo.clears, 1);
        QCOMPARE(combo.count(), 3);

        setComboItemsIfChanged(&combo, items);   // same list: untouched
        QCOMPARE(combo.clears, 1);

        auto renamed = items;
        renamed[1].first = "Drum Bus";
        setComboItemsIfChanged(&combo, renamed);
        QCOMPARE(combo.clears, 2);
        QCOMPARE(combo.itemText(1), QStringLiteral("Drum Bus"));

        selectComboData(&combo, "none");
        QCOMPARE(combo.currentIndex(), 2);
        const int sets = combo.indexSets;
        selectComboData(&combo, "none");         // already selected: no change
        QCOMPARE(combo.indexSets, sets);
        selectComboData(&combo, "missing", "track:7");
        QCOMPARE(combo.currentIndex(), 1);
        selectComboData(&combo, "missing");
        QCOMPARE(combo.currentIndex(), 0);
    }
};

OD_REGISTER_TEST(TestViewPerfFixes)
#include "tst_ViewPerfFixes.moc"
