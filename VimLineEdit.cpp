#include "VimLineEdit.h"
#include <QtCore/qnamespace.h>
#include <QtWidgets/qlineedit.h>
#include <QPainter>
// #include <QFontMetrics>
#include <QStyle>
#include <QCommonStyle>
#include <vector>


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
        current_mode = VimMode::Normal;
        cursorBackward(false);
        set_style_for_mode(current_mode);
        update(); // Trigger repaint for cursor change
        return;
    }

    if (current_mode == VimMode::Normal){
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
        setStyleSheet("background-color: lightblue;");
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
    std::vector<KeyBinding> key_bindings = {
        KeyBinding{{KeyChord{Qt::Key_Escape, Qt::NoModifier}}, VimLineEditCommand::EnterNormalMode},
        KeyBinding{{KeyChord{Qt::Key_I, Qt::NoModifier}}, VimLineEditCommand::EnterInsertMode},
        KeyBinding{{KeyChord{Qt::Key_A, Qt::NoModifier}}, VimLineEditCommand::EnterInsertModeAfter},
        KeyBinding{{KeyChord{Qt::Key_I, Qt::ShiftModifier}}, VimLineEditCommand::EnterInsertModeBegin},
        KeyBinding{{KeyChord{Qt::Key_A, Qt::ShiftModifier}}, VimLineEditCommand::EnterInsertModeEnd},
        KeyBinding{{KeyChord{Qt::Key_V, Qt::NoModifier}}, VimLineEditCommand::EnterVisualMode},
        KeyBinding{{KeyChord{Qt::Key_H, Qt::NoModifier}}, VimLineEditCommand::MoveLeft},
        KeyBinding{{KeyChord{Qt::Key_L, Qt::NoModifier}}, VimLineEditCommand::MoveRight},
        KeyBinding{{KeyChord{Qt::Key_Left, Qt::KeypadModifier}}, VimLineEditCommand::MoveLeft},
        KeyBinding{{KeyChord{Qt::Key_Right, Qt::KeypadModifier}}, VimLineEditCommand::MoveRight},
        KeyBinding{{KeyChord{Qt::Key_D, Qt::NoModifier}, KeyChord{Qt::Key_I, Qt::NoModifier}, KeyChord{Qt::Key_W, Qt::NoModifier}}, VimLineEditCommand::DeleteInsideWord},
        KeyBinding{{KeyChord{Qt::Key_F, Qt::NoModifier}}, VimLineEditCommand::FindForward},
        KeyBinding{{KeyChord{Qt::Key_F, Qt::ShiftModifier}}, VimLineEditCommand::FindBackward},
        KeyBinding{{KeyChord{Qt::Key_Semicolon, Qt::NoModifier}}, VimLineEditCommand::RepeatFind},
        KeyBinding{{KeyChord{Qt::Key_W, Qt::NoModifier}}, VimLineEditCommand::MoveWordForward},
        KeyBinding{{KeyChord{Qt::Key_W, Qt::ShiftModifier}}, VimLineEditCommand::MoveWordForwardWithSymbols},
        KeyBinding{{KeyChord{Qt::Key_E, Qt::NoModifier}}, VimLineEditCommand::MoveToEndOfWord},
        KeyBinding{{KeyChord{Qt::Key_E, Qt::ShiftModifier}}, VimLineEditCommand::MoveToEndOfWordWithSymbols},
        KeyBinding{{KeyChord{Qt::Key_B, Qt::NoModifier}}, VimLineEditCommand::MoveWordBackward},
        KeyBinding{{KeyChord{Qt::Key_B, Qt::ShiftModifier}}, VimLineEditCommand::MoveWordBackwardWithSymbols},
        KeyBinding{{KeyChord{Qt::Key_T, Qt::NoModifier}}, VimLineEditCommand::FindForwardTo},
        KeyBinding{{KeyChord{Qt::Key_T, Qt::ShiftModifier}}, VimLineEditCommand::FindBackwardTo},
        KeyBinding{{KeyChord{Qt::Key_X, Qt::NoModifier}}, VimLineEditCommand::DeleteChar},
    };

    for (const auto &binding : key_bindings) {
        input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

}

std::optional<VimLineEditCommand> VimLineEdit::handle_key_event(int key, Qt::KeyboardModifiers modifiers){

    InputTreeNode *node = current_node ? current_node : &input_tree;
    for (auto &child : node->children) {
        if (child.key_chord.key == key && child.key_chord.modifiers == modifiers) {
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
        case VimLineEditCommand::DeleteInsideWord: return "DeleteInsideWord";
        case VimLineEditCommand::DeleteInsideParentheses: return "DeleteInsideParentheses";
        case VimLineEditCommand::DeleteInsideBrackets: return "DeleteInsideBrackets";
        case VimLineEditCommand::DeleteInsideBraces: return "DeleteInsideBraces";
        case VimLineEditCommand::FindForward: return "FindForward";
        case VimLineEditCommand::FindBackward: return "FindBackward";
        case VimLineEditCommand::FindForwardTo: return "FindForwardTo";
        case VimLineEditCommand::FindBackwardTo: return "FindBackwardTo";
        case VimLineEditCommand::RepeatFind: return "RepeatFind";
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

    switch (cmd) {
        case VimLineEditCommand::EnterInsertMode:
            current_mode = VimMode::Insert;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterInsertModeAfter:
            current_mode = VimMode::Insert;
            cursorForward(false);
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterInsertModeBegin:
            current_mode = VimMode::Insert;
            setCursorPosition(0);
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterInsertModeEnd:
            current_mode = VimMode::Insert;
            setCursorPosition(text().length());
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterNormalMode:
            current_mode = VimMode::Normal;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::EnterVisualMode:
            current_mode = VimMode::Visual;
            set_style_for_mode(current_mode);
            break;
        case VimLineEditCommand::MoveLeft:
            cursorBackward(false);
            break;
        case VimLineEditCommand::MoveRight:
            cursorForward(false);
            break;
        case VimLineEditCommand::MoveWordForward:
            move_word_forward(false);
            break;
        case VimLineEditCommand::MoveWordForwardWithSymbols:
            move_word_forward(true);
            break;
        case VimLineEditCommand::MoveToEndOfWord:
            move_to_end_of_word(false);
            break;
        case VimLineEditCommand::MoveToEndOfWordWithSymbols:
            move_to_end_of_word(true);
            break;
        case VimLineEditCommand::MoveWordBackward:
            move_word_backward(false);
            break;
        case VimLineEditCommand::MoveWordBackwardWithSymbols:
            move_word_backward(true);
            break;
        case VimLineEditCommand::DeleteChar:
            delete_char();
            break;
        case VimLineEditCommand::FindForward:{
            last_find_state = FindState{FindDirection::Forward, symbol};
            handle_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::FindBackward:{
            last_find_state = FindState{FindDirection::Backward, symbol};
            handle_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::FindForwardTo:{
            last_find_state = FindState{FindDirection::ForwardTo, symbol};
            handle_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::FindBackwardTo:{
            last_find_state = FindState{FindDirection::BackwardTo, symbol};
            handle_find(last_find_state.value());
            break;
        }
        case VimLineEditCommand::RepeatFind: {
            if (last_find_state.has_value()){
                handle_find(last_find_state.value());
            }
            break;
        }
        // Add more cases for other commands as needed
        default:
            break;
    }

}

void VimLineEdit::handle_find(FindState find_state){

    int location = -1;
    switch (find_state.direction) {
        case FindDirection::Forward:
            location = text().indexOf(QChar(find_state.character.value_or(' ')), cursorPosition() + 2);
            break;
        case FindDirection::Backward:
            if (cursorPosition() == 0) return;
            location = text().lastIndexOf(QChar(find_state.character.value_or(' ')), cursorPosition() - 1);
            break;
        case FindDirection::ForwardTo:
            location = text().indexOf(QChar(find_state.character.value_or(' ')), cursorPosition() + 2);
            if (location != -1) {
                location--;
            }
            break;
        case FindDirection::BackwardTo:
            if (cursorPosition() == 0) return;
            location = text().lastIndexOf(QChar(find_state.character.value_or(' ')), cursorPosition() - 1);
            if (location != -1) {
                location++;
            }
            break;
    }

    if (location != -1) {
        setCursorPosition(location);
    }
}

void VimLineEdit::move_word_forward(bool with_symbols) {
    int pos = cursorPosition();
    const QString& t = text();
    int len = t.length();

    if (pos >= len - 1) {
        return;
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
        setCursorPosition(next_pos);
    }
}

void VimLineEdit::move_to_end_of_word(bool with_symbols) {
    int pos = cursorPosition();
    const QString& t = text();
    int len = t.length();

    if (pos >= len - 1) {
        return;
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
        setCursorPosition(next_pos);
    }
}

void VimLineEdit::move_word_backward(bool with_symbols) {
    int pos = cursorPosition();
    const QString& t = text();

    if (pos <= 0) {
        return;
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

    setCursorPosition(prev_pos);
}

void VimLineEdit::delete_char() {
    int current_pos = cursorPosition();
    QString current_text = text();
    if (current_pos < current_text.length()) {
        current_text.remove(current_pos, 1);
        setText(current_text);
        setCursorPosition(current_pos);
    }
}