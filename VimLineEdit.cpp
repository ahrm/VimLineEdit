#include "VimLineEdit.h"
#include <QtCore/qnamespace.h>
#include <QtWidgets/qlineedit.h>
#include <QPainter>
// #include <QFontMetrics>
#include <QStyle>
#include <QCommonStyle>
#include <vector>
#include <QRegularExpression>


class LineEditStyle : public QCommonStyle
{
    int font_width;
public:
  LineEditStyle(int font_width) : font_width(font_width) {}

  int pixelMetric(PixelMetric metric, const QStyleOption *option = 0, const QWidget *widget = 0) const{
        if (metric == QStyle::PM_TextCursorWidth) {
            return font_width;
        }
        return QCommonStyle::pixelMetric(metric, option, widget);
  }
};


VimLineEdit::VimLineEdit(QWidget *parent)
    : QLineEdit(parent)
{
    QFont font = this->font();
    font.setFamily("Courier New");
    font.setStyleHint(QFont::TypeWriter);
    this->setFont(font);
    add_vim_keybindings();
}

void VimLineEdit::keyPressEvent(QKeyEvent *event){
    if (event->key() == Qt::Key_Escape) {
        handle_command(VimLineEditCommand::EnterNormalMode, {});
        return;
    }

    if (current_mode == VimMode::Normal || current_mode == VimMode::Visual){
        // we don't want to handle when we only press a modifier (e.g. Shift, Ctrl, etc.) 
        int event_key = event->key();
        bool is_keypress_a_modifier = event_key == Qt::Key_Shift || event_key == Qt::Key_Control || event_key == Qt::Key_Alt || event_key == Qt::Key_Meta;

        if (!is_keypress_a_modifier && pending_symbol_command.has_value()) {
            // If we have a pending command that requires a symbol, we handle it now
            VimLineEditCommand command = pending_symbol_command.value();
            pending_symbol_command = {};

            if (event->text().size() > 0){
                handle_command(command, static_cast<char>(event->text().at(0).toLatin1()));
            }

            return;
        }

        if (!is_keypress_a_modifier && action_waiting_for_motion.has_value()){

            if (action_waiting_for_motion->surrounding_scope == SurroundingScope::None){

                if (event->key() == Qt::Key_I) {
                    action_waiting_for_motion->surrounding_scope = SurroundingScope::Inside;
                    return;
                }
                else if (event->key() == Qt::Key_A) {
                    action_waiting_for_motion->surrounding_scope = SurroundingScope::Around;
                    return;
                }

            }
            else{
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
            handle_surrounding_motion_action();
        }

        if (!is_keypress_a_modifier){
            std::optional<VimLineEditCommand> command = handle_key_event(event_key, event->modifiers());
            if (command.has_value()) {
                if (requires_symbol(command.value())) {
                    // If the command requires a symbol, we need to wait for the next key press
                    pending_symbol_command = command;
                    return;
                }
                else{
                    handle_command(command.value());
                }
            }
        }

        return;
    }

    QLineEdit::keyPressEvent(event);
}

void VimLineEdit::set_style_for_mode(VimMode mode){
    if (mode == VimMode::Normal){
        setStyleSheet("background-color: lightgray;");
        int font_width = fontMetrics().horizontalAdvance(" ");
        setStyle(new LineEditStyle(font_width));
    }
    else if (mode == VimMode::Insert) {
        setStyleSheet("background-color: white;");
        setStyle(new LineEditStyle(1));
    }
    else if (mode == VimMode::Visual) {
        setStyleSheet("background-color: pink;");
        int font_width = fontMetrics().horizontalAdvance(" ");
        setStyle(new LineEditStyle(font_width));
    }
}

void InputTreeNode::add_keybinding(const std::vector<KeyChord>& key_chords, int index, VimLineEditCommand cmd){
    if (index >= key_chords.size()) {
        command = cmd;
        return;
    }

    KeyChord current_chord = key_chords[index];
    for (auto &child : children) {
        if (child.key_chord.key == current_chord.key && child.key_chord.modifiers == current_chord.modifiers) {
            child.add_keybinding(key_chords, index + 1, cmd);
            return;
        }
    }

    InputTreeNode new_node;
    new_node.key_chord = current_chord;
    new_node.add_keybinding(key_chords, index + 1, cmd);
    children.push_back(new_node);
}

void VimLineEdit::add_vim_keybindings(){
    KeyboardModifierState CONTROL = KeyboardModifierState{false, true, false, false};
    KeyboardModifierState SHIFT = KeyboardModifierState{true, false, false, false};
    // KeyboardModifierState NOMOD = KeyboardModifierState{false, false, false, false};
     
    std::vector<KeyBinding> key_bindings = {
        KeyBinding{{KeyChord{Qt::Key_Escape, {}}}, VimLineEditCommand::EnterNormalMode},
        KeyBinding{{KeyChord{Qt::Key_I, {}}}, VimLineEditCommand::EnterInsertMode},
        KeyBinding{{KeyChord{Qt::Key_A, {}}}, VimLineEditCommand::EnterInsertModeAfter},
        KeyBinding{{KeyChord{Qt::Key_I, SHIFT}}, VimLineEditCommand::EnterInsertModeBegin},
        KeyBinding{{KeyChord{Qt::Key_A, SHIFT}}, VimLineEditCommand::EnterInsertModeEnd},
        KeyBinding{{KeyChord{Qt::Key_V, {}}}, VimLineEditCommand::EnterVisualMode},
        KeyBinding{{KeyChord{Qt::Key_H, {}}}, VimLineEditCommand::MoveLeft},
        KeyBinding{{KeyChord{Qt::Key_L, {}}}, VimLineEditCommand::MoveRight},
        KeyBinding{{KeyChord{Qt::Key_Left, {}}}, VimLineEditCommand::MoveLeft},
        KeyBinding{{KeyChord{Qt::Key_Right, {}}}, VimLineEditCommand::MoveRight},
        KeyBinding{{KeyChord{Qt::Key_F, {}}}, VimLineEditCommand::FindForward},
        KeyBinding{{KeyChord{Qt::Key_F, SHIFT}}, VimLineEditCommand::FindBackward},
        KeyBinding{{KeyChord{Qt::Key_Semicolon, {}}}, VimLineEditCommand::RepeatFind},
        KeyBinding{{KeyChord{Qt::Key_Comma, {}}}, VimLineEditCommand::RepeatFindReverse},
        KeyBinding{{KeyChord{Qt::Key_W, {}}}, VimLineEditCommand::MoveWordForward},
        KeyBinding{{KeyChord{Qt::Key_W, SHIFT}}, VimLineEditCommand::MoveWordForwardWithSymbols},
        KeyBinding{{KeyChord{Qt::Key_E, {}}}, VimLineEditCommand::MoveToEndOfWord},
        KeyBinding{{KeyChord{Qt::Key_E, SHIFT}}, VimLineEditCommand::MoveToEndOfWordWithSymbols},
        KeyBinding{{KeyChord{Qt::Key_B, {}}}, VimLineEditCommand::MoveWordBackward},
        KeyBinding{{KeyChord{Qt::Key_B, SHIFT}}, VimLineEditCommand::MoveWordBackwardWithSymbols},
        KeyBinding{{KeyChord{Qt::Key_T, {}}}, VimLineEditCommand::FindForwardTo},
        KeyBinding{{KeyChord{Qt::Key_T, SHIFT}}, VimLineEditCommand::FindBackwardTo},
        KeyBinding{{KeyChord{Qt::Key_X, {}}}, VimLineEditCommand::DeleteChar},
        KeyBinding{{KeyChord{Qt::Key_D, {}}}, VimLineEditCommand::Delete},
        KeyBinding{{KeyChord{Qt::Key_P, {}}}, VimLineEditCommand::PasteForward},
        KeyBinding{{KeyChord{Qt::Key_U, {}}}, VimLineEditCommand::Undo},
        KeyBinding{{KeyChord{Qt::Key_R, CONTROL}}, VimLineEditCommand::Redo},
    };

    for (const auto &binding : key_bindings) {
        input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

}

std::optional<VimLineEditCommand> VimLineEdit::handle_key_event(int key, Qt::KeyboardModifiers modifiers){

    InputTreeNode *node = current_node ? current_node : &input_tree;
    KeyboardModifierState modifier_state = KeyboardModifierState::from_qt_modifiers(modifiers);
    for (auto &child : node->children) {
        if (child.key_chord.key == key && child.key_chord.modifiers == modifier_state) {
            if (child.command.has_value()) {
                current_node = nullptr;
                return child.command;
            }
            else{
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
        case VimLineEditCommand::EnterInsertMode: return "EnterInsertMode";
        case VimLineEditCommand::EnterInsertModeAfter: return "EnterInsertModeAfter";
        case VimLineEditCommand::EnterInsertModeBegin: return "EnterInsertModeBegin";
        case VimLineEditCommand::EnterInsertModeEnd: return "EnterInsertModeEnd";
        case VimLineEditCommand::EnterNormalMode: return "EnterNormalMode";
        case VimLineEditCommand::EnterVisualMode: return "EnterVisualMode";
        case VimLineEditCommand::MoveLeft: return "MoveLeft";
        case VimLineEditCommand::MoveRight: return "MoveRight";
        case VimLineEditCommand::MoveToBeginning: return "MoveToBeginning";
        case VimLineEditCommand::MoveToEnd: return "MoveToEnd";
        case VimLineEditCommand::MoveWordForward: return "MoveWordForward";
        case VimLineEditCommand::MoveWordForwardWithSymbols: return "MoveWordForwardWithSymbols";
        case VimLineEditCommand::MoveToEndOfWord: return "MoveToEndOfWord";
        case VimLineEditCommand::MoveToEndOfWordWithSymbols: return "MoveToEndOfWordWithSymbols";
        case VimLineEditCommand::MoveWordBackward: return "MoveWordBackward";
        case VimLineEditCommand::MoveWordBackwardWithSymbols: return "MoveWordBackwardWithSymbols";
        case VimLineEditCommand::DeleteChar: return "DeleteChar";
        case VimLineEditCommand::Delete: return "Delete";
        case VimLineEditCommand::DeleteInsideWord: return "DeleteInsideWord";
        case VimLineEditCommand::DeleteInsideParentheses: return "DeleteInsideParentheses";
        case VimLineEditCommand::DeleteInsideBrackets: return "DeleteInsideBrackets";
        case VimLineEditCommand::DeleteInsideBraces: return "DeleteInsideBraces";
        case VimLineEditCommand::FindForward: return "FindForward";
        case VimLineEditCommand::FindBackward: return "FindBackward";
        case VimLineEditCommand::FindForwardTo: return "FindForwardTo";
        case VimLineEditCommand::FindBackwardTo: return "FindBackwardTo";
        case VimLineEditCommand::RepeatFind: return "RepeatFind";
        case VimLineEditCommand::RepeatFindReverse: return "RepeatFindReverse";
        case VimLineEditCommand::PasteForward: return "PasteForward";
        case VimLineEditCommand::Undo: return "Undo";
        case VimLineEditCommand::Redo: return "Redo";
        default: return "Unknown";
    }
}

bool requires_symbol(VimLineEditCommand cmd){
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

void VimLineEdit::handle_command(VimLineEditCommand cmd, std::optional<char> symbol){
    int new_pos = -1;
    int old_pos = cursorPosition();

    HistoryState current_state;
    current_state.text = text();
    current_state.cursor_position = old_pos;

    switch (cmd) {
        case VimLineEditCommand::EnterInsertMode:
            push_history(current_state.text, current_state.cursor_position);
            current_mode = VimMode::Insert;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterInsertModeAfter:
            current_mode = VimMode::Insert;
            new_pos = cursorPosition() + 1;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterInsertModeBegin:
            current_mode = VimMode::Insert;
            new_pos = 0;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterInsertModeEnd:
            current_mode = VimMode::Insert;
            new_pos = text().length();
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterNormalMode:
            push_history(current_state.text, current_state.cursor_position);
            current_mode = VimMode::Normal;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterVisualMode:
            current_mode = VimMode::Visual;
            visual_mode_anchor = cursorPosition();
            setSelection(visual_mode_anchor, 1);
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::MoveLeft:
            new_pos = cursorPosition() - 1;
            break;
        case VimLineEditCommand::MoveRight:
            new_pos = cursorPosition() + 1;
            break;
        case VimLineEditCommand::MoveWordForward:
            new_pos = calculate_move_word_forward(false);
            break;
        case VimLineEditCommand::MoveWordForwardWithSymbols:
            new_pos = calculate_move_word_forward(true);
            break;
        case VimLineEditCommand::MoveToEndOfWord:
            new_pos = calculate_move_to_end_of_word(false);
            break;
        case VimLineEditCommand::MoveToEndOfWordWithSymbols:
            new_pos = calculate_move_to_end_of_word(true);
            break;
        case VimLineEditCommand::Undo:
            undo();
            new_pos = cursorPosition();
            break;
        case VimLineEditCommand::Redo:
            redo();
            new_pos = cursorPosition();
            break;
        case VimLineEditCommand::MoveWordBackward:
            new_pos = calculate_move_word_backward(false);
            break;
        case VimLineEditCommand::MoveWordBackwardWithSymbols:
            new_pos = calculate_move_word_backward(true);
            break;
        case VimLineEditCommand::DeleteChar:
            push_history(current_state.text, current_state.cursor_position);
            delete_char();
            break;
        case VimLineEditCommand::Delete:
            push_history(current_state.text, current_state.cursor_position);
            action_waiting_for_motion = {ActionWaitingForMotionKind::Delete, SurroundingScope::None, SurroundingKind::None};
            break;
        case VimLineEditCommand::FindForward:{
            last_find_state = FindState{FindDirection::Forward, symbol};
            new_pos = calculate_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::FindBackward:{
            last_find_state = FindState{FindDirection::Backward, symbol};
            new_pos = calculate_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::FindForwardTo:{
            last_find_state = FindState{FindDirection::ForwardTo, symbol};
            new_pos = calculate_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::FindBackwardTo:{
            last_find_state = FindState{FindDirection::BackwardTo, symbol};
            new_pos = calculate_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::RepeatFind: {
            if (last_find_state.has_value()){
                new_pos = calculate_find(last_find_state.value());
            }
            break;
        }
        case VimLineEditCommand::RepeatFindReverse: {
            if (last_find_state.has_value()){
                new_pos = calculate_find(last_find_state.value(), true);
            }
            break;
        }
        case VimLineEditCommand::PasteForward: {
            if (last_deleted_text.size() > 0){
                // Paste the last deleted text after the cursor position
                QString current_text = text();
                int cursor_pos = cursorPosition();
                QString new_text = current_text.left(cursor_pos + 1) + last_deleted_text + current_text.mid(cursor_pos + 1);
                setText(new_text);
                new_pos = cursor_pos + last_deleted_text.size();
            }
            break;
        }
        // Add more cases for other commands as needed
        default:
            break;
    }


    if (new_pos != -1){
        if (action_waiting_for_motion.has_value()){
            if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Delete) {
                // delete from old_pos to new_pos
                if (old_pos < new_pos) {
                    QString new_text = text().remove(old_pos, new_pos + 1 - old_pos);
                    setText(new_text);
                    setCursorPosition(old_pos);
                } else if (old_pos > new_pos) {
                    QString new_text = text().remove(new_pos, old_pos - new_pos);
                    setText(new_text);
                    setCursorPosition(new_pos);
                }
            }

            action_waiting_for_motion = {};
        }
        else if (current_mode == VimMode::Visual) {
            setCursorPosition(new_pos);
            int selection_start = std::min(visual_mode_anchor, new_pos);
            int selection_end = std::max(visual_mode_anchor, new_pos);
            setSelection(selection_start, selection_end - selection_start);
        } else {
            setCursorPosition(new_pos);
        }

    }

}

int VimLineEdit::calculate_find(FindState find_state, bool reverse) const{

    int location = -1;
    FindDirection direction = reverse ? 
        (find_state.direction == FindDirection::Forward ? FindDirection::Backward : FindDirection::Forward) : 
        find_state.direction;
    switch (direction) {
        case FindDirection::Forward:
            location = text().indexOf(QChar(find_state.character.value_or(' ')), cursorPosition() + 2);
            break;
        case FindDirection::Backward:
            if (cursorPosition() == 0) return cursorPosition();
            location = text().lastIndexOf(QChar(find_state.character.value_or(' ')), cursorPosition() - 1);
            break;
        case FindDirection::ForwardTo:
            location = text().indexOf(QChar(find_state.character.value_or(' ')), cursorPosition() + 2);
            if (location != -1) {
                location--;
            }
            break;
        case FindDirection::BackwardTo:
            if (cursorPosition() == 0) return cursorPosition();
            location = text().lastIndexOf(QChar(find_state.character.value_or(' ')), cursorPosition() - 1);
            if (location != -1) {
                location++;
            }
            break;
    }

    if (location != -1) {
        return location;
    }
    return cursorPosition();
}

int VimLineEdit::calculate_move_word_forward(bool with_symbols) const {
    int pos = cursorPosition();
    const QString& t = text();
    int len = t.length();

    if (pos >= len - 1) {
        return pos;
    }

    int next_pos = pos;

    if (!t[next_pos].isSpace()){
        if (with_symbols){
            while(next_pos < len && !t[next_pos].isSpace()){
                next_pos++;
            }
        } else {
            bool is_letter = t[next_pos].isLetterOrNumber();
            while(next_pos < len && !t[next_pos].isSpace() && t[next_pos].isLetterOrNumber() == is_letter){
                next_pos++;
            }
        }
    }

    while(next_pos < len && t[next_pos].isSpace()){
        next_pos++;
    }


    if (next_pos < len) {
        return next_pos;
    }
    return pos;
}

int VimLineEdit::calculate_move_to_end_of_word(bool with_symbols) const {
    int pos = cursorPosition();
    const QString& t = text();
    int len = t.length();

    if (pos >= len - 1) {
        return pos;
    }

    int next_pos = pos;

    if (t[next_pos].isSpace()){
        while(next_pos < len && t[next_pos].isSpace()){
            next_pos++;
        }
    }

    if (with_symbols){
        while(next_pos < len -1 && !t[next_pos+1].isSpace()){
            next_pos++;
        }
    } else {
        bool is_letter = t[next_pos].isLetterOrNumber();
        while(next_pos < len -1 && !t[next_pos+1].isSpace() && t[next_pos+1].isLetterOrNumber() == is_letter){
            next_pos++;
        }
    }

    if (next_pos < len) {
        return next_pos;
    }
    return pos;
}

int VimLineEdit::calculate_move_word_backward(bool with_symbols) const {
    int pos = cursorPosition();
    const QString& t = text();

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
    } else {
        bool is_letter = t[prev_pos].isLetterOrNumber();
        while (prev_pos > 0 && !t[prev_pos - 1].isSpace() && t[prev_pos-1].isLetterOrNumber() == is_letter) {
            prev_pos--;
        }
    }

    return prev_pos;
}

void VimLineEdit::delete_char() {
    int current_pos = cursorPosition();
    QString current_text = text();
    if (current_pos < current_text.length()) {
        last_deleted_text = current_text.mid(current_pos, 1);
        current_text.remove(current_pos, 1);
        setText(current_text);
        setCursorPosition(current_pos);
    }
}

void VimLineEdit::push_history(const QString &text, int cursor_position){
    if (history.current_index >= 0 && history.current_index < history.states.size() - 1) {
        // If we are in the middle of the history, remove all states after the current index
        history.states.erase(history.states.begin() + history.current_index + 1, history.states.end());
    }

    history.states.push_back({text, cursor_position});

    if (history.states.size() > 100) {
        history.states.pop_front();
    }

    history.current_index = history.states.size() - 1;
}

void VimLineEdit::undo(){

    if (history.current_index < 0) {
        return; // No more history to undo
    }

    history.current_index--;
    const HistoryState& state = history.states[history.current_index + 1];
    setText(state.text);
    setCursorPosition(state.cursor_position);

}

void VimLineEdit::redo(){
    if (history.current_index >= history.states.size() - 1) {
        return; // No more history to redo
    }

    history.current_index++;
    const HistoryState& state = history.states[history.current_index];
    setText(state.text);
    setCursorPosition(state.cursor_position);
}

bool operator==(const KeyboardModifierState& lhs, const KeyboardModifierState& rhs) {
    return lhs.shift == rhs.shift &&
           lhs.control == rhs.control &&
           lhs.command == rhs.command &&
           lhs.alt == rhs.alt;
}

KeyboardModifierState KeyboardModifierState::from_qt_modifiers(Qt::KeyboardModifiers modifiers) {
    #ifdef Q_OS_MACOS
    // on macos control and command are swapped
    return {
        modifiers.testFlag(Qt::ShiftModifier),
        modifiers.testFlag(Qt::MetaModifier), // Command key on macOS
        modifiers.testFlag(Qt::ControlModifier), // Control key on macOS
        modifiers.testFlag(Qt::AltModifier)
    };
    #else
    return {
        modifiers.testFlag(Qt::ShiftModifier),
        modifiers.testFlag(Qt::ControlModifier),
        modifiers.testFlag(Qt::MetaModifier), // Command key on macOS
        modifiers.testFlag(Qt::AltModifier)
    };
    #endif
}

void VimLineEdit::handle_surrounding_motion_action(){
  if (action_waiting_for_motion.has_value()){
    if (action_waiting_for_motion->surrounding_scope == SurroundingScope::Inside) {
        if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Word) {
            // Handle surrounding word action
            int cursor_pos = cursorPosition();
            QString current_text = text();
            int start = current_text.lastIndexOf(QRegularExpression("\\W"), cursor_pos);
            int end = current_text.indexOf(QRegularExpression("\\W"), cursor_pos);
            if (start == -1) start = 0;
            else{
                start++;
            }

            if (end == -1) end = current_text.length();
            else{
                end--;
            }
            // setSelection(start, end - start);
            if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete) {
                // delete the selected text
                QString new_text = current_text.remove(start, end - start);
                setText(new_text);
                setCursorPosition(start);
            }
        }
    }
  }
}