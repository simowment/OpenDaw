#include "PatternSelector.h"
#include "engine/PatternManager.h"
#include "utils/ThemeManager.h"
#include "utils/IconFont.h"
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QToolButton>

namespace OpenDaw {

namespace {

QIcon colourChip(const QColor& colour, int size = 12)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(colour.darker(160), 1.0));
    p.setBrush(colour.isValid() ? colour : QColor(128, 128, 128));
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1.0, size - 1.0), 2.0, 2.0);
    return QIcon(pm);
}

// Pattern colours are chosen for dark panels; keep them legible on light ones.
QColor legibleOn(const QColor& colour, const Theme& theme)
{
    if (!colour.isValid()) return theme.text;
    return theme.isLight ? colour.darker(150) : colour.lighter(115);
}

} // namespace

PatternSelector::PatternSelector(PatternManager* patterns, QWidget* parent)
    : QWidget(parent), patterns_(patterns)
{
    setAccessibleName("Pattern Selector");
    auto& theme = ThemeManager::instance().current();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    combo_ = new QComboBox(this);
    combo_->setAccessibleName("Current Pattern");
    combo_->setToolTip("Current pattern: the channel rack edits it, and Play loops it in "
                       "Pattern mode");
    combo_->setMinimumWidth(128);
    combo_->setMaximumWidth(200);
    combo_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    combo_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(combo_);

    const int iconPx = 15;
    const QFont mi = icons::materialIcons(iconPx);
    auto makeBtn = [&](const QChar& glyph, const QString& name, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setAccessibleName(name);
        b->setToolTip(tip);
        b->setIcon(icons::glyphIcon(mi, glyph, theme.text, iconPx));
        b->setIconSize(QSize(iconPx, iconPx));
        b->setFixedSize(24, 24);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(
            QString("QToolButton { background: %1; border: 1px solid %2; border-radius: %3px; "
                    "padding: 0px; }"
                    "QToolButton:hover { background: %4; border-color: %5; }"
                    "QToolButton:pressed { background: %6; }"
                    "QToolButton:disabled { background: transparent; border-color: transparent; }")
                .arg(theme.background.name(), theme.separator.name())
                .arg(theme.cornerRadius)
                .arg(theme.surfaceLight.name(), theme.border.lighter(140).name(),
                     theme.accent.name()));
        layout->addWidget(b);
        return b;
    };
    addBtn_    = makeBtn(icons::mi::Add, "New Pattern", "New pattern");
    cloneBtn_  = makeBtn(icons::mi::ContentCopy, "Clone Pattern", "Clone the current pattern");
    renameBtn_ = makeBtn(icons::mi::Edit, "Rename Pattern", "Rename the current pattern");
    colourBtn_ = makeBtn(icons::mi::Palette, "Pattern Colour", "Pattern colour");
    deleteBtn_ = makeBtn(icons::mi::Delete, "Delete Pattern",
                         "Delete the current pattern and its playlist placements");

    connect(addBtn_, &QToolButton::clicked, this, &PatternSelector::newPattern);
    connect(cloneBtn_, &QToolButton::clicked, this, &PatternSelector::cloneCurrentPattern);
    connect(renameBtn_, &QToolButton::clicked, this, &PatternSelector::renameCurrentPattern);
    connect(colourBtn_, &QToolButton::clicked, this, &PatternSelector::showColourMenu);
    connect(deleteBtn_, &QToolButton::clicked, this, &PatternSelector::deleteCurrentPattern);

    connect(combo_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (!patterns_ || index < 0) return;
        const int id = combo_->itemData(index).toInt();
        if (id > 0) patterns_->setCurrentPattern(id);
    });

    if (patterns_) {
        connect(patterns_, &PatternManager::patternsChanged, this, &PatternSelector::rebuild);
        connect(patterns_, &PatternManager::currentPatternChanged, this,
                [this](int) { syncCurrent(); });
    }
    rebuild();
}

void PatternSelector::rebuild()
{
    QSignalBlocker block(combo_);
    combo_->clear();
    if (patterns_) {
        auto& theme = ThemeManager::instance().current();
        for (const auto& pat : patterns_->patterns()) {
            combo_->addItem(colourChip(pat.colour), pat.name, pat.id);
            combo_->setItemData(combo_->count() - 1, legibleOn(pat.colour, theme),
                                Qt::ForegroundRole);
        }
    }
    combo_->setPlaceholderText("No pattern");
    syncCurrent();
}

void PatternSelector::syncCurrent()
{
    auto& theme = ThemeManager::instance().current();
    const int current = patterns_ ? patterns_->currentPattern() : 0;
    {
        QSignalBlocker block(combo_);
        combo_->setCurrentIndex(current > 0 ? combo_->findData(current) : -1);
    }

    // The closed combo shows the pattern's name in the pattern's colour.
    QColor textColour = theme.textDim;
    if (patterns_ && current > 0)
        if (auto pat = patterns_->pattern(current))
            textColour = legibleOn(pat->colour, theme);
    combo_->setStyleSheet(QString("QComboBox { color: %1; font-weight: bold; }")
                              .arg(textColour.name()));
    updateButtons();
}

void PatternSelector::updateButtons()
{
    const bool has = patterns_ && patterns_->currentPattern() > 0;
    addBtn_->setEnabled(patterns_ != nullptr);
    cloneBtn_->setEnabled(has);
    renameBtn_->setEnabled(has);
    colourBtn_->setEnabled(has);
    deleteBtn_->setEnabled(has);
    combo_->setEnabled(patterns_ && combo_->count() > 0);
}

void PatternSelector::newPattern()
{
    if (!patterns_) return;
    const int id = patterns_->createPattern();
    if (id > 0) patterns_->setCurrentPattern(id);
}

void PatternSelector::cloneCurrentPattern()
{
    if (!patterns_ || patterns_->currentPattern() <= 0) return;
    const int id = patterns_->clonePattern(patterns_->currentPattern());
    if (id > 0) patterns_->setCurrentPattern(id);
}

void PatternSelector::renameCurrentPattern()
{
    if (!patterns_) return;
    const int id = patterns_->currentPattern();
    auto pat = id > 0 ? patterns_->pattern(id) : std::nullopt;
    if (!pat) return;
    bool ok = false;
    const QString name = QInputDialog::getText(window(), "Rename Pattern", "Pattern name:",
                                               QLineEdit::Normal, pat->name, &ok).trimmed();
    if (ok && !name.isEmpty() && name != pat->name)
        patterns_->renamePattern(id, name);
}

void PatternSelector::deleteCurrentPattern()
{
    if (!patterns_) return;
    const int id = patterns_->currentPattern();
    auto pat = id > 0 ? patterns_->pattern(id) : std::nullopt;
    if (!pat) return;

    const int placed = int(patterns_->placements(id).size());
    QString question = QString("Delete \"%1\"?").arg(pat->name);
    if (placed > 0)
        question += QString("\n\nIt is placed %1 time%2 in the playlist; those clips are "
                            "removed too.").arg(placed).arg(placed == 1 ? "" : "s");
    if (QMessageBox::question(window(), "Delete Pattern", question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    // Select a neighbour first so the rack never shows a deleted pattern.
    const auto all = patterns_->patterns();
    int next = 0;
    for (int i = 0; i < all.size(); ++i) {
        if (all[i].id != id) continue;
        if (i + 1 < all.size()) next = all[i + 1].id;
        else if (i > 0) next = all[i - 1].id;
        break;
    }
    patterns_->removePattern(id);
    if (next > 0 && patterns_->pattern(next))
        patterns_->setCurrentPattern(next);
}

void PatternSelector::showColourMenu()
{
    if (!patterns_) return;
    const int id = patterns_->currentPattern();
    auto pat = id > 0 ? patterns_->pattern(id) : std::nullopt;
    if (!pat) return;

    QMenu menu(this);
    menu.setAccessibleName("Pattern Colour");
    const auto& palette = ThemeManager::trackPalette();
    for (const auto& c : palette) {
        auto* a = menu.addAction(colourChip(c, 14), QString());
        a->setToolTip(c.name());
        const QColor colour = c;
        connect(a, &QAction::triggered, this, [this, id, colour]() {
            patterns_->setPatternColour(id, colour);
        });
    }
    menu.addSeparator();
    menu.addAction("Custom...", this, [this, id, pat]() {
        const QColor c = QColorDialog::getColor(pat->colour, window(), "Pattern Colour");
        if (c.isValid()) patterns_->setPatternColour(id, c);
    });
    menu.exec(colourBtn_->mapToGlobal(QPoint(0, colourBtn_->height())));
}

} // namespace OpenDaw
