#include "SoundPicker.h"
#include "SampleBrowserDialog.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"
#include "engine/PluginScanner.h"
#include "engine/instruments/BundledPlugins.h"
#include "engine/instruments/SoundFontInstrument.h"
#include "ui/effects/InstrumentMenu.h"
#include "utils/IconFont.h"
#include "utils/ThemeManager.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <map>
#include <vector>

namespace OpenDaw {

namespace {

QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }
QString menuText(const QString& s) { return QString(s).replace('&', "&&"); }

bool isSurgeDescription(const juce::PluginDescription& d, const juce::File& bundle)
{
    const juce::File f(d.fileOrIdentifier);
    return d.isInstrument && (f == bundle || f.isAChildOf(bundle));
}

// Surge XT's description for the session (GUI thread only).
struct SurgeCache {
    std::optional<juce::PluginDescription> desc;
    bool scanning = false;
    std::vector<std::function<void(std::optional<juce::PluginDescription>)>> waiters;
};

SurgeCache& surgeCache()
{
    static SurgeCache cache;
    return cache;
}

void finishSurge(std::optional<juce::PluginDescription> desc)
{
    auto& cache = surgeCache();
    cache.scanning = false;
    if (desc) cache.desc = desc;
    auto waiters = std::move(cache.waiters);
    cache.waiters.clear();
    for (auto& w : waiters)
        if (w) w(desc);
}

struct SurgeScan {
    PluginScanner::HelperOutcome outcome = PluginScanner::HelperOutcome::NotStarted;
    std::vector<juce::PluginDescription> found;
    QString error;
};

QIcon menuGlyph(const QFont& font, QChar glyph)
{
    return icons::glyphIcon(font, glyph, ThemeManager::instance().current().textDim, 16);
}

} // namespace

SoundPicker::SoundPicker(EditManager* editMgr, PatternManager* patterns, QWidget* dialogParent)
    : QObject(dialogParent), editMgr_(editMgr), patterns_(patterns), dialogParent_(dialogParent)
{
}

bool SoundPicker::surgeXtAvailable()
{
    return BundledPlugins::hasSurgeXt();
}

void SoundPicker::choose(const Chosen& onChosen, const SoundChoice& choice)
{
    if (!onChosen || !choice.isValid()) return;
    // After the menu's own event loop has ended.
    QTimer::singleShot(0, this, [onChosen, choice]() { onChosen(choice); });
}

void SoundPicker::exec(const QPoint& globalPos, Purpose purpose, te::EditItemID current,
                       Chosen onChosen, int sections)
{
    QMenu menu(dialogParent_);
    menu.setAccessibleName(purpose == Purpose::AddChannel ? "Add Channel Menu" : "Replace Sound Menu");
    populate(menu, purpose, current, std::move(onChosen), sections);
    menu.exec(globalPos);
}

void SoundPicker::populate(QMenu& menu, Purpose purpose, te::EditItemID current, Chosen onChosen,
                           int sections)
{
    menu.setToolTipsVisible(true);
    te::AudioTrack* track = editMgr_ && current.isValid() ? editMgr_->findAudioTrack(current) : nullptr;
    te::Plugin* instrument = track ? editMgr_->getTrackInstrument(track) : nullptr;
    const QString currentSample = track && patterns_ ? patterns_->channelSamplePath(current) : QString();
    const bool replacing = purpose == Purpose::ReplaceSound;

    if (sections & Samples) {
        auto* sample = menu.addAction(menuGlyph(icons::fontAudio(16), icons::fa::Waveform), "&Sample...");
        sample->setToolTip(replacing ? "Pick a sample from your library (FL Studio packs included)"
                                     : "Pick one or more samples: one channel each");
        connect(sample, &QAction::triggered, this, [this, onChosen, replacing]() {
            QTimer::singleShot(0, this, [this, onChosen, replacing]() {
                const QStringList files = browseSamples(!replacing,
                                                        replacing ? "Replace Sound" : "Add Sample Channel");
                if (!files.isEmpty() && onChosen) onChosen(SoundChoice::fromSamples(files));
            });
        });

        auto* recent = menu.addMenu("&Recent samples");
        recent->setAccessibleName("Recent Samples");
        const QStringList recentFiles = channelsound::recentSamples();
        for (const QString& path : recentFiles) {
            auto* a = recent->addAction(menuText(QFileInfo(path).completeBaseName()));
            a->setToolTip(QDir::toNativeSeparators(path));
            if (!currentSample.isEmpty()) {
                a->setCheckable(true);
                a->setChecked(QFileInfo(path) == QFileInfo(currentSample));
            }
            connect(a, &QAction::triggered, this, [this, onChosen, path]() {
                choose(onChosen, SoundChoice::fromSamples({path}));
            });
        }
        if (recentFiles.isEmpty()) {
            auto* none = recent->addAction("No recent samples");
            none->setEnabled(false);
        }
    }

    if (sections & Instruments) {
        if (sections & Samples) menu.addSeparator();

        auto* builtInMenu = menu.addMenu(menuGlyph(icons::materialIcons(16), icons::mi::Piano),
                                         "&Built-in instruments");
        builtInMenu->setAccessibleName("Built-in Instruments");
        builtInMenu->setToolTipsVisible(true);
        auto* builtIn = dynamic_cast<SoundFontPlugin*>(instrument);
        addBuiltInSoundMenus(
            *builtInMenu,
            builtIn ? std::function<bool(int, int)>([builtIn](int bank, int program) {
                return builtIn->bank() == bank && builtIn->program() == program;
            })
                    : std::function<bool(int, int)>(),
            [this, onChosen](int bank, int program) {
                choose(onChosen, SoundChoice::builtIn(bank, program));
            },
            false);

        if (surgeXtAvailable()) {
            auto* surge = menu.addAction(menuGlyph(icons::fontAudio(16), icons::fa::Midiplug), "Surge &XT");
            surge->setToolTip("Surge XT: a free synthesizer with hundreds of presets (bundled).\n"
                              "Double-click the channel name to open it and pick a preset.");
            if (replacing) {
                surge->setCheckable(true);
                surge->setChecked(BundledPlugins::isSurgeXt(instrument));
            }
            connect(surge, &QAction::triggered, this, [this, onChosen]() {
                QPointer<SoundPicker> self(this);
                resolveSurgeXt([self, onChosen](std::optional<juce::PluginDescription> desc) {
                    if (!self) return;
                    if (!desc) {
                        emit self->statusMessage("Surge XT couldn't be loaded", 6000);
                        return;
                    }
                    auto choice = SoundChoice::fromPlugin(*desc);
                    choice.name = QStringLiteral("Surge XT");
                    self->choose(onChosen, choice);
                });
            });
            // Find it in the background now, so it is ready when picked.
            if (!surgeCache().desc && !surgeCache().scanning) resolveSurgeXt({});
        }

        addPluginMenu(menu, instrument, onChosen);
    }
}

void SoundPicker::addPluginMenu(QMenu& menu, te::Plugin* currentInstrument, const Chosen& onChosen)
{
    auto* plugins = menu.addMenu(menuGlyph(icons::fontAudio(16), icons::fa::Midiplug), "&Plugins");
    plugins->setAccessibleName("Plugin Instruments");
    plugins->setToolTipsVisible(true);

    const auto* ext = dynamic_cast<te::ExternalPlugin*>(currentInstrument);
    const juce::File surgeBundle = BundledPlugins::surgeXt();
    if (scanner_) {
        // Instruments by manufacturer, each sorted by name.
        std::map<QString, std::vector<juce::PluginDescription>, std::less<>> byMaker;
        for (const auto& d : scanner_->getPluginList().getTypes()) {
            if (!d.isInstrument) continue;
            if (surgeBundle.exists() && isSurgeDescription(d, surgeBundle)) continue; // listed on its own
            QString maker = toQ(d.manufacturerName).trimmed();
            if (maker.isEmpty()) maker = QStringLiteral("Other");
            byMaker[maker].push_back(d);
        }
        for (auto& [maker, list] : byMaker) {
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                return a.name.compareIgnoreCase(b.name) < 0;
            });
            auto* sub = plugins->addMenu(menuText(maker));
            for (const auto& d : list) {
                QString text = toQ(d.name);
                if (d.pluginFormatName.isNotEmpty()) text += QStringLiteral("  (%1)").arg(toQ(d.pluginFormatName));
                auto* a = sub->addAction(menuText(text));
                a->setToolTip(QDir::toNativeSeparators(toQ(d.fileOrIdentifier)));
                if (ext) {
                    a->setCheckable(true);
                    a->setChecked(ext->desc.isDuplicateOf(d));
                }
                const juce::PluginDescription desc = d;
                connect(a, &QAction::triggered, this, [this, onChosen, desc]() {
                    choose(onChosen, SoundChoice::fromPlugin(desc));
                });
            }
        }
        if (byMaker.empty()) {
            auto* none = plugins->addAction("No instrument plugins found");
            none->setEnabled(false);
            none->setToolTip("Scan for plugins to list the VST/VST3 instruments installed on this computer");
        }
        plugins->addSeparator();
        auto* scan = plugins->addAction(scanner_->isScanning() ? "Scanning for plugins..."
                                                               : "S&can for plugins...");
        scan->setEnabled(!scanner_->isScanning());
        scan->setToolTip("Look for VST/VST3 plugins installed on this computer");
        connect(scan, &QAction::triggered, this, [this]() { emit scanPluginsRequested(); });
    } else {
        auto* none = plugins->addAction("Plugin list not available");
        none->setEnabled(false);
        plugins->addSeparator();
        auto* scan = plugins->addAction("S&can for plugins...");
        connect(scan, &QAction::triggered, this, [this]() { emit scanPluginsRequested(); });
    }
}

QStringList SoundPicker::browseSamples(bool multiSelect, const QString& title)
{
    juce::AudioDeviceManager* devices = nullptr;
    if (editMgr_) devices = &editMgr_->audioEngine().engine().getDeviceManager().deviceManager;
    QWidget* parent = dialogParent_ ? dialogParent_->window() : nullptr;
    return SampleBrowserDialog::choose(devices, multiSelect, title, parent);
}

void SoundPicker::resolveSurgeXt(std::function<void(std::optional<juce::PluginDescription>)> done)
{
    const juce::File bundle = BundledPlugins::surgeXt();
    if (!bundle.exists()) {
        if (done) done(std::nullopt);
        return;
    }
    auto& cache = surgeCache();
    if (!cache.desc && scanner_)
        for (const auto& d : scanner_->getPluginList().getTypes())
            if (isSurgeDescription(d, bundle)) { cache.desc = d; break; }
    if (cache.desc) {
        if (done) done(cache.desc);
        return;
    }

    // A background warm-up (no `done`) scans quietly, and only out of
    // process: loading the plugin here would block the GUI.
    const bool announce = bool(done);
    const juce::File helper = PluginScanner::helperExecutable();
    if (!announce && !helper.existsAsFile()) return;
    cache.waiters.push_back(std::move(done));
    if (cache.scanning) return;
    cache.scanning = true;

    PluginScanner* scanner = scanner_;
    // In this process (no helper): the plugin must load on the message thread.
    auto scanHere = [scanner]() -> std::optional<juce::PluginDescription> {
        juce::KnownPluginList scratch;   // no PluginScanner: the description is cached anyway
        auto& list = scanner ? scanner->getPluginList() : scratch;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto desc = BundledPlugins::surgeXtDescription(list);
        QApplication::restoreOverrideCursor();
        if (desc && scanner) scanner->saveCachedList();
        return desc;
    };

    if (!helper.existsAsFile()) {
        // Not from inside the caller's stack: callers expect `done` later.
        QTimer::singleShot(0, qApp, [scanHere]() { finishSurge(scanHere()); });
        return;
    }

    if (announce) emit statusMessage("Preparing Surge XT (first use)...", 0);
    // Owned by the application: the scan outlives a rack rebuilt meanwhile.
    auto* watcher = new QFutureWatcher<SurgeScan>(qApp);
    QPointer<SoundPicker> self(this);
    connect(watcher, &QFutureWatcherBase::finished, qApp, [watcher, scanner, scanHere, bundle, self, announce]() {
        watcher->deleteLater();
        const SurgeScan r = watcher->result();
        std::optional<juce::PluginDescription> desc;
        if (r.outcome == PluginScanner::HelperOutcome::NotStarted) {
            desc = scanHere();
        } else if (r.outcome == PluginScanner::HelperOutcome::Scanned) {
            for (const auto& d : r.found) {
                if (scanner) scanner->getPluginList().addType(d);
                if (!desc && isSurgeDescription(d, bundle)) desc = d;
            }
            if (desc && scanner) scanner->saveCachedList();
        } else {
            qWarning() << "[SoundPicker] Surge XT failed to scan:" << r.error;
        }
        if (self && (announce || !desc))
            emit self->statusMessage(desc ? QString("Surge XT is ready")
                                          : QString("Surge XT couldn't be scanned"),
                                     desc ? 3000 : 6000);
        finishSurge(desc);
    });
    const juce::String file = bundle.getFullPathName();
    watcher->setFuture(QtConcurrent::run([helper, file]() {
        SurgeScan r;
        juce::OwnedArray<juce::PluginDescription> found;
        r.outcome = PluginScanner::scanFileWithHelper(
            helper, juce::VST3PluginFormat::getFormatName(), file, found, &r.error);
        for (auto* d : found) r.found.push_back(*d);
        return r;
    }));
}

} // namespace OpenDaw
