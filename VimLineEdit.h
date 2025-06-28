
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
    FindForward,
    FindBackward,
    RepeatFind,
};

std::string to_string(VimLineEditCommand cmd);
bool requires_symbol(VimLineEditCommand cmd);


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
enum class FindDirection{
    Forward,
    Backward,
};

struct FindState{
    FindDirection direction;
    std::optional<char> character;
};

class VimLineEdit : public QLineEdit
{
    Q_OBJECT

private:
    VimMode current_mode;
    InputTreeNode input_tree;
    InputTreeNode* current_node = nullptr;

    std::optional<VimLineEditCommand> pending_symbol_command = {};

    std::optional<FindState> last_find_state = {};

    void set_style_for_mode(VimMode mode);

public:
    explicit VimLineEdit(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

    void add_vim_keybindings();
    std::optional<VimLineEditCommand> handle_key_event(int key, Qt::KeyboardModifiers modifiers);
    void handle_command(VimLineEditCommand cmd, std::optional<char> symbol = {});
    void handle_find(FindState find_state);
};

#endif // VIMLINEEDIT_H
