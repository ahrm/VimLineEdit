#ifndef VIMLINEEDIT_H
#define VIMLINEEDIT_H

#include <QLineEdit>
#include <QKeyEvent>
#include <QtGui/qevent.h>
#include <vector>
#include <string>
#include <deque>
#include <memory>
#include <unordered_map>
#include <QTextEdit>
#include <QTextCursor>

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
    EnterVisualLineMode,
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
    FindForward,
    FindBackward,
    FindForwardTo,
    FindBackwardTo,
    RepeatFind,
    RepeatFindReverse,
    RepeatSearch,
    RepeatSearchReverse,
    Delete,
    Change,
    Yank,
    DeleteToEndOfLine,
    ChangeToEndOfLine,
    PasteForward,
    PasteBackward,
    Undo,
    Redo,
    InsertLineBelow,
    InsertLineAbove,
    ToggleVisualCursor,
    MoveToBeginningOfLine,
    MoveToEndOfLine,
    DeleteCharAndEnterInsertMode,
    DeleteCurrentLine,
    ChangeCurrentLine,
    YankCurrentLine,
    CommandCommand,
    SearchCommand,
    ReverseSearchCommand,
    DeletePreviousWord,
    IncrementNextNumberOnCurrentLine,
    DecrementNextNumberOnCurrentLine,
    InsertLastInsertModeText,
    SetMark,
    GotoMark,
    RecordMacro,
    RepeatMacro,
    SearchTextUnderCursor,
    SearchTextUnderCursorBackward,
    GotoMatchingBracket,
    Uppercasify,
};


enum class ActionWaitingForMotionKind{
    Delete,
    Change,
    Yank,
    Visual,
};

struct Mark{
    int position;
    int name;
};

struct Macro{
    std::vector<std::unique_ptr<QKeyEvent>> events;
    int name;
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

struct KeyboardModifierState{
    bool shift = false;
    bool control = false;
    bool command = false;
    bool alt = false;

    static KeyboardModifierState from_qt_modifiers(Qt::KeyboardModifiers modifiers);
};

struct KeyChord{
    std::variant<int, QString> key = -1;
    KeyboardModifierState modifiers;
};


bool equal_with_shift(const KeyboardModifierState& lhs, const KeyboardModifierState& rhs);
bool equal_withotu_shift(const KeyboardModifierState &lhs, const KeyboardModifierState &rhs);

struct KeyBinding{
    std::vector<KeyChord> key_chords;
    VimLineEditCommand command;
};

struct InputTreeNode{
    KeyChord key_chord;
    std::vector<InputTreeNode> children;
    std::optional<VimLineEditCommand> command;

    void add_keybinding(const std::vector<KeyChord>& key_chords, int index, VimLineEditCommand cmd);

    InputTreeNode clone() const;

};


enum class VimMode{
    Normal,
    Insert,
    Visual,
    VisualLine,
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

struct SearchState{
    FindDirection direction;
    std::optional<QString> query;
};

struct HistoryState{
    QString text;
    int cursor_position;
    std::unordered_map<int, Mark> marks;
};

struct History{
    std::deque<HistoryState> states;
    int current_index = -1;

};

// same as QLineEdit but fires a signal when the escape key is pressed
class EscapeLineEdit : public QLineEdit
{
    Q_OBJECT
    public:
        EscapeLineEdit(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;

signals:
    void escapePressed();
};

struct LastDeletedTextState {
    QString text;
    bool is_line = false;
};

class VimLineEdit : public QTextEdit
{
    Q_OBJECT

private:
    VimMode current_mode = VimMode::Normal;
    int visual_mode_anchor = -1;
    InputTreeNode normal_mode_input_tree;
    InputTreeNode visual_mode_input_tree;
    InputTreeNode insert_mode_input_tree;
    EscapeLineEdit* command_line_edit;

    InputTreeNode* current_node = nullptr;

    std::optional<VimLineEditCommand> pending_symbol_command = {};
    std::optional<VimLineEditCommand> pending_text_command = {};

    std::optional<ActionWaitingForMotion> action_waiting_for_motion = {};

    LastDeletedTextState last_deleted_text;
    QString last_insert_mode_text = "";
    QString current_insert_mode_text = "";
    QString current_command_repeat_number = "";

    std::unordered_map<int, Mark> marks;
    std::unordered_map<int, Macro> macros;
    std::optional<Macro> current_macro = {};
    int last_macro_symbol = -1;

    std::optional<FindState> last_find_state = {};
    std::optional<SearchState> last_search_state = {};
    History history;
    int visual_line_selection_begin = -1;
    int visual_line_selection_end = -1;

    void set_style_for_mode(VimMode mode);

public:
    explicit VimLineEdit(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

    QString get_word_under_cursor_bounds(int& start, int& end);
    void add_vim_keybindings();
    std::optional<VimLineEditCommand> handle_key_event(QString event_text, int key, Qt::KeyboardModifiers modifiers);
    void handle_command(VimLineEditCommand cmd, std::optional<char> symbol = {});
    int calculate_find(FindState find_state, bool reverse=false) const;

    void resizeEvent(QResizeEvent* event) override;

private:
    int calculate_move_word_forward(bool with_symbols) const;
    int calculate_move_to_end_of_word(bool with_symbols) const;
    int calculate_move_word_backward(bool with_symbols) const;

    int calculate_move_down(int old_pos);
    int calculate_move_up(int old_pos);

    int calculate_move_down_on_screen();
    int calculate_move_up_on_screen();
    int calculate_move_on_screen(int direction);

    void delete_char(bool is_single);
    bool handle_surrounding_motion_action();

    void push_history(HistoryState state);
    void undo();
    void redo();

    void set_cursor_position(int pos);
    void set_cursor_position_with_selection(int pos);
    void set_cursor_position_with_line_selection(int pos);

    int get_line_start_position(int cursor_pos);
    int get_line_end_position(int cursor_pos);
    int get_ith_line_start_position(int i);
    void show_command_line_edit();
    void hide_command_line_edit();
    void perform_pending_text_command_with_text(QString text);
    void handle_action_waiting_for_motion(int old_pos, int new_pos, int delete_pos_offset);
    void handle_search(bool reverse=false);
    void set_last_deleted_text(QString text, bool is_line=false);
    void handle_number_increment_decrement(bool increment);
    void set_mode(VimMode mode);
    void remove_text(int begin, int num);
    void insert_text(QString text, int left_index, int right_index = -1);
    bool requires_symbol(VimLineEditCommand cmd);
    void add_event_to_current_macro(QKeyEvent* event);
    void set_visual_selection(int begin, int length);
    QString get_current_selection(int& begin, int& end);
};

#endif // VIMLINEEDIT_H
