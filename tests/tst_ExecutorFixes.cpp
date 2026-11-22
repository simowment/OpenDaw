// AiToolExecutor fixes: parameter undo survives the plugin being freed and
// re-created, run_batch is one undo step, and a failed atomic batch keeps the
// user's redo history (and never leaves its own partial changes redoable).

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExecutor.h"
#include "ai/AiToolExtensions.h"
#include "engine/EditManager.h"

#include <tracktion_engine/tracktion_engine.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <cmath>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

const QString kMissingTrack = QStringLiteral("track:987654321987");

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(1500);
}

QString trackName(te::Track* t) { return t ? QString::fromStdString(t->getName().toStdString()) : QString(); }

bool hasTrackNamed(const QString& name)
{
    for (auto* t : harness().editManager().getAudioTracks())
        if (trackName(t) == name)
            return true;
    return false;
}

juce::UndoManager& undoManager() { return harness().edit().getUndoManager(); }

int undoCount() { return undoManager().getUndoDescriptions().size(); }

// The plugin with this "plugin:<id>" on the track, or nullptr.
te::Plugin* pluginOnTrack(te::AudioTrack* track, const QString& pluginId)
{
    const auto id = ids::parse(pluginId, ids::kPlugin);
    if (!track || !id) return nullptr;
    for (auto* p : track->pluginList.getPlugins())
        if (p->itemID == *id)
            return p;
    return nullptr;
}

// PluginCache frees plugins that left the Edit on a 1 s timer.
void waitForPluginCache() { TestHarness::pump(1500); }

} // namespace

class TestExecutorFixes : public QObject {
    Q_OBJECT

private slots:
    void init() { harness().resetProject(); }

    // Finding 1: undo of a parameter change after its plugin was removed and
    // freed, then re-created by undo, must not touch the freed object.
    void undoParamAfterEffectRemovedAndFreed()
    {
        OD_CALL_OK(created, "create_audio_track", (QJsonObject{{"name", "FxHost"}}));
        const QString trackId = TestHarness::findId(created, ids::kTrack);
        QVERIFY(!trackId.isEmpty());

        OD_CALL_OK(added, "add_effect_to_track",
                   (QJsonObject{{"track", trackId}, {"effect_name", "Reverb"}}));
        const QString pluginId = TestHarness::findId(added, ids::kPlugin);
        QVERIFY2(!pluginId.isEmpty(), qPrintable(dump(added)));

        auto* plugin = pluginOnTrack(harness().trackById(trackId), pluginId);
        QVERIFY(plugin != nullptr);
        auto params = plugin->getAutomatableParameters();
        QVERIFY(!params.isEmpty());
        const juce::String paramID = params[0]->paramID;
        const float before = params[0]->getCurrentValue();
        const double target = before > 0.5f ? 0.1 : 0.9;

        OD_CALL_OK(set, "set_effect_parameter",
                   (QJsonObject{{"track", trackId}, {"effect", pluginId}, {"parameter", 0}, {"value", target}}));
        QVERIFY(std::abs(params[0]->getCurrentValue() - before) > 1.0e-3f);
        params.clear();
        plugin = nullptr;

        OD_CALL_OK(removed, "remove_effect_from_track", (QJsonObject{{"track", trackId}, {"effect", pluginId}}));
        QVERIFY(pluginOnTrack(harness().trackById(trackId), pluginId) == nullptr);
        waitForPluginCache();

        OD_CALL_OK(undo1, "undo", QJsonObject{});   // the effect comes back as a new object
        TestHarness::pump(50);
        auto* restored = pluginOnTrack(harness().trackById(trackId), pluginId);
        QVERIFY(restored != nullptr);

        OD_CALL_OK(undo2, "undo", QJsonObject{});   // the parameter change
        TestHarness::pump(50);
        restored = pluginOnTrack(harness().trackById(trackId), pluginId);
        QVERIFY(restored != nullptr);
        auto param = restored->getAutomatableParameterByID(paramID);
        QVERIFY(param != nullptr);
        QVERIFY2(std::abs(param->getCurrentValue() - before) < 1.0e-3f,
                 qPrintable(QString("value %1, expected %2").arg(param->getCurrentValue()).arg(before)));

        // Redo both and undo again: still no stale objects.
        OD_CALL_OK(redo1, "redo", QJsonObject{});
        OD_CALL_OK(redo2, "redo", QJsonObject{});
        QVERIFY(pluginOnTrack(harness().trackById(trackId), pluginId) == nullptr);
        waitForPluginCache();
        OD_CALL_OK(undo3, "undo", QJsonObject{});
        OD_CALL_OK(undo4, "undo", QJsonObject{});
        QVERIFY(pluginOnTrack(harness().trackById(trackId), pluginId) != nullptr);
    }

    // Same, for the track's volume plugin: set_track_volume, delete_track,
    // the track and its plugins freed, then undo twice.
    void undoVolumeAfterTrackDeletedAndFreed()
    {
        OD_CALL_OK(created, "create_audio_track", (QJsonObject{{"name", "Doomed Vol"}}));
        const QString trackId = TestHarness::findId(created, ids::kTrack);
        OD_CALL_OK(info0, "get_track_info", (QJsonObject{{"track", trackId}}));
        const double volBefore = info0.value("volume_db").toDouble();

        OD_CALL_OK(set, "set_track_volume", (QJsonObject{{"track", trackId}, {"volume_db", -15}}));
        OD_CALL_OK(del, "delete_track", (QJsonObject{{"track", trackId}}));
        QVERIFY(harness().trackById(trackId) == nullptr);
        waitForPluginCache();

        OD_CALL_OK(undo1, "undo", QJsonObject{});
        TestHarness::pump(50);
        QVERIFY(harness().trackById(trackId) != nullptr);
        OD_CALL_OK(info1, "get_track_info", (QJsonObject{{"track", trackId}}));
        QVERIFY2(std::abs(info1.value("volume_db").toDouble() + 15.0) < 0.05, qPrintable(dump(info1)));

        OD_CALL_OK(undo2, "undo", QJsonObject{});
        TestHarness::pump(50);
        OD_CALL_OK(info2, "get_track_info", (QJsonObject{{"track", trackId}}));
        QVERIFY2(std::abs(info2.value("volume_db").toDouble() - volBefore) < 0.05, qPrintable(dump(info2)));
    }

    // Finding 4: a successful batch, including EditManager methods that would
    // open transactions of their own (add_send), is exactly one undo step.
    void atomicBatchIsOneUndoStep()
    {
        OD_CALL_OK(created, "create_audio_track", (QJsonObject{{"name", "Src"}}));
        const QString trackId = TestHarness::findId(created, ids::kTrack);
        OD_CALL_OK(info0, "get_track_info", (QJsonObject{{"track", trackId}}));
        const double volBefore = info0.value("volume_db").toDouble();

        const int undoBefore = undoCount();
        const QJsonArray commands{
            QJsonObject{{"tool", "create_bus_track"}, {"input", QJsonObject{{"name", "Batch Bus"}}}},
            QJsonObject{{"tool", "add_send"},
                        {"input", QJsonObject{{"track", trackId}, {"bus", "Batch Bus"}, {"level_db", -10}}}},
            QJsonObject{{"tool", "set_send"},
                        {"input", QJsonObject{{"track", trackId}, {"bus", "Batch Bus"}, {"level_db", -4}}}},
            QJsonObject{{"tool", "set_track_volume"}, {"input", QJsonObject{{"track", trackId}, {"volume_db", -9}}}},
        };
        OD_CALL_OK(out, "run_batch", (QJsonObject{{"commands", commands}}));
        QVERIFY(hasTrackNamed("Batch Bus"));
        QVERIFY(!harness().editManager().getTrackSends(harness().trackById(trackId)).empty());
        QVERIFY2(undoCount() == undoBefore + 1,
                 qPrintable(QString("batch added %1 undo steps").arg(undoCount() - undoBefore)));

        OD_CALL_OK(undone, "undo", QJsonObject{});
        TestHarness::pump(50);
        QVERIFY(!hasTrackNamed("Batch Bus"));
        QVERIFY(harness().editManager().getTrackSends(harness().trackById(trackId)).empty());
        OD_CALL_OK(info1, "get_track_info", (QJsonObject{{"track", trackId}}));
        QVERIFY2(std::abs(info1.value("volume_db").toDouble() - volBefore) < 0.05, qPrintable(dump(info1)));
        QCOMPARE(undoCount(), undoBefore);
    }

    // A single mutating tool is one undo step too.
    void singleToolIsOneUndoStep()
    {
        OD_CALL_OK(src, "create_audio_track", (QJsonObject{{"name", "Src1"}}));
        const QString trackId = TestHarness::findId(src, ids::kTrack);
        OD_CALL_OK(bus, "create_bus_track", (QJsonObject{{"name", "Bus1"}}));
        const int undoBefore = undoCount();
        OD_CALL_OK(send, "add_send", (QJsonObject{{"track", trackId}, {"bus", "Bus1"}}));
        QCOMPARE(undoCount(), undoBefore + 1);
        OD_CALL_OK(set, "set_send", (QJsonObject{{"track", trackId}, {"bus", "Bus1"}, {"level_db", -3},
                                                 {"pre_fader", true}}));
        QCOMPARE(undoCount(), undoBefore + 2);
    }

    // Finding 3: the user's redo steps survive a failed atomic batch, and
    // redo then re-applies the user's edit, not the batch's partial changes.
    void redoHistoryPreservedAfterFailedBatch()
    {
        OD_CALL_OK(created, "create_midi_track", (QJsonObject{{"name", "User Edit"}}));
        OD_CALL_OK(undone, "undo", QJsonObject{});
        QVERIFY(!hasTrackNamed("User Edit"));
        QVERIFY(undoManager().canRedo());

        const QJsonArray commands{
            QJsonObject{{"tool", "create_midi_track"}, {"input", QJsonObject{{"name", "Doomed"}}}},
            QJsonObject{{"tool", "set_track_mute"}, {"input", QJsonObject{{"track", kMissingTrack}, {"muted", true}}}},
        };
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", commands}}));
        QCOMPARE(TestHarness::errorCode(out), AiErrorCode::BatchFailed);
        QVERIFY(!hasTrackNamed("Doomed"));
        QVERIFY2(TestHarness::findKey(out, {"rolled_back"}).toBool(), qPrintable(dump(out)));
        QVERIFY2(TestHarness::findKey(out, {"redo_history_preserved"}).toBool(), qPrintable(dump(out)));

        QVERIFY(undoManager().canRedo());
        OD_CALL_OK(redone, "redo", QJsonObject{});
        TestHarness::pump(50);
        QVERIFY(hasTrackNamed("User Edit"));
        QVERIFY(!hasTrackNamed("Doomed"));
    }

    // Without user redo history, the rolled-back batch must not be redoable.
    void failedBatchNotRedoable()
    {
        QVERIFY(!undoManager().canRedo());
        const int before = harness().editManager().getAudioTracks().size();
        const QJsonArray commands{
            QJsonObject{{"tool", "create_midi_track"}, {"input", QJsonObject{{"name", "Doomed2"}}}},
            QJsonObject{{"tool", "set_track_mute"}, {"input", QJsonObject{{"track", kMissingTrack}, {"muted", true}}}},
        };
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", commands}}));
        QCOMPARE(TestHarness::errorCode(out), AiErrorCode::BatchFailed);
        QVERIFY(!hasTrackNamed("Doomed2"));

        if (undoManager().canRedo()) {
            OD_CALL_OK(redone, "redo", QJsonObject{});
            TestHarness::pump(50);
        }
        QVERIFY(!hasTrackNamed("Doomed2"));
        QCOMPARE(int(harness().editManager().getAudioTracks().size()), before);
    }
};

OD_REGISTER_TEST(TestExecutorFixes)
#include "tst_ExecutorFixes.moc"
