#include "StepRow.h"
#include "utils/ThemeManager.h"
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>

namespace OpenDaw {

using namespace stepgrid;

namespace {

QColor blend(const QColor& a, const QColor& b, double t)
{
    return QColor(int(a.red() * t + b.red() * (1.0 - t)),
                  int(a.green() * t + b.green() * (1.0 - t)),
                  int(a.blue() * t + b.blue() * (1.0 - t)));
}

constexpr int kRowPadding = 3;   // above and below the cells

} // namespace

// ── StepRow ─────────────────────────────────────────────────────────────────

StepRow::StepRow(QWidget* parent)
    : QWidget(parent)
{
    setAccessibleName("Step Sequencer Row");
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(sizeHint());

    wheelTimer_ = new QTimer(this);
    wheelTimer_->setSingleShot(true);
    wheelTimer_->setInterval(kWheelCommitMs);
    connect(wheelTimer_, &QTimer::timeout, this, [this]() {
        if (drag_ == Drag::None) commit();
    });
}

StepRow::~StepRow() = default;

QSize StepRow::sizeHint() const
{
    return {stepsWidth(length_) + 2, kCellHeight + 2 * kRowPadding};
}

void StepRow::commit()
{
    wheelTimer_->stop();
    if (!dirty_) return;
    dirty_ = false;
    QVector<PatternStep> row = steps_;
    row.resize(length_);
    emit stepsEdited(row);
}

void StepRow::setSteps(const QVector<PatternStep>& steps, int length)
{
    // Don't overwrite what the user is drawing; the commit brings the model
    // back in line and refreshes the row again.
    if (isEditing()) return;
    length = std::clamp(length, 1, 256);
    steps_ = steps;
    if (steps_.size() > length) steps_.resize(length);
    if (length != length_) {
        length_ = length;
        setFixedSize(sizeHint());
        updateGeometry();
    }
    update();
}

void StepRow::setPlayingStep(int step)
{
    if (step == playing_) return;
    const int old = playing_;
    playing_ = step;
    if (old >= 0 && old < length_) update(cellRect(old).adjusted(-2, -2, 2, 2));
    if (step >= 0 && step < length_) update(cellRect(step).adjusted(-2, -2, 2, 2));
}

void StepRow::setAccentColour(const QColor& colour)
{
    accent_ = colour;
    update();
}

QRect StepRow::cellRect(int step) const
{
    return QRect(1 + stepX(step), kRowPadding, kCellWidth, kCellHeight);
}

PatternStep StepRow::stepAtIndex(int step) const
{
    if (step >= 0 && step < steps_.size()) return steps_[step];
    return {};
}

void StepRow::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double radius = std::clamp(theme.cornerRadius, 0, 3);

    for (int i = 0; i < length_; ++i) {
        const QRectF cell = QRectF(cellRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool altGroup = (i / kStepsPerGroup) % 2 == 1;
        const QColor off = altGroup ? theme.stepOffAlt : theme.stepOff;
        const PatternStep s = stepAtIndex(i);
        const bool playing = i == playing_;

        if (!s.on) {
            QColor fill = off;
            if (hover_ == i && drag_ == Drag::None) fill = fill.lighter(112);
            if (playing) fill = blend(theme.stepPlaying, off, 0.35);
            p.setPen(QPen(theme.separator, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(cell, radius, radius);
            // A faint top edge, so the pads read as buttons without gradients.
            p.setPen(QPen(fill.lighter(122), 1.0));
            p.drawLine(QPointF(cell.left() + 2, cell.top() + 1.0),
                       QPointF(cell.right() - 2, cell.top() + 1.0));
        } else {
            QColor on = theme.stepOn;
            if (playing) on = on.lighter(125);
            else if (hover_ == i && drag_ == Drag::None) on = on.lighter(108);

            // Dim pad, filled from the bottom as high as the velocity.
            p.setPen(Qt::NoPen);
            p.setBrush(blend(on, off, 0.32));
            p.drawRoundedRect(cell, radius, radius);

            const double frac = std::clamp(s.velocity / 127.0, 0.08, 1.0);
            const QRectF inner = cell.adjusted(1.0, 1.0, -1.0, -1.0);
            const QRectF fillRect(inner.left(), inner.bottom() - inner.height() * frac,
                                  inner.width(), inner.height() * frac);
            p.setBrush(on);
            p.drawRoundedRect(fillRect, std::max(0.0, radius - 1.0), std::max(0.0, radius - 1.0));

            const QColor edge = accent_.isValid() ? accent_ : on.darker(150);
            p.setPen(QPen(edge, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(cell, radius, radius);
        }

        if (playing) {
            p.setPen(QPen(theme.stepPlaying, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(cell.adjusted(-1.0, -1.0, 1.0, 1.0), radius + 1.0, radius + 1.0);
        }
    }
}

void StepRow::applyStep(int step, bool on, int velocity)
{
    if (step < 0 || step >= length_) return;
    if (steps_.size() < length_) steps_.resize(length_);
    PatternStep& s = steps_[step];
    const bool velocityChanges = velocity > 0 && velocity != s.velocity;
    if (s.on == on && !velocityChanges) return;
    s.on = on;
    if (velocity > 0) s.velocity = velocity;
    update(cellRect(step).adjusted(-2, -2, 2, 2));
    dirty_ = true;
}

void StepRow::showVelocityTip(int step, int velocity, const QPoint& globalPos)
{
    QToolTip::showText(globalPos, QString("Step %1: velocity %2").arg(step + 1).arg(velocity), this);
}

void StepRow::mousePressEvent(QMouseEvent* event)
{
    // A wheel edit still waiting goes first, as its own step.
    if (drag_ == Drag::None) commit();
    const QPoint pos = event->position().toPoint();
    const int step = stepAt(pos.x() - 1, length_);
    if (step < 0) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::RightButton) {
        drag_ = Drag::Erase;
        lastDragStep_ = step;
        applyStep(step, false, -1);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
        // Velocity: drag up or down from where the pad was clicked.
        const PatternStep cur = stepAtIndex(step);
        drag_ = Drag::Velocity;
        velocityStep_ = step;
        velocityStartY_ = pos.y();
        velocityStart_ = cur.on ? cur.velocity : 100;
        if (!cur.on) applyStep(step, true, velocityStart_);
        showVelocityTip(step, velocityStart_, event->globalPosition().toPoint());
        event->accept();
        return;
    }

    drag_ = Drag::Paint;
    paintOn_ = !stepAtIndex(step).on;
    lastDragStep_ = step;
    applyStep(step, paintOn_, -1);
    event->accept();
}

void StepRow::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();

    if (drag_ == Drag::None) {
        const int step = stepAt(pos.x() - 1, length_);
        if (step != hover_) {
            const int old = hover_;
            hover_ = step;
            if (old >= 0) update(cellRect(old).adjusted(-2, -2, 2, 2));
            if (step >= 0) update(cellRect(step).adjusted(-2, -2, 2, 2));
        }
        return;
    }

    if (drag_ == Drag::Velocity) {
        const int v = std::clamp(velocityStart_ + (velocityStartY_ - pos.y()) * 2, 1, 127);
        applyStep(velocityStep_, true, v);
        showVelocityTip(velocityStep_, v, event->globalPosition().toPoint());
        return;
    }

    // Paint / erase: fill every step crossed, so a fast drag leaves no holes.
    const int x = std::clamp(pos.x() - 1, 0, std::max(0, stepsWidth(length_) - 1));
    const int step = stepAt(x, length_);
    if (step < 0 || step == lastDragStep_) return;
    const bool on = drag_ == Drag::Paint ? paintOn_ : false;
    const int from = lastDragStep_ < 0 ? step : lastDragStep_;
    const int dir = step > from ? 1 : -1;
    for (int s = from + dir; ; s += dir) {
        applyStep(s, on, -1);
        if (s == step) break;
    }
    lastDragStep_ = step;
}

void StepRow::mouseReleaseEvent(QMouseEvent* event)
{
    const bool wasDragging = drag_ != Drag::None;
    drag_ = Drag::None;
    lastDragStep_ = -1;
    velocityStep_ = -1;
    update();
    if (wasDragging) {
        // Last: the commit can rebuild the rack (and delete this row) later,
        // but not synchronously from here.
        commit();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void StepRow::mouseDoubleClickEvent(QMouseEvent* event)
{
    // A quick second click is just another click: it toggles the step back.
    mousePressEvent(event);
}

void StepRow::wheelEvent(QWheelEvent* event)
{
    const int step = stepAt(int(event->position().x()) - 1, length_);
    const PatternStep cur = stepAtIndex(step);
    if (step < 0 || !cur.on || event->angleDelta().y() == 0) {
        event->ignore();   // let the rack scroll
        return;
    }
    const int delta = event->angleDelta().y() > 0 ? 8 : -8;
    const int v = std::clamp(cur.velocity + delta, 1, 127);
    applyStep(step, true, v);
    showVelocityTip(step, v, event->globalPosition().toPoint());
    if (drag_ == Drag::None) wheelTimer_->start();
    event->accept();
}

void StepRow::leaveEvent(QEvent* event)
{
    if (hover_ >= 0) {
        const int old = hover_;
        hover_ = -1;
        update(cellRect(old).adjusted(-2, -2, 2, 2));
    }
    QWidget::leaveEvent(event);
}

// ── StepRuler ───────────────────────────────────────────────────────────────

StepRuler::StepRuler(QWidget* parent)
    : QWidget(parent)
{
    setAccessibleName("Step Ruler");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(sizeHint());
}

QSize StepRuler::sizeHint() const
{
    return {stepsWidth(length_) + 2, 16};
}

void StepRuler::setLength(int steps)
{
    steps = std::clamp(steps, 1, 256);
    if (steps == length_) return;
    length_ = steps;
    setFixedSize(sizeHint());
    updateGeometry();
    update();
}

void StepRuler::setPlayingStep(int step)
{
    if (step == playing_) return;
    playing_ = step;
    update();
}

void StepRuler::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    QFont f = font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);

    for (int i = 0; i < length_; ++i) {
        const QRect cell(1 + stepX(i), 0, kCellWidth, height());
        // Playhead lamp under each step.
        const QRect lamp(cell.left() + 3, height() - 4, kCellWidth - 6, 3);
        p.fillRect(lamp, i == playing_ ? theme.stepPlaying : theme.controlTrack);
        if (i % kStepsPerGroup == 0) {
            p.setPen(i % (kStepsPerGroup * 4) == 0 ? theme.text : theme.textDim);
            p.drawText(QRect(cell.left(), 0, kCellWidth * 2, height() - 4),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(i + 1));
        }
    }
}

} // namespace OpenDaw
