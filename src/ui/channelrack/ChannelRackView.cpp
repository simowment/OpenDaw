#include "ChannelRackView.h"
#include "ChannelRow.h"
#include "SoundPicker.h"
#include "StepRow.h"
#include "engine/PatternManager.h"
#include "ui/effects/PluginEditorWindow.h"
#include "utils/ThemeManager.h"
#include "utils/IconFont.h"
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace OpenDaw {

namespace {

QString noteName(int note)
{
    static const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    note = std::clamp(note, 0, 127);
    return QString("%1%2").arg(kNames[note % 12]).arg(note / 12 - 1);
}

} // namespace

ChannelRackView::ChannelRackView(EditManager* editMgr, PatternManager* patterns,
                                 PatternPlayer* player, QWidget* parent)
    : QWidget(parent), editMgr_(editMgr), patterns_(patterns), player_(player)
{
    setAccessibleName("Channel Rack");
    setAcceptDrops(true);
    auto& theme = ThemeManager::instance().current();

    setAutoFillBackground(true);
    QPalette pal;
    pal.setColor(QPalette::Window, theme.background);
    setPalette(pal);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    buildHeader(layout);

    scroll_ = new QScrollArea(this);
    scroll_->setAccessibleName("Channel Rack Channels");
    scroll_->setFrameStyle(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    content_ = new QWidget();
    content_->setAutoFillBackground(true);
    content_->setPalette(pal);
    rowsLayout_ = new QVBoxLayout(content_);
    rowsLayout_->setContentsMargins(0, 2, 8, 6);
    rowsLayout_->setSpacing(0);

    // Step numbers, lined up over the step columns.
    auto* rulerRow = new QWidget(content_);
    rulerRow_ = rulerRow;
    auto* rulerLayout = new QHBoxLayout(rulerRow);
    rulerLayout->setContentsMargins(0, 0, 0, 0);
    rulerLayout->setSpacing(0);
    rulerLayout->addSpacing(ChannelRow::kLeftWidth);
    ruler_ = new StepRuler(rulerRow);
    rulerLayout->addWidget(ruler_);
    rulerLayout->addStretch(1);
    rowsLayout_->addWidget(rulerRow);

    picker_ = new SoundPicker(editMgr_, patterns_, this);
    connect(picker_, &SoundPicker::scanPluginsRequested, this, &ChannelRackView::scanPluginsRequested);
    connect(picker_, &SoundPicker::statusMessage, this, &ChannelRackView::statusMessage);

    // Rows are inserted above this button.
    addRowWidget_ = new QWidget(content_);
    addRowBtn_ = new QPushButton("+  Add channel", addRowWidget_);
    addRowBtn_->setAccessibleName("Add Channel");
    addRowBtn_->setToolTip("Add a channel: a sample, a built-in instrument, Surge XT or a VST "
                           "plugin. You can also drop audio files on the rack.");
    addRowBtn_->setFocusPolicy(Qt::NoFocus);
    addRowBtn_->setFixedSize(ChannelNameButton::kWidth, 22);
    addRowBtn_->setStyleSheet(
        QString("QPushButton { background: transparent; color: %1; border: 1px dashed %2; "
                "border-radius: %3px; font-size: 10px; padding: 0px; }"
                "QPushButton:hover { color: %4; border-color: %5; }")
            .arg(theme.textDim.name(), theme.border.lighter(130).name())
            .arg(theme.cornerRadius)
            .arg(theme.text.name(), theme.accent.name()));
    auto* addRow = new QHBoxLayout(addRowWidget_);
    addRow->setContentsMargins(ChannelRow::kLeftWidth - ChannelNameButton::kWidth - 8, 4, 0, 0);
    addRow->addWidget(addRowBtn_);
    addRow->addStretch(1);
    rowsLayout_->addWidget(addRowWidget_);

    emptyState_ = buildEmptyState(content_);
    rowsLayout_->addWidget(emptyState_, 3);
    rowsLayout_->addStretch(1);

    scroll_->setWidget(content_);
    layout->addWidget(scroll_, 1);

    connect(addRowBtn_, &QPushButton::clicked, this, [this]() {
        showAddChannelMenu(addRowBtn_->mapToGlobal(QPoint(0, addRowBtn_->height())));
    });

    // ── Model wiring ──
    connect(editMgr_, &EditManager::aboutToChangeEdit, this, [this]() { clearRows(false); });
    connect(editMgr_, &EditManager::tracksChanged, this, &ChannelRackView::scheduleRebuild);
    connect(editMgr_, &EditManager::editChanged, this, [this]() {
        // An undo can bring tracks back or take them away without tracksChanged.
        bool same = patterns_ != nullptr;
        if (same) {
            const auto channels = patterns_->channels();
            same = channels.size() == rows_.size();
            for (int i = 0; same && i < channels.size(); ++i)
                same = rows_[i] && rows_[i]->trackId() == channels[i]->itemID;
        }
        if (!same) {
            scheduleRebuild();
            return;
        }
        refreshControls();
        refreshSteps();
        refreshHeader();
    });

    if (patterns_) {
        connect(patterns_, &PatternManager::patternsChanged, this, [this]() {
            refreshHeader();
            refreshSteps();
        });
        connect(patterns_, &PatternManager::patternChanged, this, [this](int id) {
            if (id == patterns_->currentPattern()) refreshSteps();
        });
        connect(patterns_, &PatternManager::currentPatternChanged, this, [this](int) {
            refreshHeader();
            refreshSteps();
            updatePlayingStep(-1);
        });
    }
    if (player_) {
        connect(player_, &PatternPlayer::stepChanged, this, [this](int step) {
            const bool ours = patterns_ && player_->playingPattern() == patterns_->currentPattern();
            updatePlayingStep(ours ? step : -1);
        });
        connect(player_, &PatternPlayer::playingChanged, this, [this](bool playing) {
            if (!playing) updatePlayingStep(-1);
        });
    }

    rebuildRows();
}

void ChannelRackView::buildHeader(QVBoxLayout* layout)
{
    auto& theme = ThemeManager::instance().current();

    auto* header = new QWidget(this);
    header->setObjectName("channelRackHeader");
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(30);
    header->setStyleSheet(
        QString("QWidget#channelRackHeader { background: %1; border-bottom: 1px solid %2; }"
                "QLabel { background: transparent; }")
            .arg(theme.surface.name(), theme.separator.name()));

    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(8, 3, 6, 3);
    h->setSpacing(6);

    patternChip_ = new QLabel(header);
    patternChip_->setFixedSize(12, 12);
    h->addWidget(patternChip_);

    patternLabel_ = new QLabel(header);
    patternLabel_->setAccessibleName("Rack Pattern");
    patternLabel_->setMinimumWidth(120);
    h->addWidget(patternLabel_);

    auto* stepsCaption = new QLabel("STEPS", header);
    stepsCaption->setStyleSheet(QString("QLabel { color: %1; font-size: 9px; font-weight: bold; "
                                        "letter-spacing: 1px; }").arg(theme.textDim.name()));
    h->addWidget(stepsCaption);

    lengthCombo_ = new QComboBox(header);
    lengthCombo_->setAccessibleName("Pattern Length");
    lengthCombo_->setToolTip("Pattern length in steps (sixteenth notes)");
    lengthCombo_->setFocusPolicy(Qt::NoFocus);
    for (int n : {16, 32, 64})
        lengthCombo_->addItem(QString::number(n), n);
    lengthCombo_->setFixedWidth(64);
    h->addWidget(lengthCombo_);
    connect(lengthCombo_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        const int steps = lengthCombo_->itemData(index).toInt();
        const int id = ensureCurrentPattern();
        if (id > 0 && steps > 0) patterns_->setPatternLength(id, steps);
    });

    h->addStretch(1);

    placeBtn_ = new QPushButton("Place in Playlist", header);
    placeBtn_->setAccessibleName("Place Pattern In Playlist");
    placeBtn_->setToolTip("Place the current pattern in the playlist at the playhead's bar (Ctrl+P)");
    placeBtn_->setFocusPolicy(Qt::NoFocus);
    placeBtn_->setFixedHeight(22);
    h->addWidget(placeBtn_);
    connect(placeBtn_, &QPushButton::clicked, this, [this]() { emit placePatternRequested(); });

    const int iconPx = 15;
    addChannelBtn_ = new QToolButton(header);
    addChannelBtn_->setAccessibleName("Add Channel");
    addChannelBtn_->setToolTip("Add a channel: a sample, a built-in instrument, Surge XT or a "
                               "VST plugin");
    addChannelBtn_->setFocusPolicy(Qt::NoFocus);
    addChannelBtn_->setIcon(icons::glyphIcon(icons::materialIcons(iconPx), icons::mi::Add,
                                             theme.accentLight, iconPx));
    addChannelBtn_->setIconSize(QSize(iconPx, iconPx));
    addChannelBtn_->setFixedSize(24, 22);
    h->addWidget(addChannelBtn_);
    connect(addChannelBtn_, &QToolButton::clicked, this, [this]() {
        showAddChannelMenu(addChannelBtn_->mapToGlobal(QPoint(0, addChannelBtn_->height())));
    });

    layout->addWidget(header);
}

void ChannelRackView::scheduleRebuild()
{
    if (rebuildQueued_) return;
    rebuildQueued_ = true;
    // Queued: tracksChanged can arrive from inside a row's own handler.
    QTimer::singleShot(0, this, [this]() { rebuildRows(); });
}

void ChannelRackView::clearRows(bool commitPending)
{
    if (commitPending) {
        // Send waiting step edits before their rows go. Copy: a commit's
        // signals may reach refreshSteps, which walks rows_.
        const auto rows = rows_;
        for (const auto& row : rows)
            if (row && row->steps()->isEditing()) row->steps()->commit();
    }
    dropRow_ = nullptr;
    for (auto& row : rows_) {
        if (!row) continue;
        rowsLayout_->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    rows_.clear();
}

void ChannelRackView::rebuildRows()
{
    rebuildQueued_ = false;
    clearRows();

    if (patterns_ && editMgr_ && editMgr_->edit()) {
        const auto channels = patterns_->channels();
        for (int i = 0; i < channels.size(); ++i) {
            auto* row = new ChannelRow(editMgr_, patterns_, channels[i], content_);
            connect(row, &ChannelRow::selectRequested, this, &ChannelRackView::selectChannel);
            connect(row, &ChannelRow::openRequested, this, &ChannelRackView::openChannel);
            connect(row, &ChannelRow::menuRequested, this, &ChannelRackView::showChannelMenu);
            connect(row, &ChannelRow::stepsEdited, this, &ChannelRackView::onStepsEdited);
            row->setSelected(selectedId_.isValid() && row->trackId() == selectedId_);
            // After the ruler row (index 0).
            rowsLayout_->insertWidget(1 + i, row);
            rows_.push_back(row);
        }
    }

    updateEmptyState();
    addRowBtn_->setEnabled(editMgr_ && editMgr_->edit());
    addChannelBtn_->setEnabled(editMgr_ && editMgr_->edit());
    refreshHeader();
    refreshSteps();
    updatePlayingStep(-1);
}

void ChannelRackView::refreshControls()
{
    for (auto& row : rows_)
        if (row) row->refreshControls();
}

void ChannelRackView::refreshSteps()
{
    const int current = patterns_ ? patterns_->currentPattern() : 0;
    const std::optional<Pattern> pat = current > 0 ? patterns_->pattern(current) : std::nullopt;
    const int length = pat ? std::max(1, pat->lengthSteps) : 16;
    ruler_->setLength(length);
    for (auto& row : rows_) {
        if (!row) continue;
        const PatternChannel* channel = pat ? pat->channel(row->trackId()) : nullptr;
        row->steps()->setSteps(channel ? channel->steps : QVector<PatternStep>{}, length);
    }
}

void ChannelRackView::refreshHeader()
{
    auto& theme = ThemeManager::instance().current();
    const int current = patterns_ ? patterns_->currentPattern() : 0;
    const std::optional<Pattern> pat = current > 0 ? patterns_->pattern(current) : std::nullopt;

    const QColor chip = pat && pat->colour.isValid() ? pat->colour : theme.border;
    patternChip_->setStyleSheet(QString("QLabel { background: %1; border: 1px solid %2; "
                                        "border-radius: 2px; }")
                                    .arg(chip.name(), theme.separator.name()));
    if (pat) {
        const QColor text = theme.isLight ? chip.darker(150) : chip.lighter(115);
        patternLabel_->setText(pat->name);
        patternLabel_->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; "
                                             "font-size: 12px; }").arg(text.name()));
    } else {
        patternLabel_->setText("No pattern: click a step to start one");
        patternLabel_->setStyleSheet(QString("QLabel { color: %1; font-style: italic; "
                                             "font-size: 11px; }").arg(theme.textDim.name()));
    }

    const int length = pat ? pat->lengthSteps : 16;
    {
        QSignalBlocker block(lengthCombo_);
        int idx = lengthCombo_->findData(length);
        if (idx < 0) {
            // A length set elsewhere (the AI, an older project): list it too.
            int insertAt = 0;
            while (insertAt < lengthCombo_->count()
                   && lengthCombo_->itemData(insertAt).toInt() < length)
                ++insertAt;
            lengthCombo_->insertItem(insertAt, QString::number(length), length);
            idx = insertAt;
        }
        lengthCombo_->setCurrentIndex(idx);
    }
    placeBtn_->setEnabled(pat.has_value());
}

void ChannelRackView::updatePlayingStep(int step)
{
    ruler_->setPlayingStep(step);
    for (auto& row : rows_)
        if (row) row->steps()->setPlayingStep(step);
}

int ChannelRackView::ensureCurrentPattern()
{
    if (!patterns_ || !editMgr_ || !editMgr_->edit()) return 0;
    const int current = patterns_->currentPattern();
    if (current > 0 && patterns_->pattern(current)) return current;

    const auto all = patterns_->patterns();
    if (!all.isEmpty()) {
        patterns_->setCurrentPattern(all.first().id);
        return all.first().id;
    }
    const int id = patterns_->createPattern();
    if (id > 0) patterns_->setCurrentPattern(id);
    return id;
}

void ChannelRackView::onStepsEdited(te::EditItemID trackId, const QVector<PatternStep>& steps)
{
    const int id = ensureCurrentPattern();
    if (id <= 0) return;
    // The whole row in one go: one clip regeneration and one undo step per
    // click or drag.
    patterns_->setChannelSteps(id, trackId, steps);
}

void ChannelRackView::selectChannel(te::EditItemID trackId)
{
    selectedId_ = trackId;
    for (auto& row : rows_)
        if (row) row->setSelected(row->trackId() == trackId);
    if (auto* track = editMgr_ ? editMgr_->findAudioTrack(trackId) : nullptr)
        emit trackSelected(track);
}

void ChannelRackView::setSelectedTrack(te::AudioTrack* track)
{
    selectedId_ = track ? track->itemID : te::EditItemID();
    for (auto& row : rows_)
        if (row) row->setSelected(selectedId_.isValid() && row->trackId() == selectedId_);
}

void ChannelRackView::setPluginScanner(PluginScanner* scanner)
{
    picker_->setPluginScanner(scanner);
}

void ChannelRackView::selectNewChannel(te::AudioTrack* track)
{
    if (!track) return;
    selectedId_ = track->itemID;
    for (auto& row : rows_)
        if (row) row->setSelected(row->trackId() == selectedId_);
    emit trackSelected(track);
}

void ChannelRackView::openChannel(te::EditItemID trackId)
{
    selectChannel(trackId);
    if (openPluginWindow(trackId)) return;
    openPianoRoll(trackId);
}

bool ChannelRackView::openPluginWindow(te::EditItemID trackId)
{
    auto* track = editMgr_ ? editMgr_->findAudioTrack(trackId) : nullptr;
    if (!track) return false;
    auto* ext = dynamic_cast<te::ExternalPlugin*>(editMgr_->getTrackInstrument(track));
    if (!ext) return false;
    PluginEditorWindow::showForPlugin(*ext);
    return true;
}

void ChannelRackView::openPianoRoll(te::EditItemID trackId)
{
    if (!patterns_ || !editMgr_ || !editMgr_->findAudioTrack(trackId)) return;
    selectChannel(trackId);
    const int id = ensureCurrentPattern();
    if (id <= 0) return;
    // The channel's clip in the pattern's first placement (placing the
    // pattern / adding the clip when needed): piano-roll edits go back into
    // the pattern and every placement of it.
    if (auto* clip = patterns_->editableClipFor(id, trackId)) {
        emit editMgr_->midiClipDoubleClicked(clip);
        return;
    }
    emit statusMessage("Couldn't open this channel's notes in the piano roll", 6000);
}

void ChannelRackView::renameChannel(te::EditItemID trackId)
{
    auto* t = editMgr_ ? editMgr_->findAudioTrack(trackId) : nullptr;
    if (!t) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        window(), "Rename Channel", "Channel name:", QLineEdit::Normal,
        channelsound::displayName(*editMgr_, patterns_, t), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    auto* mgr = editMgr_;
    // Queued: tracksChanged rebuilds the rows, including the one that asked.
    QTimer::singleShot(0, mgr, [mgr, trackId, name]() { channelsound::renameChannel(*mgr, trackId, name); });
}

void ChannelRackView::cloneChannel(te::EditItemID trackId)
{
    if (!patterns_ || !editMgr_) return;
    QPointer<ChannelRackView> self(this);
    QTimer::singleShot(0, this, [self, trackId]() {
        if (!self) return;
        QString error;
        auto* copy = channelsound::cloneChannel(*self->editMgr_, *self->patterns_, trackId, &error);
        if (!copy) {
            emit self->statusMessage(QString("Couldn't clone the channel: %1").arg(error), 8000);
            return;
        }
        self->selectNewChannel(copy);
        emit self->statusMessage(QString("Cloned as \"%1\" (Ctrl+Z to undo)")
                                     .arg(QString::fromUtf8(copy->getName().toRawUTF8())), 5000);
    });
}

void ChannelRackView::deleteChannel(te::EditItemID trackId)
{
    auto* t = editMgr_ ? editMgr_->findAudioTrack(trackId) : nullptr;
    if (!t) return;
    const QString name = channelsound::displayName(*editMgr_, patterns_, t);
    QMessageBox box(QMessageBox::Warning, "Delete Channel",
                    QString("Delete the channel \"%1\"?").arg(name),
                    QMessageBox::NoButton, window());
    box.setInformativeText("Its sound, its notes in every pattern and its clips in the playlist "
                           "are removed. Edit > Undo (Ctrl+Z) brings them back.");
    auto* del = box.addButton("Delete", QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != del) return;
    auto* mgr = editMgr_;
    QTimer::singleShot(0, mgr, [mgr, trackId]() {
        auto* track = mgr->findAudioTrack(trackId);
        if (!track || !mgr->edit()) return;
        if (!mgr->inUndoGroup()) mgr->edit()->getUndoManager().beginNewTransaction("Delete Channel");
        mgr->removeTrack(track);
    });
}

void ChannelRackView::showChannelMenu(te::EditItemID trackId, const QPoint& globalPos)
{
    auto* track = editMgr_ ? editMgr_->findAudioTrack(trackId) : nullptr;
    if (!track) return;
    selectChannel(trackId);

    QMenu menu(this);
    menu.setAccessibleName("Channel Menu");
    menu.setToolTipsVisible(true);
    auto* mgr = editMgr_;

    if (patterns_) {
        auto* replace = menu.addMenu("Replace &sound");
        replace->setAccessibleName("Replace Sound");
        QPointer<ChannelRackView> self(this);
        picker_->populate(*replace, SoundPicker::Purpose::ReplaceSound, trackId,
                          [self, trackId](const SoundChoice& choice) {
                              if (self) self->replaceChannelSound(trackId, choice);
                          });
    }
    if (dynamic_cast<te::ExternalPlugin*>(mgr->getTrackInstrument(track)))
        menu.addAction("Open plugin &window", this, [this, trackId]() { openPluginWindow(trackId); });
    menu.addAction("&Piano roll", this, [this, trackId]() {
        QTimer::singleShot(0, this, [this, trackId]() { openPianoRoll(trackId); });
    });
    menu.addSeparator();
    menu.addAction("&Rename...", this, [this, trackId]() { renameChannel(trackId); });

    auto* colourMenu = menu.addMenu("&Colour");
    colourMenu->setAccessibleName("Channel Colour");
    const auto& palette = ThemeManager::trackPalette();
    for (int slot = 0; slot < int(palette.size()); ++slot) {
        QPixmap swatch(14, 14);
        swatch.fill(palette[size_t(slot)]);
        // Queued: the colour change refreshes the rows.
        colourMenu->addAction(QIcon(swatch), QString(), mgr, [mgr, trackId, slot]() {
            QTimer::singleShot(0, mgr, [mgr, trackId, slot]() {
                auto* t = mgr->findAudioTrack(trackId);
                if (!t || !mgr->edit()) return;
                if (!mgr->inUndoGroup())
                    mgr->edit()->getUndoManager().beginNewTransaction("Channel Colour");
                mgr->setTrackColourSlot(*t, slot);
            });
        });
    }

    if (patterns_) {
        const int root = patterns_->channelRootNote(trackId);
        menu.addAction(QString("Root &note (%1)...").arg(noteName(root)), this,
                       [this, trackId, root]() {
            bool ok = false;
            const int note = QInputDialog::getInt(
                window(), "Root Note",
                "MIDI note the channel's steps play (60 = C4; drum kits use their own map):",
                root, 0, 127, 1, &ok);
            if (ok && note != root) patterns_->setChannelRootNote(trackId, note);
        });

        auto* clear = menu.addAction("Cl&ear steps", this, [this, trackId]() {
            const int id = patterns_->currentPattern();
            if (id > 0) patterns_->clearChannel(id, trackId);
        });
        clear->setEnabled(patterns_->currentPattern() > 0);
    }

    menu.addSeparator();
    auto* clone = menu.addAction("Clo&ne channel", this, [this, trackId]() { cloneChannel(trackId); });
    clone->setToolTip("A copy of this channel: same sound and settings, same notes");
    clone->setEnabled(patterns_ != nullptr);
    menu.addAction("&Delete channel...", this, [this, trackId]() {
        QTimer::singleShot(0, this, [this, trackId]() { deleteChannel(trackId); });
    });

    menu.exec(globalPos);
}

// ── Sample channels ─────────────────────────────────────────────────────────

QStringList ChannelRackView::audioFilesIn(const QMimeData* mime)
{
    QStringList files;
    if (!mime || !mime->hasUrls()) return files;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (SampleLibrary::hasSampleExtension(path) && QFileInfo(path).isFile())
            files << path;
    }
    return files;
}

ChannelRow* ChannelRackView::rowAtGlobal(const QPoint& globalPos) const
{
    for (const auto& row : rows_) {
        if (!row || !row->isVisible()) continue;
        if (row->rect().contains(row->mapFromGlobal(globalPos))) return row;
    }
    return nullptr;
}

void ChannelRackView::setDropRow(ChannelRow* row)
{
    if (dropRow_ == row) return;
    if (dropRow_) dropRow_->setDropHighlight(false);
    dropRow_ = row;
    if (row) row->setDropHighlight(true);
}

void ChannelRackView::dragEnterEvent(QDragEnterEvent* event)
{
    if (patterns_ && editMgr_ && editMgr_->edit() && !audioFilesIn(event->mimeData()).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        setDropRow(rowAtGlobal(mapToGlobal(event->position().toPoint())));
        return;
    }
    event->ignore();
}

void ChannelRackView::dragMoveEvent(QDragMoveEvent* event)
{
    if (!patterns_ || !editMgr_ || !editMgr_->edit()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    setDropRow(rowAtGlobal(mapToGlobal(event->position().toPoint())));
}

void ChannelRackView::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDropRow(nullptr);
    QWidget::dragLeaveEvent(event);
}

void ChannelRackView::dropEvent(QDropEvent* event)
{
    const QStringList files = audioFilesIn(event->mimeData());
    ChannelRow* row = rowAtGlobal(mapToGlobal(event->position().toPoint()));
    setDropRow(nullptr);
    if (files.isEmpty() || !patterns_) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();

    // Loading samples takes a moment: do it after the drop has returned, so
    // the drag source (Explorer, the browser) isn't held up.
    const te::EditItemID target = row ? row->trackId() : te::EditItemID();
    QPointer<ChannelRackView> self(this);
    QTimer::singleShot(0, this, [self, files, target]() {
        if (!self) return;
        QStringList rest = files;
        if (target.isValid() && self->editMgr_->findAudioTrack(target)) {
            self->replaceChannelSample(target, rest.takeFirst());
            if (rest.isEmpty()) return;
        }
        self->addSampleChannels(rest);
    });
}

int ChannelRackView::addSampleChannels(const QStringList& paths)
{
    if (!patterns_ || !editMgr_ || !editMgr_->edit()) return 0;
    QStringList failed;
    QStringList readable;
    for (const QString& path : paths) {
        const juce::File file(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
        if (PatternManager::isAudioSampleFile(file)) readable << path;
        else failed << QFileInfo(path).fileName();
    }
    const auto added = channelsound::addSampleChannels(*patterns_, readable, &failed);
    if (!added.isEmpty()) selectNewChannel(added.last());
    if (!failed.isEmpty())
        emit statusMessage(QString("Couldn't add as a sample channel: %1").arg(failed.join(", ")), 8000);
    else if (added.size() == 1)
        emit statusMessage(QString("Added sample channel \"%1\"")
                               .arg(QFileInfo(readable.first()).completeBaseName()), 5000);
    else if (added.size() > 1)
        emit statusMessage(QString("Added %1 sample channels").arg(added.size()), 5000);
    return int(added.size());
}

bool ChannelRackView::replaceChannelSample(te::EditItemID trackId, const QString& path)
{
    return replaceChannelSound(trackId, SoundChoice::fromSamples({path}));
}

bool ChannelRackView::replaceChannelSound(te::EditItemID trackId, const SoundChoice& choice)
{
    if (!patterns_ || !editMgr_ || !editMgr_->edit() || !choice.isValid()) return false;
    if (choice.kind == SoundChoice::Kind::Samples) {
        const QString path = choice.samples.first();
        const juce::File file(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
        if (!PatternManager::isAudioSampleFile(file)) {
            emit statusMessage(QString("\"%1\" isn't an audio file that can be played")
                                   .arg(QFileInfo(path).fileName()), 6000);
            return false;
        }
    }
    QString error;
    if (!channelsound::replaceSound(*editMgr_, *patterns_, trackId, choice, &error)) {
        emit statusMessage(QString("Couldn't use \"%1\": %2").arg(choice.name, error), 8000);
        return false;
    }
    selectChannel(trackId);
    emit statusMessage(QString("Channel sound replaced by \"%1\" (Ctrl+Z to undo)").arg(choice.name), 5000);
    // A new plugin: show its window, its presets are picked there.
    if (choice.kind == SoundChoice::Kind::Plugin) openPluginWindow(trackId);
    return true;
}

void ChannelRackView::addChannelWithSound(const SoundChoice& choice)
{
    if (!editMgr_ || !editMgr_->edit() || !choice.isValid()) return;
    if (choice.kind == SoundChoice::Kind::Samples) {
        addSampleChannels(choice.samples);
        return;
    }
    QString error;
    auto* track = channelsound::addInstrumentChannel(*editMgr_, choice, &error);
    if (!track) {
        emit statusMessage(QString("Couldn't add \"%1\": %2").arg(choice.name, error), 8000);
        return;
    }
    selectNewChannel(track);
    emit statusMessage(QString("Added channel \"%1\": click its steps to make a beat").arg(choice.name), 5000);
    if (choice.kind == SoundChoice::Kind::Plugin) openPluginWindow(track->itemID);
}

void ChannelRackView::chooseSampleFiles()
{
    if (!patterns_ || !editMgr_ || !editMgr_->edit()) return;
    const QStringList paths = picker_->browseSamples(true, "Add Sample Channel");
    if (!paths.isEmpty()) addSampleChannels(paths);
}

void ChannelRackView::showAddChannelMenu(const QPoint& globalPos)
{
    if (!editMgr_ || !editMgr_->edit()) return;
    QPointer<ChannelRackView> self(this);
    picker_->exec(globalPos, SoundPicker::Purpose::AddChannel, {},
                  [self](const SoundChoice& choice) {
                      if (self) self->addChannelWithSound(choice);
                  },
                  patterns_ ? SoundPicker::AllSections : SoundPicker::Instruments);
}

QWidget* ChannelRackView::buildEmptyState(QWidget* parent)
{
    auto& theme = ThemeManager::instance().current();
    auto* box = new QWidget(parent);
    box->setAccessibleName("Channel Rack Empty");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(12, 16, 12, 16);
    v->setSpacing(10);
    v->addStretch(1);

    auto* title = new QLabel("No channels yet", box);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QString("QLabel { color: %1; font-size: 13px; font-weight: bold; }")
                             .arg(theme.text.name()));
    v->addWidget(title);

    auto* hint = new QLabel("A channel is a sound (a sample or an instrument) that plays the "
                            "pattern's steps.", box);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(theme.textDim.name()));
    v->addWidget(hint);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch(1);
    emptyAddSampleBtn_ = new QPushButton("Add sample...", box);
    emptyAddSampleBtn_->setAccessibleName("Add Sample Channel");
    emptyAddSampleBtn_->setIcon(icons::glyphIcon(icons::fontAudio(16), icons::fa::Waveform,
                                                 theme.accentLight, 16));
    emptyAddSampleBtn_->setFocusPolicy(Qt::NoFocus);
    emptyAddSampleBtn_->setEnabled(patterns_ != nullptr);
    buttons->addWidget(emptyAddSampleBtn_);
    connect(emptyAddSampleBtn_, &QPushButton::clicked, this, [this]() {
        QTimer::singleShot(0, this, [this]() { chooseSampleFiles(); });
    });

    emptyAddInstrumentBtn_ = new QPushButton("Add instrument", box);
    emptyAddInstrumentBtn_->setAccessibleName("Add Instrument Channel");
    emptyAddInstrumentBtn_->setIcon(icons::glyphIcon(icons::materialIcons(16), icons::mi::Piano,
                                                     theme.accentLight, 16));
    emptyAddInstrumentBtn_->setFocusPolicy(Qt::NoFocus);
    auto* instrumentMenu = new QMenu(emptyAddInstrumentBtn_);
    instrumentMenu->setAccessibleName("Add Instrument Menu");
    // Built when shown: the plugin list may have changed since.
    connect(instrumentMenu, &QMenu::aboutToShow, this, [this, instrumentMenu]() {
        instrumentMenu->clear();
        QPointer<ChannelRackView> self(this);
        picker_->populate(*instrumentMenu, SoundPicker::Purpose::AddChannel, {},
                          [self](const SoundChoice& choice) {
                              if (self) self->addChannelWithSound(choice);
                          },
                          SoundPicker::Instruments);
    });
    emptyAddInstrumentBtn_->setMenu(instrumentMenu);
    buttons->addWidget(emptyAddInstrumentBtn_);
    buttons->addStretch(1);
    v->addLayout(buttons);

    auto* drag = new QLabel("or drag samples from the Browser", box);
    drag->setAlignment(Qt::AlignCenter);
    drag->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-style: italic; }")
                            .arg(theme.textDim.name()));
    v->addWidget(drag);
    v->addStretch(1);
    return box;
}

void ChannelRackView::updateEmptyState()
{
    const bool haveEdit = editMgr_ && editMgr_->edit();
    const bool empty = rows_.isEmpty();
    emptyState_->setVisible(empty);
    rulerRow_->setVisible(!empty);
    addRowWidget_->setVisible(!empty);
    emptyAddSampleBtn_->setEnabled(haveEdit && patterns_);
    emptyAddInstrumentBtn_->setEnabled(haveEdit);
}

} // namespace OpenDaw
