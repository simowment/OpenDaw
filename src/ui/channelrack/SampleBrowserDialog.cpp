#include "SampleBrowserDialog.h"
#include "SamplePreviewPlayer.h"
#include "engine/PatternManager.h"
#include "utils/IconFont.h"
#include "utils/ThemeManager.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace OpenDaw {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kLoadedRole = Qt::UserRole + 1;   // folder item: children listed
constexpr int kPlaceholderRole = Qt::UserRole + 2;

const char* kLastFolderKey = "sampleBrowser/lastFolder";
const char* kLastQueryKey = "sampleBrowser/lastQuery";
const char* kGeometryKey = "sampleBrowser/geometry";
const char* kAutoPreviewKey = "sampleBrowser/autoPreview";
const char* kUserRootsKey = "samples/roots";

struct SearchResult {
    QVector<SampleInfo> samples;
    int total = 0;
};

struct FolderList {
    QVector<SampleFolderInfo> folders;
};

bool isFlPack(const QString& path)
{
    return path.contains(QLatin1String("Image-Line"), Qt::CaseInsensitive);
}

void addPlaceholder(QTreeWidgetItem* parent)
{
    auto* p = new QTreeWidgetItem(parent, QStringList{QStringLiteral("Loading...")});
    p->setData(0, kPlaceholderRole, true);
    p->setFlags(Qt::NoItemFlags);
}

QTreeWidgetItem* makeFolderItem(const QString& path, const QString& name, int count)
{
    auto* item = new QTreeWidgetItem(QStringList{count > 0 ? QStringLiteral("%1  (%2)").arg(name).arg(count)
                                                           : name});
    item->setData(0, kPathRole, path);
    item->setToolTip(0, QDir::toNativeSeparators(path));
    item->setIcon(0, icons::glyphIcon(icons::materialIcons(14), icons::mi::Folder,
                                      ThemeManager::instance().current().textDim, 14));
    addPlaceholder(item);
    return item;
}

} // namespace

SampleBrowserDialog::SampleBrowserDialog(juce::AudioDeviceManager* devices, bool multiSelect,
                                         const QString& title, QWidget* parent)
    : QDialog(parent), devices_(devices), multi_(multiSelect)
{
    setWindowTitle(title);
    setAccessibleName("Sample Browser");
    setSizeGripEnabled(true);
    auto& theme = ThemeManager::instance().current();
    QSettings settings;

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    search_ = new QLineEdit(this);
    search_->setAccessibleName("Search Samples");
    search_->setPlaceholderText("Search samples: kick, snare, hihat, 808, clap...");
    search_->setClearButtonEnabled(true);
    search_->setText(settings.value(kLastQueryKey).toString());
    layout->addWidget(search_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    folders_ = new QTreeWidget(splitter);
    folders_->setAccessibleName("Sample Folders");
    folders_->setHeaderHidden(true);
    folders_->setUniformRowHeights(true);
    folders_->setMinimumWidth(200);

    results_ = new QTreeWidget(splitter);
    results_->setAccessibleName("Samples");
    results_->setColumnCount(2);
    results_->setHeaderLabels({"Sample", "Folder"});
    results_->setRootIsDecorated(false);
    results_->setUniformRowHeights(true);
    results_->setAlternatingRowColors(true);
    results_->setSelectionMode(multi_ ? QAbstractItemView::ExtendedSelection
                                      : QAbstractItemView::SingleSelection);
    results_->header()->setStretchLastSection(true);
    results_->header()->resizeSection(0, 260);
    splitter->addWidget(folders_);
    splitter->addWidget(results_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    auto* bottom = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setAccessibleName("Sample Browser Status");
    status_->setStyleSheet(QString("QLabel { color: %1; }").arg(theme.textDim.name()));
    bottom->addWidget(status_, 1);

    autoPreview_ = new QCheckBox("Preview on click", this);
    autoPreview_->setAccessibleName("Preview On Click");
    autoPreview_->setChecked(settings.value(kAutoPreviewKey, true).toBool());
    autoPreview_->setEnabled(devices_ != nullptr);
    bottom->addWidget(autoPreview_);

    auto* addFolder = new QPushButton("Add folder...", this);
    addFolder->setAccessibleName("Add Sample Folder");
    addFolder->setToolTip("Add a folder of your own samples to the library");
    addFolder->setAutoDefault(false);
    bottom->addWidget(addFolder);

    auto* browse = new QPushButton("Browse files...", this);
    browse->setAccessibleName("Browse Sample Files");
    browse->setToolTip("Pick audio files anywhere on disk");
    browse->setAutoDefault(false);
    bottom->addWidget(browse);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(multi_ ? "Add" : "Use sample");
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    bottom->addWidget(buttons_);
    layout->addLayout(bottom);

    if (devices_) preview_ = std::make_unique<SamplePreviewPlayer>(*devices_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);
    connect(debounce_, &QTimer::timeout, this, &SampleBrowserDialog::runSearch);
    connect(search_, &QLineEdit::textChanged, this, [this]() { scheduleSearch(); });
    connect(search_, &QLineEdit::returnPressed, this, [this]() {
        // Enter in the search box: search now; a second Enter with a hit
        // selected chooses it.
        if (debounce_->isActive()) { debounce_->stop(); runSearch(); return; }
        if (!currentSelection().isEmpty()) accept();
    });

    connect(folders_, &QTreeWidget::itemExpanded, this, &SampleBrowserDialog::expandFolder);
    connect(folders_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                if (!item || item->data(0, kPlaceholderRole).toBool()) return;
                const QString path = item->data(0, kPathRole).toString();
                setFolder(path, path.isEmpty() ? QString() : QFileInfo(path).fileName());
            });

    connect(results_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (autoPreview_->isChecked()) preview(item);
    });
    connect(results_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem* previous) {
                // Arrow keys audition too (clicks are handled by itemClicked).
                if (item && previous && autoPreview_->isChecked()
                    && QApplication::mouseButtons() == Qt::NoButton)
                    preview(item);
            });
    connect(results_, &QTreeWidget::itemSelectionChanged, this, [this]() {
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(!currentSelection().isEmpty());
    });
    connect(results_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item) return;
        results_->setCurrentItem(item);
        accept();
    });
    connect(autoPreview_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && preview_) preview_->stop();
    });
    connect(addFolder, &QPushButton::clicked, this, &SampleBrowserDialog::addLibraryFolder);
    connect(browse, &QPushButton::clicked, this, &SampleBrowserDialog::browseFiles);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!restoreGeometry(settings.value(kGeometryKey).toByteArray())) resize(760, 480);

    folder_ = settings.value(kLastFolderKey).toString();
    if (!folder_.isEmpty() && !QDir(folder_).exists()) folder_.clear();
    folderLabel_ = folder_.isEmpty() ? QString() : QFileInfo(folder_).fileName();
    loadRoots();
    runSearch();
    search_->setFocus();
}

SampleBrowserDialog::~SampleBrowserDialog() = default;

QStringList SampleBrowserDialog::choose(juce::AudioDeviceManager* devices, bool multiSelect,
                                        const QString& title, QWidget* parent)
{
    SampleBrowserDialog dlg(devices, multiSelect, title, parent);
    if (dlg.exec() != QDialog::Accepted) return {};
    return dlg.selectedFiles();
}

void SampleBrowserDialog::done(int result)
{
    if (preview_) preview_->stop();
    if (result == QDialog::Accepted) {
        if (chosen_.isEmpty()) chosen_ = currentSelection();
        if (chosen_.isEmpty()) return;   // nothing to choose: stay open
    }
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kLastFolderKey, folder_);
    settings.setValue(kLastQueryKey, search_->text().trimmed());
    settings.setValue(kAutoPreviewKey, autoPreview_->isChecked());
    QDialog::done(result);
}

QStringList SampleBrowserDialog::currentSelection() const
{
    QStringList out;
    for (auto* item : results_->selectedItems())
        out << item->data(0, kPathRole).toString();
    if (out.isEmpty())
        if (auto* item = results_->currentItem(); item && item->isSelected())
            out << item->data(0, kPathRole).toString();
    out.removeAll(QString());
    return out;
}

void SampleBrowserDialog::loadRoots()
{
    ++rootsGeneration_;
    folders_->clear();
    auto* all = new QTreeWidgetItem(folders_, QStringList{"All samples"});
    all->setData(0, kPathRole, QString());
    all->setIcon(0, icons::glyphIcon(icons::materialIcons(14), icons::mi::LibraryMusic,
                                     ThemeManager::instance().current().accentLight, 14));

    QStringList roots = SampleLibrary::sampleRoots();
    // FL Studio packs first: that's where an FL user's drums are.
    std::stable_partition(roots.begin(), roots.end(), isFlPack);
    QTreeWidgetItem* current = folder_.isEmpty() ? all : nullptr;
    for (const QString& root : roots) {
        QString name = QFileInfo(root).fileName();
        if (isFlPack(root)) name = QStringLiteral("FL Studio Packs");
        auto* item = makeFolderItem(root, name, 0);
        folders_->addTopLevelItem(item);
        if (!folder_.isEmpty() && QDir(folder_) == QDir(root)) current = item;
    }
    if (roots.isEmpty()) {
        auto* none = new QTreeWidgetItem(folders_, QStringList{"No sample folders found"});
        none->setFlags(Qt::NoItemFlags);
    }
    {
        const QSignalBlocker block(folders_);
        if (current) folders_->setCurrentItem(current);
    }
    // The roots with many packs: show their folders straight away.
    if (folders_->topLevelItemCount() > 1)
        if (auto* first = folders_->topLevelItem(1); first && isFlPack(first->data(0, kPathRole).toString()))
            first->setExpanded(true);
}

void SampleBrowserDialog::expandFolder(QTreeWidgetItem* item)
{
    if (!item || item->data(0, kLoadedRole).toBool()) return;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;
    item->setData(0, kLoadedRole, true);

    // Listing walks the folder (counting samples below it): off the GUI
    // thread. The item may be gone by the time it is done (roots reloaded).
    auto* watcher = new QFutureWatcher<FolderList>(this);
    const int rootsGen = rootsGeneration_;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, item, rootsGen]() {
        watcher->deleteLater();
        // Folder items only go away when the roots are reloaded.
        if (rootsGen != rootsGeneration_) return;
        auto* target = item;
        const auto list = watcher->result();
        // Drop the placeholder.
        for (int i = target->childCount() - 1; i >= 0; --i)
            if (target->child(i)->data(0, kPlaceholderRole).toBool()) delete target->takeChild(i);
        for (const auto& f : list.folders) {
            auto* child = makeFolderItem(f.path, f.name, f.sampleCount);
            target->addChild(child);
        }
        if (list.folders.isEmpty())
            target->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
    });
    watcher->setFuture(QtConcurrent::run([path]() {
        FolderList out;
        out.folders = SampleLibrary::listFolders(path);
        return out;
    }));
}

void SampleBrowserDialog::setFolder(const QString& folder, const QString& label)
{
    if (folder == folder_) return;
    folder_ = folder;
    folderLabel_ = label;
    debounce_->stop();
    runSearch();
}

void SampleBrowserDialog::scheduleSearch()
{
    debounce_->start();
}

void SampleBrowserDialog::runSearch()
{
    const int gen = ++generation_;
    const QString query = search_->text().trimmed();
    const QString folder = folder_;
    status_->setText("Searching...");

    auto* watcher = new QFutureWatcher<SearchResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, gen, query]() {
        watcher->deleteLater();
        if (gen != generation_) return;   // a newer search is on its way
        const SearchResult r = watcher->result();

        const QString keep = results_->currentItem()
                                 ? results_->currentItem()->data(0, kPathRole).toString() : QString();
        results_->setUpdatesEnabled(false);
        results_->clear();
        QList<QTreeWidgetItem*> items;
        items.reserve(r.samples.size());
        QTreeWidgetItem* again = nullptr;
        for (const auto& s : r.samples) {
            auto* item = new QTreeWidgetItem(QStringList{s.name, s.folder});
            item->setData(0, kPathRole, s.path);
            item->setToolTip(0, QDir::toNativeSeparators(s.path));
            item->setToolTip(1, QDir::toNativeSeparators(s.path));
            items << item;
            if (!keep.isEmpty() && s.path == keep) again = item;
        }
        results_->addTopLevelItems(items);
        if (again) {
            const QSignalBlocker block(results_);
            results_->setCurrentItem(again);
        }
        results_->setUpdatesEnabled(true);

        const QString where = folderLabel_.isEmpty() ? QString() : QStringLiteral(" in %1").arg(folderLabel_);
        if (r.total == 0)
            status_->setText(query.isEmpty() ? QStringLiteral("No samples%1").arg(where)
                                             : QStringLiteral("Nothing matches \"%1\"%2").arg(query, where));
        else if (r.total > r.samples.size())
            status_->setText(QStringLiteral("%1 of %2 samples%3: type to narrow down")
                                 .arg(r.samples.size()).arg(r.total).arg(where));
        else
            status_->setText(QStringLiteral("%1 sample%2%3").arg(r.total).arg(r.total == 1 ? "" : "s").arg(where));
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(!currentSelection().isEmpty());
    });
    watcher->setFuture(QtConcurrent::run([query, folder]() {
        SearchResult r;
        r.samples = SampleLibrary::findSamples(query, folder, kResultLimit, &r.total);
        return r;
    }));
}

void SampleBrowserDialog::preview(QTreeWidgetItem* item)
{
    if (!item || !preview_) return;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;
    if (!preview_->play(path))
        status_->setText(QStringLiteral("Can't play %1").arg(QFileInfo(path).fileName()));
}

void SampleBrowserDialog::addLibraryFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Add Sample Folder", QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    if (dir.isEmpty()) return;
    QSettings settings;
    QStringList roots = settings.value(kUserRootsKey).toStringList();
    const QString clean = QDir::fromNativeSeparators(QDir(dir).absolutePath());
    if (!roots.contains(clean)) roots << clean;
    settings.setValue(kUserRootsKey, roots);
    SampleLibrary::invalidateCache();
    folder_ = clean;
    folderLabel_ = QFileInfo(clean).fileName();
    loadRoots();
    runSearch();
}

void SampleBrowserDialog::browseFiles()
{
    QString start = folder_;
    if (start.isEmpty()) {
        const QStringList roots = SampleLibrary::sampleRoots();
        start = roots.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation)
                                : roots.first();
    }
    const QString filter = "Audio Files (*.wav *.aif *.aiff *.flac *.ogg *.mp3);;All Files (*)";
    QStringList paths;
    if (multi_) {
        paths = QFileDialog::getOpenFileNames(this, windowTitle(), start, filter);
    } else {
        const QString one = QFileDialog::getOpenFileName(this, windowTitle(), start, filter);
        if (!one.isEmpty()) paths << one;
    }
    if (paths.isEmpty()) return;
    chosen_ = paths;
    accept();
}

} // namespace OpenDaw
