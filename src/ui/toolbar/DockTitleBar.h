#pragma once

#include <QWidget>

class QDockWidget;
class QToolButton;

namespace OpenDaw {

// A flat, compact title bar for dock panels: the title in small capitals on
// the theme's dockTitle colour, with float and close buttons. Mouse presses on
// the bar itself are left to the QDockWidget, so dragging, docking and
// double-click-to-float work as with the native title bar.
class DockTitleBar : public QWidget {
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* dock);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Installs a DockTitleBar on the dock.
    static void install(QDockWidget* dock);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateButtons();

    QDockWidget* dock_ = nullptr;
    QToolButton* floatBtn_ = nullptr;
    QToolButton* closeBtn_ = nullptr;
};

} // namespace OpenDaw
