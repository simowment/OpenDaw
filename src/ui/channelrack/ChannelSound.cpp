#include "ChannelSound.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"
#include "engine/instruments/BundledPlugins.h"
#include "engine/instruments/SoundFontInstrument.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <utility>

namespace OpenDaw {

namespace {

const juce::Identifier kSampleProp("OpenDaw_sample");
const juce::Identifier kRootNoteProp("OpenDaw_rootNote");
const juce::Identifier kColourSlotProp("OpenDaw_colourSlot");
const char* kRecentKey = "channelRack/recentSamples";

QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }
juce::String toJ(const QString& s) { return juce::String::fromUTF8(s.toUtf8().constData()); }
juce::File fileFor(const QString& path)
{
    return juce::File(toJ(QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())));
}

QString trackName(te::AudioTrack* t) { return t ? toQ(t->getName()) : QString(); }

// One undo step for a composite edit, with one round of EditManager signals.
class ChannelEdit {
public:
    ChannelEdit(EditManager& em, const char* name) : em_(em), batch_(em)
    {
        if (em.edit() && !em.inUndoGroup()) {
            em.edit()->getUndoManager().beginNewTransaction(name);
            owns_ = true;
        }
        em.beginUndoGroup();
    }
    ~ChannelEdit() { em_.endUndoGroup(); }
    // True when this step began the transaction (no caller's group is open).
    bool ownsTransaction() const { return owns_; }
    ChannelEdit(const ChannelEdit&) = delete;
    ChannelEdit& operator=(const ChannelEdit&) = delete;

private:
    EditManager& em_;
    EditManager::ScopedSignalBatch batch_;
    bool owns_ = false;
};

bool fail(QString* error, const QString& why)
{
    if (error) *error = why;
    return false;
}

// Puts the choice's instrument on the track (BuiltIn / Plugin).
bool applyInstrument(EditManager& em, te::AudioTrack& track, const SoundChoice& choice, QString* error)
{
    if (choice.kind == SoundChoice::Kind::BuiltIn) {
        if (!SoundFontLibrary::instance().isAvailable())
            return fail(error, QStringLiteral("The built-in sounds (GeneralUser-GS.sf2) were not found."));
        em.setTrackBuiltInInstrument(track, choice.bank, choice.program);
        if (!dynamic_cast<SoundFontPlugin*>(em.getTrackInstrument(&track)))
            return fail(error, QStringLiteral("Could not create the built-in instrument."));
        return true;
    }
    if (choice.kind == SoundChoice::Kind::Plugin) {
        em.setTrackInstrument(track, choice.plugin);
        auto* inst = em.getTrackInstrument(&track);
        if (!inst)
            return fail(error, QStringLiteral("Could not load %1.").arg(toQ(choice.plugin.name)));
        if (auto* ext = dynamic_cast<te::ExternalPlugin*>(inst)) {
            const auto why = ext->getLoadError();
            if (why.isNotEmpty())
                return fail(error, QStringLiteral("Could not load %1: %2")
                                       .arg(toQ(choice.plugin.name), toQ(why)));
        }
        return true;
    }
    return fail(error, QStringLiteral("No instrument was chosen."));
}

} // namespace

// ── SoundChoice ─────────────────────────────────────────────────────────────

bool SoundChoice::isValid() const
{
    switch (kind) {
    case Kind::Samples: return !samples.isEmpty();
    case Kind::BuiltIn: return bank >= 0 && program >= 0;
    case Kind::Plugin:  return plugin.fileOrIdentifier.isNotEmpty();
    case Kind::None:    break;
    }
    return false;
}

SoundChoice SoundChoice::fromSamples(const QStringList& paths)
{
    SoundChoice c;
    c.kind = Kind::Samples;
    c.samples = paths;
    if (!paths.isEmpty()) c.name = QFileInfo(paths.first()).completeBaseName();
    return c;
}

SoundChoice SoundChoice::builtIn(int bank, int program)
{
    SoundChoice c;
    c.kind = Kind::BuiltIn;
    c.bank = bank;
    c.program = program;
    c.name = channelsound::builtInPresetName(bank, program);
    return c;
}

SoundChoice SoundChoice::fromPlugin(const juce::PluginDescription& desc)
{
    SoundChoice c;
    c.kind = Kind::Plugin;
    c.plugin = desc;
    c.name = toQ(desc.name);
    return c;
}

namespace channelsound {

// ── Reading ─────────────────────────────────────────────────────────────────

ChannelSoundKind kind(EditManager& em, PatternManager* pm, te::AudioTrack* track)
{
    if (!track) return ChannelSoundKind::None;
    auto* inst = em.getTrackInstrument(track);
    if (!inst) return ChannelSoundKind::None;
    if (dynamic_cast<te::SamplerPlugin*>(inst)
        && (!pm || !pm->channelSamplePath(track->itemID).isEmpty()))
        return ChannelSoundKind::Sample;
    if (dynamic_cast<SoundFontPlugin*>(inst)) return ChannelSoundKind::BuiltIn;
    return ChannelSoundKind::Plugin;
}

QString builtInPresetName(int bank, int program)
{
    auto& lib = SoundFontLibrary::instance();
    if (lib.isAvailable())
        if (const auto* preset = lib.find(bank, program)) return preset->name;
    return bank == 128 ? QStringLiteral("Drum Kit %1").arg(program + 1)
                       : QStringLiteral("GM Program %1").arg(program + 1);
}

QString soundName(EditManager& em, PatternManager* pm, te::AudioTrack* track)
{
    if (!track) return {};
    auto* inst = em.getTrackInstrument(track);
    if (!inst) return {};
    if (pm) {
        const QString sample = pm->channelSamplePath(track->itemID);
        if (!sample.isEmpty()) return QFileInfo(sample).completeBaseName();
    }
    if (auto* sf = dynamic_cast<SoundFontPlugin*>(inst))
        return builtInPresetName(sf->bank(), sf->program());
    if (BundledPlugins::isSurgeXt(inst)) return QStringLiteral("Surge XT");
    return toQ(inst->getName());
}

bool isDefaultName(const QString& name)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*(track|audio|midi|channel|instrument)?\\s*\\d*\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(name).hasMatch();
}

QString displayName(EditManager& em, PatternManager* pm, te::AudioTrack* track)
{
    if (!track) return {};
    const QString name = trackName(track).trimmed();
    if (!isDefaultName(name)) return name;
    const QString sound = soundName(em, pm, track);
    if (!sound.isEmpty()) return sound;
    static const QRegularExpression digits(QStringLiteral("(\\d+)"));
    const auto m = digits.match(name);
    if (m.hasMatch()) return QStringLiteral("Channel %1").arg(m.captured(1));
    const int index = pm ? pm->channels().indexOf(track) : -1;
    return QStringLiteral("Channel %1").arg(index >= 0 ? index + 1 : 1);
}

QString uniqueCloneName(EditManager& em, const QString& base)
{
    QSet<QString> taken;
    for (auto* t : em.getAudioTracks()) taken.insert(trackName(t));
    // "Kick #2" clones as "Kick #3", not "Kick #2 #2".
    static const QRegularExpression suffix(QStringLiteral("^(.*\\S)\\s+#\\d+$"));
    QString stem = base.trimmed();
    if (const auto m = suffix.match(stem); m.hasMatch()) stem = m.captured(1);
    if (stem.isEmpty()) stem = QStringLiteral("Channel");
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 #%2").arg(stem).arg(n);
        if (!taken.contains(candidate)) return candidate;
    }
}

// ── Editing ─────────────────────────────────────────────────────────────────

te::AudioTrack* addInstrumentChannel(EditManager& em, const SoundChoice& choice, QString* error)
{
    if (!em.edit()) { fail(error, QStringLiteral("No project is open.")); return nullptr; }
    if (em.isRenderInProgress()) { fail(error, QStringLiteral("A render is in progress.")); return nullptr; }
    if (choice.kind != SoundChoice::Kind::BuiltIn && choice.kind != SoundChoice::Kind::Plugin) {
        fail(error, QStringLiteral("No instrument was chosen."));
        return nullptr;
    }

    te::AudioTrack* track = nullptr;
    QString why;
    {
        ChannelEdit step(em, "Add Channel");
        track = em.addMidiTrack();
        if (!track) {
            why = QStringLiteral("Could not create the channel.");
        } else if (applyInstrument(em, *track, choice, &why)) {
            const QString name = choice.name.isEmpty() ? soundName(em, nullptr, track) : choice.name;
            if (!name.isEmpty()) track->setName(toJ(name));
        }
        if (!why.isEmpty() && track) {
            em.removeTrack(track);
            track = nullptr;
        }
    }
    if (!track) fail(error, why);
    return track;
}

QVector<te::AudioTrack*> addSampleChannels(PatternManager& pm, const QStringList& paths,
                                           QStringList* errors)
{
    QVector<te::AudioTrack*> added;
    QStringList ok;
    for (const QString& path : paths) {
        QString why;
        if (auto* t = pm.addSampleChannel(fileFor(path), {}, &why)) {
            added.push_back(t);
            ok << path;
        } else if (errors) {
            *errors << QStringLiteral("%1 (%2)").arg(QFileInfo(path).fileName(), why);
        }
    }
    rememberSamples(ok);
    return added;
}

bool replaceSound(EditManager& em, PatternManager& pm, te::EditItemID trackId,
                  const SoundChoice& choice, QString* error)
{
    if (!em.edit()) return fail(error, QStringLiteral("No project is open."));
    if (em.isRenderInProgress()) return fail(error, QStringLiteral("A render is in progress."));
    auto* track = em.findAudioTrack(trackId);
    if (!track) return fail(error, QStringLiteral("The channel no longer exists."));
    if (!choice.isValid()) return fail(error, QStringLiteral("No sound was chosen."));

    const QString oldName = trackName(track).trimmed();
    const QString oldSound = soundName(em, &pm, track);
    const bool rename = isDefaultName(oldName) || (!oldSound.isEmpty() && oldName == oldSound);

    QString why;
    bool owns = false;
    {
        ChannelEdit step(em, "Replace Sound");
        owns = step.ownsTransaction();
        if (choice.kind == SoundChoice::Kind::Samples) {
            const QString path = choice.samples.first();
            if (pm.setChannelSample(trackId, fileFor(path), &why)) rememberSamples({path});
        } else {
            applyInstrument(em, *track, choice, &why);
        }
        if (why.isEmpty() && rename) {
            if (auto* t = em.findAudioTrack(trackId)) {
                const QString name = soundName(em, &pm, t);
                if (!name.isEmpty()) t->setName(toJ(name));
            }
        }
        em.notifyEdited();
    }
    if (!why.isEmpty()) {
        // A half-done replacement (old instrument removed, new one failed)
        // would leave the channel silent: roll this step back (only its own
        // actions; inside a caller's group the caller rolls back).
        if (owns) {
            em.edit()->getUndoManager().undoCurrentTransactionOnly();
            em.notifyEdited();
            emit em.tracksChanged();
        }
        return fail(error, why);
    }
    // The rack and the headers show the name.
    if (rename) emit em.tracksChanged();
    return true;
}

te::AudioTrack* cloneChannel(EditManager& em, PatternManager& pm, te::EditItemID trackId,
                             QString* error)
{
    auto* edit = em.edit();
    if (!edit) { fail(error, QStringLiteral("No project is open.")); return nullptr; }
    if (em.isRenderInProgress()) { fail(error, QStringLiteral("A render is in progress.")); return nullptr; }
    auto* src = em.findAudioTrack(trackId);
    if (!src) { fail(error, QStringLiteral("The channel no longer exists.")); return nullptr; }

    te::AudioTrack* copy = nullptr;
    {
        ChannelEdit step(em, "Clone Channel");
        PatternManager::ScopedExternalTransaction patternStep(pm);
        copy = em.addAudioTrack();
        if (!copy) { fail(error, QStringLiteral("Could not create the channel.")); return nullptr; }
        auto* um = &edit->getUndoManager();
        em.markAsMidiTrack(copy);

        // The instrument with its current settings (patch, sampler sound).
        if (auto* inst = em.getTrackInstrument(src)) {
            inst->flushPluginStateToValueTree();
            auto state = inst->state.createCopy();
            te::EditItemID::remapIDs(state, nullptr, *edit);
            if (auto plugin = edit->getPluginCache().createNewPlugin(state))
                copy->pluginList.insertPlugin(plugin, 0, nullptr);
        }
        for (const auto& prop : {kSampleProp, kRootNoteProp, kColourSlotProp})
            if (src->state.hasProperty(prop))
                copy->state.setProperty(prop, src->state.getProperty(prop), um);

        // Mix.
        if (auto* from = src->getVolumePlugin())
            if (auto* to = copy->getVolumePlugin()) {
                to->setSliderPos(from->getSliderPos());
                to->setPan(from->getPan());
            }
        copy->setMute(src->isMuted(false));
        copy->setName(toJ(uniqueCloneName(em, displayName(em, &pm, src))));

        // Its notes in every pattern.
        for (const auto& pattern : pm.patterns()) {
            const auto notes = pm.channelNotes(pattern.id, trackId);
            if (!notes.isEmpty()) pm.setChannelNotes(pattern.id, copy->itemID, notes);
        }
        em.notifyEdited();
    }
    return copy;
}

bool renameChannel(EditManager& em, te::EditItemID trackId, const QString& name)
{
    const QString trimmed = name.trimmed();
    auto* track = em.findAudioTrack(trackId);
    if (!track || trimmed.isEmpty() || !em.edit()) return false;
    if (trackName(track) == trimmed) return true;
    {
        ChannelEdit step(em, "Rename Channel");
        track->setName(toJ(trimmed));
        em.notifyEdited();
    }
    emit em.tracksChanged();
    return true;
}

// ── Recent samples ──────────────────────────────────────────────────────────

QStringList recentSamples()
{
    QStringList out;
    const QStringList stored = QSettings().value(kRecentKey).toStringList();
    for (const QString& p : stored)
        if (!p.isEmpty() && QFileInfo(p).isFile() && !out.contains(p)) out << p;
    return out.mid(0, kMaxRecentSamples);
}

void rememberSamples(const QStringList& paths)
{
    if (paths.isEmpty()) return;
    QSettings settings;
    QStringList list = settings.value(kRecentKey).toStringList();
    for (const QString& raw : paths) {
        const QString p = QDir::fromNativeSeparators(QFileInfo(raw).absoluteFilePath());
        list.removeAll(p);
        list.prepend(p);
    }
    while (list.size() > kMaxRecentSamples) list.removeLast();
    settings.setValue(kRecentKey, list);
}

} // namespace channelsound
} // namespace OpenDaw
