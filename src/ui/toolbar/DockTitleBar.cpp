#include "DockTitleBar.h"
#include "utils/ThemeManager.h"
#include "utils/IconFont.h"
#include <QDockWidget>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>
#include <algorithm>

namespace OpenDaw {

namespace {
constexpr int kTitleHeight = 22;

QFont titleFont(const QFont& base)
{
    QFont f = base;
    f.setPixelSize(11);
    f.setBold(true);
    f.setCapitalization(QFont::SmallCaps);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    return f;
}
} // namespace

DockTitleBar::DockTitleBar(QDockWidget* dock)
    : QWidget(dock), dock_(dock)
{
    setAccessibleName("Panel Title Bar");
    setAttribute(Qt::WA_OpaquePaintEvent);
    auto& theme = ThemeManager::instance().current();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 3, 0);
    layout->setSpacing(1);
    layout->addStretch();

    const int iconPx = 13;
    const QFont mi = icons::materialIcons(iconPx);
    auto makeBtn = [&](const QChar& glyph, const QString& name) {
        auto* b = new QToolButton(this);
        b->setAccessibleName(name);
        b->setToolTip(name);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIcon(icons::glyphIcon(mi, glyph, theme.dockTitleText, iconPx));
        b->setIconSize(QSize(iconPx, iconPx));
        b->setFixedSize(18, 18);
        b->setStyleSheet(
            QString("QToolButton { background: transparent; border: none; border-radius: %1px; "
                    "padding: 0px; }"
                    "QToolButton:hover { background: %2; }")
                .arg(theme.cornerRadius).arg(theme.surfaceLight.name()));
        layout->addWidget(b);
        return b;
    };
    floatBtn_ = makeBtn(icons::mi::OpenInNew, "Float or Dock Panel");
    closeBtn_ = makeBtn(icons::mi::Close, "Close Panel");

    connect(floatBtn_, &QToolButton::clicked, this, [this]() {
        if (dock_) dock_->setFloating(!dock_->isFloating());
    });
    connect(closeBtn_, &QToolButton::clicked, this, [this]() {
        if (dock_) dock_->close();
    });
    connect(dock_, &QDockWidget::featuresChanged, this, [this]() { updateButtons(); });
    connect(dock_, &QDockWidget::windowTitleChanged, this, [this]() { update(); });
    updateButtons();
}

void DockTitleBar::install(QDockWidget* dock)
{
    if (dock) dock->setTitleBarWidget(new DockTitleBar(dock));
}

QSize DockTitleBar::sizeHint() const
{
    const QFontMetrics fm(titleFont(font()));
    return {fm.horizontalAdvance(dock_ ? dock_->windowTitle() : QString()) + 60, kTitleHeight};
}

QSize DockTitleBar::minimumSizeHint() const
{
    return {48, kTitleHeight};
}

void DockTitleBar::updateButtons()
{
    if (!dock_) return;
    const auto features = dock_->features();
    floatBtn_->setVisible(features.testFlag(QDockWidget::DockWidgetFloatable));
    closeBtn_->setVisible(features.testFlag(QDockWidget::DockWidgetClosable));
}

void DockTitleBar::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    p.fillRect(rect(), theme.dockTitle);
    p.fillRect(QRect(0, height() - 1, width(), 1), theme.separator);

    // Room for the visible buttons on the right.
    int right = width() - 4;
    if (floatBtn_->isVisible()) right -= 19;
    if (closeBtn_->isVisible()) right -= 19;

    const QFont f = titleFont(font());
    p.setFont(f);
    p.setPen(theme.dockTitleText);
    const QRect textRect(8, 0, std::max(0, right - 8), height());
    const QString title = dock_ ? dock_->windowTitle() : QString();
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(f).elidedText(title, Qt::ElideRight, textRect.width()));
}

} // namespace OpenDaw
