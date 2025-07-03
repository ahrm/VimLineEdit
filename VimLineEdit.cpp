#include "VimLineEdit.h"
#include <QPainter>
#include <QtCore/qnamespace.h>
#include <QtGui/qtextcursor.h>
#include <QtWidgets/qlineedit.h>
// #include <QFontMetrics>
#include <QCommonStyle>
#include <QRegularExpression>
#include <QStyle>
#include <QTextBlock>
#include <QTextLayout>
#include <QtWidgets/qtextedit.h>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

class LineEditStyle : public QCommonStyle {
    int font_width;

  public:
    LineEditStyle(int font_width) : font_width(font_width) {}

    int pixelMetric(PixelMetric metric, const QStyleOption *option = 0,
                    const QWidget *widget = 0) const {
        if (metric == QStyle::PM_TextCursorWidth) {
            return font_width;
        }
        return QCommonStyle::pixelMetric(metric, option, widget);
    }
};

VimLineEdit::VimLineEdit(QWidget *parent) : QTextEdit(parent) {
    QFont font = this->font();
    font.setFamily("Courier New");
    font.setStyleHint(QFont::TypeWriter);
    this->setFont(font);
    add_vim_keybindings();

    command_line_edit = new EscapeLineEdit(this);
    command_line_edit->setFont(font);
    command_line_edit->hide();

    command_line_edit->setStyleSheet("background-color: lightgray;");

    QObject::connect(command_line_edit, &EscapeLineEdit::returnPressed, [&](){
        QString text = command_line_edit->text();
        perform_pending_text_command_with_text(text);
        hide_command_line_edit();

    });

    QObject::connect(command_line_edit, &EscapeLineEdit::escapePressed, [&](){
        hide_command_line_edit();
    });

    set_style_for_mode(current_mode);
}

void VimLineEdit::keyPressEvent(QKeyEvent *event) {

    if (event->key() == Qt::Key_Escape) {
        handle_command(VimLineEditCommand::EnterNormalMode, {});
        return;
    }

    if (current_mode == VimMode::Normal || current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
        // we don't want to handle when we only press a modifier (e.g. Shift, Ctrl, etc.)
        int event_key = event->key();
        bool is_keypress_a_modifier = event_key == Qt::Key_Shift || event_key == Qt::Key_Control ||
                                      event_key == Qt::Key_Alt || event_key == Qt::Key_Meta;

        if (!is_keypress_a_modifier && pending_symbol_command.has_value()) {
            // If we have a pending command that requires a symbol, we handle it now
            VimLineEditCommand command = pending_symbol_command.value();
            pending_symbol_command = {};

            if (event->text().size() > 0) {
                handle_command(command, static_cast<char>(event->text().at(0).toLatin1()));
            }

            return;
        }

        if (!is_keypress_a_modifier && action_waiting_for_motion.has_value()) {

            if (action_waiting_for_motion->surrounding_scope == SurroundingScope::None) {

                if (event->key() == Qt::Key_I) {
                    action_waiting_for_motion->surrounding_scope = SurroundingScope::Inside;
                    return;
                }
                else if (event->key() == Qt::Key_A) {
                    action_waiting_for_motion->surrounding_scope = SurroundingScope::Around;
                    return;
                }
                else{
                    // pressing dd should delete the current line
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete && event->key() == Qt::Key_D){
                        action_waiting_for_motion = {};
                        handle_command(VimLineEditCommand::DeleteCurrentLine);
                        return;
                    }
                    // pressing cc should change the current line
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change && event->key() == Qt::Key_C){
                        action_waiting_for_motion = {};
                        handle_command(VimLineEditCommand::ChangeCurrentLine);
                        return;
                    }
                }
            }
            else {
                switch (event->key()) {
                case Qt::Key_ParenLeft:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Parentheses;
                    break;
                case Qt::Key_BraceLeft:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Braces;
                    break;
                case Qt::Key_BracketLeft:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Brackets;
                    break;
                case Qt::Key_QuoteLeft:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::SingleQuotes;
                    break;
                case Qt::Key_QuoteDbl:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::DoubleQuotes;
                    break;
                case Qt::Key_W:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Word;
                    break;
                default:
                    // If the key is not recognized, we reset the action waiting for motion
                    action_waiting_for_motion = {};
                    return;
                }
            }
            if (handle_surrounding_motion_action()) {
                return;
            }
        }

        if (!is_keypress_a_modifier) {
            std::optional<VimLineEditCommand> command =
                handle_key_event(event->text(), event_key, event->modifiers());
            if (command.has_value()) {
                if (requires_symbol(command.value())) {
                    // If the command requires a symbol, we need to wait for the next key press
                    pending_symbol_command = command;
                    return;
                }
                else {
                    handle_command(command.value());
                }
            }
        }

        return;
    }
    else{
        Qt::KeyboardModifier CONTROL = Qt::ControlModifier;
        #ifdef Q_OS_MACOS
        CONTROL = Qt::MetaModifier; // On macOS, Control is often used as Command
        #endif
        auto event_key = event->key();
        bool is_keypress_a_modifier = event_key == Qt::Key_Shift || event_key == Qt::Key_Control ||
                                      event_key == Qt::Key_Alt || event_key == Qt::Key_Meta;

        if ((!is_keypress_a_modifier) && event->modifiers().testFlag(CONTROL)) {
            std::optional<VimLineEditCommand> command =
                handle_key_event(event->text(), event->key(), event->modifiers());
            
            if (command){
                handle_command(command.value());
                return;
            }
        }
    }

    QTextEdit::keyPressEvent(event);
}

void VimLineEdit::set_style_for_mode(VimMode mode) {
    if (mode == VimMode::Normal) {
        // setStyleSheet("background-color: lightgray;");
        int font_width = fontMetrics().horizontalAdvance(" ");
        setCursorWidth(font_width);
        // setStyle(new LineEditStyle(font_width));
    }
    else if (mode == VimMode::Insert) {
        // setStyleSheet("background-color: white;");
        setCursorWidth(1);
        // setStyle(new LineEditStyle(1));
    }
    else if (mode == VimMode::Visual || mode == VimMode::VisualLine) {
        // setStyleSheet("background-color: pink;");
        int font_width = fontMetrics().horizontalAdvance(" ");
        setCursorWidth(font_width);
        // setStyle(new LineEditStyle(font_width));
    }
}

void InputTreeNode::add_keybinding(const std::vector<KeyChord> &key_chords, int index,
                                   VimLineEditCommand cmd) {
    if (index >= key_chords.size()) {
        command = cmd;
        return;
    }

    KeyChord current_chord = key_chords[index];
    for (auto &child : children) {
        if (child.key_chord.key == current_chord.key &&
            equal_with_shift(child.key_chord.modifiers, current_chord.modifiers)) {
            child.add_keybinding(key_chords, index + 1, cmd);
            return;
        }
    }

    InputTreeNode new_node;
    new_node.key_chord = current_chord;
    new_node.add_keybinding(key_chords, index + 1, cmd);
    children.push_back(new_node);
}

void VimLineEdit::add_vim_keybindings() {
    KeyboardModifierState CONTROL = KeyboardModifierState{false, true, false, false};

    // KeyboardModifierState SHIFT = KeyboardModifierState{true, false, false, false};
    // KeyboardModifierState NOMOD = KeyboardModifierState{false, false, false, false};

    std::vector<KeyBinding> key_bindings = {
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"g", {}}},
                   VimLineEditCommand::GotoBegin},
        KeyBinding{{KeyChord{"G", {}}}, VimLineEditCommand::GotoEnd},
        KeyBinding{{KeyChord{Qt::Key_Escape, {}}}, VimLineEditCommand::EnterNormalMode},
        KeyBinding{{KeyChord{"i", {}}}, VimLineEditCommand::EnterInsertMode},
        KeyBinding{{KeyChord{"a", {}}}, VimLineEditCommand::EnterInsertModeAfter},
        KeyBinding{{KeyChord{"I", {}}}, VimLineEditCommand::EnterInsertModeBeginLine},
        KeyBinding{{KeyChord{"A", {}}}, VimLineEditCommand::EnterInsertModeEndLine},
        KeyBinding{{KeyChord{"v", {}}}, VimLineEditCommand::EnterVisualMode},
        KeyBinding{{KeyChord{"V", {}}}, VimLineEditCommand::EnterVisualLineMode},
        KeyBinding{{KeyChord{"h", {}}}, VimLineEditCommand::MoveLeft},
        KeyBinding{{KeyChord{"l", {}}}, VimLineEditCommand::MoveRight},
        KeyBinding{{KeyChord{"j", {}}}, VimLineEditCommand::MoveDown},
        KeyBinding{{KeyChord{"k", {}}}, VimLineEditCommand::MoveUp},
        KeyBinding{{KeyChord{Qt::Key_Left, {}}}, VimLineEditCommand::MoveLeft},
        KeyBinding{{KeyChord{Qt::Key_Right, {}}}, VimLineEditCommand::MoveRight},
        KeyBinding{{KeyChord{Qt::Key_Up, {}}}, VimLineEditCommand::MoveUp},
        KeyBinding{{KeyChord{Qt::Key_Down, {}}}, VimLineEditCommand::MoveDown},
        KeyBinding{{KeyChord{"f", {}}}, VimLineEditCommand::FindForward},
        KeyBinding{{KeyChord{"F", {}}}, VimLineEditCommand::FindBackward},
        KeyBinding{{KeyChord{";", {}}}, VimLineEditCommand::RepeatFind},
        KeyBinding{{KeyChord{",", {}}}, VimLineEditCommand::RepeatFindReverse},
        KeyBinding{{KeyChord{"n", {}}}, VimLineEditCommand::RepeatSearch},
        KeyBinding{{KeyChord{"N", {}}}, VimLineEditCommand::RepeatSearchReverse},
        KeyBinding{{KeyChord{"w", {}}}, VimLineEditCommand::MoveWordForward},
        KeyBinding{{KeyChord{"W", {}}}, VimLineEditCommand::MoveWordForwardWithSymbols},
        KeyBinding{{KeyChord{"e", {}}}, VimLineEditCommand::MoveToEndOfWord},
        KeyBinding{{KeyChord{"E", {}}}, VimLineEditCommand::MoveToEndOfWordWithSymbols},
        KeyBinding{{KeyChord{"b", {}}}, VimLineEditCommand::MoveWordBackward},
        KeyBinding{{KeyChord{"B", {}}}, VimLineEditCommand::MoveWordBackwardWithSymbols},
        KeyBinding{{KeyChord{"t", {}}}, VimLineEditCommand::FindForwardTo},
        KeyBinding{{KeyChord{"T", {}}}, VimLineEditCommand::FindBackwardTo},
        KeyBinding{{KeyChord{"x", {}}}, VimLineEditCommand::DeleteChar},
        KeyBinding{{KeyChord{"D", {}}}, VimLineEditCommand::DeleteToEndOfLine},
        KeyBinding{{KeyChord{"C", {}}}, VimLineEditCommand::ChangeToEndOfLine},
        KeyBinding{{KeyChord{"d", {}}}, VimLineEditCommand::Delete},
        KeyBinding{{KeyChord{"c", {}}}, VimLineEditCommand::Change},
        KeyBinding{{KeyChord{"p", {}}}, VimLineEditCommand::PasteForward},
        KeyBinding{{KeyChord{"u", {}}}, VimLineEditCommand::Undo},
        KeyBinding{{KeyChord{Qt::Key_R, CONTROL}}, VimLineEditCommand::Redo},
        KeyBinding{{KeyChord{"o", {}}}, VimLineEditCommand::InsertLineBelow},
        KeyBinding{{KeyChord{"O", {}}}, VimLineEditCommand::InsertLineAbove},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"k", {}}}, VimLineEditCommand::MoveUpOnScreen},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"j", {}}}, VimLineEditCommand::MoveDownOnScreen},
        KeyBinding{{KeyChord{"0", {}}}, VimLineEditCommand::MoveToBeginningOfLine},
        KeyBinding{{KeyChord{"_", {}}}, VimLineEditCommand::MoveToBeginningOfLine},
        KeyBinding{{KeyChord{"^", {}}}, VimLineEditCommand::MoveToBeginningOfLine},
        KeyBinding{{KeyChord{"$", {}}}, VimLineEditCommand::MoveToEndOfLine},
        KeyBinding{{KeyChord{"s", {}}}, VimLineEditCommand::DeleteCharAndEnterInsertMode},
        KeyBinding{{KeyChord{":", {}}}, VimLineEditCommand::CommandCommand},
        KeyBinding{{KeyChord{"/", {}}}, VimLineEditCommand::SearchCommand},
        KeyBinding{{KeyChord{"?", {}}}, VimLineEditCommand::ReverseSearchCommand},
        KeyBinding{{KeyChord{Qt::Key_A, CONTROL}}, VimLineEditCommand::IncrementNextNumberOnCurrentLine},
        KeyBinding{{KeyChord{Qt::Key_X, CONTROL}}, VimLineEditCommand::DecrementNextNumberOnCurrentLine},
    };

    for (const auto &binding : key_bindings) {
        normal_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

    visual_mode_input_tree = normal_mode_input_tree.clone();
    std::vector<KeyBinding> visual_mode_keybindings = {
        KeyBinding{{KeyChord{"o", {}}}, VimLineEditCommand::ToggleVisualCursor},
    };

    for (const auto &binding : visual_mode_keybindings) {
        visual_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

    std::vector<KeyBinding> insert_mode_keybindings = {
        KeyBinding{{KeyChord{Qt::Key_W, CONTROL}}, VimLineEditCommand::DeletePreviousWord},
    };

    for (const auto &binding : insert_mode_keybindings) {
        insert_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

}

std::optional<VimLineEditCommand> VimLineEdit::handle_key_event(QString event_text, int key,
                                                                Qt::KeyboardModifiers modifiers) {

    InputTreeNode* current_mode_root = &normal_mode_input_tree;
    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
        current_mode_root = &visual_mode_input_tree;
    }
    if (current_mode == VimMode::Insert) {
        current_mode_root = &insert_mode_input_tree;
    }
    InputTreeNode *node = current_node ? current_node : current_mode_root;
    KeyboardModifierState modifier_state = KeyboardModifierState::from_qt_modifiers(modifiers);
    for (auto &child : node->children) {
        bool keys_are_equal = false;

        std::function<bool(const KeyboardModifierState&, const KeyboardModifierState&)> comparator = equal_with_shift;

        if (std::holds_alternative<QString>(child.key_chord.key)){
            keys_are_equal = (std::get<QString>(child.key_chord.key) == event_text);
            comparator = equal_withotu_shift;
        }
        else{
            keys_are_equal = (std::get<int>(child.key_chord.key) == key);
        }
        if (keys_are_equal && comparator(child.key_chord.modifiers, modifier_state)) {
            if (child.command.has_value()) {
                current_node = nullptr;
                return child.command;
            }
            else {
                current_node = &child;
                return {};
            }
        }
    }
    // if no matching key chord is found, reset current_node
    current_node = nullptr;
    return {};
}

std::string to_string(VimLineEditCommand cmd) {
    switch (cmd) {
    case VimLineEditCommand::GotoBegin:
        return "GotoBegin";
    case VimLineEditCommand::GotoEnd:
        return "GotoEnd";
    case VimLineEditCommand::EnterInsertMode:
        return "EnterInsertMode";
    case VimLineEditCommand::EnterInsertModeAfter:
        return "EnterInsertModeAfter";
    case VimLineEditCommand::EnterInsertModeBeginLine:
        return "EnterInsertModeBeginLine";
    case VimLineEditCommand::EnterInsertModeEndLine:
        return "EnterInsertModeEndLine";
    case VimLineEditCommand::EnterInsertModeBegin:
        return "EnterInsertModeBegin";
    case VimLineEditCommand::EnterInsertModeEnd:
        return "EnterInsertModeEnd";
    case VimLineEditCommand::EnterNormalMode:
        return "EnterNormalMode";
    case VimLineEditCommand::EnterVisualMode:
        return "EnterVisualMode";
    case VimLineEditCommand::EnterVisualLineMode:
        return "EnterVisualLineMode";
    case VimLineEditCommand::MoveLeft:
        return "MoveLeft";
    case VimLineEditCommand::MoveRight:
        return "MoveRight";
    case VimLineEditCommand::MoveUp:
        return "MoveUp";
    case VimLineEditCommand::MoveDown:
        return "MoveDown";
    case VimLineEditCommand::MoveUpOnScreen:
        return "MoveUpOnScreen";
    case VimLineEditCommand::MoveDownOnScreen:
        return "MoveDownOnScreen";
    case VimLineEditCommand::MoveToBeginning:
        return "MoveToBeginning";
    case VimLineEditCommand::MoveToEnd:
        return "MoveToEnd";
    case VimLineEditCommand::MoveWordForward:
        return "MoveWordForward";
    case VimLineEditCommand::MoveWordForwardWithSymbols:
        return "MoveWordForwardWithSymbols";
    case VimLineEditCommand::MoveToEndOfWord:
        return "MoveToEndOfWord";
    case VimLineEditCommand::MoveToEndOfWordWithSymbols:
        return "MoveToEndOfWordWithSymbols";
    case VimLineEditCommand::MoveWordBackward:
        return "MoveWordBackward";
    case VimLineEditCommand::MoveWordBackwardWithSymbols:
        return "MoveWordBackwardWithSymbols";
    case VimLineEditCommand::DeleteChar:
        return "DeleteChar";
    case VimLineEditCommand::Delete:
        return "Delete";
    case VimLineEditCommand::Change:
        return "Change";
    case VimLineEditCommand::DeleteToEndOfLine:
        return "Delete";
    case VimLineEditCommand::ChangeToEndOfLine:
        return "Change";
    case VimLineEditCommand::FindForward:
        return "FindForward";
    case VimLineEditCommand::FindBackward:
        return "FindBackward";
    case VimLineEditCommand::FindForwardTo:
        return "FindForwardTo";
    case VimLineEditCommand::FindBackwardTo:
        return "FindBackwardTo";
    case VimLineEditCommand::RepeatFind:
        return "RepeatFind";
    case VimLineEditCommand::RepeatFindReverse:
        return "RepeatFindReverse";
    case VimLineEditCommand::RepeatSearch:
        return "RepeatSearch";
    case VimLineEditCommand::RepeatSearchReverse:
        return "RepeatSearchReverse";
    case VimLineEditCommand::PasteForward:
        return "PasteForward";
    case VimLineEditCommand::Undo:
        return "Undo";
    case VimLineEditCommand::Redo:
        return "Redo";
    case VimLineEditCommand::InsertLineBelow:
        return "InsertLineBelow";
    case VimLineEditCommand::InsertLineAbove:
        return "InsertLineAbove";
    case VimLineEditCommand::ToggleVisualCursor:
        return "ToggleVisualCursor";
    case VimLineEditCommand::MoveToBeginningOfLine:
        return "MoveToBeginningOfLine";
    case VimLineEditCommand::MoveToEndOfLine:
        return "MoveToEndOfLine";
    case VimLineEditCommand::DeleteCharAndEnterInsertMode:
        return "DeleteCharAndEnterInsertMode";
    case VimLineEditCommand::DeleteCurrentLine:
        return "DeleteCurrentLine";
    case VimLineEditCommand::ChangeCurrentLine:
        return "ChangeCurrentLine";
    case VimLineEditCommand::CommandCommand:
        return "CommandCommand";
    case VimLineEditCommand::SearchCommand:
        return "SearchCommand";
    case VimLineEditCommand::ReverseSearchCommand:
        return "ReverseSearchCommand";
    case VimLineEditCommand::DeletePreviousWord:
        return "DeletePreviousWord";
    case VimLineEditCommand::IncrementNextNumberOnCurrentLine:
        return "IncrementNextNumberOnCurrentLine";
    case VimLineEditCommand::DecrementNextNumberOnCurrentLine:
        return "DecrementNextNumberOnCurrentLine";
    default:
        return "Unknown";
    }
}

bool requires_symbol(VimLineEditCommand cmd) {
    switch (cmd) {
    case VimLineEditCommand::FindForward:
    case VimLineEditCommand::FindBackward:
    case VimLineEditCommand::FindForwardTo:
    case VimLineEditCommand::FindBackwardTo:
        return true;
    default:
        return false;
    }
}

void VimLineEdit::handle_command(VimLineEditCommand cmd, std::optional<char> symbol) {
    int new_pos = -1;
    int old_pos = textCursor().position();
    // some commands' delete differs from the way they move
    // for example, pressing w moves the cursor to the beginning of the next word,
    // but does not delete the first character of the next word, but pressing e
    // moves the cursor to the end of the current word and de deletes that character
    int delete_pos_offset = 0;

    HistoryState current_state;
    current_state.text = toPlainText();
    current_state.cursor_position = old_pos;

    switch (cmd) {
    case VimLineEditCommand::EnterInsertMode:
        push_history(current_state.text, current_state.cursor_position);
        current_mode = VimMode::Insert;
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::EnterInsertModeAfter:
        current_mode = VimMode::Insert;
        new_pos = textCursor().position() + 1;
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::EnterInsertModeBegin:
        current_mode = VimMode::Insert;
        new_pos = 0;
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::EnterInsertModeEnd:
        current_mode = VimMode::Insert;
        new_pos = current_state.text.length();
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::EnterInsertModeBeginLine:
        current_mode = VimMode::Insert;
        new_pos = get_line_start_position(textCursor().position());
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::EnterInsertModeEndLine:
        current_mode = VimMode::Insert;
        new_pos = get_line_end_position(textCursor().position());
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::GotoBegin:
        new_pos = get_line_start_position(0);
        break;
    case VimLineEditCommand::GotoEnd:
        new_pos = get_line_end_position(current_state.text.length());
        break;
    case VimLineEditCommand::MoveToBeginningOfLine:{
        new_pos = get_line_start_position(textCursor().position());
        break;
    }
    case VimLineEditCommand::MoveToEndOfLine:{
        new_pos = get_line_end_position(textCursor().position()) - 1;
        delete_pos_offset = 1;
        break;
    }
    case VimLineEditCommand::EnterNormalMode: {
        if (visual_line_selection_begin != -1){
            setExtraSelections(QList<QTextEdit::ExtraSelection>());
            visual_line_selection_begin = -1;
            visual_line_selection_end = -1;
        }

        push_history(current_state.text, current_state.cursor_position);
        current_mode = VimMode::Normal;
        set_style_for_mode(current_mode);

        if (textCursor().position() <= 0) {
            new_pos = 0;
        }
        else {
            new_pos = textCursor().position();
        }

        // In normal mode, cursor should be on a character, not between
        // characters Move cursor back by one position unless we're already at
        // the beginning of a line
        bool is_at_beginning_of_line = (new_pos > 0 && current_state.text[new_pos - 1] == '\n');
        if (textCursor().position() > 0) {
            if (!is_at_beginning_of_line) {
                new_pos = textCursor().position() - 1;
            }
            else {
                new_pos = textCursor().position();
            }
        }
        break;
    }
    case VimLineEditCommand::EnterVisualMode:
        current_mode = VimMode::Visual;
        visual_mode_anchor = textCursor().position();
        // setSelection(visual_mode_anchor, 1);
        set_style_for_mode(current_mode);
        break;
    case VimLineEditCommand::EnterVisualLineMode:
        current_mode = VimMode::VisualLine;
        visual_mode_anchor = textCursor().position();
        set_style_for_mode(current_mode);
        // Immediately select the current line
        set_cursor_position_with_line_selection(textCursor().position());
        break;
    case VimLineEditCommand::MoveLeft:
        new_pos = textCursor().position() - 1;
        break;
    case VimLineEditCommand::MoveRight:
        new_pos = textCursor().position() + 1;
        break;
    case VimLineEditCommand::MoveUp:
        new_pos = calculate_move_up();
        break;
    case VimLineEditCommand::MoveDown:
        new_pos = calculate_move_down();
        break;
    case VimLineEditCommand::MoveUpOnScreen:
        new_pos = calculate_move_up_on_screen();
        break;
    case VimLineEditCommand::MoveDownOnScreen:
        new_pos = calculate_move_down_on_screen();
        break;
    case VimLineEditCommand::MoveWordForward:
        new_pos = calculate_move_word_forward(false);
        break;
    case VimLineEditCommand::MoveWordForwardWithSymbols:
        new_pos = calculate_move_word_forward(true);
        break;
    case VimLineEditCommand::MoveToEndOfWord:
        delete_pos_offset = 1;
        new_pos = calculate_move_to_end_of_word(false);
        break;
    case VimLineEditCommand::MoveToEndOfWordWithSymbols:
        delete_pos_offset = 1;
        new_pos = calculate_move_to_end_of_word(true);
        break;
    case VimLineEditCommand::Undo:
        undo();
        break;
    case VimLineEditCommand::Redo:
        redo();
        break;
    case VimLineEditCommand::MoveWordBackward:
        new_pos = calculate_move_word_backward(false);
        break;
    case VimLineEditCommand::MoveWordBackwardWithSymbols:
        new_pos = calculate_move_word_backward(true);
        break;
    case VimLineEditCommand::DeleteCharAndEnterInsertMode:
    case VimLineEditCommand::DeleteChar:
        push_history(current_state.text, current_state.cursor_position);
        delete_char();
        if (cmd == VimLineEditCommand::DeleteCharAndEnterInsertMode) {
            current_mode = VimMode::Insert;
            set_style_for_mode(current_mode);
        }
        break;
    case VimLineEditCommand::Delete:
        push_history(current_state.text, current_state.cursor_position);
        action_waiting_for_motion = {ActionWaitingForMotionKind::Delete, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::Change:
        push_history(current_state.text, current_state.cursor_position);
        action_waiting_for_motion = {ActionWaitingForMotionKind::Change, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::DeleteToEndOfLine: 
    case VimLineEditCommand::ChangeToEndOfLine:
    {
        push_history(current_state.text, current_state.cursor_position);
        int cursor_pos = textCursor().position();
        int line_end = get_line_end_position(cursor_pos);
        int cursor_offset = cmd == VimLineEditCommand::DeleteToEndOfLine ? -1 : 0;
        
        if (cursor_pos < line_end) {
            set_last_deleted_text(current_state.text.mid(cursor_pos, line_end - cursor_pos));
            QString new_text = current_state.text.remove(cursor_pos, line_end - cursor_pos);
            setText(new_text);
            set_cursor_position(cursor_pos + cursor_offset);
        }
        if (cmd == VimLineEditCommand::ChangeToEndOfLine){
            current_mode = VimMode::Insert;
            set_style_for_mode(current_mode);
        }
        break;
    }
    case VimLineEditCommand::FindForward: {
        last_find_state = FindState{FindDirection::Forward, symbol};
        new_pos = calculate_find(last_find_state.value());
        break;
    }
    case VimLineEditCommand::FindBackward: {
        last_find_state = FindState{FindDirection::Backward, symbol};
        new_pos = calculate_find(last_find_state.value());
        break;
    }
    case VimLineEditCommand::FindForwardTo: {
        last_find_state = FindState{FindDirection::ForwardTo, symbol};
        new_pos = calculate_find(last_find_state.value());
        break;
    }
    case VimLineEditCommand::FindBackwardTo: {
        last_find_state = FindState{FindDirection::BackwardTo, symbol};
        new_pos = calculate_find(last_find_state.value());
        break;
    }
    case VimLineEditCommand::RepeatFind: {
        if (last_find_state.has_value()) {
            new_pos = calculate_find(last_find_state.value());
        }
        break;
    }
    case VimLineEditCommand::RepeatFindReverse: {
        if (last_find_state.has_value()) {
            new_pos = calculate_find(last_find_state.value(), true);
        }
        break;
    }
    case VimLineEditCommand::RepeatSearch: {
        handle_search();
        break;
    }
    case VimLineEditCommand::RepeatSearchReverse: {
        handle_search(true);
        break;
    }
    case VimLineEditCommand::ChangeCurrentLine:
    case VimLineEditCommand::DeleteCurrentLine: {
        int cursor_pos = textCursor().position();
        int line_start = get_line_start_position(cursor_pos);
        int line_end = get_line_end_position(cursor_pos);

        set_last_deleted_text(current_state.text.mid(line_start, line_end - line_start), true);

        if (cmd == VimLineEditCommand::DeleteCurrentLine){
            line_end++;
        }

        push_history(current_state.text, current_state.cursor_position);
        QString new_text = current_state.text.remove(line_start, line_end - line_start);
        setText(new_text);
        set_cursor_position(line_start);

        if (cmd == VimLineEditCommand::ChangeCurrentLine){
            current_mode = VimMode::Insert;
            set_style_for_mode(current_mode);
        }

        break;
    }
    case VimLineEditCommand::PasteForward: {
        if (last_deleted_text.text.size() > 0) {
            QString current_text = current_state.text;
            int cursor_pos = textCursor().position();
            
            if (last_deleted_text.is_line) {
                // Paste the line below the current line
                int line_end = get_line_end_position(cursor_pos);
                QString new_text = current_text.left(line_end) + "\n" + last_deleted_text.text + 
                                   current_text.mid(line_end);
                setText(new_text);
                new_pos = line_end + 1;
            } else {
                // Paste the last deleted text after the cursor position
                QString new_text = current_text.left(cursor_pos + 1) + last_deleted_text.text +
                                   current_text.mid(cursor_pos + 1);
                setText(new_text);
                new_pos = cursor_pos + last_deleted_text.text.size();
            }
        }
        break;
    }
    case VimLineEditCommand::InsertLineBelow: {
        push_history(current_state.text, current_state.cursor_position);
        QString current_text = current_state.text;
        int cursor_pos = textCursor().position();
        int line_end = get_line_end_position(cursor_pos);
        QString new_text = current_text.left(line_end) + "\n" + current_text.mid(line_end);
        setText(new_text);
        new_pos = line_end + 1;
        current_mode = VimMode::Insert;
        set_style_for_mode(current_mode);
        break;
    }
    case VimLineEditCommand::DeletePreviousWord: {
        QString current_text = current_state.text;
        int cursor_pos = textCursor().position();
        int previous_space_index = current_text.lastIndexOf(' ', cursor_pos - 1);
        if (previous_space_index != -1) {
            QString word_to_delete = current_text.mid(previous_space_index, cursor_pos - previous_space_index);
            current_text.remove(previous_space_index, cursor_pos - previous_space_index);
            setText(current_text);
            new_pos = previous_space_index;
        }

        break;
    }
    case VimLineEditCommand::InsertLineAbove: {
        push_history(current_state.text, current_state.cursor_position);
        QString current_text = current_state.text;
        int cursor_pos = textCursor().position();
        int line_start = get_line_start_position(cursor_pos);
        QString new_text = current_text.left(line_start) + "\n" + current_text.mid(line_start);
        setText(new_text);
        new_pos = line_start;
        current_mode = VimMode::Insert;
        set_style_for_mode(current_mode);
        break;
    }
    case VimLineEditCommand::SearchCommand:
    case VimLineEditCommand::ReverseSearchCommand:
    case VimLineEditCommand::CommandCommand: {
        pending_text_command = cmd;
        show_command_line_edit();
        break;
    }
    case VimLineEditCommand::DecrementNextNumberOnCurrentLine:
    case VimLineEditCommand::IncrementNextNumberOnCurrentLine: {
        push_history(current_state.text, current_state.cursor_position);
        handle_number_increment_decrement(cmd == VimLineEditCommand::IncrementNextNumberOnCurrentLine);
        break;
    }
    case VimLineEditCommand::ToggleVisualCursor: {
        if (current_mode == VimMode::Visual) {
            int current_cursor_pos = textCursor().position();
            int temp = visual_mode_anchor;
            visual_mode_anchor = current_cursor_pos;
            new_pos = temp;
        }
        if (current_mode == VimMode::VisualLine){
            // swap between the first and last line of the selection, but keep the cursor position in the line (if possible)
            int current_cursor_pos = textCursor().position();
            int start_line = get_line_start_position(visual_line_selection_begin);
            int end_line = get_line_start_position(visual_line_selection_end-1);
            int current_line = get_line_start_position(current_cursor_pos);

            if (current_line == start_line){
                visual_mode_anchor = current_cursor_pos;
                int offset = current_cursor_pos - start_line;
                int target_offset = end_line + offset;
                set_cursor_position(target_offset);
            }
            else{
                visual_mode_anchor = current_cursor_pos;
                int offset = current_cursor_pos - end_line;
                int target_offset = start_line + offset;
                set_cursor_position(target_offset);
            }

        }
        break;
    }
    // Add more cases for other commands as needed
    default:
        break;
    }

    // we we have a text selected in visual mode and then perform change or delete
    // we should use the selected text as the target of the change/delete
    if (action_waiting_for_motion.has_value() && (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine)){
        QTextCursor cursor = textCursor();

        if (current_mode == VimMode::Visual){
            set_last_deleted_text(cursor.selectedText());
            cursor.removeSelectedText();
        }
        if (current_mode == VimMode::VisualLine){
            int start = visual_line_selection_begin;
            int end = visual_line_selection_end;

            set_last_deleted_text(current_state.text.mid(start, end - start - 1), true);
            QString new_text = current_state.text.remove(start, end - start);

            setText(new_text);
            set_cursor_position(start);
        }

        if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change){
            current_mode = VimMode::Insert;
            set_style_for_mode(current_mode);
        }

        if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete){
            current_mode = VimMode::Normal;
            set_style_for_mode(current_mode);
        }

        action_waiting_for_motion = {};

    }

    if (current_mode == VimMode::Normal && new_pos == current_state.text.size()) {
        new_pos = current_state.text.size() - 1;
    }

    if (new_pos != -1) {
        if (action_waiting_for_motion.has_value()) {
            handle_action_waiting_for_motion(old_pos, new_pos, delete_pos_offset);
        }
        else if (current_mode == VimMode::Visual) {
            set_cursor_position_with_selection(new_pos + delete_pos_offset);
        }
        else if (current_mode == VimMode::VisualLine) {
            set_cursor_position_with_line_selection(new_pos);
        }
        else {
            set_cursor_position(new_pos);
        }
    }
}

int VimLineEdit::calculate_find(FindState find_state, bool reverse) const {

    int location = -1;
    FindDirection direction =
        reverse ? (find_state.direction == FindDirection::Forward ? FindDirection::Backward
                                                                  : FindDirection::Forward)
                : find_state.direction;
    switch (direction) {
    case FindDirection::Forward:
        location = toPlainText().indexOf(QChar(find_state.character.value_or(' ')),
                                         textCursor().position() + 2);
        break;
    case FindDirection::Backward:
        if (textCursor().position() == 0)
            return textCursor().position();
        location = toPlainText().lastIndexOf(QChar(find_state.character.value_or(' ')),
                                             textCursor().position() - 1);
        break;
    case FindDirection::ForwardTo:
        location = toPlainText().indexOf(QChar(find_state.character.value_or(' ')),
                                         textCursor().position() + 2);
        if (location != -1) {
            location--;
        }
        break;
    case FindDirection::BackwardTo:
        if (textCursor().position() == 0)
            return textCursor().position();
        location = toPlainText().lastIndexOf(QChar(find_state.character.value_or(' ')),
                                             textCursor().position() - 1);
        if (location != -1) {
            location++;
        }
        break;
    }

    if (location != -1) {
        return location;
    }
    return textCursor().position();
}

int VimLineEdit::calculate_move_word_forward(bool with_symbols) const {
    int pos = textCursor().position();
    const QString &t = toPlainText();
    int len = t.length();

    if (pos >= len - 1) {
        return pos;
    }

    int next_pos = pos;

    if (!t[next_pos].isSpace()) {
        if (with_symbols) {
            while (next_pos < len && !t[next_pos].isSpace()) {
                next_pos++;
            }
        }
        else {
            bool is_letter = t[next_pos].isLetterOrNumber();
            while (next_pos < len && !t[next_pos].isSpace() &&
                   t[next_pos].isLetterOrNumber() == is_letter) {
                next_pos++;
            }
        }
    }

    while (next_pos < len && t[next_pos].isSpace()) {
        next_pos++;
    }

    if (next_pos < len) {
        return next_pos;
    }
    return pos;
}

int VimLineEdit::calculate_move_to_end_of_word(bool with_symbols) const {
    int pos = textCursor().position();
    const QString &t = toPlainText();
    int len = t.length();

    if (pos >= len - 1) {
        return pos;
    }

    int next_pos = pos;

    // If we're at a space, move to the next non-space character
    if (t[next_pos].isSpace()) {
        while (next_pos < len && t[next_pos].isSpace()) {
            next_pos++;
        }
    }
    // If we're at the end of a word (next character is space or end), move to next word
    else if (next_pos + 1 < len && t[next_pos + 1].isSpace()) {
        next_pos++;
        while (next_pos < len && t[next_pos].isSpace()) {
            next_pos++;
        }
    }
    // If we're in the middle of a word, move to the end of current word
    else {
        next_pos++;
    }

    if (next_pos >= len) {
        return len - 1;
    }

    // Now move to the end of the current word
    if (with_symbols) {
        while (next_pos < len - 1 && !t[next_pos + 1].isSpace()) {
            next_pos++;
        }
    }
    else {
        bool is_letter = t[next_pos].isLetterOrNumber();
        while (next_pos < len - 1 && !t[next_pos + 1].isSpace() &&
               t[next_pos + 1].isLetterOrNumber() == is_letter) {
            next_pos++;
        }
    }

    return next_pos;
}

int VimLineEdit::calculate_move_word_backward(bool with_symbols) const {
    int pos = textCursor().position();
    const QString &t = toPlainText();

    if (pos <= 0) {
        return pos;
    }

    int prev_pos = pos - 1;

    while (prev_pos > 0 && t[prev_pos].isSpace()) {
        prev_pos--;
    }

    if (with_symbols) {
        while (prev_pos > 0 && !t[prev_pos - 1].isSpace()) {
            prev_pos--;
        }
    }
    else {
        bool is_letter = t[prev_pos].isLetterOrNumber();
        while (prev_pos > 0 && !t[prev_pos - 1].isSpace() &&
               t[prev_pos - 1].isLetterOrNumber() == is_letter) {
            prev_pos--;
        }
    }

    return prev_pos;
}

void VimLineEdit::delete_char() {
    int current_pos = textCursor().position();
    QString current_text = toPlainText();
    if (current_pos < current_text.length()) {
        set_last_deleted_text(current_text.mid(current_pos, 1));
        current_text.remove(current_pos, 1);
        setText(current_text);
        set_cursor_position(current_pos);
    }
}

void VimLineEdit::push_history(const QString &text, int cursor_position) {
    if (history.current_index >= 0 && history.current_index < history.states.size() - 1) {
        // If we are in the middle of the history, remove all states after the current index
        history.states.erase(history.states.begin() + history.current_index + 1,
                             history.states.end());
    }

    history.states.push_back({text, cursor_position});

    if (history.states.size() > 100) {
        history.states.pop_front();
    }

    history.current_index = history.states.size() - 1;
}

void VimLineEdit::undo() {

    if (history.current_index < 0) {
        return;
    }

    history.current_index--;
    const HistoryState &state = history.states[history.current_index + 1];
    setText(state.text);
    set_cursor_position(state.cursor_position);
}

void VimLineEdit::redo() {
    if (history.current_index >= history.states.size() - 1) {
        return;
    }

    history.current_index++;
    const HistoryState &state = history.states[history.current_index];
    setText(state.text);
    set_cursor_position(state.cursor_position);
}

bool equal_with_shift(const KeyboardModifierState &lhs, const KeyboardModifierState &rhs) {
    return lhs.shift == rhs.shift && lhs.control == rhs.control && lhs.command == rhs.command &&
           lhs.alt == rhs.alt;
}

bool equal_withotu_shift(const KeyboardModifierState &lhs, const KeyboardModifierState &rhs) {
    return lhs.control == rhs.control && lhs.command == rhs.command &&
           lhs.alt == rhs.alt;
}

KeyboardModifierState KeyboardModifierState::from_qt_modifiers(Qt::KeyboardModifiers modifiers) {
#ifdef Q_OS_MACOS
    // on macos control and command are swapped
    return {modifiers.testFlag(Qt::ShiftModifier),
            modifiers.testFlag(Qt::MetaModifier),    // Command key on macOS
            modifiers.testFlag(Qt::ControlModifier), // Control key on macOS
            modifiers.testFlag(Qt::AltModifier)};
#else
    return {modifiers.testFlag(Qt::ShiftModifier),
            modifiers.testFlag(Qt::ControlModifier),
            modifiers.testFlag(Qt::MetaModifier),
            modifiers.testFlag(Qt::AltModifier)};
#endif
}

bool VimLineEdit::handle_surrounding_motion_action() {
    if (action_waiting_for_motion.has_value()) {
        if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Word) {
            // Handle surrounding word action
            int cursor_pos = textCursor().position();
            QString current_text = toPlainText();

            // If cursor is not on a word character, don't do anything
            if (cursor_pos >= current_text.length() ||
                !(current_text[cursor_pos].isLetterOrNumber() || current_text[cursor_pos] == '_')) {
                return true;
            }

            // Find word boundaries - only include word characters (letters, numbers,
            // underscore)
            int start = cursor_pos;
            int end = cursor_pos;

            // Move start backward to beginning of word
            while (start > 0 &&
                   (current_text[start - 1].isLetterOrNumber() || current_text[start - 1] == '_')) {
                start--;
            }

            // Move end forward to end of word (start from cursor, move until non-word char)
            while (end < current_text.length() &&
                   (current_text[end].isLetterOrNumber() || current_text[end] == '_')) {
                end++;
            }

            // include the following spaces
            if (action_waiting_for_motion->surrounding_scope == SurroundingScope::Around) {
                while (end < current_text.length() && current_text[end].isSpace()) {
                    end++;
                }
            }

            // Only proceed if we found a word
            if (start < end) {
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                    // Store deleted text for paste operation
                    set_last_deleted_text(current_text.mid(start, end - start));
                    // Delete only the word characters
                    QString new_text = current_text.remove(start, end - start);
                    setText(new_text);
                    set_cursor_position(start);

                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                        current_mode = VimMode::Insert;
                        set_style_for_mode(current_mode);
                    }
                }
            }
            action_waiting_for_motion = {};
            return true;
        }
        else {
            char begin_symbol, end_symbol;
            if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Parentheses) {
                begin_symbol = '(';
                end_symbol = ')';
            }
            else if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Brackets) {
                begin_symbol = '[';
                end_symbol = ']';
            }
            else if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Braces) {
                begin_symbol = '{';
                end_symbol = '}';
            }
            else if (action_waiting_for_motion->surrounding_kind == SurroundingKind::DoubleQuotes) {
                begin_symbol = '"';
                end_symbol = '"';
            }
            else if (action_waiting_for_motion->surrounding_kind == SurroundingKind::SingleQuotes) {
                begin_symbol = '\'';
                end_symbol = '\'';
            }
            else if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Backticks) {
                begin_symbol = '`';
                end_symbol = '`';
            }
            else {
                return false; // Unsupported surrounding kind
            }

            int cursor_pos = textCursor().position();
            QString current_text = toPlainText();
            int start = cursor_pos;
            int end = cursor_pos;
            bool found_begin = false;
            bool found_end = false;
            // Find the beginning of the surrounding
            while (start > 0) {
                if (current_text[start - 1] == begin_symbol) {
                    found_begin = true;
                    start--;
                    break;
                }
                start--;
            }
            // Find the end of the surrounding
            while (end < current_text.length()) {
                if (current_text[end] == end_symbol) {
                    found_end = true;
                    end++;
                    break;
                }
                end++;
            }

            if (action_waiting_for_motion->surrounding_scope == SurroundingScope::Inside) {
                // exclude the surrounding symbols
                if (found_begin && found_end) {
                    start++;
                    end--;
                }
            }

            // If we found both begin and end symbols, perform the action
            if (found_begin && found_end) {
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                    // Store deleted text for paste operation
                    set_last_deleted_text(current_text.mid(start, end - start));
                    // Delete the surrounding symbols
                    QString new_text = current_text.remove(start, end - start);
                    setText(new_text);
                    set_cursor_position(start);
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                        current_mode = VimMode::Insert;
                        set_style_for_mode(current_mode);
                    }
                }
            }
            action_waiting_for_motion = {};
            return true;
        }
    }
    return false;
}

void VimLineEdit::set_cursor_position(int pos) {
    QTextCursor cursor = textCursor();
    cursor.setPosition(pos);
    setTextCursor(cursor);
}

void VimLineEdit::set_cursor_position_with_selection(int pos) {
    QTextCursor cursor = textCursor();
    cursor.setPosition(visual_mode_anchor, QTextCursor::MoveAnchor);
    cursor.setPosition(pos, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
}

void VimLineEdit::set_cursor_position_with_line_selection(int pos) {
    QTextCursor cursor = textCursor();
    
    // Get line boundaries for both anchor and current position
    int anchor_line_start = get_line_start_position(visual_mode_anchor);
    int anchor_line_end = get_line_end_position(visual_mode_anchor);
    int pos_line_start = get_line_start_position(pos);
    int pos_line_end = get_line_end_position(pos);
    
    // Determine selection direction and boundaries
    int selection_start, selection_end;
    if (visual_mode_anchor <= pos) {
        // Forward selection: from start of anchor line to end of current line
        selection_start = anchor_line_start;
        selection_end = pos_line_end;
    } else {
        // Backward selection: from start of current line to end of anchor line
        selection_start = pos_line_start;
        selection_end = anchor_line_end;
    }
    
    // Include the newline character if it exists (except for the last line)
    const QString &text = toPlainText();
    if (selection_end < text.length() && text[selection_end] == '\n') {
        selection_end++;
    }
    
    cursor.setPosition(selection_start, QTextCursor::MoveAnchor);
    cursor.setPosition(selection_end, QTextCursor::KeepAnchor);
    visual_line_selection_begin = selection_start;
    visual_line_selection_end = selection_end;


    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;

    QColor selection_foreground_color = palette().color(QPalette::Text);
    QColor selection_background_color = palette().color(QPalette::Highlight);
    selection.format.setBackground(selection_background_color);
    selection.format.setForeground(selection_foreground_color);

    setExtraSelections({selection});
    set_cursor_position(pos);
}

int VimLineEdit::get_line_start_position(int cursor_pos) {
    const QString &text = toPlainText();
    int pos = cursor_pos;

    while (pos > 0 && text[pos - 1] != '\n') {
        pos--;
    }

    return pos;
}

int VimLineEdit::get_line_end_position(int cursor_pos) {
    const QString &text = toPlainText();
    int pos = cursor_pos;
    int length = text.length();

    while (pos < length && text[pos] != '\n') {
        pos++;
    }

    return pos;
}

int VimLineEdit::calculate_move_up() {
    int cursor_pos = textCursor().position();
    const QString &text = toPlainText();

    int current_line_start = get_line_start_position(cursor_pos);
    int column_offset = cursor_pos - current_line_start;

    // If we're already on the first line, stay at current position
    if (current_line_start == 0) {
        return cursor_pos;
    }

    // Find the start of the previous line
    int prev_line_end = current_line_start - 1; // The newline character
    int prev_line_start = get_line_start_position(prev_line_end);

    // Calculate the length of the previous line
    int prev_line_length = prev_line_end - prev_line_start;

    // Try to maintain the same column position, but clamp to line length
    int new_column = std::min(column_offset, prev_line_length);

    return prev_line_start + new_column;
}

int VimLineEdit::calculate_move_down() {
    int cursor_pos = textCursor().position();
    const QString &text = toPlainText();

    int current_line_start = get_line_start_position(cursor_pos);
    int column_offset = cursor_pos - current_line_start;

    // Find the end of the current line
    int current_line_end = get_line_end_position(cursor_pos);

    // If we're at the last line, stay at current position
    if (current_line_end >= text.length()) {
        return cursor_pos;
    }

    // Find the start of the next line (skip the newline character)
    int next_line_start = current_line_end + 1;

    // If there's no next line, stay at current position
    if (next_line_start >= text.length()) {
        return cursor_pos;
    }

    // Find the end of the next line
    int next_line_end = get_line_end_position(next_line_start);
    int next_line_length = next_line_end - next_line_start;

    // Try to maintain the same column position, but clamp to line length
    int new_column = std::min(column_offset, next_line_length);

    return next_line_start + new_column;
}

int VimLineEdit::calculate_move_up_on_screen() { return calculate_move_on_screen(-1); }

int VimLineEdit::calculate_move_down_on_screen() { return calculate_move_on_screen(1); }

int VimLineEdit::calculate_move_on_screen(int direction) {
    QTextCursor cursor = textCursor();
    int current_pos = cursor.position();

    // Use the document's text layout directly
    QTextDocument *doc = document();
    QTextBlock current_block = doc->findBlock(current_pos);

    if (!current_block.isValid()) {
        return current_pos;
    }

    // Get the layout for the current block
    QTextLayout *layout = current_block.layout();
    if (!layout) {
        return current_pos;
    }

    // Find position within the block
    int pos_in_block = current_pos - current_block.position();

    // Find which line within the block contains our cursor
    int current_line_index = -1;
    for (int i = 0; i < layout->lineCount(); ++i) {
        QTextLine line = layout->lineAt(i);
        if (pos_in_block >= line.textStart() &&
            pos_in_block < line.textStart() + line.textLength()) {
            current_line_index = i;
            break;
        }
    }

    if (current_line_index == -1)
        return current_pos;

    // Calculate character index within the current visual line
    QTextLine current_line = layout->lineAt(current_line_index);
    int char_index_in_line = pos_in_block - current_line.textStart();

    // Try to move within the same block first
    int target_line_index = current_line_index + direction;
    if (target_line_index >= 0 && target_line_index < layout->lineCount()) {
        QTextLine target_line = layout->lineAt(target_line_index);
        int target_index = std::min(char_index_in_line, target_line.textLength() - 1);
        return current_block.position() + target_line.textStart() + target_index;
    }

    // Move to adjacent block
    QTextBlock target_block = (direction > 0) ? current_block.next() : current_block.previous();
    if (!target_block.isValid()) {
        return current_pos; // Already at boundary
    }

    QTextLayout *target_layout = target_block.layout();
    if (!target_layout || target_layout->lineCount() == 0) {
        return current_pos;
    }

    // Choose first or last line of the target block
    int target_block_line_index = (direction > 0) ? 0 : target_layout->lineCount() - 1;
    QTextLine target_line = target_layout->lineAt(target_block_line_index);
    int target_index = std::min(char_index_in_line, target_line.textLength() - 1);
    return target_block.position() + target_line.textStart() + target_index;
}

InputTreeNode InputTreeNode::clone() const {
    // create a deep clone
    InputTreeNode new_node;
    new_node.key_chord = this->key_chord;
    new_node.command = this->command;
    for (const auto& child : this->children) {
        new_node.children.push_back(child.clone());
    }
    return new_node;
}

void VimLineEdit::resizeEvent(QResizeEvent *event) {
    // move the command line edit to the bottom
    command_line_edit->resize(event->size().width(), command_line_edit->height());
    command_line_edit->move(0, height() - command_line_edit->height());
    QTextEdit::resizeEvent(event);
}

void VimLineEdit::show_command_line_edit(){
    command_line_edit->setText("");
    command_line_edit->show();
    command_line_edit->setFocus();
}

void VimLineEdit::hide_command_line_edit(){
    command_line_edit->setText("");
    command_line_edit->hide();
    setFocus();
}

void VimLineEdit::perform_pending_text_command_with_text(QString text){
    if (pending_text_command.has_value()){
        switch (pending_text_command.value()) {
        case VimLineEditCommand::CommandCommand: {
            qDebug() << "performing command: " << text;
        }
        case VimLineEditCommand::ReverseSearchCommand:
        case VimLineEditCommand::SearchCommand: {
            SearchState search_state;
            search_state.direction = (pending_text_command.value() == VimLineEditCommand::ReverseSearchCommand) ? FindDirection::Backward : FindDirection::Forward;
            search_state.query = text;
            last_search_state = search_state;
            handle_search();
        }
        default:
        }
    }

}

void VimLineEdit::handle_action_waiting_for_motion(int old_pos, int new_pos, int delete_pos_offset){
    if (action_waiting_for_motion.has_value()) {
        QString current_text = toPlainText();
        if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Delete ||
            action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Change) {
            // delete from old_pos to new_pos
            if (old_pos < new_pos) {
                set_last_deleted_text(
                    current_text.mid(old_pos, new_pos + delete_pos_offset - old_pos));
                QString new_text =
                    current_text.remove(old_pos, new_pos + delete_pos_offset - old_pos);
                setText(new_text);
                set_cursor_position(old_pos);
            }
            else if (old_pos > new_pos) {
                set_last_deleted_text(current_text.mid(new_pos, old_pos - new_pos));
                QString new_text = current_text.remove(new_pos, old_pos - new_pos);
                setText(new_text);
                set_cursor_position(new_pos);
            }

            if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Change) {
                current_mode = VimMode::Insert;
                set_style_for_mode(current_mode);
            }
        }

        action_waiting_for_motion = {};
    }
}

void VimLineEdit::handle_search(bool reverse){
    if (!last_search_state.has_value()) {
        return;
    }

    QString document_text = toPlainText().toLower();
    QString text = last_search_state->query.value().toLower();

    int from = 0;
    std::vector<int> found_indices;
    while (from < document_text.length()) {
        int index = document_text.indexOf(text, from);
        if (index == -1) {
            break; // No more occurrences found
        }
        found_indices.push_back(index);
        from = index + text.length(); // Move past the found occurrence
    }
    
    if (found_indices.empty()) {
        return; // No matches found
    }
    
    int current_pos = textCursor().position();
    int target_index = -1;
    bool is_reversed = last_search_state->direction == FindDirection::Forward 
                       ? reverse
                       : !reverse;
    
    if (is_reversed) {
        // Search backward: find the last occurrence before current position
        for (int i = found_indices.size() - 1; i >= 0; i--) {
            if (found_indices[i] < current_pos) {
                target_index = found_indices[i];
                break;
            }
        }
        // If no occurrence before current position, wrap to the last occurrence
        if (target_index == -1) {
            target_index = found_indices.back();
        }
    } else {
        // Search forward: find the first occurrence after current position
        for (int index : found_indices) {
            if (index > current_pos) {
                target_index = index;
                break;
            }
        }
        // If no occurrence after current position, wrap to the first occurrence
        if (target_index == -1) {
            target_index = found_indices[0];
        }
    }

    
    if (target_index != -1) {
        set_cursor_position(target_index);
    }

    if (action_waiting_for_motion.has_value()){
        handle_action_waiting_for_motion(current_pos, target_index, 0);
    }
}

EscapeLineEdit::EscapeLineEdit(QWidget *parent) : QLineEdit(parent) {

}

void EscapeLineEdit::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        emit escapePressed();
    }
    QLineEdit::keyPressEvent(event);
}

void VimLineEdit::set_last_deleted_text(QString text, bool is_line){
    last_deleted_text.text = text;
    last_deleted_text.is_line = is_line;
}

void VimLineEdit::handle_number_increment_decrement(bool increment) {
    QString current_text = toPlainText();
    int cursor_pos = textCursor().position();
    
    // Get current line boundaries
    int line_start = get_line_start_position(cursor_pos);
    int line_end = get_line_end_position(cursor_pos);
    
    // Extract current line
    QString line = current_text.mid(line_start, line_end - line_start);
    
    // Find the next number on the line starting from cursor position
    QRegularExpression number_regex("(-?\\d+)");
    QRegularExpressionMatchIterator matches = number_regex.globalMatch(line);
    
    int relative_cursor_pos = cursor_pos - line_start + 1;
    int number_start = -1;
    int number_end = -1;
    QString number_str;
    
    // Find the first number at or after the cursor position
    while (matches.hasNext()) {
        QRegularExpressionMatch match = matches.next();
        if (match.capturedEnd() >= relative_cursor_pos) {
            number_start = match.capturedStart();
            number_end = match.capturedEnd();
            number_str = match.captured(1);
            break;
        }
    }
    
    // If no number found after cursor, find the first number on the line
    if (number_start == -1) {
        matches = number_regex.globalMatch(line);
        if (matches.hasNext()) {
            QRegularExpressionMatch match = matches.next();
            number_start = match.capturedStart();
            number_end = match.capturedEnd();
            number_str = match.captured(1);
        }
    }
    
    // If we found a number, modify it
    if (number_start != -1) {
        bool ok;
        int number = number_str.toInt(&ok);
        if (ok) {
            int new_number = increment ? number + 1 : number - 1;
            QString new_number_str = QString::number(new_number);
            
            // Replace the number in the text
            int absolute_start = line_start + number_start;
            int absolute_end = line_start + number_end;
            
            QString new_text = current_text.left(absolute_start) + 
                              new_number_str + 
                              current_text.mid(absolute_end);
            
            setText(new_text);
            
            // Position cursor at the end of the modified number
            int new_cursor_pos = absolute_start + new_number_str.length() - 1;
            set_cursor_position(new_cursor_pos);
        }
    }
}