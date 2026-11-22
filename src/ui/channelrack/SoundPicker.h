#pragma once

#include "ChannelSound.h"
#include "engine/AudioEngine.h"
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <functional>
#include <optional>

class QMenu;
class QWidget;

namespace OpenDaw {

class EditManager;
class PatternManager;
class PluginScanner;

// The Channel Rack's sound menu (FL Studio's "Add channel" / "Replace"):
//   Sample...                 the SampleBrowserDialog (search + folders + preview)
//   Recent samples  >         the last 10 samples used (QSettings)
//   Built-in instruments >    the General MIDI set, by family
//   Surge XT                  the bundled synth (when shipped)
//   Plugins  >                instruments in the plugin list, by manufacturer,
//                             and "Scan for plugins..."
// Picking an entry hands a SoundChoice to the caller once the menu has
// closed (from the event loop), so the caller may rebuild widgets freely.
// Surge XT's plugin description is found once, off the GUI thread (the
// OpenDawPluginScanner helper), and cached for the session.
class SoundPicker : public QObject {
    Q_OBJECT

public:
    enum class Purpose { AddChannel, ReplaceSound };
    enum Section { Samples = 1, Instruments = 2, AllSections = Samples | Instruments };
    using Chosen = std::function<void(const SoundChoice&)>;

    // dialogParent: parent of the sample browser (the rack). Any may be null.
    SoundPicker(EditManager* editMgr, PatternManager* patterns, QWidget* dialogParent);

    // The plugin list for "Plugins" (and where a Surge XT scan is recorded).
    // Without it, "Plugins" says the list isn't available.
    void setPluginScanner(PluginScanner* scanner) { scanner_ = scanner; }
    PluginScanner* pluginScanner() const { return scanner_; }

    // Fills `menu`. `current`: the channel whose sound is being replaced (its
    // sound is ticked), or invalid.
    void populate(QMenu& menu, Purpose purpose, te::EditItemID current, Chosen onChosen,
                  int sections = AllSections);
    // A popup menu with populate()'s entries at globalPos.
    void exec(const QPoint& globalPos, Purpose purpose, te::EditItemID current, Chosen onChosen,
              int sections = AllSections);

    // The sample browser; the chosen files (empty when cancelled).
    QStringList browseSamples(bool multiSelect, const QString& title);

    // Surge XT's description: at once when known, otherwise once the
    // background scan is done (nullopt when it isn't shipped or failed).
    // `done` runs on the GUI thread, never re-entrantly from this call when a
    // scan is needed.
    void resolveSurgeXt(std::function<void(std::optional<juce::PluginDescription>)> done);
    static bool surgeXtAvailable();

signals:
    void scanPluginsRequested();
    void statusMessage(const QString& text, int timeoutMs);

private:
    void addPluginMenu(QMenu& menu, te::Plugin* currentInstrument, const Chosen& onChosen);
    void choose(const Chosen& onChosen, const SoundChoice& choice);

    QPointer<EditManager> editMgr_;
    QPointer<PatternManager> patterns_;
    QPointer<QWidget> dialogParent_;
    PluginScanner* scanner_ = nullptr;
};

} // namespace OpenDaw
