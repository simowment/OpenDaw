#pragma once

#include "engine/PatternManager.h"
#include <QColor>
#include <QVector>
#include <QTimer>
#include <QWidget>

namespace OpenDaw {

// Geometry shared by the step grid and its ruler, so the columns line up.
namespace stepgrid {
    constexpr int kCellWidth  = 16;
    constexpr int kCellHeight = 22;
    constexpr int kCellGap    = 2;
    constexpr int kGroupGap   = 5;   // extra space between groups of four steps
    constexpr int kStepsPerGroup = 4;

    inline int stepX(int step)
    {
        return step * (kCellWidth + kCellGap) + (step / kStepsPerGroup) * kGroupGap;
    }
    inline int stepsWidth(int steps)
    {
        return steps <= 0 ? 0 : stepX(steps - 1) + kCellWidth;
    }
    // The step whose column contains x (the gap after a step counts as that
    // step, so a drag never falls through), or -1 outside 0..steps-1.
    inline int stepAt(int x, int steps)
    {
        if (x < 0 || steps <= 0) return -1;
        constexpr int groupWidth = kStepsPerGroup * (kCellWidth + kCellGap) + kGroupGap;
        const int group = x / groupWidth;
        const int within = x - group * groupWidth;
        int inGroup = within / (kCellWidth + kCellGap);
        if (inGroup >= kStepsPerGroup) inGroup = kStepsPerGroup - 1;
        const int step = group * kStepsPerGroup + inGroup;
        return step < steps ? step : -1;
    }
}

// One channel's row of steps in the current pattern, painted in one widget
// (no widget per step). Click toggles a step, dragging paints the same state
// across the row, right-click / right-drag clears, Ctrl- or Shift-drag (or the
// wheel) sets a step's velocity, shown as the height of its fill.
//
// Edits are shown at once but committed in one go: a click or drag when the
// mouse is released, wheel turns once the wheel rests (kWheelCommitMs). Each
// commit regenerates the pattern's clips and is one undo step, so a drag
// across 16 steps is one regeneration instead of 16.
class StepRow : public QWidget {
    Q_OBJECT

public:
    explicit StepRow(QWidget* parent = nullptr);
    ~StepRow() override;

    static constexpr int kWheelCommitMs = 250;

    // steps may be shorter than length (missing steps are off). Ignored while
    // an edit is in progress (the row shows the user's uncommitted steps).
    void setSteps(const QVector<PatternStep>& steps, int length);
    // True while a drag or an uncommitted wheel edit is pending.
    bool isEditing() const { return drag_ != Drag::None || dirty_; }
    // Sends a pending edit now (no-op when there is none).
    void commit();
    void setPlayingStep(int step);   // -1: none
    void setAccentColour(const QColor& colour);   // the channel's colour, for the on-steps' edge
    int length() const { return length_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    // The whole row after an edit (sized to length()).
    void stepsEdited(const QVector<PatternStep>& steps);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Drag { None, Paint, Erase, Velocity };

    QRect cellRect(int step) const;
    PatternStep stepAtIndex(int step) const;
    void applyStep(int step, bool on, int velocity);
    void showVelocityTip(int step, int velocity, const QPoint& globalPos);

    QVector<PatternStep> steps_;
    int length_ = 16;
    int playing_ = -1;
    int hover_ = -1;
    QColor accent_;

    Drag drag_ = Drag::None;
    bool paintOn_ = true;
    int lastDragStep_ = -1;
    int velocityStep_ = -1;
    int velocityStartY_ = 0;
    int velocityStart_ = 100;
    bool dirty_ = false;          // steps_ has edits not yet sent
    QTimer* wheelTimer_ = nullptr;
};

// Step numbers over the grid, with the pattern playhead marked.
class StepRuler : public QWidget {
    Q_OBJECT

public:
    explicit StepRuler(QWidget* parent = nullptr);
    void setLength(int steps);
    void setPlayingStep(int step);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int length_ = 16;
    int playing_ = -1;
};

} // namespace OpenDaw
