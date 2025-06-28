
#ifndef VIMLINEEDIT_H
#define VIMLINEEDIT_H

#include <QLineEdit>
#include <QKeyEvent>
#include <vector>
#include <string>

enum class VimLineEditCommand{
    EnterInsertMode,
    EnterInsertModeAfter,
    EnterInsertModeBegin,
    EnterInsertModeEnd,
    EnterNormalMode,
    EnterVisualMode,
    MoveLeft,
    MoveRight,
    MoveToBeginning,
    MoveToEnd,
    MoveWordLeft,
    MoveWordRight,
    DeleteInsideWord,
    DeleteInsideParentheses,
    DeleteInsideBrackets,
    DeleteInsideBraces,
};

std::string to_string(VimLineEditCommand cmd);


struct KeyChord{
    int key;
    Qt::KeyboardModifiers modifiers;
};

struct KeyBinding{
    std::vector<KeyChord> key_chords;
    VimLineEditCommand command;
};

struct InputTreeNode{
    KeyChord key_chord;
    std::vector<InputTreeNode> children;
    std::optional<VimLineEditCommand> command;

    void add_keybinding(const std::vector<KeyChord>& key_chords, int index, VimLineEditCommand cmd);

};


enum class VimMode{
    Normal,
    Insert,
    Visual,
};

class VimLineEdit : public QLineEdit
{
    Q_OBJECT

private:
    VimMode current_mode;
    InputTreeNode input_tree;
    InputTreeNode* current_node = nullptr;

    void set_style_for_mode(VimMode mode);

public:
    explicit VimLineEdit(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

    void add_vim_keybindings();
    std::optional<VimLineEditCommand> handle_key_event(int key, Qt::KeyboardModifiers modifiers);
    void handle_command(VimLineEditCommand cmd);
};

#endif // VIMLINEEDIT_H
