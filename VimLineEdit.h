
#ifndef VIMLINEEDIT_H
#define VIMLINEEDIT_H

#include <QLineEdit>
#include <QKeyEvent>
#include <vector>
#include <string>
#include <deque>
#include <QTextEdit>


enum class VimLineEditCommand{
    GotoBegin,
    GotoEnd,
    EnterInsertMode,
    EnterInsertModeAfter,
    EnterInsertModeBegin,
    EnterInsertModeEnd,
    EnterInsertModeBeginLine,
    EnterInsertModeEndLine,
    EnterNormalMode,
    EnterVisualMode,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveUpOnScreen,
    MoveDownOnScreen,
    MoveToBeginning,
    MoveToEnd,
    MoveWordForward,
    MoveWordForwardWithSymbols,
    MoveToEndOfWord,
    MoveToEndOfWordWithSymbols,
    MoveWordBackward,
    MoveWordBackwardWithSymbols,
    DeleteChar,
    DeleteInsideWord,
    DeleteInsideParentheses,
    DeleteInsideBrackets,
    DeleteInsideBraces,
    FindForward,
    FindBackward,
    FindForwardTo,
    FindBackwardTo,
    RepeatFind,
    RepeatFindReverse,
    Delete,
    PasteForward,
    Undo,
    Redo,
    InsertLineBelow,
    InsertLineAbove
};

enum class ActionWaitingForMotionKind{
    Delete,
    Change,
    Yank,
    Visual,
};

enum class SurroundingScope{
    None,
    Around,
    Inside,
};

enum class SurroundingKind{
    None,
    Parentheses,
    Brackets,
    Braces,
    SingleQuotes,
    DoubleQuotes,
    Backticks,
    Word,
};

struct ActionWaitingForMotion{
    ActionWaitingForMotionKind kind;
    SurroundingScope surrounding_scope = SurroundingScope::None;
    SurroundingKind surrounding_kind = SurroundingKind::None;
};

std::string to_string(VimLineEditCommand cmd);
bool requires_symbol(VimLineEditCommand cmd);

struct KeyboardModifierState{
    bool shift = false;
    bool control = false;
    bool command = false;
    bool alt = false;

    static KeyboardModifierState from_qt_modifiers(Qt::KeyboardModifiers modifiers);
};

struct KeyChord{
    int key;
    KeyboardModifierState modifiers;
};


bool operator==(const KeyboardModifierState& lhs, const KeyboardModifierState& rhs);

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
    ForwardTo,
    BackwardTo,
};

struct FindState{
    FindDirection direction;
    std::optional<char> character;
};

struct HistoryState{
    QString text;
    int cursor_position;
};

struct History{
    std::deque<HistoryState> states;
    int current_index = -1;

};

class VimLineEdit : public QTextEdit
{
    Q_OBJECT

private:
    VimMode current_mode;
    int visual_mode_anchor = -1;
    InputTreeNode input_tree;
    InputTreeNode* current_node = nullptr;

    std::optional<VimLineEditCommand> pending_symbol_command = {};
    std::optional<ActionWaitingForMotion> action_waiting_for_motion = {};
    QString last_deleted_text = "";

    std::optional<FindState> last_find_state = {};
    History history;

    void set_style_for_mode(VimMode mode);

public:
    explicit VimLineEdit(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

    void add_vim_keybindings();
    std::optional<VimLineEditCommand> handle_key_event(int key, Qt::KeyboardModifiers modifiers);
    void handle_command(VimLineEditCommand cmd, std::optional<char> symbol = {});
    int calculate_find(FindState find_state, bool reverse=false) const;

private:
    int calculate_move_word_forward(bool with_symbols) const;
    int calculate_move_to_end_of_word(bool with_symbols) const;
    int calculate_move_word_backward(bool with_symbols) const;

    int calculate_move_down();
    int calculate_move_up();

    int calculate_move_down_on_screen();
    int calculate_move_up_on_screen();
    int calculate_move_on_screen(int direction);

    void delete_char();
    void handle_surrounding_motion_action();

    void push_history(const QString& text, int cursor_position);
    void undo();
    void redo();

    void set_cursor_position(int pos);

    int get_line_start_position(int cursor_pos);
    int get_line_end_position(int cursor_pos);
};

#endif // VIMLINEEDIT_H
