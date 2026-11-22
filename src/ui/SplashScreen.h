#pragma once

#include <QWidget>
#include <QPixmap>
#include <QString>

namespace OpenDaw {

class SplashScreen : public QWidget {
    Q_OBJECT

public:
    explicit SplashScreen(bool deleteOnClose = true, QWidget* parent = nullptr);

    // About box: shows "Click anywhere to continue" and waits for a click.
    void finish();
    // Start-up: fades out on its own (no click needed) and emits dismissed().
    void finishAndClose(int fadeMs = 250);

signals:
    void dismissed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QPixmap background_;
    bool    ready_ = false;
    bool    closing_ = false;
    void dismiss();
};

} // namespace OpenDaw
