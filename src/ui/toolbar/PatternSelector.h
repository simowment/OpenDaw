#pragma once

#include <QWidget>

class QComboBox;
class QToolButton;

namespace OpenDaw {

class PatternManager;

// The top strip's pattern picker: a combo of the Edit's patterns, each shown
// in its colour, plus new / clone / rename / colour / delete buttons. It holds
// no state of its own: the patterns and the current one live in PatternManager,
// so the picker survives a window rebuild (theme change) unchanged.
class PatternSelector : public QWidget {
    Q_OBJECT

public:
    explicit PatternSelector(PatternManager* patterns, QWidget* parent = nullptr);

    // Also used by menu actions.
    void newPattern();
    void cloneCurrentPattern();
    void renameCurrentPattern();
    void deleteCurrentPattern();

private:
    void rebuild();
    void syncCurrent();
    void updateButtons();
    void showColourMenu();

    PatternManager* patterns_ = nullptr;
    QComboBox*   combo_     = nullptr;
    QToolButton* addBtn_    = nullptr;
    QToolButton* cloneBtn_  = nullptr;
    QToolButton* renameBtn_ = nullptr;
    QToolButton* colourBtn_ = nullptr;
    QToolButton* deleteBtn_ = nullptr;
};

} // namespace OpenDaw
