// FL Studio style Channel Rack logic (src/ui/channelrack/ChannelSound): a
// channel shows and is named after its sound, new instrument channels are one
// undo step, "Replace sound" keeps the channel's notes and renames it only
// when it was named after its old sound, "Clone channel" copies the sound and
// the notes, and the recent-samples list.

#include "TestHarness.h"

#include "engine/EditManager.h"
#include "engine/PatternManager.h"
#include "engine/instruments/SoundFontInstrument.h"
#include "ui/channelrack/ChannelSound.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace OpenDaw;
using OpenDaw::Test::harness;

namespace {

constexpr double kSampleRate = 44100.0;

juce::File toJuce(const QString& path)
{
    return juce::File(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
}

bool writeHit(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const auto file = toJuce(path);
    const int n = int(0.3 * kSampleRate);
    juce::AudioBuffer<float> buffer(1, n);
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kSampleRate;
        buffer.setSample(0, i, float(0.8 * std::exp(-t * 8.0) * std::sin(2.0 * 3.14159265 * 150.0 * t)));
    }
    file.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream>(file.createOutputStream());
    if (!stream) return false;
    const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate(kSampleRate).withNumChannels(1).withBitsPerSample(16);
    auto writer = wav.createWriterFor(stream, opts);
    return writer && writer->writeFromAudioSampleBuffer(buffer, 0, n);
}

PatternManager& pm() { return harness().patternManager(); }
EditManager& em() { return harness().editManager(); }
QString nameOf(te::AudioTrack* t) { return t ? QString::fromUtf8(t->getName().toRawUTF8()) : QString(); }
bool haveBuiltIn() { return SoundFontLibrary::instance().isAvailable(); }

QVector<PatternStep> stepsOn(std::initializer_list<int> on, int length = 16)
{
    QVector<PatternStep> steps(length);
    for (int i : on) steps[i].on = true;
    return steps;
}

int stepsOnCount(int patternId, te::EditItemID id)
{
    const auto p = pm().pattern(patternId);
    const auto* ch = p ? p->channel(id) : nullptr;
    if (!ch) return 0;
    int n = 0;
    for (const auto& s : ch->steps) n += s.on ? 1 : 0;
    return n;
}

// Restores QSettings "channelRack/recentSamples" when a test ends.
struct RecentSetting {
    QVariant old;
    bool had = false;
    RecentSetting()
    {
        QSettings s;
        had = s.contains("channelRack/recentSamples");
        old = s.value("channelRack/recentSamples");
        s.remove("channelRack/recentSamples");
    }
    ~RecentSetting()
    {
        QSettings s;
        if (had) s.setValue("channelRack/recentSamples", old);
        else s.remove("channelRack/recentSamples");
    }
};

} // namespace

class TestChannelRackFl : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    QString hit(const QString& name)
    {
        const QString path = QDir(dir_.path()).filePath(name + ".wav");
        if (!QFileInfo::exists(path)) writeHit(path);
        return path;
    }

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }
    void init() { harness().resetProject(); }

    void defaultNames()
    {
        QVERIFY(channelsound::isDefaultName("Track 3"));
        QVERIFY(channelsound::isDefaultName("track"));
        QVERIFY(channelsound::isDefaultName("Channel 12"));
        QVERIFY(channelsound::isDefaultName(""));
        QVERIFY(!channelsound::isDefaultName("Kick"));
        QVERIFY(!channelsound::isDefaultName("Track of the day"));
        QVERIFY(!channelsound::isDefaultName("808 Bass"));
    }

    // A plain MIDI track named "Track N" is shown under its sound's name,
    // and a track without an instrument as "Channel N": never "Track".
    void displayNameHidesTrack()
    {
        auto* t = em().addMidiTrack();
        QVERIFY(t);
        t->setName("Track 7");
        if (haveBuiltIn()) {
            QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::BuiltIn);
            QCOMPARE(channelsound::displayName(em(), &pm(), t), channelsound::builtInPresetName(0, 0));
        }
        em().removeTrackInstrument(*t);
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::None);
        QCOMPARE(channelsound::displayName(em(), &pm(), t), QString("Channel 7"));
        QVERIFY(!channelsound::displayName(em(), &pm(), t).contains("Track"));
        t->setName("Lead");
        QCOMPARE(channelsound::displayName(em(), &pm(), t), QString("Lead"));
    }

    void addBuiltInChannelIsOneUndoStep()
    {
        if (!haveBuiltIn()) QSKIP("The built-in SoundFont isn't available");
        const int before = em().trackCount();
        auto choice = SoundChoice::builtIn(0, 33);   // a bass
        QString error;
        auto* t = channelsound::addInstrumentChannel(em(), choice, &error);
        QVERIFY2(t, qPrintable(error));
        QCOMPARE(em().trackCount(), before + 1);
        QCOMPARE(nameOf(t), choice.name);
        QVERIFY(!choice.name.isEmpty());
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::BuiltIn);
        auto* sf = dynamic_cast<SoundFontPlugin*>(em().getTrackInstrument(t));
        QVERIFY(sf);
        QCOMPARE(sf->program(), 33);
        QVERIFY(pm().channels().contains(t));

        harness().edit().getUndoManager().undo();
        harness().pump(50);
        QCOMPARE(em().trackCount(), before);
    }

    // Replacing a sample channel's sound with an instrument keeps the track and
    // its steps; it is renamed because it was named after the sample.
    void replaceSampleWithInstrumentKeepsNotes()
    {
        if (!haveBuiltIn()) QSKIP("The built-in SoundFont isn't available");
        auto* t = pm().addSampleChannel(toJuce(hit("Kick Hard")));
        QVERIFY(t);
        const auto id = t->itemID;
        QCOMPARE(nameOf(t), QString("Kick Hard"));
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::Sample);
        QCOMPARE(channelsound::soundName(em(), &pm(), t), QString("Kick Hard"));

        const int pat = pm().createPattern();
        QVERIFY(pat > 0);
        QVERIFY(pm().setChannelSteps(pat, id, stepsOn({0, 4, 8, 12})));
        QCOMPARE(stepsOnCount(pat, id), 4);

        const auto choice = SoundChoice::builtIn(0, 0);
        QString error;
        QVERIFY2(channelsound::replaceSound(em(), pm(), id, choice, &error), qPrintable(error));
        t = em().findAudioTrack(id);
        QVERIFY(t);
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::BuiltIn);
        QCOMPARE(nameOf(t), choice.name);
        QCOMPARE(stepsOnCount(pat, id), 4);

        // One undo step brings the sample (and its name) back.
        harness().edit().getUndoManager().undo();
        harness().pump(50);
        t = em().findAudioTrack(id);
        QVERIFY(t);
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::Sample);
        QCOMPARE(nameOf(t), QString("Kick Hard"));
        QCOMPARE(stepsOnCount(pat, id), 4);
    }

    // A name the user gave is kept when the sound changes.
    void replaceKeepsCustomName()
    {
        if (!haveBuiltIn()) QSKIP("The built-in SoundFont isn't available");
        auto* t = channelsound::addInstrumentChannel(em(), SoundChoice::builtIn(0, 0));
        QVERIFY(t);
        const auto id = t->itemID;
        QVERIFY(channelsound::renameChannel(em(), id, "My Keys"));
        QVERIFY(channelsound::replaceSound(em(), pm(), id, SoundChoice::builtIn(0, 4)));
        QCOMPARE(nameOf(em().findAudioTrack(id)), QString("My Keys"));

        // Sample replacing an instrument named after it: renamed after the file.
        QVERIFY(channelsound::renameChannel(em(), id, channelsound::builtInPresetName(0, 4)));
        QVERIFY(channelsound::replaceSound(em(), pm(), id, SoundChoice::fromSamples({hit("Snare Tight")})));
        t = em().findAudioTrack(id);
        QCOMPARE(nameOf(t), QString("Snare Tight"));
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::Sample);
    }

    // A sound that can't be loaded leaves the channel as it was.
    void failedReplaceLeavesChannelAlone()
    {
        auto* t = pm().addSampleChannel(toJuce(hit("Clap")));
        QVERIFY(t);
        const auto id = t->itemID;
        QString error;
        QVERIFY(!channelsound::replaceSound(em(), pm(), id,
                                            SoundChoice::fromSamples({QDir(dir_.path()).filePath("missing.wav")}),
                                            &error));
        QVERIFY(!error.isEmpty());
        t = em().findAudioTrack(id);
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::Sample);
        QCOMPARE(nameOf(t), QString("Clap"));

        juce::PluginDescription bogus;
        bogus.name = "No Such Synth";
        bogus.pluginFormatName = "VST3";
        bogus.fileOrIdentifier = QDir::toNativeSeparators(QDir(dir_.path()).filePath("NoSuchSynth.vst3")).toStdString();
        bogus.isInstrument = true;
        const bool replaced = channelsound::replaceSound(em(), pm(), id, SoundChoice::fromPlugin(bogus), &error);
        t = em().findAudioTrack(id);
        QVERIFY(t);
        QVERIFY2(!replaced, "a plugin that can't be loaded must not replace the sound");
        QVERIFY(!error.isEmpty());
        // Rolled back: still the sample channel it was.
        QCOMPARE(channelsound::kind(em(), &pm(), t), ChannelSoundKind::Sample);
        QCOMPARE(nameOf(t), QString("Clap"));
    }

    void cloneCopiesSoundAndNotes()
    {
        auto* t = pm().addSampleChannel(toJuce(hit("Hat Closed")));
        QVERIFY(t);
        const auto id = t->itemID;
        const int pat = pm().createPattern();
        QVERIFY(pm().setChannelSteps(pat, id, stepsOn({2, 6, 10, 14})));
        if (auto* vp = t->getVolumePlugin()) vp->setPan(-0.5f);

        const int before = em().trackCount();
        QString error;
        auto* copy = channelsound::cloneChannel(em(), pm(), id, &error);
        QVERIFY2(copy, qPrintable(error));
        QCOMPARE(em().trackCount(), before + 1);
        QVERIFY(copy->itemID != id);
        QCOMPARE(nameOf(copy), QString("Hat Closed #2"));
        QCOMPARE(channelsound::kind(em(), &pm(), copy), ChannelSoundKind::Sample);
        QCOMPARE(QFileInfo(pm().channelSamplePath(copy->itemID)).fileName(), QString("Hat Closed.wav"));
        QCOMPARE(stepsOnCount(pat, copy->itemID), 4);
        QCOMPARE(pm().channelNotes(pat, copy->itemID).size(), pm().channelNotes(pat, id).size());
        if (auto* vp = copy->getVolumePlugin()) QCOMPARE(vp->getPan(), -0.5f);
        // The source keeps its own instrument (a copy, not a shared plugin).
        QVERIFY(em().getTrackInstrument(copy) != em().getTrackInstrument(em().findAudioTrack(id)));

        // Cloning the clone: "#3", not "#2 #2".
        auto* third = channelsound::cloneChannel(em(), pm(), copy->itemID);
        QVERIFY(third);
        QCOMPARE(nameOf(third), QString("Hat Closed #3"));

        // One undo step each.
        harness().edit().getUndoManager().undo();
        harness().pump(50);
        QCOMPARE(em().trackCount(), before + 1);
        harness().edit().getUndoManager().undo();
        harness().pump(50);
        QCOMPARE(em().trackCount(), before);
        QCOMPARE(stepsOnCount(pat, id), 4);
    }

    void recentSamplesList()
    {
        RecentSetting guard;
        QVERIFY(channelsound::recentSamples().isEmpty());
        QStringList files;
        for (int i = 0; i < channelsound::kMaxRecentSamples + 3; ++i)
            files << hit(QString("Recent %1").arg(i));
        channelsound::rememberSamples(files);
        auto recent = channelsound::recentSamples();
        QCOMPARE(recent.size(), channelsound::kMaxRecentSamples);
        // Most recent first.
        QCOMPARE(QFileInfo(recent.first()).completeBaseName(),
                 QString("Recent %1").arg(channelsound::kMaxRecentSamples + 2));

        // Using one again moves it to the front, without a duplicate.
        channelsound::rememberSamples({recent.last()});
        const QString last = recent.last();
        recent = channelsound::recentSamples();
        QCOMPARE(recent.first(), last);
        QCOMPARE(recent.count(last), 1);

        // Files that are gone are left out.
        QVERIFY(QFile::remove(recent.first()));
        QVERIFY(!channelsound::recentSamples().contains(last));

        // Adding sample channels records them.
        const auto added = channelsound::addSampleChannels(pm(), {hit("Fresh Kick")});
        QCOMPARE(added.size(), 1);
        QCOMPARE(QFileInfo(channelsound::recentSamples().first()).completeBaseName(), QString("Fresh Kick"));
    }
};

OD_REGISTER_TEST(TestChannelRackFl)
#include "tst_ChannelRackFl.moc"
