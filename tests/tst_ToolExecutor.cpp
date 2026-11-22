// AiToolExecutor through CommandGateway (tools/list, tools/call): stable ids,
// structured errors, schema validation, MIDI round trip, run_batch, undo.

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExecutor.h"
#include "engine/EditManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtTest>
#include <cmath>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

// Every tool that existed before the agent API work.
const QStringList kOriginalTools{
    "get_project_info", "get_track_list", "get_track_info", "create_audio_track", "create_midi_track",
    "create_bus_track", "list_instruments", "set_track_instrument", "delete_track", "rename_track",
    "set_track_mute", "set_track_solo", "set_track_volume", "set_track_pan", "set_track_mono",
    "set_track_record_enabled", "get_available_inputs", "assign_input_to_track", "clear_track_input",
    "set_track_output", "clear_track_output", "add_send", "set_send", "remove_send",
    "list_available_effects", "get_track_effects", "add_effect_to_track", "remove_effect_from_track",
    "set_effect_parameter", "set_effect_bypass", "play", "stop", "record", "get_transport_state",
    "set_position", "analyze_track_levels", "analyze_master_levels", "measure_mix_loudness",
    "analyze_frequency_balance", "analyze_stereo_image", "analyze_transients", "analyze_masking",
    "apply_mix_preset", "set_track_target_peak", "set_track_dynamic_goal", "set_reverb_character",
    "set_bus_glue", "set_master_target", "create_mix_buses_from_roles", "auto_route_by_name_patterns",
    "group_tracks_by_inferred_role", "preview_mix_plan", "commit_last_mix_stage", "revert_last_mix_stage",
    "set_tempo", "set_time_signature", "save_project", "undo", "redo", "setup_midi_channels",
    "set_channel_name", "get_channel_names", "create_midi_clip", "add_midi_notes", "get_clips_on_track",
    "clear_midi_notes", "set_clip_channel", "get_midi_notes", "remove_midi_notes",
};

const QStringList kNewTools{
    "run_batch", "render_region", "export_stems",
    "create_pattern", "set_pattern_steps", "get_pattern", "place_pattern",
};

const QString kMissingTrack = QStringLiteral("track:987654321987");

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(1500);
}

int trackCount() { return harness().editManager().getAudioTracks().size(); }

QString trackName(te::Track* t) { return t ? QString::fromStdString(t->getName().toStdString()) : QString(); }

bool hasTrackNamed(const QString& name)
{
    for (auto* t : harness().editManager().getAudioTracks())
        if (trackName(t) == name)
            return true;
    return false;
}

// Code of a failed run_batch command nested anywhere in the result.
bool containsCode(const QJsonValue& v, const QString& code)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.value("code").toString() == code)
            return true;
        for (auto it = o.begin(); it != o.end(); ++it)
            if (containsCode(it.value(), code))
                return true;
    } else if (v.isArray()) {
        for (const auto& x : v.toArray())
            if (containsCode(x, code))
                return true;
    } else if (v.isString()) {
        return v.toString() == code;
    }
    return false;
}

} // namespace

class TestToolExecutor : public QObject {
    Q_OBJECT

    // Creates a MIDI track through the tool and returns its id.
    QString createMidiTrack(const QString& name)
    {
        const QJsonObject out = harness().call(QStringLiteral("create_midi_track"), QJsonObject{{"name", name}});
        if (harness().lastCallFailed())
            return {};
        return TestHarness::findId(out, ids::kTrack);
    }

private slots:
    void init() { harness().resetProject(); }

    void toolsListComplete()
    {
        const QJsonObject resp = harness().rpc(QStringLiteral("tools/list"));
        const QJsonArray tools = resp.value("result").toObject().value("tools").toArray();
        QSet<QString> names;
        for (const auto& v : tools) {
            const QJsonObject t = v.toObject();
            const QString name = t.value("name").toString();
            names.insert(name);
            const QJsonObject schema = t.value("inputSchema").toObject();
            QVERIFY2(!schema.isEmpty(), qPrintable(name + " has no inputSchema"));
            QCOMPARE(schema.value("type").toString(), QStringLiteral("object"));
            if (schema.contains("properties"))
                QVERIFY2(schema.value("properties").isObject(), qPrintable(name));
            if (schema.contains("required"))
                QVERIFY2(schema.value("required").isArray(), qPrintable(name));
        }
        for (const auto& n : kOriginalTools)
            QVERIFY2(names.contains(n), qPrintable("missing original tool " + n));
        for (const auto& n : kNewTools)
            QVERIFY2(names.contains(n), qPrintable("missing new tool " + n));
        QVERIFY(names.size() >= kOriginalTools.size() + kNewTools.size());

        // The executor and the gateway agree.
        auto& ex = harness().executor();
        QCOMPARE(int(ex.toolDefinitions().size()), int(tools.size()));
        for (const auto& n : names)
            QVERIFY2(ex.hasTool(n), qPrintable(n));
        QVERIFY(!ex.hasTool("no_such_tool"));
        QVERIFY(ex.isDestructive("delete_track"));
        QVERIFY(!ex.toolFlags("get_project_info").mutating);
        QVERIFY(!ex.toolFlags("play").batchable);
        QVERIFY(!ex.toolFlags("run_batch").batchable);
        QVERIFY(ex.toolFlags("create_midi_track").batchable);
    }

    void createMidiTrackReturnsId()
    {
        const int before = trackCount();
        OD_CALL_OK(out, "create_midi_track", (QJsonObject{{"name", "Lead"}}));
        const QString id = TestHarness::findId(out, ids::kTrack);
        QVERIFY2(ids::isIdOfKind(id, ids::kTrack), qPrintable(dump(out)));
        QCOMPARE(trackCount(), before + 1);

        auto* track = harness().trackById(id);
        QVERIFY(track);
        QCOMPARE(trackName(track), QStringLiteral("Lead"));
        QCOMPARE(ids::of(track), id);

        // Every resolver form reaches the same track.
        auto& ex = harness().executor();
        QCOMPARE(ex.resolveTrack(QJsonValue(id)), track);
        QCOMPARE(ex.resolveTrack(QJsonObject{{"id", id}}), track);
        QCOMPARE(ex.resolveTrack(QJsonValue("lead")), track);
        QCOMPARE(ex.resolveTrack(QJsonValue(before)), track);
        QVERIFY(ex.resolveTrack(QJsonValue(kMissingTrack)) == nullptr);
    }

    void idsAreStable()
    {
        const QString idA = createMidiTrack("Alpha");
        const QString idB = createMidiTrack("Beta");
        QVERIFY(!idA.isEmpty() && !idB.isEmpty());
        QVERIFY(idA != idB);

        // Rename, then address by id.
        OD_CALL_OK(renamed, "rename_track", (QJsonObject{{"track", idB}, {"new_name", "Beta 2"}}));
        QCOMPARE(trackName(harness().trackById(idB)), QStringLiteral("Beta 2"));
        OD_CALL_OK(muted, "set_track_mute", (QJsonObject{{"track", idB}, {"muted", true}}));
        QVERIFY(harness().trackById(idB)->isMuted(false));

        // Delete an earlier track: indices shift, the id does not.
        const int indexBefore = harness().editManager().getAudioTracks().indexOf(harness().trackById(idB));
        OD_CALL_OK(deleted, "delete_track", (QJsonObject{{"track", idA}}));
        QVERIFY(harness().trackById(idA) == nullptr);
        auto* b = harness().trackById(idB);
        QVERIFY(b);
        QCOMPARE(harness().editManager().getAudioTracks().indexOf(b), indexBefore - 1);

        OD_CALL_OK(solo, "set_track_solo", (QJsonObject{{"track", idB}, {"solo", true}}));
        QVERIFY(b->isSolo(false));
        OD_CALL_OK(info, "get_track_info", (QJsonObject{{"track", idB}}));
        QVERIFY2(dump(info).contains("Beta 2"), qPrintable(dump(info)));
        QCOMPARE(TestHarness::findId(info, ids::kTrack), idB);

        // The id of a deleted track is NOT_FOUND, not some other track.
        OD_CALL_ERR(gone, "set_track_mute", (QJsonObject{{"track", idA}, {"muted", true}}));
        QCOMPARE(TestHarness::errorCode(gone), AiErrorCode::NotFound);
    }

    void unknownIdIsNotFound()
    {
        OD_CALL_ERR(out, "set_track_mute", (QJsonObject{{"track", kMissingTrack}, {"muted", true}}));
        QCOMPARE(TestHarness::errorCode(out), AiErrorCode::NotFound);

        OD_CALL_ERR(byName, "set_track_mute", (QJsonObject{{"track", "No Such Track"}, {"muted", true}}));
        QCOMPARE(TestHarness::errorCode(byName), AiErrorCode::NotFound);

        OD_CALL_ERR(clip, "get_midi_notes", (QJsonObject{{"clip_id", "clip:987654321987"}}));
        QCOMPARE(TestHarness::errorCode(clip), AiErrorCode::NotFound);
    }

    void schemaViolationIsInvalidArgument()
    {
        OD_CALL_ERR(wrongType, "set_track_volume", (QJsonObject{{"track", 0}, {"volume_db", "loud"}}));
        QCOMPARE(TestHarness::errorCode(wrongType), AiErrorCode::InvalidArgument);
        // The error names the offending field ("$.volume_db").
        const QJsonValue path = TestHarness::findKey(wrongType, {"path"});
        const QString where = path.isString() ? path.toString()
                                              : wrongType.value("error").toObject().value("message").toString();
        QVERIFY2(where.contains("volume_db"), qPrintable(dump(wrongType)));

        OD_CALL_ERR(outOfRange, "set_track_volume", (QJsonObject{{"track", 0}, {"volume_db", 40}}));
        QCOMPARE(TestHarness::errorCode(outOfRange), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(missing, "set_track_volume", (QJsonObject{{"track", 0}}));
        QCOMPARE(TestHarness::errorCode(missing), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(unknownProp, "set_track_mute", (QJsonObject{{"track", 0}, {"muted", true}, {"mute", true}}));
        QCOMPARE(TestHarness::errorCode(unknownProp), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(fractionalIndex, "set_track_mute", (QJsonObject{{"track", 1.5}, {"muted", true}}));
        QCOMPARE(TestHarness::errorCode(fractionalIndex), AiErrorCode::InvalidArgument);

        // Nothing ran: track 0 is unmuted at its default level.
        QVERIFY(!harness().editManager().getAudioTrack(0)->isMuted(false));
    }

    void unknownToolDirect()
    {
        AiToolCall call;
        call.id = "t1";
        call.name = "definitely_not_a_tool";
        const AiToolResult r = harness().executor().execute(call);
        QVERIFY(r.isError);
        QCOMPARE(r.errorCode, AiErrorCode::UnknownTool);
        QCOMPARE(r.toolUseId, QStringLiteral("t1"));
    }

    void midiClipRoundTrip()
    {
        const QString trackId = createMidiTrack("Keys");
        QVERIFY(!trackId.isEmpty());

        OD_CALL_OK(clipOut, "create_midi_clip",
                   (QJsonObject{{"track", trackId}, {"start_beat", 4}, {"length_beats", 8}, {"name", "Riff"}}));
        const QString clipId = TestHarness::findId(clipOut, ids::kClip);
        QVERIFY2(ids::isIdOfKind(clipId, ids::kClip), qPrintable(dump(clipOut)));
        auto* clip = dynamic_cast<te::MidiClip*>(harness().clipById(clipId));
        QVERIFY(clip);
        QCOMPARE(clip->getTrack(), static_cast<te::Track*>(harness().trackById(trackId)));

        const QJsonArray notes{
            QJsonObject{{"note", 60}, {"start_beat", 0}, {"length_beats", 1}, {"velocity", 100}},
            QJsonObject{{"note", 64}, {"start_beat", 1}, {"length_beats", 0.5}, {"velocity", 90}},
            QJsonObject{{"note", 67}, {"start_beat", 2.5}, {"length_beats", 1.5}, {"velocity", 80}},
        };
        // By clip_id alone: no track needed.
        OD_CALL_OK(added, "add_midi_notes", (QJsonObject{{"clip_id", clipId}, {"notes", notes}}));
        QCOMPARE(clip->getSequence().getNotes().size(), 3);

        OD_CALL_OK(read, "get_midi_notes", (QJsonObject{{"clip_id", clipId}}));
        const QJsonArray got = TestHarness::findKey(read, {"notes"}).toArray();
        QCOMPARE(int(got.size()), 3);
        QMap<int, QJsonObject> byNote;
        for (const auto& n : got)
            byNote.insert(n.toObject().value("note").toInt(), n.toObject());
        for (const auto& n : notes) {
            const QJsonObject want = n.toObject();
            const int key = want.value("note").toInt();
            QVERIFY2(byNote.contains(key), qPrintable(dump(read)));
            const QJsonObject have = byNote[key];
            QCOMPARE(have.value("start_beat").toDouble(), want.value("start_beat").toDouble());
            QCOMPARE(have.value("length_beats").toDouble(), want.value("length_beats").toDouble());
            QCOMPARE(have.value("velocity").toInt(), want.value("velocity").toInt());
        }

        // The clip id survives renaming its track.
        OD_CALL_OK(ren, "rename_track", (QJsonObject{{"track", trackId}, {"new_name", "Keys 2"}}));
        OD_CALL_OK(cleared, "clear_midi_notes", (QJsonObject{{"clip_id", clipId}}));
        QCOMPARE(clip->getSequence().getNotes().size(), 0);

        // The executor's clip resolver agrees.
        QString error;
        QCOMPARE(harness().executor().resolveClip(nullptr, QJsonObject{{"clip_id", clipId}}, &error),
                 static_cast<te::Clip*>(clip));
        QVERIFY(harness().executor().resolveClip(nullptr, QJsonObject{{"clip_id", "clip:987654321987"}}, &error)
                == nullptr);
        QVERIFY(!error.isEmpty());
    }

    void runBatchSuccess()
    {
        const int before = trackCount();
        const QJsonArray commands{
            QJsonObject{{"tool", "create_midi_track"}, {"input", QJsonObject{{"name", "Batch One"}}}},
            QJsonObject{{"tool", "create_audio_track"}, {"input", QJsonObject{{"name", "Batch Two"}}}},
            QJsonObject{{"tool", "set_tempo"}, {"input", QJsonObject{{"bpm", 96}}}},
        };
        OD_CALL_OK(out, "run_batch", (QJsonObject{{"commands", commands}}));
        QCOMPARE(trackCount(), before + 2);
        QVERIFY(hasTrackNamed("Batch One"));
        QVERIFY(hasTrackNamed("Batch Two"));
        QVERIFY(std::abs(harness().editManager().getBpm() - 96.0) < 0.01);

        const QJsonArray results = out.value("results").toArray();
        QCOMPARE(int(results.size()), 3);
        for (const auto& r : results)
            QVERIFY2(r.toObject().value("ok").toBool(), qPrintable(dump(out)));
        // Each command's own result is returned, ids included.
        QVERIFY(ids::isIdOfKind(TestHarness::findId(results[0], ids::kTrack), ids::kTrack));
    }

    // Regression: parameter writes (volume, effect parameters) used to bypass
    // the undo history, so a failed atomic batch left them applied and its
    // rollback undid an unrelated earlier step instead.
    void runBatchRollsBackParameterChanges()
    {
        OD_CALL_OK(created, "create_midi_track", (QJsonObject{{"name", "Keep"}}));
        const QString id = TestHarness::findId(created, "track");
        QVERIFY(!id.isEmpty());
        OD_CALL_OK(info0, "get_track_info", (QJsonObject{{"track", id}}));
        const double volBefore = info0.value("volume_db").toDouble();

        const QJsonArray commands{
            QJsonObject{{"tool", "set_track_volume"}, {"input", QJsonObject{{"track", id}, {"volume_db", -9}}}},
            QJsonObject{{"tool", "set_track_volume"}, {"input", QJsonObject{{"track", kMissingTrack}, {"volume_db", -3}}}},
        };
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", commands}}));
        QCOMPARE(TestHarness::errorCode(out), AiErrorCode::BatchFailed);
        // Only the batch's own work was reverted: the track made before it survives.
        QVERIFY2(harness().trackById(id) != nullptr, qPrintable(dump(out)));
        const QJsonArray reverted = TestHarness::findKey(out, {"reverted_transactions"}).toArray();
        for (const auto& r : reverted)
            QVERIFY2(!r.toString().contains("create_midi_track"), qPrintable(dump(out)));

        OD_CALL_OK(info1, "get_track_info", (QJsonObject{{"track", id}}));
        QVERIFY2(std::abs(info1.value("volume_db").toDouble() - volBefore) < 0.05, qPrintable(dump(info1)));
    }

    void undoRevertsVolumeChange()
    {
        OD_CALL_OK(created, "create_audio_track", (QJsonObject{{"name", "Vol"}}));
        const QString id = TestHarness::findId(created, "track");
        OD_CALL_OK(info0, "get_track_info", (QJsonObject{{"track", id}}));
        const double volBefore = info0.value("volume_db").toDouble();
        OD_CALL_OK(set, "set_track_volume", (QJsonObject{{"track", id}, {"volume_db", -12}}));
        OD_CALL_OK(info1, "get_track_info", (QJsonObject{{"track", id}}));
        QVERIFY(std::abs(info1.value("volume_db").toDouble() + 12.0) < 0.05);
        OD_CALL_OK(undone, "undo", QJsonObject{});
        OD_CALL_OK(info2, "get_track_info", (QJsonObject{{"track", id}}));
        QVERIFY2(std::abs(info2.value("volume_db").toDouble() - volBefore) < 0.05, qPrintable(dump(info2)));
        QVERIFY(harness().trackById(id) != nullptr);
    }

    void runBatchAtomicRollback()
    {
        const int before = trackCount();
        const double bpmBefore = harness().editManager().getBpm();
        const QJsonArray commands{
            QJsonObject{{"tool", "create_midi_track"}, {"input", QJsonObject{{"name", "Doomed"}}}},
            QJsonObject{{"tool", "set_track_mute"}, {"input", QJsonObject{{"track", kMissingTrack}, {"muted", true}}}},
            QJsonObject{{"tool", "set_tempo"}, {"input", QJsonObject{{"bpm", 150}}}},
        };
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", commands}, {"atomic", true}}));
        QCOMPARE(TestHarness::errorCode(out), AiErrorCode::BatchFailed);
        const QJsonValue failedIndex = TestHarness::findKey(out, {"failed_index"});
        QVERIFY2(failedIndex.isDouble(), qPrintable(dump(out)));
        QCOMPARE(failedIndex.toInt(), 1);
        QVERIFY2(containsCode(out, AiErrorCode::NotFound), qPrintable(dump(out)));

        // The first command's track creation was rolled back; the third never ran.
        QCOMPARE(trackCount(), before);
        QVERIFY(!hasTrackNamed("Doomed"));
        QVERIFY(std::abs(harness().editManager().getBpm() - bpmBefore) < 0.01);
    }

    void runBatchValidatesEveryCommandFirst()
    {
        const int before = trackCount();
        const QJsonArray commands{
            QJsonObject{{"tool", "create_midi_track"}, {"input", QJsonObject{{"name", "Never"}}}},
            QJsonObject{{"tool", "set_track_volume"}, {"input", QJsonObject{{"track", 0}, {"volume_db", "x"}}}},
        };
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", commands}}));
        const QString code = TestHarness::errorCode(out);
        QVERIFY2(code == AiErrorCode::InvalidArgument || code == AiErrorCode::BatchFailed, qPrintable(dump(out)));
        QVERIFY(containsCode(out, AiErrorCode::InvalidArgument));
        QCOMPARE(trackCount(), before);
        QVERIFY(!hasTrackNamed("Never"));
    }

    void runBatchRejectsNonBatchable()
    {
        const int before = trackCount();
        const QJsonArray commands{
            QJsonObject{{"tool", "create_midi_track"}, {"input", QJsonObject{{"name", "Nope"}}}},
            QJsonObject{{"tool", "play"}},
        };
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", commands}}));
        QVERIFY2(containsCode(out, AiErrorCode::NotAllowed), qPrintable(dump(out)));
        QVERIFY(!harness().editManager().transport().isPlaying());
        QCOMPARE(trackCount(), before);

        const QJsonArray nested{
            QJsonObject{{"tool", "run_batch"},
                        {"input", QJsonObject{{"commands", QJsonArray{QJsonObject{{"tool", "get_track_list"}}}}}}},
        };
        OD_CALL_ERR(out2, "run_batch", (QJsonObject{{"commands", nested}}));
        QVERIFY2(containsCode(out2, AiErrorCode::NotAllowed), qPrintable(dump(out2)));

        OD_CALL_ERR(unknown, "run_batch",
                    (QJsonObject{{"commands", QJsonArray{QJsonObject{{"tool", "not_a_tool"}}}}}));
        QVERIFY2(containsCode(unknown, AiErrorCode::UnknownTool) || containsCode(unknown, AiErrorCode::InvalidArgument),
                 qPrintable(dump(unknown)));
    }

    void undoRedoAfterToolCall()
    {
        const QString id = createMidiTrack("Original");
        QVERIFY(!id.isEmpty());
        OD_CALL_OK(ren, "rename_track", (QJsonObject{{"track", id}, {"new_name", "Renamed"}}));
        QCOMPARE(trackName(harness().trackById(id)), QStringLiteral("Renamed"));

        OD_CALL_OK(undone, "undo", QJsonObject{});
        QCOMPARE(trackName(harness().trackById(id)), QStringLiteral("Original"));

        OD_CALL_OK(redone, "redo", QJsonObject{});
        QCOMPARE(trackName(harness().trackById(id)), QStringLiteral("Renamed"));

        // Undoing further removes the track the tool created. Creating a MIDI
        // track may take two steps (the track, then its built-in instrument,
        // which EditManager records as its own "Change Instrument" step).
        const int count = trackCount();
        for (int i = 0; i < 4 && harness().trackById(id) != nullptr; ++i) {
            OD_CALL_OK(u, "undo", QJsonObject{});
        }
        QVERIFY(harness().trackById(id) == nullptr);
        QCOMPARE(trackCount(), count - 1);
    }

    void readOnlyToolsWork()
    {
        OD_CALL_OK(info, "get_project_info", QJsonObject{});
        const QJsonArray tracks = TestHarness::findKey(info, {"tracks"}).toArray();
        QCOMPARE(int(tracks.size()), trackCount());
        for (const auto& t : tracks)
            QVERIFY2(ids::isIdOfKind(TestHarness::findId(t, ids::kTrack), ids::kTrack), qPrintable(dump(info)));

        OD_CALL_OK(list, "get_track_list", QJsonObject{});
        QVERIFY(!TestHarness::findId(list, ids::kTrack).isEmpty());
    }
};

OD_REGISTER_TEST(TestToolExecutor)
#include "tst_ToolExecutor.moc"
