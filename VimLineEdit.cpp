#include <algorithm>
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
#include <_stdio.h>
#include <functional>
#include <utility>
#include <variant>
#include <vector>
#include <QClipboard>

namespace QVimEditor{


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

VimEditor::VimEditor(QWidget *editor_widget) : editor_widget(editor_widget) {

    if (dynamic_cast<QLineEdit*>(editor_widget)){
        adapter = new QLineEditAdapter(dynamic_cast<QLineEdit*>(editor_widget));
    }
    else{
        adapter = new QTextEditAdapter(dynamic_cast<QTextEdit*>(editor_widget));
    }

    QFont font = editor_widget->font();
    font.setFamily("Courier New");
    font.setStyleHint(QFont::TypeWriter);
    editor_widget->setFont(font);
    add_vim_keybindings();

    command_line_edit = new EscapeLineEdit(editor_widget);
    command_line_edit->setFont(font);
    command_line_edit->hide();

    command_line_edit->setStyleSheet("background-color: #222222;");

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

bool VimEditor::key_press_event(QKeyEvent *event) {

    if (event->key() == Qt::Key_Return && current_mode == VimMode::Normal) {
        if (dynamic_cast<QLineEditAdapter*>(adapter)){
            return true;
        }
        else{
            VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
            if (text_edit) {
                emit text_edit->normalEnterPressed();
            }
            return false;
        }
    }
    if (event->key() == Qt::Key_Escape) {
        if (current_mode == VimMode::Normal){
            return true;
        }
        else{
            add_event_to_current_macro(event);
            handle_command(VimLineEditCommand::EnterNormalMode, {});
            return false;
        }
    }
    if (current_mode != VimMode::Normal || (event->key() != Qt::Key_Q)){
        add_event_to_current_macro(event);
    }

    if (current_mode == VimMode::Normal || current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
        // we don't want to handle when we only press a modifier (e.g. Shift, Ctrl, etc.)
        int event_key = event->key();
        bool is_keypress_a_modifier = event_key == Qt::Key_Shift || event_key == Qt::Key_Control ||
                                      event_key == Qt::Key_Alt || event_key == Qt::Key_Meta;

        bool is_key_a_number = (event_key >= Qt::Key_0 && event_key <= Qt::Key_9);

        if (!is_keypress_a_modifier && !action_waiting_for_motion.has_value() && is_key_a_number){
            current_command_repeat_number.append(event->text());
        }

        if (!is_keypress_a_modifier && pending_symbol_command.has_value()) {
            // If we have a pending command that requires a symbol, we handle it now
            VimLineEditCommand command = pending_symbol_command.value();
            pending_symbol_command = {};

            if (event->text().size() > 0) {
                handle_command(command, static_cast<char>(event->text().at(0).toLatin1()));
            }

            return false;
        }

        if (!is_keypress_a_modifier && action_waiting_for_motion.has_value()) {

            if (action_waiting_for_motion->surrounding_scope == SurroundingScope::None) {

                if (event->key() == Qt::Key_I) {
                    action_waiting_for_motion->surrounding_scope = SurroundingScope::Inside;
                    return false;
                }
                else if (event->key() == Qt::Key_A) {
                    action_waiting_for_motion->surrounding_scope = SurroundingScope::Around;
                    return false;
                }
                else{
                    // pressing dd should delete the current line
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete && event->key() == Qt::Key_D){
                        action_waiting_for_motion = {};
                        handle_command(VimLineEditCommand::DeleteCurrentLine);
                        return false;
                    }
                    // pressing cc should change the current line
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change && event->key() == Qt::Key_C){
                        action_waiting_for_motion = {};
                        handle_command(VimLineEditCommand::ChangeCurrentLine);
                        return false;
                    }
                    // pressing yy should yank the current line
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank && event->key() == Qt::Key_Y){
                        action_waiting_for_motion = {};
                        handle_command(VimLineEditCommand::YankCurrentLine);
                        return false;
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
                    return false;
                }
            }
            if (handle_surrounding_motion_action()) {
                return false;
            }
        }

        if (!is_keypress_a_modifier) {
            std::optional<VimLineEditCommand> command =
                handle_key_event(event->text(), event_key, event->modifiers());
            if (command.has_value()) {
                if (requires_symbol(command.value())) {
                    // If the command requires a symbol, we need to wait for the next key press
                    pending_symbol_command = command;
                    return false;
                }
                else {
                    handle_command(command.value());
                }
            }
        }

        return false;
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
                return false;
            }
        }
    }

    if (current_mode == VimMode::Insert){
        current_insert_mode_text += event->text();
        int text_size = event->text().size();

        if (event->key() == Qt::Key_Backspace) {
            text_size = -1;
            current_insert_mode_text = current_insert_mode_text.left(current_insert_mode_text.length() - 1);
        }

        int current_position = get_cursor_position();
        for (auto& [_, mark] : marks){
            if (mark.position > current_position){
                mark.position += text_size;
            }
        }
    }

    return true;
}

void VimEditor::set_style_for_mode(VimMode mode) {
    if (mode == VimMode::Normal) {
        // setStyleSheet("background-color: lightgray;");
        int font_width = adapter->get_font_metrics().horizontalAdvance(" ");
        adapter->set_cursor_width(font_width);
        // setCursorWidth(font_width);
        // setStyle(new LineEditStyle(font_width));
    }
    else if (mode == VimMode::Insert) {
        // setStyleSheet("background-color: white;");
        // setCursorWidth(1);
        adapter->set_cursor_width(1);
        // setStyle(new LineEditStyle(1));
    }
    else if (mode == VimMode::Visual || mode == VimMode::VisualLine) {
        // setStyleSheet("background-color: pink;");
        int font_width = adapter->get_font_metrics().horizontalAdvance(" ");
        // setCursorWidth(font_width);
        adapter->set_cursor_width(font_width);
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

void VimEditor::add_vim_keybindings() {
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
        KeyBinding{{KeyChord{"y", {}}}, VimLineEditCommand::Yank},
        KeyBinding{{KeyChord{"p", {}}}, VimLineEditCommand::PasteForward},
        KeyBinding{{KeyChord{"P", {}}}, VimLineEditCommand::PasteBackward},
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
        KeyBinding{{KeyChord{"m", {}}}, VimLineEditCommand::SetMark},
        KeyBinding{{KeyChord{"`", {}}}, VimLineEditCommand::GotoMark},
        KeyBinding{{KeyChord{"q", {}}}, VimLineEditCommand::RecordMacro},
        KeyBinding{{KeyChord{"@", {}}}, VimLineEditCommand::RepeatMacro},
        KeyBinding{{KeyChord{"*", {}}}, VimLineEditCommand::SearchTextUnderCursor},
        KeyBinding{{KeyChord{"#", {}}}, VimLineEditCommand::SearchTextUnderCursorBackward},
        KeyBinding{{KeyChord{"%", {}}}, VimLineEditCommand::GotoMatchingBracket},
        KeyBinding{{KeyChord{"~", {}}}, VimLineEditCommand::SwapCaseCharacterUnderCursor},
        KeyBinding{{KeyChord{"}", {}}}, VimLineEditCommand::MoveToTheNextParagraph},
        KeyBinding{{KeyChord{"{", {}}}, VimLineEditCommand::MoveToThePreviousParagraph},
    };

    for (const auto &binding : key_bindings) {
        normal_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

    visual_mode_input_tree = normal_mode_input_tree.clone();
    std::vector<KeyBinding> visual_mode_keybindings = {
        KeyBinding{{KeyChord{"o", {}}}, VimLineEditCommand::ToggleVisualCursor},
        KeyBinding{{KeyChord{"U", {}}}, VimLineEditCommand::Uppercasify},
        KeyBinding{{KeyChord{"~", {}}}, VimLineEditCommand::SwapCaseSelection},
    };

    for (const auto &binding : visual_mode_keybindings) {
        visual_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

    std::vector<KeyBinding> insert_mode_keybindings = {
        KeyBinding{{KeyChord{Qt::Key_W, CONTROL}}, VimLineEditCommand::DeletePreviousWord},
        KeyBinding{{KeyChord{Qt::Key_A, CONTROL}}, VimLineEditCommand::InsertLastInsertModeText},
    };

    for (const auto &binding : insert_mode_keybindings) {
        insert_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

}

std::optional<VimLineEditCommand> VimEditor::handle_key_event(QString event_text, int key,
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

QString to_string(VimLineEditCommand cmd) {
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
    case VimLineEditCommand::Yank:
        return "Yank";
    case VimLineEditCommand::DeleteToEndOfLine:
        return "DeleteToEndOfLine";
    case VimLineEditCommand::ChangeToEndOfLine:
        return "ChangeToEndOfLine";
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
    case VimLineEditCommand::PasteBackward:
        return "PasteBackward";
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
    case VimLineEditCommand::YankCurrentLine:
        return "YankCurrentLine";
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
    case VimLineEditCommand::InsertLastInsertModeText:
        return "InsertLastInsertModeText";
    case VimLineEditCommand::SetMark:
        return "SetMark";
    case VimLineEditCommand::GotoMark:
        return "GotoMark";
    case VimLineEditCommand::RecordMacro:
        return "RecordMacro";
    case VimLineEditCommand::RepeatMacro:
        return "RepeatMacro";
    case VimLineEditCommand::SearchTextUnderCursor:
        return "SearchTextUnderCursor";
    case VimLineEditCommand::SearchTextUnderCursorBackward:
        return "SearchTextUnderCursorBackward";
    case VimLineEditCommand::GotoMatchingBracket:
        return "GotoMatchingBracket";
    case VimLineEditCommand::Uppercasify:
        return "Uppercasify";
    case VimLineEditCommand::SwapCaseCharacterUnderCursor:
        return "SwapcaseCharacterUnderCursor";
    case VimLineEditCommand::SwapCaseSelection:
        return "SwapCaseSelection";
    case VimLineEditCommand::MoveToTheNextParagraph:
        return "MoveToTheNextParagraph";
    case VimLineEditCommand::MoveToThePreviousParagraph:
        return "MoveToThePreviousParagraph";
    default:
        return "Unknown";
    }
}

bool VimEditor::requires_symbol(VimLineEditCommand cmd) {
    switch (cmd) {
    case VimLineEditCommand::FindForward:
    case VimLineEditCommand::FindBackward:
    case VimLineEditCommand::FindForwardTo:
    case VimLineEditCommand::FindBackwardTo:
    case VimLineEditCommand::SetMark:
    case VimLineEditCommand::GotoMark:
    case VimLineEditCommand::RepeatMacro:
        return true;
    case VimLineEditCommand::RecordMacro:
        return !current_macro.has_value();
    default:
        return false;
    }
}

void VimEditor::handle_command(VimLineEditCommand cmd, std::optional<char> symbol) {

    // don't handle the 0 key while we are typing a repeat number
    if (cmd == VimLineEditCommand::MoveToBeginningOfLine && current_command_repeat_number.size() > 1) {
        return;
    }

    int num_repeats = 1;
    if (current_command_repeat_number.size() > 0){
        bool ok;
        num_repeats = current_command_repeat_number.toInt(&ok);
        if (!ok) {
            num_repeats = 1;
        }
        current_command_repeat_number = "";
    }


    int new_pos = -1;
    int old_pos = get_cursor_position();
    // some commands' delete differs from the way they move
    // for example, pressing w moves the cursor to the beginning of the next word,
    // but does not delete the first character of the next word, but pressing e
    // moves the cursor to the end of the current word and de deletes that character
    int delete_pos_offset = 0;

    HistoryState current_state;
    current_state.text = adapter->get_text();
    current_state.cursor_position = old_pos;
    current_state.marks = marks;
    bool should_reset_desired_pos = true;

    switch (cmd) {
    case VimLineEditCommand::EnterInsertMode:
        push_history(current_state);
        set_mode(VimMode::Insert);
        break;
    case VimLineEditCommand::EnterInsertModeAfter:{
        set_mode(VimMode::Insert);
        int current_pos = get_cursor_position();

        bool is_ony_empty_line = false;
        if (current_pos >= 0 && current_pos < current_state.text.length()) {
            if (current_state.text[current_pos] == '\n'){
                is_ony_empty_line = true;
            }
        }

        if (!is_ony_empty_line){
            new_pos = get_cursor_position() + 1;
        }
        break;
    }
    case VimLineEditCommand::EnterInsertModeBegin:
        set_mode(VimMode::Insert);
        new_pos = 0;
        break;
    case VimLineEditCommand::EnterInsertModeEnd:
        set_mode(VimMode::Insert);
        new_pos = current_state.text.length();
        break;
    case VimLineEditCommand::Uppercasify: {
        int begin, end;
        QString selection = get_current_selection(begin, end);
        if (selection.size() > 0){
            insert_text(selection.toUpper(), begin, end);
        }
        set_mode(VimMode::Normal);
        set_cursor_position(begin);
        break;

    }
    case VimLineEditCommand::SwapCaseSelection: {
        int begin, end;
        QString selection = get_current_selection(begin, end);
        if (selection.size() > 0){
            insert_text(swap_case(selection), begin, end);
        }
        set_mode(VimMode::Normal);
        set_cursor_position(begin);
        break;

    }
    case VimLineEditCommand::EnterInsertModeBeginLine:
        set_mode(VimMode::Insert);
        new_pos = get_line_start_position(get_cursor_position());
        break;
    case VimLineEditCommand::RecordMacro:
        if (current_macro.has_value()){
            macros[current_macro->name] = std::move(current_macro.value());
            current_macro = {};
        }
        else{
            Macro new_macro;
            new_macro.name = symbol.value();
            current_macro = std::move(new_macro);
        }
        break;
    case VimLineEditCommand::RepeatMacro: {
        int macro_symbol = symbol.value();

        if (symbol.value() == '@'){
            macro_symbol = last_macro_symbol;
        }

        if (macros.find(macro_symbol) != macros.end()){
            Macro& macro = macros[macro_symbol];
            for (int j = 0; j < num_repeats; j++){
                for (int i = 0; i < macro.events.size(); i++) {
                    auto clone = macro.events[i].get()->clone();
                    if (key_press_event(clone)){
                        adapter->key_press_event(clone);
                    }
                    delete clone;
                }
            }
            last_macro_symbol = macro_symbol;
        }
        break;
    }
    case VimLineEditCommand::SearchTextUnderCursor:
    case VimLineEditCommand::SearchTextUnderCursorBackward: {
        int begin, end;
        QString word_under_cursor = get_word_under_cursor_bounds(begin, end);

        if (word_under_cursor.size() > 0){
            SearchState new_search_state;
            new_search_state.direction = cmd == VimLineEditCommand::SearchTextUnderCursor ?  FindDirection::Forward : FindDirection::Backward;
            new_search_state.query = word_under_cursor;
            last_search_state = new_search_state;
            handle_search();
        }
        break;
    }

    case VimLineEditCommand::EnterInsertModeEndLine:
        set_mode(VimMode::Insert);
        new_pos = get_line_end_position(get_cursor_position());
        break;
    case VimLineEditCommand::GotoBegin:
        if (num_repeats <= 1){
            new_pos = get_line_start_position(0);
        }
        else {
            new_pos = get_ith_line_start_position(num_repeats - 1);
        }
        break;
    case VimLineEditCommand::GotoEnd:
        if (num_repeats <= 1){
            new_pos = get_line_end_position(current_state.text.length());
        }
        else {
            new_pos = get_ith_line_start_position(num_repeats - 1);
        }
        break;
    case VimLineEditCommand::MoveToBeginningOfLine:{
        new_pos = get_line_start_position(get_cursor_position());
        break;
    }
    case VimLineEditCommand::MoveToEndOfLine:{
        new_pos = get_line_end_position(get_cursor_position()) - 1;
        delete_pos_offset = 1;
        break;
    }
    case VimLineEditCommand::MoveToTheNextParagraph:{
        int next_paragraph_start = current_state.text.indexOf("\n\n", get_cursor_position());
        if (next_paragraph_start == -1) {
            new_pos = current_state.text.length();
        }
        else {
            new_pos = next_paragraph_start + 2;
        }
        break;
    }
    case VimLineEditCommand::MoveToThePreviousParagraph:{
        int previous_paragraph_end = current_state.text.mid(0, get_cursor_position()-1).lastIndexOf("\n\n");
        if (previous_paragraph_end == -1 || previous_paragraph_end > get_cursor_position()) {
            new_pos = 0;
        }
        else {
            new_pos = previous_paragraph_end + 2;
        }
        break;
    }
    case VimLineEditCommand::InsertLastInsertModeText:{
        // Insert the last text entered in insert mode in the current position
        if (last_insert_mode_text.size() > 0) {
            int cursor_position = get_cursor_position();
            insert_text(last_insert_mode_text, get_cursor_position());
            set_cursor_position(cursor_position + last_insert_mode_text.size());
            current_insert_mode_text += last_insert_mode_text;
        }
        break;
    }
    case VimLineEditCommand::EnterNormalMode: {
        if (visual_line_selection_begin != -1){
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
            visual_line_selection_begin = -1;
            visual_line_selection_end = -1;
        }
        if (current_mode == VimMode::Visual){
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
        }

        push_history(current_state);
        VimMode previous_mode = current_mode;
        set_mode(VimMode::Normal);

        if (get_cursor_position() <= 0) {
            new_pos = 0;
        }
        else {
            new_pos = get_cursor_position();
        }

        action_waiting_for_motion = {};

        // In normal mode, cursor should be on a character, not between
        // characters Move cursor back by one position unless we're already at
        // the beginning of a line
        if (previous_mode == VimMode::Insert){
            bool is_at_beginning_of_line = (new_pos > 0 && current_state.text[new_pos - 1] == '\n');
            if (get_cursor_position() > 0) {
                if (!is_at_beginning_of_line) {
                    new_pos = get_cursor_position() - 1;
                }
                else {
                    new_pos = get_cursor_position();
                }
            }
        }
        break;
    }
    case VimLineEditCommand::EnterVisualMode: {
        set_mode(VimMode::Visual);
        visual_mode_anchor = get_cursor_position();
        set_visual_selection(visual_mode_anchor, 1);

        action_waiting_for_motion = {ActionWaitingForMotionKind::Visual, SurroundingScope::None, SurroundingKind::None};

        break;
    }
    case VimLineEditCommand::EnterVisualLineMode:
        set_mode(VimMode::VisualLine);
        visual_mode_anchor = get_cursor_position();
        // Immediately select the current line
        set_cursor_position_with_line_selection(get_cursor_position());
        break;
    case VimLineEditCommand::MoveLeft: {
        int old_pos = get_cursor_position();
        new_pos = old_pos - num_repeats;
        if (new_pos >= 0 && current_state.text[new_pos] == '\n') {
            new_pos = old_pos;
        }
        break;
    }
    case VimLineEditCommand::MoveRight: {
        int old_pos = get_cursor_position();
        if ((get_cursor_position() + num_repeats - 1) < current_state.text.length()) {
            new_pos = get_cursor_position() + num_repeats;
        }
        else {
            new_pos = current_state.text.length();
        }
        if (new_pos < current_state.text.length() && current_state.text[new_pos] == '\n') {
            new_pos = old_pos;
        }
        break;
    }
    case VimLineEditCommand::MoveUp:
        should_reset_desired_pos = false;
        new_pos = get_cursor_position();
        for (int i = 0; i < num_repeats; i++) {
            new_pos = calculate_move_up(new_pos);
        }
        break;
    case VimLineEditCommand::MoveDown:
        should_reset_desired_pos = false;
        new_pos = get_cursor_position();
        for (int i = 0; i < num_repeats; i++) {
            new_pos = calculate_move_down(new_pos);
        }
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
        push_history(current_state);
        for (int i = 0; i < num_repeats; i++){
            delete_char(num_repeats == 1);
        }
        if (cmd == VimLineEditCommand::DeleteCharAndEnterInsertMode) {
            set_mode(VimMode::Insert);
        }
        break;
    case VimLineEditCommand::Delete:
        push_history(current_state);
        action_waiting_for_motion = {ActionWaitingForMotionKind::Delete, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::Change:
        push_history(current_state);
        action_waiting_for_motion = {ActionWaitingForMotionKind::Change, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::Yank:
        action_waiting_for_motion = {ActionWaitingForMotionKind::Yank, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::DeleteToEndOfLine: 
    case VimLineEditCommand::ChangeToEndOfLine:
    {
        push_history(current_state);
        int cursor_pos = get_cursor_position();
        int line_end = get_line_end_position(cursor_pos);
        int cursor_offset = cmd == VimLineEditCommand::DeleteToEndOfLine ? -1 : 0;
        
        if (cursor_pos < line_end) {
            set_last_deleted_text(current_state.text.mid(cursor_pos, line_end - cursor_pos));
            remove_text(cursor_pos, line_end - cursor_pos);
            set_cursor_position(cursor_pos + cursor_offset);
        }
        if (cmd == VimLineEditCommand::ChangeToEndOfLine){
            set_mode(VimMode::Insert);
        }
        break;
    }
    case VimLineEditCommand::FindForward: {
        last_find_state = FindState{FindDirection::Forward, symbol};
        new_pos = calculate_find(last_find_state.value());
        delete_pos_offset = 1;
        break;
    }
    case VimLineEditCommand::SetMark: {
        Mark mark;
        mark.name = symbol.value();
        mark.position = get_cursor_position();
        marks[symbol.value()] = mark;
        break;
    }
    case VimLineEditCommand::GotoMark: {
        if (marks.find(symbol.value()) != marks.end()) {
            Mark mark = marks[symbol.value()];
            // move the cursor to the mark location
            new_pos = mark.position;
        }
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
        delete_pos_offset = 1;
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
    case VimLineEditCommand::YankCurrentLine:
    case VimLineEditCommand::ChangeCurrentLine:
    case VimLineEditCommand::DeleteCurrentLine: {
        int cursor_pos = get_cursor_position();
        int line_start = get_line_start_position(cursor_pos);
        int line_end = get_line_end_position(cursor_pos);

        set_last_deleted_text(current_state.text.mid(line_start, line_end - line_start), true);

        if (cmd != VimLineEditCommand::YankCurrentLine){

            if (cmd == VimLineEditCommand::DeleteCurrentLine) {
                line_end++;
            }

            push_history(current_state);
            remove_text(line_start, line_end - line_start);
            set_cursor_position(line_start);

            if (cmd == VimLineEditCommand::ChangeCurrentLine) {
                set_mode(VimMode::Insert);
            }
        }

        break;
    }
    case VimLineEditCommand::PasteForward: {
        if (last_deleted_text.text.size() > 0) {
            QString current_text = current_state.text;
            int cursor_pos = get_cursor_position();

            for (int i = 0; i < num_repeats; i++){
                if (last_deleted_text.is_line) {
                    // Paste the line below the current line
                    int line_end = get_line_end_position(cursor_pos);
                    insert_text("\n" + last_deleted_text.text, line_end);

                    new_pos = line_end + 1;
                }
                else {
                    // Paste the last deleted text after the cursor position
                    insert_text(last_deleted_text.text, cursor_pos + 1);
                    new_pos = cursor_pos + last_deleted_text.text.size();
                }
                cursor_pos = new_pos;
            }
        }
        break;
    }
    case VimLineEditCommand::PasteBackward: {
        if (last_deleted_text.text.size() > 0) {
            QString current_text = current_state.text;
            int cursor_pos = get_cursor_position();

            if (last_deleted_text.is_line) {
                // Paste the line above the current line
                int line_start = get_line_start_position(cursor_pos);
                insert_text(last_deleted_text.text + "\n", line_start);
                new_pos = line_start;
            } else {
                // Paste the last deleted text before the cursor position
                insert_text(last_deleted_text.text, cursor_pos);
                new_pos = cursor_pos + last_deleted_text.text.size();
            }
        }
        break;
    }
    case VimLineEditCommand::InsertLineBelow: {
        push_history(current_state);
        QString current_text = current_state.text;
        int cursor_pos = get_cursor_position();
        int line_end = get_line_end_position(cursor_pos);
        insert_text("\n", line_end);
        new_pos = line_end + 1;
        set_mode(VimMode::Insert);
        break;
    }
    case VimLineEditCommand::DeletePreviousWord: {
        QString current_text = current_state.text;
        int cursor_pos = get_cursor_position();
        // find the last space or newline
        int previous_space_index = current_text.lastIndexOf(QRegularExpression("\\s"), std::max(cursor_pos - 2, 0));
        int previous_non_space_index = current_text.lastIndexOf(QRegularExpression("\\S"), std::max(previous_space_index - 1, 0));

        if (cursor_pos-1 == previous_non_space_index){
            break;
        }

        if (previous_space_index != -1){
            previous_space_index = previous_non_space_index != -1 ? previous_non_space_index + 1 : previous_space_index;
        }

        int delete_begin = previous_space_index + 1;
        int delete_length = cursor_pos - delete_begin;
        if (previous_space_index == -1){
            previous_space_index = 0;
            delete_begin = 0;
            delete_length = cursor_pos;
        }
        remove_text(delete_begin, delete_length);

        if (current_mode == VimMode::Insert){
            current_insert_mode_text = current_insert_mode_text.left(current_insert_mode_text.length() - delete_length);
        }

        new_pos = delete_begin;

        break;
    }
    case VimLineEditCommand::InsertLineAbove: {
        push_history(current_state);
        QString current_text = current_state.text;
        int cursor_pos = get_cursor_position();
        int line_start = get_line_start_position(cursor_pos);
        insert_text("\n", line_start);
        new_pos = line_start;
        set_mode(VimMode::Insert);
        break;
    }
    case VimLineEditCommand::SearchCommand:
    case VimLineEditCommand::ReverseSearchCommand:
    case VimLineEditCommand::CommandCommand: {
        pending_text_command = cmd;
        show_command_line_edit(to_string(cmd));
        break;
    }
    case VimLineEditCommand::DecrementNextNumberOnCurrentLine:
    case VimLineEditCommand::IncrementNextNumberOnCurrentLine: {
        push_history(current_state);
        handle_number_increment_decrement(cmd == VimLineEditCommand::IncrementNextNumberOnCurrentLine);
        break;
    }
    case VimLineEditCommand::SwapCaseCharacterUnderCursor: {
        int cursor_pos = get_cursor_position();
        if (cursor_pos < 0 || cursor_pos >= current_state.text.length()) {
            break;
        }

        QChar current_char = current_state.text[cursor_pos];
        if (current_char.isLetter()) {
            // Swap case of the character under the cursor
            QString new_text = current_state.text;
            if (current_char.isUpper()) {
                new_text[cursor_pos] = current_char.toLower();
            } else {
                new_text[cursor_pos] = current_char.toUpper();
            }
            adapter->set_text(new_text);
        }
        if (cursor_pos < current_state.text.length()) {
            new_pos = cursor_pos + 1;
        }
        break;
    }
    case VimLineEditCommand::GotoMatchingBracket: {
        int cursor_pos = get_cursor_position();
        std::unordered_map<int, int> matching_brackets = {
            {'(', ')'},
            {')', '('},
            {'{', '}'},
            {'}', '{'},
            {'[', ']'},
            {']', '['},
            {'<', '>'},
            {'>', '<'}
        };
        std::vector<int> opening_brackets = {'(', '{', '[', '<'};

        if (cursor_pos < 0 || cursor_pos >= current_state.text.length()) {
            break;
        }
        QChar current_char = current_state.text[cursor_pos];
        if (matching_brackets.find(current_char.unicode()) == matching_brackets.end()) {
            // not a bracket
            break;
        }
        int target_pos = -1;
        int bracket_count = 0;
        char target_bracket = matching_brackets[current_char.unicode()];
        bool is_opening_bracket = std::find(opening_brackets.begin(), opening_brackets.end(), current_char.unicode()) != opening_brackets.end();
        if (is_opening_bracket){
            target_pos = current_state.text.indexOf(target_bracket, cursor_pos + 1);
        }
        else{
            target_pos = current_state.text.lastIndexOf(target_bracket, cursor_pos - 1);
        }

        if (target_pos != -1) {
            new_pos = target_pos;
        }


        break;
    }
    case VimLineEditCommand::ToggleVisualCursor: {
        if (current_mode == VimMode::Visual) {
            int current_cursor_pos = get_cursor_position();
            int temp = visual_mode_anchor;
            visual_mode_anchor = current_cursor_pos;
            new_pos = temp;
        }
        if (current_mode == VimMode::VisualLine){
            // swap between the first and last line of the selection, but keep the cursor position in the line (if possible)
            int current_cursor_pos = get_cursor_position();
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

    if (should_reset_desired_pos){
        desired_index_in_line = {};
    }

    // we we have a text selected in visual mode and then perform change or delete
    // we should use the selected text as the target of the change/delete
    if (action_waiting_for_motion.has_value() &&
        (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change || action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete || action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) &&
        (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine)){
        int selection_begin, selection_end;
        QString selected_text = get_current_selection(selection_begin, selection_end);

        if (current_mode == VimMode::Visual) {
            auto selections = adapter->get_extra_selections();
            set_last_deleted_text(selected_text);
            if (cmd !=  VimLineEditCommand::Yank) {
                // int start_pos = cursor.selectionStart();
                remove_text(selection_begin, selection_end - selection_begin);
            }
            set_cursor_position(selection_begin);
        }
        if (current_mode == VimMode::VisualLine) {
            int start = visual_line_selection_begin;
            int end = visual_line_selection_end;
            int offset = (end == current_state.text.size()) ? 1 : 0;
            if (current_state.text[end-1] == '\n') {
                offset = 0;
            }
            // // offset = 0;

            set_last_deleted_text(current_state.text.mid(start, end - start - 1 + offset), true);
            if (cmd !=  VimLineEditCommand::Yank) {
                remove_text(start, end - start);
            }
            set_cursor_position(start);
        }

        if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
            set_mode(VimMode::Insert);
        }

        if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete || action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) {
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
        }

        action_waiting_for_motion = {};

    }

    if (cmd != VimLineEditCommand::EnterNormalMode && current_mode == VimMode::Normal && new_pos == current_state.text.size()) {
        new_pos = current_state.text.size() - 1;
    }

    if (new_pos != -1) {
        if (action_waiting_for_motion.has_value() && action_waiting_for_motion->kind != ActionWaitingForMotionKind::Visual) {
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

int VimEditor::calculate_find(FindState find_state, bool reverse) const {

    int location = -1;
    FindDirection direction =
        reverse ? (find_state.direction == FindDirection::Forward ? FindDirection::Backward
                                                                  : FindDirection::Forward)
                : find_state.direction;
    QString text = adapter->get_text();
    switch (direction) {
    case FindDirection::Forward:
        location = text.indexOf(QChar(find_state.character.value_or(' ')),
                                         get_cursor_position() + 2);
        break;
    case FindDirection::Backward:
        if (get_cursor_position() == 0)
            return get_cursor_position();
        location = text.lastIndexOf(QChar(find_state.character.value_or(' ')),
                                             get_cursor_position() - 1);
        break;
    case FindDirection::ForwardTo:
        location = text.indexOf(QChar(find_state.character.value_or(' ')),
                                         get_cursor_position() + 2);
        if (location != -1) {
            location--;
        }
        break;
    case FindDirection::BackwardTo:
        if (get_cursor_position() == 0)
            return get_cursor_position();
        location = text.lastIndexOf(QChar(find_state.character.value_or(' ')),
                                             get_cursor_position() - 1);
        if (location != -1) {
            location++;
        }
        break;
    }

    if (location != -1) {
        return location;
    }
    return get_cursor_position();
}

int VimEditor::calculate_move_word_forward(bool with_symbols) const {
    int pos = get_cursor_position();
    const QString t = adapter->get_text();
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

int VimEditor::calculate_move_to_end_of_word(bool with_symbols) const {
    int pos = get_cursor_position();
    const QString t = adapter->get_text();
    int len = t.length();

    if (pos >= len - 1) {
        return len - 1;
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

int VimEditor::calculate_move_word_backward(bool with_symbols) const {
    int pos = get_cursor_position();
    const QString t = adapter->get_text();

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

void VimEditor::delete_char(bool is_single) {
    int current_pos = get_cursor_position();
    QString current_text = adapter->get_text();
    if (current_pos <= current_text.length()) {
        if (current_pos == current_text.length() || current_text[current_pos] == '\n') {
            if (!is_single){
                // if the character is a newline, we should not delete it when executing a command with num_repeats
                return;
            }
            else{
                // otherwise, try to delete the previous character
                if (current_pos > 0 && current_text[current_pos - 1] != '\n') {
                    current_pos--;
                }

            }
        }

        set_last_deleted_text(current_text.mid(current_pos, 1));
        remove_text(current_pos, 1);
        set_cursor_position(current_pos);
    }
}

void VimEditor::push_history(HistoryState state) {
    if (history.current_index >= 0 && history.current_index < history.states.size() - 1) {
        // If we are in the middle of the history, remove all states after the current index
        history.states.erase(history.states.begin() + history.current_index + 1,
                             history.states.end());
    }

    history.states.push_back(state);

    if (history.states.size() > 100) {
        history.states.pop_front();
    }

    history.current_index = history.states.size() - 1;
}

void VimEditor::undo() {

    if (history.current_index < 0) {
        return;
    }

    history.current_index--;
    const HistoryState &state = history.states[history.current_index + 1];
    adapter->set_text(state.text);
    set_cursor_position(state.cursor_position);
    marks = state.marks;
}

void VimEditor::redo() {
    if (history.current_index >= history.states.size() - 1) {
        return;
    }

    history.current_index++;
    const HistoryState &state = history.states[history.current_index];
    adapter->set_text(state.text);
    set_cursor_position(state.cursor_position);
    marks = state.marks;
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

QString VimEditor::get_word_under_cursor_bounds(int &start, int &end){
    // Handle surrounding word action
    int cursor_pos = get_cursor_position();
    QString current_text = adapter->get_text();

    // If cursor is not on a word character, don't do anything
    if (cursor_pos >= current_text.length() ||
        !(current_text[cursor_pos].isLetterOrNumber() || current_text[cursor_pos] == '_')) {
        return "";
    }

    // Find word boundaries - only include word characters (letters, numbers,
    // underscore)
    start = cursor_pos;
    end = cursor_pos;

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
    return current_text.mid(start, end - start);
}

bool VimEditor::handle_surrounding_motion_action() {
    if (action_waiting_for_motion.has_value()) {
        if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Word) {

            int start, end;
            QString text_under_cursor = get_word_under_cursor_bounds(start, end);
            if (text_under_cursor.size() == 0){
                return true;
            }
            // Only proceed if we found a word
            if (start < end) {
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                    // Store deleted text for paste operation
                    set_last_deleted_text(text_under_cursor);
                    // Delete only the word characters
                    remove_text(start, end - start);
                    set_cursor_position(start);

                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                        set_mode(VimMode::Insert);
                    }
                }
                else if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) {
                    set_last_deleted_text(text_under_cursor);
                }
                else if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Visual) {
                    visual_mode_anchor = start;
                    set_cursor_position(end - 1);
                    set_visual_selection(start, end - start);
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

            int cursor_pos = get_cursor_position();
            QString current_text = adapter->get_text();
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
                    remove_text(start, end - start);
                    set_cursor_position(start);
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                        set_mode(VimMode::Insert);
                    }
                }
                else if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) {
                    set_last_deleted_text(current_text.mid(start, end - start));
                }
                else if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Visual) {
                    visual_mode_anchor = start;
                    set_cursor_position(end - 1);
                    set_visual_selection(start, end - start);
                }
            }
            action_waiting_for_motion = {};
            return true;
        }
    }
    return false;
}

void VimEditor::set_cursor_position(int pos) {
    adapter->set_cursor_position(pos);
}

void VimEditor::set_cursor_position_with_selection(int pos) {
    adapter->set_cursor_position_with_selection(pos, visual_mode_anchor);
}

void VimEditor::set_cursor_position_with_line_selection(int pos) {
    if (!dynamic_cast<QTextEditAdapter*>(adapter)){
        return;
    }
    QTextEditAdapter *text_adapter = static_cast<QTextEditAdapter*>(adapter);
    QTextCursor cursor = text_adapter->text_edit->textCursor();
    int current_cursor_pos = get_cursor_position();
    
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
    const QString &text = adapter->get_text();
    if (selection_end < text.length() && text[selection_end] == '\n') {
        selection_end++;
    }
    
    cursor.setPosition(selection_start, QTextCursor::MoveAnchor);
    cursor.setPosition(selection_end, QTextCursor::KeepAnchor);
    visual_line_selection_begin = selection_start;
    visual_line_selection_end = selection_end;


    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;

    QColor selection_foreground_color = text_adapter->text_edit->palette().color(QPalette::Text);
    QColor selection_background_color = text_adapter->text_edit->palette().color(QPalette::Highlight);
    selection.format.setBackground(selection_background_color);
    selection.format.setForeground(selection_foreground_color);

    adapter->set_extra_selections({selection});
    set_cursor_position(pos);
}

int VimEditor::get_line_start_position(int cursor_pos) {
    const QString text = adapter->get_text();
    int pos = cursor_pos;
    pos = std::min<int>(pos, text.size());

    while (pos > 0 && text[pos - 1] != '\n') {
        pos--;
    }

    return pos;
}

int VimEditor::get_ith_line_start_position(int i) {
    const QString text = adapter->get_text();
    int line_start = 0;
    for (int line = 0; line < i && line_start < text.length(); ++line) {
        line_start = text.indexOf('\n', line_start);
        if (line_start == -1) {
            break;
        }
        line_start++;
    }
    return line_start;
}


int VimEditor::get_line_end_position(int cursor_pos) {
    const QString text = adapter->get_text();
    int pos = cursor_pos;
    int length = text.length();

    while (pos < length && text[pos] != '\n') {
        pos++;
    }

    return pos;
}

int VimEditor::calculate_move_up(int cursor_pos) {
    const QString text = adapter->get_text();

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

    if (!desired_index_in_line.has_value()){
        desired_index_in_line = column_offset;
    }
    else{
        column_offset = desired_index_in_line.value();
    }

    column_offset = std::min<int>(column_offset, prev_line_end - prev_line_start);

    // Try to maintain the same column position, but clamp to line length
    int new_column = std::min(column_offset, prev_line_length-1);

    return prev_line_start + new_column;
}

int VimEditor::calculate_move_down(int cursor_pos) {
    const QString &text = adapter->get_text();

    int current_line_start = get_line_start_position(cursor_pos);
    int column_offset = cursor_pos - current_line_start;

    // Find the end of the current line
    int current_line_end = get_line_end_position(cursor_pos);

    if (!desired_index_in_line.has_value()){
        desired_index_in_line = column_offset;
    }
    else{
        column_offset = desired_index_in_line.value();
    }


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

    column_offset = std::min<int>(column_offset, next_line_end - next_line_start);

    // Try to maintain the same column position, but clamp to line length
    int new_column = std::min(column_offset, next_line_length-1);

    return next_line_start + new_column;
}

int VimEditor::calculate_move_up_on_screen() { return calculate_move_on_screen(-1); }

int VimEditor::calculate_move_down_on_screen() { return calculate_move_on_screen(1); }

int VimEditor::calculate_move_on_screen(int direction) {
    int current_pos = get_cursor_position();

    // Use the document's text layout directly
    QTextDocument *doc = adapter->get_document();

    if (doc == nullptr) {
        return 0;
    }

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

// void VimEditor::resizeEvent(QResizeEvent *event) {
//     // move the command line edit to the bottom
//     command_line_edit->resize(event->size().width(), command_line_edit->height());
//     command_line_edit->move(0, height() - command_line_edit->height());
//     BaseClass::resizeEvent(event);
// }

QColor get_darker_color(QColor color){
    // get a darker version of the color, unless the color is black, in which case return a lighter version
    if (color == Qt::black) {
        return QColor(50, 50, 50); // dark gray
    }
    return color.darker(150);
}

void VimEditor::show_command_line_edit(QString placeholder_text){
    // get editor widget's background color
    QColor background_color = get_darker_color(editor_widget->palette().color(QPalette::Base));
    QColor text_color = editor_widget->palette().color(QPalette::Text);

    command_line_edit->setText("");
    command_line_edit->setPlaceholderText(placeholder_text);
    command_line_edit->show();
    command_line_edit->setFocus();
    command_line_edit->setStyleSheet(QString("QLineEdit { background-color: %1; color: %2; }")
                                     .arg(background_color.name(), text_color.name()));
}

void VimEditor::hide_command_line_edit(){
    command_line_edit->setText("");
    command_line_edit->hide();
    adapter->set_focus();
}

void VimEditor::perform_pending_text_command_with_text(QString text){
    if (pending_text_command.has_value()){
        switch (pending_text_command.value()) {
        case VimLineEditCommand::CommandCommand: {
            handle_text_command(text);
            break;
        }
        case VimLineEditCommand::ReverseSearchCommand:
        case VimLineEditCommand::SearchCommand: {
            if (text.size() == 0) break;
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

void VimEditor::handle_action_waiting_for_motion(int old_pos, int new_pos, int delete_pos_offset){
    if (action_waiting_for_motion.has_value()) {
        QString current_text = adapter->get_text();
        if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Delete ||
            action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Change) {
            // delete from old_pos to new_pos
            if (old_pos < new_pos) {
                set_last_deleted_text(
                    current_text.mid(old_pos, new_pos + delete_pos_offset - old_pos));
                remove_text(old_pos, new_pos + delete_pos_offset - old_pos);
                set_cursor_position(old_pos);
            }
            else if (old_pos > new_pos) {
                set_last_deleted_text(current_text.mid(new_pos, old_pos - new_pos));
                remove_text(new_pos, old_pos - new_pos);
                set_cursor_position(new_pos);
            }

            if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Change) {
                set_mode(VimMode::Insert);
            }
        }

        action_waiting_for_motion = {};
    }
}

void VimEditor::handle_search(bool reverse){
    if (!last_search_state.has_value()) {
        return;
    }

    QString document_text = adapter->get_text().toLower();
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
    
    int current_pos = get_cursor_position();
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
    else if (current_mode == VimMode::Visual) {
        set_cursor_position_with_selection(target_index);
    }
    else if (current_mode == VimMode::VisualLine) {
        set_cursor_position_with_line_selection(target_index);
    }
}

EscapeLineEdit::EscapeLineEdit(QWidget *parent) : QLineEdit(parent) {

}

void EscapeLineEdit::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        emit escapePressed();
        // consume the event
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        emit returnPressed();
        // consume the event
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void VimEditor::set_last_deleted_text(QString text, bool is_line){
    last_deleted_text.text = text;
    last_deleted_text.is_line = is_line;
}

void VimEditor::handle_number_increment_decrement(bool increment) {
    QString current_text = adapter->get_text();
    int cursor_pos = get_cursor_position();

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
            
            insert_text(new_number_str, absolute_start, absolute_end);
            
            // Position cursor at the end of the modified number
            int new_cursor_pos = absolute_start + new_number_str.length() - 1;
            set_cursor_position(new_cursor_pos);
        }
    }
}

void VimEditor::set_mode(VimMode mode){
    if (current_mode == VimMode::Insert){
        last_insert_mode_text = current_insert_mode_text;
    }

    current_mode = mode;

    if (mode == VimMode::Insert){
        current_insert_mode_text = "";
    }

    if (mode == VimMode::Normal){
        current_command_repeat_number = "";
    }

    set_style_for_mode(current_mode);
}

void VimEditor::remove_text(int begin, int num){
    insert_text("", begin, begin + num);
}

void VimEditor::insert_text(QString text, int left_index, int right_index){
    if (right_index == -1){
        right_index  = left_index;
    }

    std::vector<int> marks_to_delete;

    for (auto& [name, mark] : marks){
        if (mark.position >= left_index && (mark.position < right_index)){
            marks_to_delete.push_back(name);
        }
        else if (mark.position >= left_index){
            mark.position -= (right_index - left_index);
            mark.position += text.size();
        }
    }

    for (int mark_to_delete : marks_to_delete){
        auto it = marks.find(mark_to_delete);
        if (it != marks.end()) {
            marks.erase(it);
        }
    }

    QString old_text = adapter->get_text();
    QString new_text = old_text.mid(0, left_index) + text + old_text.mid(right_index);
    adapter->set_text(new_text);
}

void VimEditor::add_event_to_current_macro(QKeyEvent *event){
    if (current_macro.has_value()) {
        current_macro->events.push_back(std::unique_ptr<QKeyEvent>(event->clone()));
    }

}

void VimEditor::set_visual_selection(int begin, int length){
    adapter->set_visual_selection(begin, length);
}

QString VimEditor::get_current_selection(int &begin, int &end){
    return adapter->get_current_selection(begin, end);
}

QString swap_case(QString input){
    QString result;
    result.reserve(input.size());
    for (QChar ch : input) {
        if (ch.isLower()) {
            result.append(ch.toUpper());
        } else if (ch.isUpper()) {
            result.append(ch.toLower());
        } else {
            result.append(ch);
        }
    }
    return result;

}

void VimEditor::handle_text_command(QString text){

    VimLineEdit* line_edit = dynamic_cast<VimLineEdit*>(editor_widget);
    VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
    if (text == "w" || text == "wq" || text == "write"){
        if (line_edit){
            emit line_edit->writeCommand();
        }
        if (text_edit){
            emit text_edit->writeCommand();
        }
    }

    if (text == "q" || text == "quit" || text == "wq"){
        if (line_edit){
            emit line_edit->quitCommand();
        }
        if (text_edit){
            emit text_edit->quitCommand();
        }
    }
    if (text == "q!" || text == "quit!"){
        if (line_edit){
            emit line_edit->forceQuitCommand();
        }
        if (text_edit){
            emit text_edit->forceQuitCommand();
        }
    }

    else {
        qDebug() << "Unknown command: " << text;
    }
}

int VimEditor::get_cursor_position() const {
    return adapter->get_cursor_position();
}

QLineEditAdapter::QLineEditAdapter(QLineEdit *line_edit) : line_edit(line_edit) {
}

QString QLineEditAdapter::get_text() const {
    return line_edit->text();
}

QTextEditAdapter::QTextEditAdapter(QTextEdit* text_edit) : text_edit(text_edit) {

}

QString QTextEditAdapter::get_text() const {
    return text_edit->toPlainText();
}

void QLineEditAdapter::set_cursor_width(int width) {
    line_edit->setStyle(new LineEditStyle(width));
}

void QTextEditAdapter::set_cursor_width(int width) {
    text_edit->setCursorWidth(width);
}

void QTextEditAdapter::set_extra_selections(const QList<QTextEdit::ExtraSelection> &selections) {
    text_edit->setExtraSelections(selections);

}

void QLineEditAdapter::set_extra_selections(const QList<QTextEdit::ExtraSelection> &selections) {
}

void QTextEditAdapter::set_text(QString text) {
    text_edit->setPlainText(text);
}

void QLineEditAdapter::set_text(QString text) {
    line_edit->setText(text);
}

QList<QTextEdit::ExtraSelection> QTextEditAdapter::get_extra_selections() const {
    return text_edit->extraSelections();
}

QList<QTextEdit::ExtraSelection> QLineEditAdapter::get_extra_selections() const {
    return {};
}

void QTextEditAdapter::set_cursor_position(int pos) {
    QTextCursor cursor = text_edit->textCursor();
    cursor.setPosition(pos);
    text_edit->setTextCursor(cursor);
}

void QLineEditAdapter::set_cursor_position(int pos) {
    line_edit->setCursorPosition(pos);
}

void QTextEditAdapter::set_visual_selection(int begin, int length) {
    QTextCursor cursor = text_edit->textCursor();
    cursor.setPosition(begin);
    cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, length);

    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;

    QColor selection_foreground_color = text_edit->palette().color(QPalette::Text);
    QColor selection_background_color = text_edit->palette().color(QPalette::Highlight);
    selection.format.setBackground(selection_background_color);
    selection.format.setForeground(selection_foreground_color);

    text_edit->setExtraSelections({selection});
}

void QLineEditAdapter::set_visual_selection(int begin, int length) {
    line_edit->setSelection(begin, length);
}

void QTextEditAdapter::set_cursor_position_with_selection(int pos, int anchor) {

    QTextCursor cursor = text_edit->textCursor();
    
    int selection_min = std::min<int> (anchor, pos);
    int selection_max = std::max<int> (anchor, pos);

    set_visual_selection(selection_min, selection_max - selection_min + 1);
    cursor.setPosition(pos, QTextCursor::KeepAnchor);

    text_edit->setTextCursor(cursor);
}

void QLineEditAdapter::set_cursor_position_with_selection(int pos, int anchor) {
    int start = std::min<int>(pos, anchor);
    int end = std::max<int>(pos, anchor);
    line_edit->setSelection(start, end - start);
}

int QTextEditAdapter::get_cursor_position() const {
    QTextCursor cursor = text_edit->textCursor();
    return cursor.position();
}

int QLineEditAdapter::get_cursor_position() const {
    return line_edit->cursorPosition();
}

QString QLineEditAdapter::get_current_selection(int &begin, int &end) const {
    begin = line_edit->selectionStart();
    end = line_edit->selectionEnd();
    return line_edit->selectedText();
}

QString QTextEditAdapter::get_current_selection(int &begin, int &end) const {
    QTextCursor cursor = text_edit->extraSelections().isEmpty() ? text_edit->textCursor() : text_edit->extraSelections().first().cursor;
    begin = cursor.selectionStart();
    end = cursor.selectionEnd();
    return cursor.selectedText();
}

QTextDocument* QTextEditAdapter::get_document() {
    return text_edit->document();
}

QTextDocument* QLineEditAdapter::get_document() {
    return nullptr;
}

void QLineEditAdapter::set_focus() {
    line_edit->setFocus();
}

void QTextEditAdapter::set_focus() {
    text_edit->setFocus();
}

QFontMetrics QTextEditAdapter::get_font_metrics(){
    return text_edit->fontMetrics();
}

QFontMetrics QLineEditAdapter::get_font_metrics(){
    return line_edit->fontMetrics();
}

void QLineEditAdapter::key_press_event(QKeyEvent *kevent) {
    QCoreApplication::sendEvent(line_edit, kevent);
}

void QTextEditAdapter::key_press_event(QKeyEvent *kevent) {
    QCoreApplication::sendEvent(text_edit, kevent);
}

VimLineEdit::VimLineEdit(QWidget *parent) : QLineEdit(parent) {
    editor = new VimEditor(this);
}

void VimLineEdit::keyPressEvent(QKeyEvent *event) {
    if ((!vim_enabled) || editor->key_press_event(event)){
        QLineEdit::keyPressEvent(event);
    }
}

void VimLineEdit::resizeEvent(QResizeEvent *event) {
    editor->command_line_edit->resize(event->size().width(), editor->command_line_edit->height());
    editor->command_line_edit->move(0, height() - editor->command_line_edit->height());
    QLineEdit::resizeEvent(event);
}

VimTextEdit::VimTextEdit(QWidget *parent) : QTextEdit(parent) {
    editor = new VimEditor(this);
}

void VimTextEdit::keyPressEvent(QKeyEvent *event) {
    if ((!vim_enabled) || editor->key_press_event(event)){
        QTextEdit::keyPressEvent(event);
    }
}

void VimTextEdit::resizeEvent(QResizeEvent *event) {
    editor->command_line_edit->resize(event->size().width(), editor->command_line_edit->height());
    editor->command_line_edit->move(0, height() - editor->command_line_edit->height());
    QTextEdit::resizeEvent(event);
}


void VimLineEdit::set_vim_enabled(bool enabled){
    vim_enabled = enabled;
}

bool VimLineEdit::get_vim_enabled(){
    return vim_enabled;
}

void VimTextEdit::set_vim_enabled(bool enabled){
    vim_enabled = enabled;
}

bool VimTextEdit::get_vim_enabled(){
    return vim_enabled;
}

void VimEditor::goto_line(int line_number){
    int pos = get_ith_line_start_position(line_number);
    adapter->set_cursor_position(pos);
}

void VimEditor::goto_begin(){
    adapter->set_cursor_position(0);
}

void VimEditor::goto_end(){
    adapter->set_cursor_position(adapter->get_text().length() - 1);
}

void VimTextEdit::focusInEvent(QFocusEvent* event){
    emit focusGained();
    return QTextEdit::focusInEvent(event);
}
void VimTextEdit::focusOutEvent(QFocusEvent* event){
    if (focusWidget() != editor->command_line_edit){
        emit focusLost();
    }
    return QTextEdit::focusOutEvent(event);
}

}
