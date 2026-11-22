#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace juce { class AudioDeviceManager; }

namespace OpenDaw {

class SamplePreviewPlayer;

// Picks samples for Channel Rack channels: a search box, the sample folders
// (SampleLibrary::sampleRoots(), FL Studio packs first) and the matching
// files (SampleLibrary::findSamples, debounced and run off the GUI thread).
// Clicking a file previews it on the audio device; double-click or OK
// chooses it. With multiSelect, several files can be chosen at once (one
// channel each). The last folder and query are remembered (QSettings
// "sampleBrowser/...").
class SampleBrowserDialog : public QDialog {
    Q_OBJECT

public:
    // devices may be null (no preview then).
    SampleBrowserDialog(juce::AudioDeviceManager* devices, bool multiSelect,
                        const QString& title, QWidget* parent = nullptr);
    ~SampleBrowserDialog() override;

    QStringList selectedFiles() const { return chosen_; }

    // Runs the dialog; the chosen files, or empty when cancelled.
    static QStringList choose(juce::AudioDeviceManager* devices, bool multiSelect,
                              const QString& title, QWidget* parent);

    static constexpr int kDebounceMs = 150;
    static constexpr int kResultLimit = 500;

    void done(int result) override;

private:
    void loadRoots();
    void expandFolder(QTreeWidgetItem* item);
    void setFolder(const QString& folder, const QString& label);
    void scheduleSearch();
    void runSearch();
    void preview(QTreeWidgetItem* item);
    void addLibraryFolder();
    void browseFiles();
    QStringList currentSelection() const;

    juce::AudioDeviceManager* devices_ = nullptr;
    std::unique_ptr<SamplePreviewPlayer> preview_;
    bool multi_ = false;

    QLineEdit* search_ = nullptr;
    QTreeWidget* folders_ = nullptr;
    QTreeWidget* results_ = nullptr;
    QLabel* status_ = nullptr;
    QCheckBox* autoPreview_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QTimer* debounce_ = nullptr;

    QString folder_;        // absolute; empty = every root
    QString folderLabel_;
    int generation_ = 0;    // the search whose results are wanted
    int rootsGeneration_ = 0; // bumped when the folder tree is rebuilt
    QStringList chosen_;
};

} // namespace OpenDaw
