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
        case VimLineEditCommand::MoveWordLeft: return "MoveWordLeft";
        case VimLineEditCommand::MoveWordRight: return "MoveWordRight";
        case VimLineEditCommand::DeleteInsideWord: return "DeleteInsideWord";
        case VimLineEditCommand::DeleteInsideParentheses: return "DeleteInsideParentheses";
        case VimLineEditCommand::DeleteInsideBrackets: return "DeleteInsideBrackets";
        case VimLineEditCommand::DeleteInsideBraces: return "DeleteInsideBraces";
        case VimLineEditCommand::FindForward: return "FindForward";
        case VimLineEditCommand::FindBackward: return "FindBackward";
        case VimLineEditCommand::RepeatFind: return "RepeatFind";
        default: return "Unknown";
    }
}

bool requires_symbol(VimLineEditCommand cmd){
    switch (cmd) {
        case VimLineEditCommand::FindForward:
        case VimLineEditCommand::FindBackward:
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

    if (find_state.direction == FindDirection::Forward) {
        int location = text().indexOf(QChar(find_state.character.value_or(' ')), cursorPosition() + 2);
        if (location != -1) {
            setCursorPosition(location);
        }
    }
    else{
        if (cursorPosition() == 0) return;

        int location = text().lastIndexOf(QChar(find_state.character.value_or(' ')), cursorPosition() - 1);
        if (location != -1) {
            setCursorPosition(location);
        }
    }
}