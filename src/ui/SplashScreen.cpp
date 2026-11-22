#include "SplashScreen.h"
#include <QKeyEvent>
#include <QPropertyAnimation>
#include "utils/ThemeManager.h"
#include <QPainter>
#include <QSettings>
#include <QScreen>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QFont>
#include <QDebug>

#ifndef OpenDaw_VERSION
#define OpenDaw_VERSION "dev"
#endif

namespace OpenDaw {

static constexpr int kSplashWidth  = 800;
static constexpr int kSplashHeight = 450;

// The splash shows before the main window loads the saved theme, so look the
// saved one up here (read-only) and fall back to the current (default) theme.
static Theme splashTheme()
{
    const QString saved = QSettings().value("ui/theme").toString();
    if (!saved.isEmpty())
        for (const auto& t : ThemeManager::builtInThemes())
            if (t.name.compare(saved, Qt::CaseInsensitive) == 0)
                return t;
    return ThemeManager::instance().current();
}

static QColor withAlpha(const QColor& c, int alpha)
{
    QColor out = c;
    out.setAlpha(alpha);
    return out;
}

SplashScreen::SplashScreen(bool deleteOnClose, QWidget* parent)
    // Not "stay on top": the splash must never cover other programs or the
    // main window once it is up.
    : QWidget(parent, Qt::FramelessWindowHint | Qt::SplashScreen)
{
    if (deleteOnClose)
        setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::PointingHandCursor);

    QPixmap raw(":/splash.png");
    if (raw.isNull()) {
        qWarning() << "SplashScreen: failed to load :/splash.png";
        background_ = QPixmap(kSplashWidth, kSplashHeight);
        background_.fill(splashTheme().background.darker(160));
    } else {
        background_ = raw.scaled(kSplashWidth, kSplashHeight,
                                 Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
    }

    setFixedSize(kSplashWidth, kSplashHeight);

    if (auto* screen = QGuiApplication::primaryScreen()) {
        QRect screenGeom = screen->availableGeometry();
        move((screenGeom.width()  - width())  / 2,
             (screenGeom.height() - height()) / 2);
    }
}

void SplashScreen::finish()
{
    ready_ = true;
    update();
}

void SplashScreen::finishAndClose(int fadeMs)
{
    ready_ = true;
    if (closing_) return;
    closing_ = true;
    if (fadeMs <= 0 || !isVisible()) {
        dismiss();
        return;
    }
    auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(fadeMs);
    fade->setStartValue(windowOpacity());
    fade->setEndValue(0.0);
    connect(fade, &QPropertyAnimation::finished, this, &SplashScreen::dismiss);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void SplashScreen::dismiss()
{
    emit dismissed();
    close();
}

void SplashScreen::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        closing_ = true;
        dismiss();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SplashScreen::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    p.drawPixmap(0, 0, background_);

    const Theme theme = splashTheme();
    // The text sits on a dark band whatever the theme, so it stays legible
    // over the artwork; the accent colours come from the theme.
    const QColor band = theme.isLight ? theme.text.darker(140) : theme.background.darker(170);
    const QColor titleColour = theme.isLight ? theme.background : theme.text;
    const QColor dimColour = theme.isLight ? theme.surface.darker(115) : theme.textDim;

    int textZoneY = height() * 3 / 4;
    QLinearGradient fade(0, textZoneY - 60, 0, textZoneY + 10);
    fade.setColorAt(0.0, withAlpha(band, 0));
    fade.setColorAt(1.0, withAlpha(band, 225));
    p.fillRect(0, textZoneY - 60, width(), height() - textZoneY + 60, fade);
    // A thin accent rule along the bottom edge.
    p.fillRect(0, height() - 3, width(), 3, theme.accent);

    int textAreaTop = textZoneY - 30;

    QFont titleFont("Segoe UI", 26, QFont::Bold);
    p.setFont(titleFont);
    p.setPen(titleColour);
    p.drawText(QRect(0, textAreaTop, width(), 36), Qt::AlignCenter, "OpenDaw");

    QFont versionFont("Segoe UI", 11);
    p.setFont(versionFont);
    p.setPen(theme.accentLight);
    QString versionStr = QString("Version %1").arg(OpenDaw_VERSION);
    p.drawText(QRect(0, textAreaTop + 36, width(), 20), Qt::AlignCenter, versionStr);

    QFont devFont("Segoe UI", 9);
    p.setFont(devFont);
    p.setPen(titleColour.darker(120));
    p.drawText(QRect(0, textAreaTop + 58, width(), 18), Qt::AlignCenter,
               "Developed by Glen Rhodes");

    QFont smallFont("Segoe UI", 8);
    p.setFont(smallFont);
    p.setPen(dimColour);
    QString copyright = QString::fromUtf8(
        "\u00A9 2025\u20132026 OpenDaw  \u2022  Licensed under GPLv3");
    p.drawText(QRect(0, textAreaTop + 78, width(), 16), Qt::AlignCenter, copyright);

    if (!ready_) {
        QFont loadFont("Segoe UI", 8, QFont::Normal, true);
        p.setFont(loadFont);
        p.setPen(dimColour);
        p.drawText(QRect(0, height() - 22, width(), 18), Qt::AlignCenter, "Loading\u2026");
    } else {
        QFont loadFont("Segoe UI", 8);
        p.setFont(loadFont);
        p.setPen(dimColour);
        p.drawText(QRect(0, height() - 22, width(), 18), Qt::AlignCenter,
                   "Click anywhere to continue");
    }
}

void SplashScreen::mousePressEvent(QMouseEvent* event)
{
    // A click always gets it out of the way, even while still loading.
    closing_ = true;
    if (ready_) {
        dismiss();
    } else {
        hide();
    }
    QWidget::mousePressEvent(event);
}

} // namespace OpenDaw
