#include <algorithm>
#include <climits>
#include "VimLineEdit.h"
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QKeySequence>
#include <QScrollBar>
#include <QtCore/qnamespace.h>
#include <QtGui/qtextcursor.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qwidget.h>
#include <iostream>
// #include <QFontMetrics>
#include <QCommonStyle>
#include <QRegularExpression>
#include <QStyle>
#include <QTextBlock>
#include <QTextLayout>
#include <QtWidgets/qtextedit.h>
#include <cstdio>
#include <functional>
#include <utility>
#include <variant>
#include <vector>
#include <QClipboard>
#include <QApplication>
#include <QMenu>

namespace QVimEditor{
const int SEARCH_HIGHLIGHT_PROPERTY_INDEX = 31;


// Translate Vim's default-magic operators to the regular-expression engine
// shared by searching, highlighting, and substitution.
static QRegularExpression vim_regex(const QString &pattern) {
    QString expression;
    bool very_magic = false, literal = false, in_class = false, in_repeat = false;
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::MultilineOption;
    for (int i = 0; i < pattern.size(); ++i) {
        QChar ch = pattern[i];
        if (ch == '\\' && i + 1 < pattern.size()) {
            QChar next = pattern[++i];
            if (!in_class && (next == 'c' || next == 'C')) {
                if (next == 'c') options |= QRegularExpression::CaseInsensitiveOption;
                else options &= ~QRegularExpression::CaseInsensitiveOption;
            } else if (!in_class && (next == 'v' || next == 'm' || next == 'V')) {
                very_magic = next == 'v';
                literal = next == 'V';
            } else if (!in_class && (next == '<' || next == '>')) {
                expression += "\\b";
            } else if (!in_class && !very_magic && QString("()+?|{}").contains(next)) {
                expression += next;
                if (next == '{') in_repeat = true;
            } else if (!in_class && next == '=') {
                expression += '?';
            } else {
                expression += '\\';
                expression += next;
            }
            continue;
        }
        if (literal) {
            expression += QRegularExpression::escape(QString(ch));
            continue;
        }
        if (ch == '[') in_class = true;
        if (!in_class && !very_magic && QString("()+?|{}").contains(ch) && !(ch == '}' && in_repeat)) {
            expression += '\\';
        }
        expression += ch;
        if (ch == ']') in_class = false;
        if (ch == '}') in_repeat = false;
    }
    return QRegularExpression(expression, options);
}

struct PatternMatch { int start; int length; };
static std::vector<PatternMatch> pattern_matches(const QString &text, const QString &pattern) {
    std::vector<PatternMatch> result;
    if (pattern.isEmpty()) return result;
    auto regex = vim_regex(pattern);
    if (!regex.isValid()) return result;
    auto matches = regex.globalMatch(text);
    while (matches.hasNext()) {
        auto match = matches.next();
        result.push_back({static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength())});
    }
    return result;
}

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
        initial_search_scroll_value = std::nullopt;
        QString text = command_line_edit->text();
        if (recording_change && !replaying_change) {
            for (QChar ch : text.mid(1)) {
                change_keys.emplace_back(new QKeyEvent(QEvent::KeyPress, ch.toUpper().unicode(), Qt::NoModifier, QString(ch)));
            }
            change_keys.emplace_back(new QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier));
        }
        perform_pending_text_command_with_text(text.mid(1));
        hide_command_line_edit(false);
        if (recording_change && !replaying_change && current_mode == VimMode::Normal) {
            if (adapter->get_text() != change_start_text) last_change_keys = std::move(change_keys);
            recording_change = false;
            change_keys.clear();
        }

    });

    QObject::connect(command_line_edit, &EscapeLineEdit::escapePressed, [&](){
        hide_command_line_edit(true);
    });

    QObject::connect(command_line_edit, &EscapeLineEdit::textChanged, [&](const QString& new_text){
        if (new_text.size() == 0){
            // close the command line if the user deletes all text (e.g. by pressing backspace)
            hide_command_line_edit(true);
        }
        else{
            if (pending_text_command.has_value() && (pending_text_command.value() == VimLineEditCommand::SearchCommand || pending_text_command.value() == VimLineEditCommand::ReverseSearchCommand)){
                highlight_matches(new_text.mid(1));
                scroll_to_first_match(new_text);
            }
        }
    });

    set_style_for_mode(current_mode);
}

void VimEditor::highlight_matches(QString pattern){
    QTextEditAdapter *text_adapter = dynamic_cast<QTextEditAdapter*>(adapter);
    if (text_adapter == nullptr) return;

    QString current_text = adapter->get_text();

    QList<QTextEdit::ExtraSelection> selections;


    QTextCharFormat search_highlight_format;
    QColor highlight_color = editor_widget->palette().color(QPalette::HighlightedText);
    QColor inverted_highlight_color = QColor(255 - highlight_color.red(), 255 - highlight_color.green(), 255 - highlight_color.blue());

    search_highlight_format.setBackground(highlight_color);
    search_highlight_format.setForeground(inverted_highlight_color);

    // identifier to distinguish search highlights from other highlights
    search_highlight_format.setProperty(QTextFormat::UserProperty, SEARCH_HIGHLIGHT_PROPERTY_INDEX);

    // search_highlight_format.setObjectType(int atype)

    for (const auto &match : pattern_matches(current_text, pattern)) {
        int selection_begin = match.start;
        int selection_end = match.start + match.length;

        QTextCursor cursor = text_adapter->text_edit->textCursor();

        cursor.setPosition(selection_begin, QTextCursor::MoveAnchor);
        cursor.setPosition(selection_end, QTextCursor::KeepAnchor);

        QTextEdit::ExtraSelection selection;
        selection.cursor = cursor;
        selection.format = search_highlight_format;

        QColor selection_foreground_color = text_adapter->text_edit->palette().color(QPalette::Text);
        QColor selection_background_color = text_adapter->text_edit->palette().color(QPalette::Highlight);

        // selection.format.setBackground(selection_background_color);
        // selection.format.setForeground(selection_foreground_color);

        selections.append(selection);


    }

    QList<QTextEdit::ExtraSelection> old_extra_selections = adapter->get_extra_selections();
    // QList<QTextEdit::ExtraSelection> old_selections_to_keep;
    for (const QTextEdit::ExtraSelection& old_selection : old_extra_selections) {
        // Restore the old selection format
        // selection.format = search_highlight_format;
        if (old_selection.format.property(QTextFormat::UserProperty).toInt() != SEARCH_HIGHLIGHT_PROPERTY_INDEX){
            // old_selections_to_keep.append(old_selection);
            selections.append(old_selection);

        }
    }

    adapter->set_extra_selections(selections);

}

void VimEditor::scroll_to_first_match(const QString &new_text) {
    QTextEditAdapter *text_adapter = dynamic_cast<QTextEditAdapter*>(adapter);
    if (text_adapter == nullptr || text_adapter->text_edit == nullptr) {
        return;
    }

    QString pattern = new_text.mid(1);
    if (pattern.isEmpty()) {
        if (initial_search_scroll_value.has_value()) {
            text_adapter->text_edit->verticalScrollBar()->setValue(initial_search_scroll_value.value());
        }
        return;
    }

    QString current_text = text_adapter->get_text();
    int current_pos = get_cursor_position();
    int target_index = -1;

    bool is_reverse = (pending_text_command.has_value() && pending_text_command.value() == VimLineEditCommand::ReverseSearchCommand) || new_text.startsWith('?');

    auto matches = pattern_matches(current_text, pattern);
    if (!matches.empty()) {
        target_index = is_reverse ? matches.back().start : matches.front().start;
        if (is_reverse) {
            for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
                if (it->start < current_pos) { target_index = it->start; break; }
            }
        } else {
            for (const auto &match : matches) {
                if (match.start > current_pos) { target_index = match.start; break; }
            }
        }
    }

    if (target_index != -1) {
        QTextCursor cursor(text_adapter->text_edit->document());
        cursor.setPosition(target_index);
        QRect cursor_rect = text_adapter->text_edit->cursorRect(cursor);
        QRect viewport_rect = text_adapter->text_edit->viewport()->rect();
        if (!viewport_rect.contains(cursor_rect)) {
            int current_scroll = text_adapter->text_edit->verticalScrollBar()->value();
            int target_scroll = current_scroll + cursor_rect.top() - viewport_rect.height() / 2;
            text_adapter->text_edit->verticalScrollBar()->setValue(target_scroll);
        }
    } else {
        if (initial_search_scroll_value.has_value()) {
            text_adapter->text_edit->verticalScrollBar()->setValue(initial_search_scroll_value.value());
        }
    }
}

bool VimEditor::key_press_event(QKeyEvent *event, bool &should_propagate) {
    if (replaying_change) return process_key_press_event(event, should_propagate);
    bool was_normal = current_mode == VimMode::Normal;
    if (!recording_change && was_normal) {
        recording_change = true;
        change_start_text = adapter->get_text();
        change_keys.clear();
    }
    if (recording_change) change_keys.emplace_back(event->clone());
    bool result = process_key_press_event(event, should_propagate);
    if (pending_text_command) {
        // Preserve an operator's keys; its search motion is recorded on submission.
        if (!action_waiting_for_motion) {
            recording_change = false;
            change_keys.clear();
        }
    } else if (current_mode == VimMode::Normal && !action_waiting_for_motion &&
               !pending_symbol_command && !current_node && current_command_repeat_number.isEmpty() &&
               !current_paste_register) {
        auto modifiers = KeyboardModifierState::from_qt_modifiers(event->modifiers());
        bool history_command = was_normal && (event->text() == "u" ||
            (event->key() == Qt::Key_R && modifiers.control));
        bool dot_command = was_normal && event->text() == ".";
        if (recording_change && !history_command && !dot_command && adapter->get_text() != change_start_text) {
            last_change_keys = std::move(change_keys);
        }
        recording_change = false;
        change_keys.clear();
    }
    return result;
}

bool VimEditor::process_key_press_event(QKeyEvent* event, bool& should_propagate) {

    should_propagate = false;
    
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
            action_waiting_for_motion = {};
            pending_symbol_command = {};
            current_node = nullptr;
            current_command_repeat_number.clear();
            pending_operator_count = 1;
            current_paste_register = {};
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

    if (current_mode == VimMode::Normal || current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock) {
        // we don't want to handle when we only press a modifier (e.g. Shift, Ctrl, etc.)
        int event_key = event->key();
        bool is_keypress_a_modifier = event_key == Qt::Key_Shift || event_key == Qt::Key_Control ||
                                      event_key == Qt::Key_Alt || event_key == Qt::Key_Meta;

        bool is_key_a_number = event->text().size() == 1 &&
                               event->text().at(0).isDigit() &&
                               !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier));
        if (is_key_a_number && !pending_symbol_command &&
            (event->text() != "0" || !current_command_repeat_number.isEmpty())) {
            current_command_repeat_number.append(event->text());
            return false;
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

            if ((action_waiting_for_motion->kind == ActionWaitingForMotionKind::Indent && event->text() == ">") ||
                (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Unindent && event->text() == "<") ||
                (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Reindent && event->text() == "=") ||
                (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Lowercase && event->text() == "u") ||
                (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Uppercase && event->text() == "U")) {
                handle_command(VimLineEditCommand::TransformCurrentLine);
                return false;
            }

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
                case Qt::Key_ParenRight:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Parentheses;
                    break;
                case Qt::Key_BraceLeft:
                case Qt::Key_BraceRight:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Braces;
                    break;
                case Qt::Key_BracketLeft:
                case Qt::Key_BracketRight:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Brackets;
                    break;
                case Qt::Key_Less:
                case Qt::Key_Greater:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::AngleBrackets;
                    break;
                case Qt::Key_QuoteLeft:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Backticks;
                    break;
                case Qt::Key_Apostrophe:
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
                    return false;
                }
            }
        }

        if (current_node != nullptr || is_key_a_number) {
            return false;
        }

        bool has_modifier_shortcut = event->modifiers().testFlag(Qt::ControlModifier) ||
                                     event->modifiers().testFlag(Qt::MetaModifier) ||
                                     event->modifiers().testFlag(Qt::AltModifier);
        if (has_modifier_shortcut) {
            should_propagate = true;
            return false;
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

    if (current_mode == VimMode::Insert && replacing) {
        int cursor = get_cursor_position();
        if (event->key() == Qt::Key_Backspace && !replace_steps.empty() &&
            cursor == replace_steps.back().cursor_position + 1) {
            auto state = replace_steps.back();
            replace_steps.pop_back();
            adapter->set_text(state.text);
            set_cursor_position(state.cursor_position);
            marks = state.marks;
            if (!current_insert_mode_text.isEmpty()) current_insert_mode_text.chop(1);
            return false;
        }
        if (!event->text().isEmpty() && event->text().at(0).isPrint() &&
            !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            replace_steps.push_back({adapter->get_text(), cursor, marks});
            insert_text(event->text(), cursor, std::min(cursor + static_cast<int>(event->text().size()), get_line_end_position(cursor)));
            set_cursor_position(cursor + event->text().size());
            current_insert_mode_text += event->text();
            return false;
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

void VimEditor::update_cursor_style() {
    set_style_for_mode(current_mode);
}

void VimEditor::set_style_for_mode(VimMode mode) {
    bool is_vim_enabled = true;
    if (auto* line_edit = dynamic_cast<VimLineEdit*>(editor_widget)) {
        is_vim_enabled = line_edit->get_vim_enabled();
    } else if (auto* text_edit = dynamic_cast<VimTextEdit*>(editor_widget)) {
        is_vim_enabled = text_edit->get_vim_enabled();
    }

    if (!is_vim_enabled) {
        mode = VimMode::Insert;
    }

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
    else if (mode == VimMode::Visual || mode == VimMode::VisualLine || mode == VimMode::VisualBlock) {
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
    KeyboardModifierState COMMAND = KeyboardModifierState{false, false, true, false};

    // KeyboardModifierState SHIFT = KeyboardModifierState{true, false, false, false};
    // KeyboardModifierState NOMOD = KeyboardModifierState{false, false, false, false};

    std::vector<KeyBinding> key_bindings = {
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"g", {}}},
                   VimLineEditCommand::GotoBegin},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"v", {}}},
                   VimLineEditCommand::ReselectVisualMode},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"f", {}}},
                   VimLineEditCommand::OpenFile},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"d", {}}},
                   VimLineEditCommand::OpenConfig},
        KeyBinding{{KeyChord{"G", {}}}, VimLineEditCommand::GotoEnd},
        KeyBinding{{KeyChord{Qt::Key_Escape, {}}}, VimLineEditCommand::EnterNormalMode},
        KeyBinding{{KeyChord{"i", {}}}, VimLineEditCommand::EnterInsertMode},
        KeyBinding{{KeyChord{"a", {}}}, VimLineEditCommand::EnterInsertModeAfter},
        KeyBinding{{KeyChord{"I", {}}}, VimLineEditCommand::EnterInsertModeBeginLine},
        KeyBinding{{KeyChord{"A", {}}}, VimLineEditCommand::EnterInsertModeEndLine},
        KeyBinding{{KeyChord{"v", {}}}, VimLineEditCommand::EnterVisualMode},
        KeyBinding{{KeyChord{"V", {}}}, VimLineEditCommand::EnterVisualLineMode},
        KeyBinding{{KeyChord{Qt::Key_V, CONTROL}}, VimLineEditCommand::EnterVisualBlockMode},
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
        KeyBinding{{KeyChord{"r", {}}}, VimLineEditCommand::ReplaceChar},
        KeyBinding{{KeyChord{"R", {}}}, VimLineEditCommand::EnterReplaceMode},
        KeyBinding{{KeyChord{">", {}}}, VimLineEditCommand::Indent},
        KeyBinding{{KeyChord{"<", {}}}, VimLineEditCommand::Unindent},
        KeyBinding{{KeyChord{"=", {}}}, VimLineEditCommand::Reindent},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"u", {}}}, VimLineEditCommand::Lowercase},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"U", {}}}, VimLineEditCommand::Uppercase},
        KeyBinding{{KeyChord{"J", {}}}, VimLineEditCommand::JoinLines},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"J", {}}}, VimLineEditCommand::JoinLinesWithoutSpace},
        KeyBinding{{KeyChord{"D", {}}}, VimLineEditCommand::DeleteToEndOfLine},
        KeyBinding{{KeyChord{"C", {}}}, VimLineEditCommand::ChangeToEndOfLine},
        KeyBinding{{KeyChord{"d", {}}}, VimLineEditCommand::Delete},
        KeyBinding{{KeyChord{"c", {}}}, VimLineEditCommand::Change},
        KeyBinding{{KeyChord{"y", {}}}, VimLineEditCommand::Yank},
        KeyBinding{{KeyChord{"p", {}}}, VimLineEditCommand::PasteForward},
        KeyBinding{{KeyChord{"P", {}}}, VimLineEditCommand::PasteBackward},
        KeyBinding{{KeyChord{"u", {}}}, VimLineEditCommand::Undo},
        KeyBinding{{KeyChord{".", {}}}, VimLineEditCommand::RepeatLastChange},
        KeyBinding{{KeyChord{Qt::Key_R, CONTROL}}, VimLineEditCommand::Redo},
        KeyBinding{{KeyChord{"o", {}}}, VimLineEditCommand::InsertLineBelow},
        KeyBinding{{KeyChord{"O", {}}}, VimLineEditCommand::InsertLineAbove},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"k", {}}}, VimLineEditCommand::MoveUpOnScreen},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"j", {}}}, VimLineEditCommand::MoveDownOnScreen},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{Qt::Key_A, CONTROL}}, VimLineEditCommand::ProgressiveIncrement},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{Qt::Key_X, CONTROL}}, VimLineEditCommand::ProgressiveDecrement},
        KeyBinding{{KeyChord{"0", {}}}, VimLineEditCommand::MoveToBeginningOfLine},
        KeyBinding{{KeyChord{"_", {}}}, VimLineEditCommand::MoveToFirstNonblank},
        KeyBinding{{KeyChord{"^", {}}}, VimLineEditCommand::MoveToFirstNonblank},
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
        KeyBinding{{KeyChord{"\"", {}}}, VimLineEditCommand::SelectPasteRegister},
        KeyBinding{{KeyChord{"@", {}}}, VimLineEditCommand::RepeatMacro},
        KeyBinding{{KeyChord{"*", {}}}, VimLineEditCommand::SearchTextUnderCursor},
        KeyBinding{{KeyChord{"#", {}}}, VimLineEditCommand::SearchTextUnderCursorBackward},
        KeyBinding{{KeyChord{"%", {}}}, VimLineEditCommand::GotoMatchingBracket},
        KeyBinding{{KeyChord{"~", {}}}, VimLineEditCommand::SwapCaseCharacterUnderCursor},
        KeyBinding{{KeyChord{"}", {}}}, VimLineEditCommand::MoveToTheNextParagraph},
        KeyBinding{{KeyChord{"{", {}}}, VimLineEditCommand::MoveToThePreviousParagraph},
        KeyBinding{{KeyChord{"Z", {}}, KeyChord{"Z", {}}},
                   VimLineEditCommand::SaveAndQuit},
        KeyBinding{{KeyChord{"z", {}}, KeyChord{"z", {}}},
                   VimLineEditCommand::CenterOnCursor},
        KeyBinding{{KeyChord{"K", {}}}, VimLineEditCommand::ViewDocumentation},
        KeyBinding{{KeyChord{Qt::Key_I, CONTROL}}, VimLineEditCommand::JumpForward},
        KeyBinding{{KeyChord{Qt::Key_O, CONTROL}}, VimLineEditCommand::JumpBackward},
#ifdef Q_OS_MACOS
        KeyBinding{{KeyChord{Qt::Key_Slash, COMMAND}}, VimLineEditCommand::ToggleComment},
#endif
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
        KeyBinding{{KeyChord{Qt::Key_X, CONTROL}, KeyChord{Qt::Key_N, CONTROL}}, VimLineEditCommand::AutoComplete}
    };

    for (const auto &binding : insert_mode_keybindings) {
        insert_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

}

std::optional<VimLineEditCommand> VimEditor::handle_key_event(QString event_text, int key,
                                                                Qt::KeyboardModifiers modifiers) {

    InputTreeNode* current_mode_root = &normal_mode_input_tree;
    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock) {
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

QString get_initial_command_text(VimLineEditCommand cmd){
    switch (cmd) {
        case VimLineEditCommand::CommandCommand:
            return ":";
        case VimLineEditCommand::SearchCommand:
            return "/";
        case VimLineEditCommand::ReverseSearchCommand:
            return "?";
        default:
            return "";
    }
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
    case VimLineEditCommand::EnterVisualBlockMode:
        return "EnterVisualBlockMode";
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
    case VimLineEditCommand::ProgressiveIncrement:
        return "ProgressiveIncrement";
    case VimLineEditCommand::ProgressiveDecrement:
        return "ProgressiveDecrement";
    case VimLineEditCommand::InsertLastInsertModeText:
        return "InsertLastInsertModeText";
    case VimLineEditCommand::SetMark:
        return "SetMark";
    case VimLineEditCommand::GotoMark:
        return "GotoMark";
    case VimLineEditCommand::RecordMacro:
        return "RecordMacro";
    case VimLineEditCommand::SelectPasteRegister:
        return "SelectPasteRegister";
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
    case VimLineEditCommand::SaveAndQuit:
        return "SaveAndQuit";
    case VimLineEditCommand::CenterOnCursor:
        return "CenterOnCursor";
    case VimLineEditCommand::AutoComplete:
        return "AutoComplete";
    case VimLineEditCommand::ViewDocumentation:
        return "ViewDocumentation";
    case VimLineEditCommand::OpenFile:
        return "OpenFile";
    case VimLineEditCommand::OpenConfig:
        return "OpenConfig";
    case VimLineEditCommand::JumpForward:
        return "JumpForward";
    case VimLineEditCommand::JumpBackward:
        return "JumpBackward";
    case VimLineEditCommand::ToggleComment:
        return "ToggleComment";
    case VimLineEditCommand::ReselectVisualMode:
        return "ReselectVisualMode";
    default:
        return "Unknown";
    }
}

bool is_jump_command(VimLineEditCommand cmd) {
    switch (cmd) {
    case VimLineEditCommand::GotoBegin:
    case VimLineEditCommand::GotoEnd:
    case VimLineEditCommand::GotoMatchingBracket:
    case VimLineEditCommand::MoveToTheNextParagraph:
    case VimLineEditCommand::MoveToThePreviousParagraph:
    case VimLineEditCommand::GotoMark:
        return true;
    default:
        return false;
    }
}

bool VimEditor::requires_symbol(VimLineEditCommand cmd) {
    switch (cmd) {
    case VimLineEditCommand::ReplaceChar:
    case VimLineEditCommand::FindForward:
    case VimLineEditCommand::FindBackward:
    case VimLineEditCommand::FindForwardTo:
    case VimLineEditCommand::FindBackwardTo:
    case VimLineEditCommand::SetMark:
    case VimLineEditCommand::GotoMark:
    case VimLineEditCommand::RepeatMacro:
    case VimLineEditCommand::SelectPasteRegister:
        return true;
    case VimLineEditCommand::RecordMacro:
        return !current_macro.has_value();
    default:
        return false;
    }
}

void VimEditor::handle_command(VimLineEditCommand cmd, std::optional<char> symbol) {

    // Vim mutations bypass QTextEdit's native read-only handling.
    auto* text_edit = qobject_cast<QTextEdit*>(editor_widget);
    if (text_edit && text_edit->isReadOnly()) {
        switch (cmd) {
        case VimLineEditCommand::EnterInsertMode:
        case VimLineEditCommand::EnterInsertModeAfter:
        case VimLineEditCommand::EnterInsertModeBegin:
        case VimLineEditCommand::EnterInsertModeEnd:
        case VimLineEditCommand::EnterInsertModeBeginLine:
        case VimLineEditCommand::EnterInsertModeEndLine:
        case VimLineEditCommand::Indent:
        case VimLineEditCommand::Unindent:
        case VimLineEditCommand::Reindent:
        case VimLineEditCommand::Lowercase:
        case VimLineEditCommand::Uppercase:
        case VimLineEditCommand::TransformCurrentLine:
        case VimLineEditCommand::JoinLines:
        case VimLineEditCommand::JoinLinesWithoutSpace:
        case VimLineEditCommand::EnterReplaceMode:
        case VimLineEditCommand::ReplaceChar:
        case VimLineEditCommand::DeleteChar:
        case VimLineEditCommand::Delete:
        case VimLineEditCommand::Change:
        case VimLineEditCommand::DeleteToEndOfLine:
        case VimLineEditCommand::ChangeToEndOfLine:
        case VimLineEditCommand::PasteForward:
        case VimLineEditCommand::PasteBackward:
        case VimLineEditCommand::RepeatLastChange:
        case VimLineEditCommand::Undo:
        case VimLineEditCommand::Redo:
        case VimLineEditCommand::InsertLineBelow:
        case VimLineEditCommand::InsertLineAbove:
        case VimLineEditCommand::DeleteCharAndEnterInsertMode:
        case VimLineEditCommand::DeleteCurrentLine:
        case VimLineEditCommand::ChangeCurrentLine:
        case VimLineEditCommand::DeletePreviousWord:
        case VimLineEditCommand::IncrementNextNumberOnCurrentLine:
        case VimLineEditCommand::DecrementNextNumberOnCurrentLine:
        case VimLineEditCommand::ProgressiveIncrement:
        case VimLineEditCommand::ProgressiveDecrement:
        case VimLineEditCommand::InsertLastInsertModeText:
        case VimLineEditCommand::Uppercasify:
        case VimLineEditCommand::SwapCaseCharacterUnderCursor:
        case VimLineEditCommand::SwapCaseSelection:
        case VimLineEditCommand::ToggleComment:
        case VimLineEditCommand::AutoComplete:
            current_command_repeat_number.clear();
            return;
        default:
            break;
        }
    }

    const bool has_explicit_count = !current_command_repeat_number.isEmpty();
    bool valid_count = false;
    int num_repeats = current_command_repeat_number.toInt(&valid_count);
    if (!valid_count || num_repeats < 1) num_repeats = 1;
    current_command_repeat_number.clear();
    // Clamp arithmetic to the largest count representable by this editor.
    num_repeats = static_cast<int>(std::min<qint64>(
        static_cast<qint64>(num_repeats) * pending_operator_count, INT_MAX));
    pending_operator_count = 1;

    // Ask for more text before taking snapshots; prepending can shift positions.
    if (auto* vim = dynamic_cast<VimTextEdit*>(editor_widget)) {
        const int position = get_cursor_position();
        std::optional<bool> boundary;
        switch (cmd) {
        case VimLineEditCommand::MoveUp:
        case VimLineEditCommand::MoveUpOnScreen:
            if (get_line_start_position(position) == 0) {
                boundary = false;
            }
            break;
        case VimLineEditCommand::MoveDown:
        case VimLineEditCommand::MoveDownOnScreen:
            if (get_line_end_position(position) == adapter->get_text().size()) {
                boundary = true;
            }
            break;
        case VimLineEditCommand::MoveWordForward:
        case VimLineEditCommand::MoveWordForwardWithSymbols:
            if (calculate_move_word_forward(cmd == VimLineEditCommand::MoveWordForwardWithSymbols) == position) {
                boundary = true;
            }
            break;
        case VimLineEditCommand::MoveWordBackward:
        case VimLineEditCommand::MoveWordBackwardWithSymbols:
            if (calculate_move_word_backward(cmd == VimLineEditCommand::MoveWordBackwardWithSymbols) == position) {
                boundary = false;
            }
            break;
        case VimLineEditCommand::MoveToEndOfWord:
        case VimLineEditCommand::MoveToEndOfWordWithSymbols:
            if (position >= adapter->get_text().size() - 1) {
                boundary = true;
            }
            break;
        default:
            break;
        }
        if (boundary.has_value()) {
            emit vim->textBoundaryRequested(*boundary);
        }
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

    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock) {
        last_visual_mode = current_mode;
        last_visual_anchor = visual_mode_anchor;
        last_visual_pos = get_cursor_position();
    }

    if ((cmd == VimLineEditCommand::PasteForward || cmd == VimLineEditCommand::PasteBackward) &&
        (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock)) {
        auto contents = get_last_deleted_text(current_paste_register);
        if (!contents) return;
        push_history(current_state);
        int begin, end;
        QString replaced = get_current_selection(begin, end);
        bool linewise = current_mode == VimMode::VisualLine;
        QString replacement = contents->text.repeated(num_repeats);
        if (current_mode == VimMode::VisualBlock) {
            auto bounds = get_visual_block_bounds();
            for (int line = bounds.max_line; line >= bounds.min_line; --line) {
                int start = bounds.line_starts[line];
                int finish = get_line_end_position(start);
                int left = std::min(start + bounds.min_col, finish);
                int right = std::min(start + bounds.max_col + 1, finish);
                insert_text(replacement, left, right);
            }
            begin = bounds.line_starts[bounds.min_line] + bounds.min_col;
        } else {
            if (linewise) {
                begin = visual_line_selection_begin;
                end = visual_line_selection_end;
                replaced = current_state.text.mid(begin, end - begin);
                if (replaced.endsWith('\n')) replaced.chop(1);
                if (end < current_state.text.size()) replacement += "\n";
            }
            insert_text(replacement, begin, end);
        }
        if (cmd == VimLineEditCommand::PasteForward) set_last_deleted_text(replaced, {}, linewise);
        action_waiting_for_motion = {};
        current_paste_register = {};
        set_mode(VimMode::Normal);
        adapter->set_extra_selections({});
        visual_line_selection_begin = visual_line_selection_end = -1;
        set_cursor_position(linewise ? begin : begin + std::max(0, static_cast<int>(replacement.size()) - 1));
        return;
    }

    switch (cmd) {
    case VimLineEditCommand::EnterInsertMode:
        insert_repeat_count = num_repeats;
        push_history(current_state);
        set_mode(VimMode::Insert);
        break;
    case VimLineEditCommand::EnterInsertModeAfter:{
        insert_repeat_count = num_repeats;
        push_history(current_state);
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
        push_history(current_state);
        set_mode(VimMode::Insert);
        new_pos = 0;
        break;
    case VimLineEditCommand::EnterInsertModeEnd:
        push_history(current_state);
        set_mode(VimMode::Insert);
        new_pos = current_state.text.length();
        break;
    case VimLineEditCommand::Uppercasify: {
        if (current_mode == VimMode::VisualBlock) {
            apply_transform_to_visual_block([](const QString &slice) { return slice.toUpper(); });
            break;
        }
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
        if (current_mode == VimMode::VisualBlock) {
            apply_transform_to_visual_block([this](const QString &slice) { return swap_case(slice); });
            break;
        }
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
        insert_repeat_count = num_repeats;
        push_history(current_state);
        if (current_mode == VimMode::VisualBlock) {
            enter_visual_block_insert_mode(false);
            break;
        }
        set_mode(VimMode::Insert);
        new_pos = get_line_start_position(get_cursor_position());
        while (new_pos < get_line_end_position(old_pos) &&
               (current_state.text[new_pos] == ' ' || current_state.text[new_pos] == '\t')) ++new_pos;
        break;
    case VimLineEditCommand::EnterInsertModeEndLine:
        insert_repeat_count = num_repeats;
        push_history(current_state);
        if (current_mode == VimMode::VisualBlock) {
            enter_visual_block_insert_mode(true);
            break;
        }
        set_mode(VimMode::Insert);
        new_pos = get_line_end_position(get_cursor_position());
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
    case VimLineEditCommand::SelectPasteRegister:
        current_paste_register = symbol.value();
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
                    bool should_propagate = false;
                    if (key_press_event(clone, should_propagate)){
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
            new_search_state.query = "\\<" + QRegularExpression::escape(word_under_cursor) + "\\>";
            last_search_state = new_search_state;
            handle_search();
        }
        break;
    }
    case VimLineEditCommand::GotoBegin:
        if (num_repeats <= 1){
            new_pos = get_line_start_position(0);
        }
        else {
            new_pos = get_ith_line_start_position(num_repeats - 1);
        }
        break;
    case VimLineEditCommand::GotoEnd:
        if (!has_explicit_count) {
            new_pos = get_line_start_position(current_state.text.size());
        } else {
            new_pos = get_ith_line_start_position(num_repeats - 1);
        }
        while (new_pos < get_line_end_position(new_pos) &&
               (current_state.text[new_pos] == ' ' || current_state.text[new_pos] == '\t')) ++new_pos;
        break;
    case VimLineEditCommand::MoveToFirstNonblank:
        new_pos = get_line_start_position(old_pos);
        while (new_pos < get_line_end_position(old_pos) &&
               (current_state.text[new_pos] == ' ' || current_state.text[new_pos] == '\t')) ++new_pos;
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
            new_pos = next_paragraph_start + 1;
        }
        break;
    }
    case VimLineEditCommand::MoveToThePreviousParagraph:{
        int previous_paragraph_end = current_state.text.mid(0, get_cursor_position()-1).lastIndexOf("\n\n");
        if (previous_paragraph_end == -1 || previous_paragraph_end > get_cursor_position()) {
            new_pos = 0;
        }
        else {
            new_pos = previous_paragraph_end + 1;
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
    case VimLineEditCommand::SaveAndQuit:{
        emit_save();
        emit_quit();
        break;
    }
    case VimLineEditCommand::CenterOnCursor:{
        center_on_cursor();
        break;
    }
    case VimLineEditCommand::AutoComplete:{
        VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
        if (text_edit){
            QString previous_word = get_previous_word();
            QStringList suggestions = text_edit->get_autocomplete_suggestions(previous_word);
            text_edit->show_autocomplete_suggestions(suggestions);
        }
        break;
    }
    case VimLineEditCommand::ViewDocumentation:{
        VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
        if (text_edit){
            QString previous_word = get_word_under_cursor();
            text_edit->show_documentation(previous_word);
        }
        break;
    }
    case VimLineEditCommand::OpenFile:{
        emit_open_file();
        break;
    }
    case VimLineEditCommand::OpenConfig:{
        emit_open_config();
        break;
    }
    case VimLineEditCommand::EnterNormalMode: {
        if (visual_line_selection_begin != -1){
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
            visual_line_selection_begin = -1;
            visual_line_selection_end = -1;
        }
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualBlock){
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
        }

        if (current_mode == VimMode::Insert && insert_repeat_count > 1) {
            QString repeated = current_insert_mode_text.repeated(insert_repeat_count - 1);
            int cursor = get_cursor_position();
            insert_text(repeated, cursor);
            set_cursor_position(cursor + repeated.size());
        }
        insert_repeat_count = 1;

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

            // when we insert some text in visual block mode and press escape
            // the cursor should be on the last character inserted
            if (visual_block_insert_state.has_value()){
                new_pos--;
            }

            replicate_visual_block_insert_text();
            bool is_at_beginning_of_line = (new_pos > 0 && adapter->get_text()[new_pos - 1] == '\n');
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
        if (current_mode == VimMode::Visual) {
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            break;
        }
        set_mode(VimMode::Visual);
        visual_mode_anchor = get_cursor_position();
        set_visual_selection(visual_mode_anchor, 1);
        action_waiting_for_motion = {ActionWaitingForMotionKind::Visual, SurroundingScope::None, SurroundingKind::None};
        break;
    }
    case VimLineEditCommand::EnterVisualLineMode: {
        if (current_mode == VimMode::VisualLine) {
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            visual_line_selection_begin = -1;
            visual_line_selection_end = -1;
            break;
        }
        set_mode(VimMode::VisualLine);
        visual_mode_anchor = get_cursor_position();
        set_cursor_position_with_line_selection(get_cursor_position());
        break;
    }
    case VimLineEditCommand::EnterVisualBlockMode: {
        if (current_mode == VimMode::VisualBlock) {
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            break;
        }
        set_mode(VimMode::VisualBlock);
        visual_mode_anchor = get_cursor_position();
        set_cursor_position_with_block_selection(get_cursor_position());
        action_waiting_for_motion = {ActionWaitingForMotionKind::Visual, SurroundingScope::None, SurroundingKind::None};
        break;
    }
    case VimLineEditCommand::MoveLeft:
        new_pos = std::max(get_line_start_position(old_pos), old_pos - num_repeats);
        break;
    case VimLineEditCommand::MoveRight: {
        int end = get_line_end_position(old_pos);
        // An exclusive operator motion may reach just past the final character.
        if (!action_waiting_for_motion || action_waiting_for_motion->kind == ActionWaitingForMotionKind::Visual) {
            end = std::max(get_line_start_position(old_pos), end - 1);
        }
        new_pos = old_pos + std::min(num_repeats, std::max(0, end - old_pos));
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
    case VimLineEditCommand::MoveWordForwardWithSymbols: {
        bool with_symbols = cmd == VimLineEditCommand::MoveWordForwardWithSymbols;
        bool change_word = action_waiting_for_motion &&
                           action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change &&
                           old_pos < current_state.text.size() && !current_state.text[old_pos].isSpace();
        new_pos = old_pos;
        for (int i = 0; i < num_repeats - (change_word ? 1 : 0); ++i) {
            int next = calculate_move_word_forward(with_symbols, new_pos);
            if (i == num_repeats - 1 && action_waiting_for_motion &&
                action_waiting_for_motion->kind != ActionWaitingForMotionKind::Visual &&
                new_pos < current_state.text.size() && !current_state.text[new_pos].isSpace()) {
                next = std::min(next, get_line_end_position(new_pos));
            }
            if (next == new_pos) break;
            new_pos = next;
        }
        if (change_word) {
            new_pos = std::min(new_pos, static_cast<int>(current_state.text.size()) - 1);
            const QString &text = current_state.text;
            bool word = text[new_pos].isLetterOrNumber() || text[new_pos] == '_';
            while (new_pos + 1 < text.size() && !text[new_pos + 1].isSpace() &&
                   (with_symbols || (text[new_pos + 1].isLetterOrNumber() || text[new_pos + 1] == '_') == word)) {
                ++new_pos;
            }
            delete_pos_offset = 1;
        }
        break;
    }
    case VimLineEditCommand::MoveToEndOfWord:
        delete_pos_offset = 1;
        new_pos = old_pos;
        for (int i = 0; i < num_repeats; ++i) {
            int next = calculate_move_to_end_of_word(false, new_pos);
            if (next == new_pos) break;
            new_pos = next;
        }
        break;
    case VimLineEditCommand::MoveToEndOfWordWithSymbols:
        delete_pos_offset = 1;
        new_pos = old_pos;
        for (int i = 0; i < num_repeats; ++i) {
            int next = calculate_move_to_end_of_word(true, new_pos);
            if (next == new_pos) break;
            new_pos = next;
        }
        break;
    case VimLineEditCommand::RepeatLastChange: {
        if (last_change_keys.empty() || replaying_change) break;
        std::vector<std::unique_ptr<QKeyEvent>> events;
        size_t first = 0;
        if (has_explicit_count) {
            while (first < last_change_keys.size() && last_change_keys[first]->text().size() == 1 &&
                   last_change_keys[first]->text().at(0).isDigit()) ++first;
            for (QChar digit : QString::number(num_repeats)) {
                events.emplace_back(new QKeyEvent(QEvent::KeyPress, digit.unicode(), Qt::NoModifier, QString(digit)));
            }
        }
        for (size_t i = first; i < last_change_keys.size(); ++i) events.emplace_back(last_change_keys[i]->clone());
        replaying_change = true;
        for (const auto &event : events) {
            QWidget *target = pending_text_command ? command_line_edit : editor_widget;
            QApplication::sendEvent(target, event.get());
        }
        replaying_change = false;
        if (has_explicit_count) last_change_keys = std::move(events);
        break;
    }
    case VimLineEditCommand::Undo:
        undo();
        break;
    case VimLineEditCommand::Redo:
        redo();
        break;
    case VimLineEditCommand::MoveWordBackward:
        new_pos = old_pos;
        for (int i = 0; i < num_repeats; ++i) {
            int next = calculate_move_word_backward(false, new_pos);
            if (next == new_pos) break;
            new_pos = next;
        }
        break;
    case VimLineEditCommand::MoveWordBackwardWithSymbols:
        new_pos = old_pos;
        for (int i = 0; i < num_repeats; ++i) {
            int next = calculate_move_word_backward(true, new_pos);
            if (next == new_pos) break;
            new_pos = next;
        }
        break;
    case VimLineEditCommand::Lowercase:
    case VimLineEditCommand::Uppercase: {
        push_history(current_state);
        pending_operator_count = num_repeats;
        action_waiting_for_motion = {cmd == VimLineEditCommand::Lowercase ?
            ActionWaitingForMotionKind::Lowercase : ActionWaitingForMotionKind::Uppercase};
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
            handle_action_waiting_for_motion(std::min(old_pos, visual_mode_anchor),
                std::max(old_pos, visual_mode_anchor), 1, current_mode == VimMode::VisualLine);
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            pending_operator_count = 1;
        }
        break;
    }
    case VimLineEditCommand::Indent:
    case VimLineEditCommand::Unindent:
    case VimLineEditCommand::Reindent: {
        push_history(current_state);
        pending_operator_count = num_repeats;
        action_waiting_for_motion = {cmd == VimLineEditCommand::Indent ?
            ActionWaitingForMotionKind::Indent : (cmd == VimLineEditCommand::Unindent ?
            ActionWaitingForMotionKind::Unindent : ActionWaitingForMotionKind::Reindent)};
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
            handle_action_waiting_for_motion(std::min(old_pos, visual_mode_anchor),
                                            std::max(old_pos, visual_mode_anchor), 0, true);
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            pending_operator_count = 1;
        }
        break;
    }
    case VimLineEditCommand::TransformCurrentLine:
        new_pos = old_pos;
        for (int i = 1; i < num_repeats; ++i) {
            int end = get_line_end_position(new_pos);
            if (end >= current_state.text.size()) break;
            new_pos = end + 1;
        }
        break;
    case VimLineEditCommand::JoinLines:
    case VimLineEditCommand::JoinLinesWithoutSpace: {
        push_history(current_state);
        int start = old_pos;
        int joins = std::max(2, num_repeats) - 1;
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
            start = std::min(old_pos, visual_mode_anchor);
            joins = current_state.text.mid(start, std::abs(old_pos - visual_mode_anchor)).count('\n');
        }
        for (int i = 0; i < joins; ++i) {
            QString text = adapter->get_text();
            int end = get_line_end_position(start);
            if (end >= text.size()) break;
            int next = end + 1;
            QString separator;
            if (cmd == VimLineEditCommand::JoinLines) {
                while (next < text.size() && (text[next] == ' ' || text[next] == '\t')) ++next;
                if (end > get_line_start_position(start) && !text[end - 1].isSpace() &&
                    next < text.size() && text[next] != '\n' && text[next] != ')') separator = " ";
            }
            insert_text(separator, end, next);
            new_pos = end;
        }
        action_waiting_for_motion = {};
        set_mode(VimMode::Normal);
        adapter->set_extra_selections({});
        break;
    }
    case VimLineEditCommand::EnterReplaceMode:
        push_history(current_state);
        set_mode(VimMode::Insert);
        replacing = true;
        replace_steps.clear();
        break;
    case VimLineEditCommand::ReplaceChar: {
        int end = get_line_end_position(old_pos);
        if (!symbol || num_repeats > end - old_pos) break;
        push_history(current_state);
        insert_text(QString(num_repeats, QChar(*symbol)), old_pos, old_pos + num_repeats);
        new_pos = old_pos + num_repeats - 1;
        break;
    }
    case VimLineEditCommand::DeleteCharAndEnterInsertMode:
    case VimLineEditCommand::DeleteChar:
        push_history(current_state);
        // if we have a text selection, delete it instead of the character under the cursor
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock) {
            delete_selection();
            if (cmd == VimLineEditCommand::DeleteCharAndEnterInsertMode) {
                set_mode(VimMode::Insert);
            }
            break;
        }
        else{
            int end = std::min(get_line_end_position(old_pos), old_pos + num_repeats);
            if (end > old_pos) {
                set_last_deleted_text(current_state.text.mid(old_pos, end - old_pos), current_paste_register);
                remove_text(old_pos, end - old_pos);
                int cursor = old_pos;
                if (cmd == VimLineEditCommand::DeleteChar) {
                    cursor = std::min(cursor, std::max(get_line_start_position(cursor), get_line_end_position(cursor) - 1));
                }
                set_cursor_position(cursor);
            }
            if (cmd == VimLineEditCommand::DeleteCharAndEnterInsertMode) {
                set_mode(VimMode::Insert);
            }
        }
        break;
    case VimLineEditCommand::Delete:
        push_history(current_state);
        pending_operator_count = num_repeats;
        action_waiting_for_motion = {ActionWaitingForMotionKind::Delete, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::Change:
        push_history(current_state);
        pending_operator_count = num_repeats;
        action_waiting_for_motion = {ActionWaitingForMotionKind::Change, SurroundingScope::None,
                                     SurroundingKind::None};
        break;
    case VimLineEditCommand::Yank:
        pending_operator_count = num_repeats;
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
            set_last_deleted_text(current_state.text.mid(cursor_pos, line_end - cursor_pos), current_paste_register);
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
        new_pos = calculate_find(last_find_state.value(), false, num_repeats);
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
        new_pos = calculate_find(last_find_state.value(), false, num_repeats);
        break;
    }
    case VimLineEditCommand::FindForwardTo: {
        last_find_state = FindState{FindDirection::ForwardTo, symbol};
        new_pos = calculate_find(last_find_state.value(), false, num_repeats);
        delete_pos_offset = 1;
        break;
    }
    case VimLineEditCommand::FindBackwardTo: {
        last_find_state = FindState{FindDirection::BackwardTo, symbol};
        new_pos = calculate_find(last_find_state.value(), false, num_repeats);
        break;
    }
    case VimLineEditCommand::RepeatFind:
    case VimLineEditCommand::RepeatFindReverse: {
        if (last_find_state) {
            bool reverse = cmd == VimLineEditCommand::RepeatFindReverse;
            new_pos = calculate_find(*last_find_state, reverse, num_repeats, true);
            bool forward = last_find_state->direction == FindDirection::Forward ||
                           last_find_state->direction == FindDirection::ForwardTo;
            delete_pos_offset = (forward != reverse) ? 1 : 0;
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

        for (int i = 1; i < num_repeats && line_end < current_state.text.size(); ++i) {
            line_end = get_line_end_position(line_end + 1);
        }

        set_last_deleted_text(current_state.text.mid(line_start, line_end - line_start), current_paste_register, true,
                              cmd == VimLineEditCommand::YankCurrentLine);

        if (cmd != VimLineEditCommand::YankCurrentLine){

            if (cmd == VimLineEditCommand::DeleteCurrentLine) {
                if (line_end < current_state.text.size() && current_state.text[line_end] == '\n') {
                    line_end++;
                } else if (line_start > 0 && current_state.text[line_start - 1] == '\n') {
                    line_start--;
                }
            }

            push_history(current_state);
            remove_text(line_start, line_end - line_start);

            int updated_len = adapter->get_text().size();
            if (line_start >= updated_len) {
                new_pos = std::max(0, updated_len - 1);
            }
            else {
                new_pos = line_start;
            }

            if (cmd == VimLineEditCommand::ChangeCurrentLine) {
                set_mode(VimMode::Insert);
            }
        }

        break;
    }
    case VimLineEditCommand::PasteForward: {
        push_history(current_state);
        std::optional<LastDeletedTextState> last_deleted_text = get_last_deleted_text(current_paste_register);
        if (last_deleted_text && last_deleted_text->text.size() > 0) {
            QString current_text = current_state.text;
            int cursor_pos = get_cursor_position();

            for (int i = 0; i < num_repeats; i++){
                if (last_deleted_text->is_line) {
                    // Paste the line below the current line
                    int line_end = get_line_end_position(cursor_pos);
                    insert_text("\n" + last_deleted_text->text, line_end);

                    new_pos = line_end + 1;
                }
                else {
                    // Paste the last deleted text after the cursor position
                    insert_text(last_deleted_text->text, cursor_pos + 1);
                    new_pos = cursor_pos + last_deleted_text->text.size();
                }
                cursor_pos = new_pos;
            }
        }
        break;
    }
    case VimLineEditCommand::PasteBackward: {
        push_history(current_state);
        std::optional<LastDeletedTextState> last_deleted_text = get_last_deleted_text(current_paste_register);
        if (last_deleted_text && last_deleted_text->text.size() > 0) {
            QString current_text = current_state.text;
            int cursor_pos = get_cursor_position();

            if (last_deleted_text->is_line) {
                // Paste the line above the current line
                int line_start = get_line_start_position(cursor_pos);
                insert_text(last_deleted_text->text + "\n", line_start);
                new_pos = line_start;
            } else {
                // Paste the last deleted text before the cursor position
                insert_text(last_deleted_text->text, cursor_pos);
                new_pos = cursor_pos + last_deleted_text->text.size();
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
        show_command_line_edit(get_initial_command_text(cmd));
        break;
    }
    case VimLineEditCommand::DecrementNextNumberOnCurrentLine:
    case VimLineEditCommand::IncrementNextNumberOnCurrentLine: {
        push_history(current_state);
        handle_number_increment_decrement(cmd == VimLineEditCommand::IncrementNextNumberOnCurrentLine, num_repeats);
        break;
    }
    case VimLineEditCommand::ProgressiveDecrement:
    case VimLineEditCommand::ProgressiveIncrement: {
        push_history(current_state);
        handle_number_increment_decrement(cmd == VimLineEditCommand::ProgressiveIncrement, num_repeats, true);
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
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualBlock) {
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
    case VimLineEditCommand::JumpForward: {
        if (jump_index >= 0 && jump_index < static_cast<int>(jumps.size()) - 1) {
            jump_index++;
            new_pos = jumps[jump_index];
        }
        break;
    }
    case VimLineEditCommand::JumpBackward: {
        if (jump_index == static_cast<int>(jumps.size())) {
            int current_pos = get_cursor_position();
            bool pushed = false;
            if (jumps.empty() || jumps.back() != current_pos) {
                jumps.push_back(current_pos);
                pushed = true;
            }
            int target_index = static_cast<int>(jumps.size()) - 2;
            if (target_index >= 0) {
                jump_index = target_index;
                new_pos = jumps[jump_index];
            } else {
                if (pushed) {
                    jumps.pop_back();
                }
            }
        } else {
            if (jump_index > 0) {
                jump_index--;
                new_pos = jumps[jump_index];
            }
        }
        break;
    }
    case VimLineEditCommand::ToggleComment: {
        toggle_comment(current_state, num_repeats, new_pos);
        break;
    }
    case VimLineEditCommand::ReselectVisualMode: {
        if (last_visual_mode.has_value() && last_visual_anchor != -1 && last_visual_pos != -1) {
            int text_len = adapter->get_text().length();
            if (text_len > 0) {
                int anchor = std::max(0, std::min(last_visual_anchor, text_len));
                int pos = std::max(0, std::min(last_visual_pos, text_len));

                VimMode mode_to_restore = last_visual_mode.value();
                set_mode(mode_to_restore);
                visual_mode_anchor = anchor;

                if (mode_to_restore == VimMode::Visual) {
                    set_cursor_position_with_selection(pos);
                    action_waiting_for_motion = {ActionWaitingForMotionKind::Visual, SurroundingScope::None, SurroundingKind::None};
                } else if (mode_to_restore == VimMode::VisualLine) {
                    set_cursor_position_with_line_selection(pos);
                } else if (mode_to_restore == VimMode::VisualBlock) {
                    set_cursor_position_with_block_selection(pos);
                    action_waiting_for_motion = {ActionWaitingForMotionKind::Visual, SurroundingScope::None, SurroundingKind::None};
                }
            }
        }
        break;
    }
    // Add more cases for other commands as needed
    default:
        break;
    }

    if (cmd != VimLineEditCommand::SelectPasteRegister && !action_waiting_for_motion.has_value()){
        current_paste_register = {};
    }

    if (should_reset_desired_pos){
        desired_index_in_line = {};
    }

    // we we have a text selected in visual mode and then perform change or delete
    // we should use the selected text as the target of the change/delete
    if (action_waiting_for_motion.has_value() &&
        (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change || action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete || action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) &&
        (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock)){
        int selection_begin, selection_end;
        QString selected_text = get_current_selection(selection_begin, selection_end);

        if (current_mode == VimMode::Visual) {
            auto selections = adapter->get_extra_selections();
            set_last_deleted_text(selected_text, current_paste_register);
            if (cmd !=  VimLineEditCommand::Yank) {
                // int start_pos = cursor.selectionStart();
                remove_text(selection_begin, selection_end - selection_begin);
            }
            set_cursor_position(selection_begin);
        }
        if (current_mode == VimMode::VisualLine) {
            int start = visual_line_selection_begin;
            int end = visual_line_selection_end;
            if ((start == -1) || (end == -1)) {
                return;
            }
            int offset = (end == current_state.text.size()) ? 1 : 0;
            if (current_state.text[end-1] == '\n') {
                offset = 0;
            }
            // // offset = 0;

            set_last_deleted_text(current_state.text.mid(start, end - start - 1 + offset), current_paste_register, true);
            if (cmd !=  VimLineEditCommand::Yank) {
                remove_text(start, end - start);
            }
            set_cursor_position(start);
        }
        if (current_mode == VimMode::VisualBlock) {
            handle_visual_block_action(cmd);
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

    if (new_pos == -1 && (cmd == VimLineEditCommand::FindForward || cmd == VimLineEditCommand::FindBackward ||
        cmd == VimLineEditCommand::FindForwardTo || cmd == VimLineEditCommand::FindBackwardTo ||
        cmd == VimLineEditCommand::RepeatFind || cmd == VimLineEditCommand::RepeatFindReverse)) {
        action_waiting_for_motion = {};
    }

    QString updated_text = adapter->get_text();
    if (cmd != VimLineEditCommand::EnterNormalMode && current_mode == VimMode::Normal && !action_waiting_for_motion && updated_text.size() > 0 && new_pos == updated_text.size()) {
        if (new_pos > 0 && updated_text[new_pos - 1] != '\n') {
            new_pos = updated_text.size() - 1;
        }
    }

    if (new_pos != -1) {
        if (is_jump_command(cmd) && new_pos != old_pos && !action_waiting_for_motion.has_value()) {
            add_jump(old_pos);
        }
        if (action_waiting_for_motion.has_value() && action_waiting_for_motion->kind != ActionWaitingForMotionKind::Visual) {
            bool linewise = cmd == VimLineEditCommand::MoveUp || cmd == VimLineEditCommand::MoveDown ||
                            cmd == VimLineEditCommand::GotoBegin || cmd == VimLineEditCommand::GotoEnd ||
                            cmd == VimLineEditCommand::TransformCurrentLine;
            handle_action_waiting_for_motion(old_pos, new_pos, delete_pos_offset, linewise);
        }
        else if (current_mode == VimMode::Visual) {
            set_cursor_position_with_selection(new_pos + delete_pos_offset);
        }
        else if (current_mode == VimMode::VisualLine) {
            set_cursor_position_with_line_selection(new_pos);
        }
        else if (current_mode == VimMode::VisualBlock) {
            set_cursor_position_with_block_selection(new_pos);
        }
        else {
            set_cursor_position(new_pos);
        }
    }

    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock) {
        last_visual_mode = current_mode;
        last_visual_anchor = visual_mode_anchor;
        last_visual_pos = get_cursor_position();
    }
}


int VimEditor::calculate_find(FindState state, bool reverse, int count, bool repeat) const {
    QString text = adapter->get_text();
    int cursor = get_cursor_position();
    bool forward = state.direction == FindDirection::Forward || state.direction == FindDirection::ForwardTo;
    bool till = state.direction == FindDirection::ForwardTo || state.direction == FindDirection::BackwardTo;
    if (reverse) forward = !forward;
    int step = forward ? 1 : -1;
    int location = cursor;
    for (int occurrence = 0; occurrence < count; ++occurrence) {
        int scan = location + step;
        if (repeat && till && occurrence == 0 && scan >= 0 && scan < text.size() &&
            text[scan] == QChar(state.character.value_or(' '))) scan += step;
        while (scan >= 0 && scan < text.size() && text[scan] != '\n' &&
               text[scan] != QChar(state.character.value_or(' '))) scan += step;
        if (scan < 0 || scan >= text.size() || text[scan] == '\n') return -1;
        location = scan;
    }
    return till ? location - step : location;
}

int VimEditor::calculate_move_word_forward(bool with_symbols, int pos) const {
    if (pos < 0) pos = get_cursor_position();
    const QString t = adapter->get_text();
    int len = t.length();

    bool operating = action_waiting_for_motion && action_waiting_for_motion->kind != ActionWaitingForMotionKind::Visual;
    if (pos >= len - 1) {
        return operating ? len : std::max(0, len - 1);
    }

    int next_pos = pos;

    if (!t[next_pos].isSpace()) {
        if (with_symbols) {
            while (next_pos < len && !t[next_pos].isSpace()) {
                next_pos++;
            }
        }
        else {
            bool is_word = t[next_pos].isLetterOrNumber() || t[next_pos] == '_';
            while (next_pos < len && !t[next_pos].isSpace() &&
                   (t[next_pos].isLetterOrNumber() || t[next_pos] == '_') == is_word) {
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
    return operating ? len : std::max(0, len - 1);
}

int VimEditor::calculate_move_to_end_of_word(bool with_symbols, int pos) const {
    if (pos < 0) pos = get_cursor_position();
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
        bool is_word = t[next_pos].isLetterOrNumber() || t[next_pos] == '_';
        while (next_pos < len - 1 && !t[next_pos + 1].isSpace() &&
               (t[next_pos + 1].isLetterOrNumber() || t[next_pos + 1] == '_') == is_word) {
            next_pos++;
        }
    }

    return next_pos;
}

int VimEditor::calculate_move_word_backward(bool with_symbols, int pos) const {
    if (pos < 0) pos = get_cursor_position();
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
        bool is_word = t[prev_pos].isLetterOrNumber() || t[prev_pos] == '_';
        while (prev_pos > 0 && !t[prev_pos - 1].isSpace() &&
               (t[prev_pos - 1].isLetterOrNumber() || t[prev_pos - 1] == '_') == is_word) {
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

        set_last_deleted_text(current_text.mid(current_pos, 1), current_paste_register);
        remove_text(current_pos, 1);
        set_cursor_position(current_pos);
    }
}

void VimEditor::push_history(HistoryState state) {
    // Entries after the cursor belong to an abandoned redo branch.
    history.states.erase(history.states.begin() + (history.current_index + 1),
                         history.states.end());
    if (!history.states.empty() && history.states.back().text == state.text) {
        return;
    }
    history.states.push_back(std::move(state));
    if (history.states.size() > 100) {
        history.states.pop_front();
    }
    history.current_index = static_cast<int>(history.states.size()) - 1;
}

void VimEditor::undo() {
    while (history.current_index >= 0) {
        HistoryState current{adapter->get_text(), get_cursor_position(), marks};
        HistoryState &state = history.states[history.current_index--];
        bool changed = state.text != current.text;
        adapter->set_text(state.text);
        set_cursor_position(state.cursor_position);
        marks = state.marks;
        state = std::move(current);
        if (changed) break;
    }
}

void VimEditor::redo() {
    while (history.current_index + 1 < static_cast<int>(history.states.size())) {
        HistoryState current{adapter->get_text(), get_cursor_position(), marks};
        HistoryState &state = history.states[++history.current_index];
        bool changed = state.text != current.text;
        adapter->set_text(state.text);
        set_cursor_position(state.cursor_position);
        marks = state.marks;
        state = std::move(current);
        if (changed) break;
    }
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
    if (action_waiting_for_motion && action_waiting_for_motion->surrounding_scope == SurroundingScope::Around) {
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
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Lowercase ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Uppercase) {
                    handle_action_waiting_for_motion(start, end, 0);
                    return true;
                }
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                    // Store deleted text for paste operation
                    set_last_deleted_text(text_under_cursor, current_paste_register);
                    // Delete only the word characters
                    remove_text(start, end - start);
                    set_cursor_position(start);

                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                        set_mode(VimMode::Insert);
                    }
                }
                else if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) {
                    set_last_deleted_text(text_under_cursor, current_paste_register);
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
            else if (action_waiting_for_motion->surrounding_kind == SurroundingKind::AngleBrackets) {
                begin_symbol = '<';
                end_symbol = '>';
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
            int begin_to_find = 1;
            int end_to_find = 1;
            // An opening delimiter under the cursor belongs to this object.
            if (begin_symbol != end_symbol && cursor_pos < current_text.size() &&
                current_text[cursor_pos] == begin_symbol) {
                start = cursor_pos + 1;
                end = cursor_pos + 1;
            }

            // Find the beginning of the surrounding
            while (start > 0) {
                if (current_text[start - 1] == begin_symbol) {
                    begin_to_find--;
                    if (begin_to_find == 0) {
                        found_begin = true;
                        start--;
                        break;
                    }
                }
                if (current_text[start - 1] == end_symbol) {
                    begin_to_find++;
                }
                start--;
            }
            // Find the end of the surrounding
            while (end < current_text.length()) {
                if (current_text[end] == end_symbol) {
                    end_to_find--;
                    if (end_to_find == 0) {
                        found_end = true;
                        end++;
                        break;
                    }
                }
                if (current_text[end] == begin_symbol) {
                    end_to_find++;
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
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Lowercase ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Uppercase) {
                    handle_action_waiting_for_motion(start, end, 0);
                    return true;
                }
                if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Delete ||
                    action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                    // Store deleted text for paste operation
                    set_last_deleted_text(current_text.mid(start, end - start), current_paste_register);
                    // Delete the surrounding symbols
                    remove_text(start, end - start);
                    set_cursor_position(start);
                    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
                        set_mode(VimMode::Insert);
                    }
                }
                else if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) {
                    set_last_deleted_text(current_text.mid(start, end - start), current_paste_register);
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

void VimEditor::text_prepended(int length) {
    for (int* pos : {&visual_mode_anchor, &last_visual_anchor, &last_visual_pos,
                     &visual_line_selection_begin, &visual_line_selection_end}) {
        if (*pos >= 0) {
            *pos += length;
        }
    }
    for (auto& [name, mark] : marks) {
        mark.position += length;
    }
    for (int& pos : jumps) {
        pos += length;
    }
}

void VimEditor::set_cursor_position(int pos) {
    adapter->set_cursor_position(pos);
}

void VimEditor::set_cursor_position_with_selection(int pos) {
    adapter->set_cursor_position_with_selection(pos, visual_mode_anchor);
}

void VimEditor::set_cursor_position_with_line_selection(int pos) {
    bool is_text_edit = dynamic_cast<QTextEditAdapter*>(adapter) != nullptr;
    bool is_line_edit = dynamic_cast<QLineEditAdapter*>(adapter) != nullptr;
    if (!is_text_edit && !is_line_edit) {
        return;
    }
    
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
    
    visual_line_selection_begin = selection_start;
    visual_line_selection_end = selection_end;

    if (is_text_edit) {
        QTextEditAdapter *text_adapter = static_cast<QTextEditAdapter*>(adapter);
        QTextCursor cursor = text_adapter->text_edit->textCursor();
        cursor.setPosition(selection_start, QTextCursor::MoveAnchor);
        cursor.setPosition(selection_end, QTextCursor::KeepAnchor);

        QTextEdit::ExtraSelection selection;
        selection.cursor = cursor;

        QColor selection_foreground_color = text_adapter->text_edit->palette().color(QPalette::Text);
        QColor selection_background_color = text_adapter->text_edit->palette().color(QPalette::Highlight);
        selection.format.setBackground(selection_background_color);
        selection.format.setForeground(selection_foreground_color);

        adapter->set_extra_selections({selection});
        set_cursor_position(pos);
    }
    else if (is_line_edit) {
        QLineEditAdapter *line_adapter = static_cast<QLineEditAdapter*>(adapter);
        if (visual_mode_anchor <= pos) {
            line_adapter->line_edit->setSelection(selection_start, selection_end - selection_start);
        } else {
            line_adapter->line_edit->setSelection(selection_end, selection_start - selection_end);
        }
    }
}

void VimEditor::get_line_and_col(int pos, int &line_idx, int &col_idx) const {
    const QString text = adapter->get_text();
    int p = std::max(0, std::min<int>(pos, text.length()));
    line_idx = 0;
    int line_start = 0;
    for (int i = 0; i < p; ++i) {
        if (text[i] == '\n') {
            line_idx++;
            line_start = i + 1;
        }
    }
    col_idx = p - line_start;
}

std::vector<int> VimEditor::get_line_starts(const QString &text) {
    std::vector<int> line_starts;
    line_starts.push_back(0);
    for (int i = 0; i < text.length(); ++i) {
        if (text[i] == '\n') {
            line_starts.push_back(i + 1);
        }
    }
    return line_starts;
}

VisualBlockBounds VimEditor::get_visual_block_bounds(int pos) const {
    if (pos == -1) {
        pos = get_cursor_position();
    }
    const QString text = adapter->get_text();
    int anchor_line, anchor_col, pos_line, pos_col;
    get_line_and_col(visual_mode_anchor, anchor_line, anchor_col);
    get_line_and_col(pos, pos_line, pos_col);

    VisualBlockBounds bounds;
    bounds.min_line = std::min(anchor_line, pos_line);
    bounds.max_line = std::max(anchor_line, pos_line);
    bounds.min_col = std::min(anchor_col, pos_col);
    bounds.max_col = std::max(anchor_col, pos_col);
    bounds.line_starts = get_line_starts(text);
    return bounds;
}

void VimEditor::apply_transform_to_visual_block(const std::function<QString(const QString&)> &transform_fn) {
    VisualBlockBounds bounds = get_visual_block_bounds();
    const QString text = adapter->get_text();

    for (int l = bounds.max_line; l >= bounds.min_line && l < static_cast<int>(bounds.line_starts.size()); --l) {
        int l_start = bounds.line_starts[l];
        int l_end = (l + 1 < static_cast<int>(bounds.line_starts.size())) ? bounds.line_starts[l + 1] - 1 : text.length();
        int l_len = l_end - l_start;
        if (bounds.min_col < l_len) {
            int start_idx = l_start + bounds.min_col;
            int end_idx = l_start + std::min(bounds.max_col, l_len - 1);
            QString slice = text.mid(start_idx, end_idx - start_idx + 1);
            insert_text(transform_fn(slice), start_idx, end_idx + 1);
        }
    }
    int first_line_start = bounds.line_starts[bounds.min_line];
    set_mode(VimMode::Normal);
    adapter->set_extra_selections({});
    set_cursor_position(first_line_start + bounds.min_col);
}

void VimEditor::enter_visual_block_insert_mode(bool at_end) {
    VisualBlockBounds bounds = get_visual_block_bounds();
    int target_col = at_end ? bounds.max_col + 1 : bounds.min_col;
    visual_block_insert_state = VisualBlockInsertState{bounds.min_line, bounds.max_line, target_col};
    set_mode(VimMode::Insert);
    adapter->set_extra_selections({});

    const QString text = adapter->get_text();
    int l_start = bounds.line_starts[bounds.min_line];
    int l_end = (bounds.min_line + 1 < static_cast<int>(bounds.line_starts.size())) ? bounds.line_starts[bounds.min_line + 1] - 1 : text.length();
    int col_pos = std::min(target_col, l_end - l_start);
    set_cursor_position(l_start + col_pos);
}

void VimEditor::handle_visual_block_action(VimLineEditCommand cmd) {
    VisualBlockBounds bounds = get_visual_block_bounds();
    if (action_waiting_for_motion.has_value() && action_waiting_for_motion->kind == ActionWaitingForMotionKind::Change) {
        visual_block_insert_state = VisualBlockInsertState{
            bounds.min_line,
            bounds.max_line,
            bounds.min_col
        };
    }

    const QString text = adapter->get_text();
    QStringList yanked_block;
    for (int l = bounds.min_line; l <= bounds.max_line && l < static_cast<int>(bounds.line_starts.size()); ++l) {
        int l_start = bounds.line_starts[l];
        int l_end = (l + 1 < static_cast<int>(bounds.line_starts.size())) ? bounds.line_starts[l + 1] - 1 : text.length();
        int l_len = l_end - l_start;
        if (bounds.min_col < l_len) {
            int start_idx = l_start + bounds.min_col;
            int end_idx = l_start + std::min(bounds.max_col, l_len - 1);
            yanked_block.append(text.mid(start_idx, end_idx - start_idx + 1));
        } else {
            yanked_block.append("");
        }
    }
    set_last_deleted_text(yanked_block.join("\n"), current_paste_register, false);

    if (cmd != VimLineEditCommand::Yank) {
        for (int l = bounds.max_line; l >= bounds.min_line && l < static_cast<int>(bounds.line_starts.size()); --l) {
            int l_start = bounds.line_starts[l];
            int l_end = (l + 1 < static_cast<int>(bounds.line_starts.size())) ? bounds.line_starts[l + 1] - 1 : text.length();
            int l_len = l_end - l_start;
            if (bounds.min_col < l_len) {
                int start_idx = l_start + bounds.min_col;
                int end_idx = l_start + std::min(bounds.max_col, l_len - 1);
                remove_text(start_idx, end_idx - start_idx + 1);
            }
        }
    }
    int first_line_start = bounds.line_starts[bounds.min_line];
    set_cursor_position(first_line_start + bounds.min_col);
}

void VimEditor::replicate_visual_block_insert_text() {
    if (!visual_block_insert_state.has_value()) {
        return;
    }
    auto vbis = visual_block_insert_state.value();
    visual_block_insert_state = {};
    QString inserted_text = current_insert_mode_text;
    if (inserted_text.isEmpty()) {
        return;
    }
    const QString doc_text = adapter->get_text();
    std::vector<int> line_starts = get_line_starts(doc_text);
    for (int l = vbis.max_line; l >= vbis.min_line + 1 && l < static_cast<int>(line_starts.size()); --l) {
        int l_start = line_starts[l];
        int l_end = (l + 1 < static_cast<int>(line_starts.size())) ? line_starts[l + 1] - 1 : doc_text.length();
        int l_len = l_end - l_start;
        int insert_col = std::min(vbis.col, l_len);
        insert_text(inserted_text, l_start + insert_col);
    }
}

void VimEditor::set_cursor_position_with_block_selection(int pos) {
    bool is_text_edit = dynamic_cast<QTextEditAdapter*>(adapter) != nullptr;
    bool is_line_edit = dynamic_cast<QLineEditAdapter*>(adapter) != nullptr;
    if (!is_text_edit && !is_line_edit) {
        return;
    }

    VisualBlockBounds bounds = get_visual_block_bounds(pos);
    const QString text = adapter->get_text();

    if (is_text_edit) {
        QTextEditAdapter *text_adapter = static_cast<QTextEditAdapter*>(adapter);
        QList<QTextEdit::ExtraSelection> selections;

        for (int l = bounds.min_line; l <= bounds.max_line && l < static_cast<int>(bounds.line_starts.size()); ++l) {
            int l_start = bounds.line_starts[l];
            int l_end = (l + 1 < static_cast<int>(bounds.line_starts.size())) ? bounds.line_starts[l + 1] - 1 : text.length();
            int l_len = l_end - l_start;
            if (bounds.min_col < l_len) {
                int start_idx = l_start + bounds.min_col;
                int end_idx = l_start + std::min(bounds.max_col, l_len - 1);

                QTextCursor cursor(text_adapter->text_edit->document());
                cursor.setPosition(start_idx, QTextCursor::MoveAnchor);
                cursor.setPosition(end_idx + 1, QTextCursor::KeepAnchor);

                QTextEdit::ExtraSelection selection;
                selection.cursor = cursor;
                QColor selection_foreground_color = text_adapter->text_edit->palette().color(QPalette::Text);
                QColor selection_background_color = text_adapter->text_edit->palette().color(QPalette::Highlight);
                selection.format.setBackground(selection_background_color);
                selection.format.setForeground(selection_foreground_color);
                selections.append(selection);
            }
        }
        adapter->set_extra_selections(selections);
        set_cursor_position(pos);
    }
    else if (is_line_edit) {
        QLineEditAdapter *line_adapter = static_cast<QLineEditAdapter*>(adapter);
        int start = std::min<int>(bounds.min_col, text.length());
        int end = std::min<int>(bounds.max_col + 1, text.length());
        line_adapter->line_edit->setSelection(start, std::max(0, end - start));
    }
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

    // If the previous line is empty, position should be at its start
    if (prev_line_length <= 0) {
        return prev_line_start;
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
    if (next_line_start > text.length()) {
        return cursor_pos;
    }

    // Find the end of the next line
    int next_line_end = get_line_end_position(next_line_start);
    int next_line_length = next_line_end - next_line_start;

    // If the next line is empty, position should be at its start.
    if (next_line_length <= 0) {
        return next_line_start;
    }

    // Clamp desired column to the last character index of the next line
    column_offset = std::min<int>(column_offset, next_line_length - 1);

    // Try to maintain the same column position, but clamp to line length
    int new_column = std::min<int>(column_offset, next_line_length - 1);

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
        int target_index;
        // If the target visual line is empty, position at its start
        if (target_line.textLength() <= 0) {
            target_index = 0;
        } else {
            target_index = std::min(char_index_in_line, target_line.textLength() - 1);
        }
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
    int target_index;
    if (target_line.textLength() <= 0) {
        target_index = 0;
    } else {
        target_index = std::min(char_index_in_line, target_line.textLength() - 1);
    }
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

void VimEditor::show_command_line_edit(QString initial_command_text, QString placeholder_text){
    QTextEditAdapter *text_adapter = dynamic_cast<QTextEditAdapter*>(adapter);
    if (text_adapter && text_adapter->text_edit) {
        initial_search_scroll_value = text_adapter->text_edit->verticalScrollBar()->value();
    } else {
        initial_search_scroll_value = std::nullopt;
    }

    // get editor widget's background color
    QColor background_color = get_darker_color(editor_widget->palette().color(QPalette::Base));
    QColor text_color = editor_widget->palette().color(QPalette::Text);

    command_line_edit->setText(initial_command_text);
    command_line_edit->setPlaceholderText(placeholder_text);
    command_line_edit->show();
    command_line_edit->raise();
    command_line_edit->setFocus();
    command_line_edit->setStyleSheet(QString("QLineEdit { background-color: %1; color: %2; }")
                                     .arg(background_color.name(), text_color.name()));
}

void VimEditor::hide_command_line_edit(bool was_escaped){
    pending_text_command = {};
    if (was_escaped) {
        action_waiting_for_motion = {};
        recording_change = false;
        change_keys.clear();
    }
    if (initial_search_scroll_value.has_value()) {
        QTextEditAdapter *text_adapter = dynamic_cast<QTextEditAdapter*>(adapter);
        if (text_adapter && text_adapter->text_edit) {
            text_adapter->text_edit->verticalScrollBar()->setValue(initial_search_scroll_value.value());
        }
        initial_search_scroll_value = std::nullopt;
        if (was_escaped) {
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
        }
    }

    command_line_edit->blockSignals(true);
    command_line_edit->clear();
    command_line_edit->blockSignals(false);
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
            break;
        }
        default:
            break;
        }
    }

}

void VimEditor::handle_action_waiting_for_motion(int old_pos, int new_pos, int delete_pos_offset, bool linewise){
    if (action_waiting_for_motion.has_value()) {
        QString current_text = adapter->get_text();
        auto kind = action_waiting_for_motion->kind;
        if (kind == ActionWaitingForMotionKind::Lowercase || kind == ActionWaitingForMotionKind::Uppercase) {
            int begin = std::min(old_pos, new_pos);
            int end = std::max(old_pos, new_pos) + (new_pos >= old_pos ? delete_pos_offset : 0);
            if (linewise) {
                begin = get_line_start_position(begin);
                end = get_line_end_position(std::max(old_pos, new_pos));
            }
            QString text = current_text.mid(begin, end - begin);
            insert_text(kind == ActionWaitingForMotionKind::Lowercase ? text.toLower() : text.toUpper(), begin, end);
            set_cursor_position(begin);
            action_waiting_for_motion = {};
            return;
        }
        if (kind == ActionWaitingForMotionKind::Indent || kind == ActionWaitingForMotionKind::Unindent ||
            kind == ActionWaitingForMotionKind::Reindent) {
            int begin = get_line_start_position(std::min(old_pos, new_pos));
            int end = get_line_end_position(std::max(old_pos, new_pos));
            QStringList lines = current_text.mid(begin, end - begin).split('\n');
            int previous_indent = 0;
            for (int previous = begin - 1; previous >= 0;) {
                int start = get_line_start_position(previous);
                QString line = current_text.mid(start, previous - start);
                if (!line.trimmed().isEmpty()) {
                    for (QChar ch : line) {
                        if (ch == ' ') ++previous_indent;
                        else if (ch == '\t') previous_indent += 8 - previous_indent % 8;
                        else break;
                    }
                    break;
                }
                previous = start - 1;
            }
            for (QString &line : lines) {
                int prefix = 0, columns = 0;
                while (prefix < line.size() && (line[prefix] == ' ' || line[prefix] == '\t')) {
                    columns += line[prefix] == '\t' ? 8 - columns % 8 : 1;
                    ++prefix;
                }
                if (prefix == line.size()) continue;
                columns = kind == ActionWaitingForMotionKind::Reindent ? previous_indent :
                          (kind == ActionWaitingForMotionKind::Indent ? columns + 8 : std::max(0, columns - 8));
                line = QString(columns / 8, '\t') + QString(columns % 8, ' ') + line.mid(prefix);
            }
            insert_text(lines.join("\n"), begin, end);
            int cursor = begin;
            QString result = adapter->get_text();
            while (cursor < result.size() && (result[cursor] == ' ' || result[cursor] == '\t')) ++cursor;
            set_cursor_position(cursor);
            action_waiting_for_motion = {};
            return;
        }
        if (linewise) {
            auto kind = action_waiting_for_motion->kind;
            int begin = get_line_start_position(std::min(old_pos, new_pos));
            int end = get_line_end_position(std::max(old_pos, new_pos));
            set_last_deleted_text(current_text.mid(begin, end - begin), current_paste_register, true);
            if (kind == ActionWaitingForMotionKind::Yank) {
                set_cursor_position(old_pos);
            } else {
                if (kind == ActionWaitingForMotionKind::Delete) {
                    if (end < current_text.size()) ++end;
                    else if (begin > 0) --begin;
                }
                remove_text(begin, end - begin);
                set_cursor_position(std::min(begin, static_cast<int>(adapter->get_text().size())));
                if (kind == ActionWaitingForMotionKind::Change) set_mode(VimMode::Insert);
            }
            action_waiting_for_motion = {};
            return;
        }
        if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank) {
            int begin = std::min(old_pos, new_pos);
            int end = std::max(old_pos, new_pos) + (new_pos >= old_pos ? delete_pos_offset : 0);
            set_last_deleted_text(current_text.mid(begin, end - begin), current_paste_register);
            set_cursor_position(old_pos);
        }
        if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Delete ||
            action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Change) {
            // delete from old_pos to new_pos
            if (old_pos < new_pos || (old_pos == new_pos && delete_pos_offset > 0)) {
                set_last_deleted_text(
                    current_text.mid(old_pos, new_pos + delete_pos_offset - old_pos), current_paste_register);
                remove_text(old_pos, new_pos + delete_pos_offset - old_pos);
                set_cursor_position(old_pos);
            }
            else if (old_pos > new_pos) {
                set_last_deleted_text(current_text.mid(new_pos, old_pos - new_pos), current_paste_register);
                remove_text(new_pos, old_pos - new_pos);
                set_cursor_position(new_pos);
            }

            if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Change) {
                set_mode(VimMode::Insert);
            }
        }

        if (action_waiting_for_motion.value().kind == ActionWaitingForMotionKind::Visual) {
            set_cursor_position_with_selection(new_pos);
        }

        action_waiting_for_motion = {};
    }
}

void VimEditor::handle_search(bool reverse){
    if (!last_search_state.has_value()) {
        return;
    }

    std::vector<int> found_indices;
    for (const auto &match : pattern_matches(adapter->get_text(), last_search_state->query.value_or(""))) {
        found_indices.push_back(match.start);
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
        if (target_index != current_pos && !action_waiting_for_motion.has_value()) {
            add_jump(current_pos);
        }
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
    else if (current_mode == VimMode::VisualBlock) {
        set_cursor_position_with_block_selection(target_index);
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

void VimEditor::set_last_deleted_text(QString text, std::optional<char> reg, bool is_line, bool is_yank) {
    if (reg == '_') return;
    LastDeletedTextState state{text, is_line};
    is_yank = is_yank || (action_waiting_for_motion && action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank);
    if (is_yank && (!reg || reg == '"')) paste_registers['0'] = state;
    last_deleted_text_ = state;
    if (reg && *reg != '"') {
        if (*reg == '+' || *reg == '*') QApplication::clipboard()->setText(text);
        else paste_registers[*reg] = state;
    }
}

std::optional<LastDeletedTextState> VimEditor::get_last_deleted_text(std::optional<char> reg){
    if (reg.has_value() && *reg != '"'){
        if (reg.value() == '+' || reg.value() == '*'){
            // return the contents of the system clipboard
            QClipboard *clipboard = QApplication::clipboard();
            LastDeletedTextState clipboard_state;
            clipboard_state.text = clipboard->text();
            clipboard_state.is_line = false;
            return clipboard_state;
        }
        auto it = paste_registers.find(reg.value());
        if (it != paste_registers.end()){
            return it->second;
        }
        else{
            return {};
        }
    }
    return last_deleted_text_;
}

void VimEditor::handle_number_increment_decrement(bool increment, int count, bool progressive) {
    QString current_text = adapter->get_text();
    int cursor_pos = get_cursor_position();

    bool has_selection = false;
    int begin = -1, end = -1;
    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
        get_current_selection(begin, end);
        if (begin != -1 && end != -1 && begin < end) {
            has_selection = true;
        }
    }

    if (has_selection) {
        // Build all lines ranges
        struct Line {
            int start;
            int end;
        };
        std::vector<Line> all_lines;
        int text_len = current_text.length();
        int p = 0;
        while (p <= text_len) {
            int l_start = p;
            while (p < text_len && current_text[p] != '\n') {
                p++;
            }
            all_lines.push_back({l_start, p});
            p++; // skip '\n'
        }

        auto get_line_index_of_pos = [&](int pos) -> int {
            for (size_t i = 0; i < all_lines.size(); ++i) {
                int line_limit = (i + 1 < all_lines.size()) ? all_lines[i + 1].start : all_lines[i].end + 1;
                if (pos >= all_lines[i].start && pos < line_limit) {
                    return static_cast<int>(i);
                }
            }
            return static_cast<int>(all_lines.size() - 1);
        };

        int start_line_idx = get_line_index_of_pos(begin);
        int end_line_idx = get_line_index_of_pos(std::max(begin, end - 1));
        if (start_line_idx > end_line_idx) {
            std::swap(start_line_idx, end_line_idx);
        }

        std::vector<int> selected_line_indices;
        for (int i = start_line_idx; i <= end_line_idx; ++i) {
            selected_line_indices.push_back(i);
        }

        int new_cursor_pos = begin;

        // Process from bottom to top so length changes don't shift earlier line boundaries
        for (int j = static_cast<int>(selected_line_indices.size()) - 1; j >= 0; --j) {
            int i = selected_line_indices[j];
            int L_start = all_lines[i].start;
            int L_end = all_lines[i].end;

            int sel_start = std::max(L_start, begin);
            int sel_end = std::min(L_end, end);
            int relative_sel_start = sel_start - L_start;
            int relative_sel_end = sel_end - L_start;

            QString line_text = current_text.mid(L_start, L_end - L_start);
            QRegularExpression number_regex("(-?\\d+)");
            QRegularExpressionMatchIterator matches = number_regex.globalMatch(line_text);
            
            int number_start = -1;
            int number_end = -1;
            QString number_str;
            while (matches.hasNext()) {
                QRegularExpressionMatch match = matches.next();
                int m_start = match.capturedStart();
                int m_end = match.capturedEnd();
                if (std::max(m_start, relative_sel_start) < std::min(m_end, relative_sel_end)) {
                    number_start = m_start;
                    number_end = m_end;
                    number_str = match.captured(1);
                    break;
                }
            }

            if (number_start != -1) {
                bool ok;
                int number = number_str.toInt(&ok);
                if (ok) {
                    int change = count;
                    if (progressive) {
                        change = (j + 1) * count;
                    }
                    int new_number = increment ? number + change : number - change;
                    QString new_number_str = QString::number(new_number);

                    int absolute_start = L_start + number_start;
                    int absolute_end = L_start + number_end;

                    insert_text(new_number_str, absolute_start, absolute_end);

                    new_cursor_pos = absolute_start + new_number_str.length() - 1;
                }
            }
        }

        // Return to normal mode, clear highlights and position cursor
        if (visual_line_selection_begin != -1) {
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
            visual_line_selection_begin = -1;
            visual_line_selection_end = -1;
        }
        if (current_mode == VimMode::Visual) {
            adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
        }
        set_mode(VimMode::Normal);
        set_cursor_position(new_cursor_pos);

    } else {
        // Normal Mode single-line increment/decrement logic
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
                int change = count;
                int new_number = increment ? number + change : number - change;
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
}

void VimEditor::toggle_comment(const HistoryState &current_state, int num_repeats, int &new_pos) {
    push_history(current_state);

    QString current_text = current_state.text;
    int text_len = current_text.length();
    int old_pos = get_cursor_position();

    struct Line {
        int start;
        int end;
    };
    std::vector<Line> all_lines;
    int p = 0;
    while (p <= text_len) {
        int l_start = p;
        while (p < text_len && current_text[p] != '\n') {
            p++;
        }
        all_lines.push_back({l_start, p});
        p++; // skip '\n'
    }

    auto get_line_index_of_pos = [&](int pos) -> int {
        pos = std::max(0, std::min(pos, text_len));
        for (size_t i = 0; i < all_lines.size(); ++i) {
            int line_limit = (i + 1 < all_lines.size()) ? all_lines[i + 1].start : text_len + 1;
            if (pos >= all_lines[i].start && pos < line_limit) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(all_lines.size() - 1);
    };

    int start_line_idx = 0;
    int end_line_idx = 0;

    int sel_begin = -1, sel_end = -1;
    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
        get_current_selection(sel_begin, sel_end);
    }

    if (sel_begin != -1 && sel_end != -1 && sel_begin <= sel_end) {
        if (sel_begin == sel_end) {
            start_line_idx = get_line_index_of_pos(sel_begin);
            end_line_idx = start_line_idx;
        } else {
            start_line_idx = get_line_index_of_pos(sel_begin);
            end_line_idx = get_line_index_of_pos(sel_end - 1);
        }
    } else {
        start_line_idx = get_line_index_of_pos(old_pos);
        end_line_idx = std::min<int>(static_cast<int>(all_lines.size()) - 1, start_line_idx + num_repeats - 1);
    }

    if (start_line_idx > end_line_idx) {
        std::swap(start_line_idx, end_line_idx);
    }

    bool all_commented = true;
    int non_empty_lines_count = 0;

    for (int i = start_line_idx; i <= end_line_idx; ++i) {
        QString line_str = current_text.mid(all_lines[i].start, all_lines[i].end - all_lines[i].start);
        int k = 0;
        while (k < line_str.length() && (line_str[k] == ' ' || line_str[k] == '\t')) {
            k++;
        }
        if (k < line_str.length()) {
            non_empty_lines_count++;
            bool line_is_commented = (line_str[k] == '#');
            if (!line_is_commented) {
                all_commented = false;
                break;
            }
        }
    }
    if (non_empty_lines_count == 0) {
        all_commented = false;
    }

    int cursor_line_idx = get_line_index_of_pos(old_pos);
    new_pos = old_pos;

    for (int i = end_line_idx; i >= start_line_idx; --i) {
        int L_start = all_lines[i].start;
        int L_end = all_lines[i].end;
        QString line_str = current_text.mid(L_start, L_end - L_start);

        int k = 0;
        while (k < line_str.length() && (line_str[k] == ' ' || line_str[k] == '\t')) {
            k++;
        }

        int delta = 0;
        if (all_commented) {
            if (k < line_str.length() && line_str[k] == '#') {
                int remove_len = 1;
                if (k + 1 < line_str.length() && line_str[k + 1] == ' ') {
                    remove_len = 2;
                }
                line_str.remove(k, remove_len);
                delta = -remove_len;

                if (i == cursor_line_idx) {
                    int comment_offset = L_start + k;
                    if (old_pos >= comment_offset + remove_len) {
                        new_pos = old_pos - remove_len;
                    } else if (old_pos >= comment_offset) {
                        new_pos = comment_offset;
                    } else {
                        new_pos = old_pos;
                    }
                }
            }
        } else {
            line_str.insert(k, "# ");
            delta = 2;

            if (i == cursor_line_idx) {
                int comment_offset = L_start + k;
                if (old_pos >= comment_offset) {
                    new_pos = old_pos + 2;
                } else {
                    new_pos = old_pos;
                }
            }
        }

        if (delta != 0) {
            insert_text(line_str, L_start, L_end);
            if (i < cursor_line_idx) {
                new_pos += delta;
            }
        }
    }

    if (visual_line_selection_begin != -1) {
        adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
        visual_line_selection_begin = -1;
        visual_line_selection_end = -1;
    }
    if (current_mode == VimMode::Visual || current_mode == VimMode::VisualBlock) {
        adapter->set_extra_selections(QList<QTextEdit::ExtraSelection>());
    }
    set_mode(VimMode::Normal);
}

void VimEditor::set_mode(VimMode mode){
    if (mode != VimMode::Insert) {
        replacing = false;
        replace_steps.clear();
    }
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

void VimEditor::emit_save(){
    VimLineEdit* line_edit = dynamic_cast<VimLineEdit*>(editor_widget);
    VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
    if (line_edit) {
        emit line_edit->writeCommand();
    }
    if (text_edit) {
        emit text_edit->writeCommand();
    }
}

void VimEditor::emit_open_file(){
    VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
    if (text_edit) {
        emit text_edit->openFile();
    }
}

void VimEditor::emit_open_config(){
    VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
    if (text_edit) {
        QString text_under_cursor = get_word_under_cursor();
        if (text_under_cursor.size() > 0){
            emit text_edit->openConfig(text_under_cursor);
        }
    }
}


void VimEditor::emit_quit(){
    VimLineEdit* line_edit = dynamic_cast<VimLineEdit*>(editor_widget);
    VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
    if (line_edit) {
        emit line_edit->quitCommand();
    }
    if (text_edit) {
        emit text_edit->quitCommand();
    }

}

bool VimEditor::handle_substitute(const QString &command) {
    static const QRegularExpression prefix(R"(^(%|[.$0-9]+(?:,[.$0-9]+)?)?s([^\w\s\\]))");
    auto parsed = prefix.match(command);
    if (!parsed.hasMatch()) return false;
    auto *widget = qobject_cast<QTextEdit*>(editor_widget);
    if (widget && widget->isReadOnly()) return true;
    QChar delimiter = parsed.captured(2).at(0);
    int position = parsed.capturedEnd();
    auto read_part = [&](QString &part) {
        while (position < command.size()) {
            QChar ch = command[position++];
            if (ch == delimiter) return true;
            part += ch;
            if (ch == '\\' && position < command.size()) part += command[position++];
        }
        return false;
    };
    QString pattern, replacement;
    if (!read_part(pattern)) return true;
    read_part(replacement); // Vim permits an omitted final delimiter.
    QString flags = command.mid(position);
    for (QChar flag : flags) {
        if (!QString("giIe").contains(flag)) return true;
    }
    if (pattern.isEmpty() && last_search_state) pattern = last_search_state->query.value_or("");
    auto regex = vim_regex(pattern);
    if (flags.contains('i')) regex.setPatternOptions(regex.patternOptions() | QRegularExpression::CaseInsensitiveOption);
    if (flags.contains('I')) regex.setPatternOptions(regex.patternOptions() & ~QRegularExpression::CaseInsensitiveOption);
    if (pattern.isEmpty() || !regex.isValid()) return true;

    QString text = adapter->get_text();
    auto starts = get_line_starts(text);
    int current_line = 0, column = 0;
    get_line_and_col(get_cursor_position(), current_line, column);
    int first = current_line, last = current_line;
    QString range = parsed.captured(1);
    auto address = [&](const QString &value) {
        if (value == ".") return current_line;
        if (value == "$") return static_cast<int>(starts.size()) - 1;
        return std::clamp(value.toInt() - 1, 0, static_cast<int>(starts.size()) - 1);
    };
    if (range == "%") { first = 0; last = starts.size() - 1; }
    else if (!range.isEmpty()) {
        auto addresses = range.split(',');
        first = address(addresses.front());
        last = address(addresses.back());
    }
    if (first > last) return true;
    struct Edit { int start, length; QString text; };
    std::vector<Edit> edits;
    int last_changed_line = first;
    for (int line = first; line <= last; ++line) {
        int start = starts[line];
        QString content = text.mid(start, get_line_end_position(start) - start);
        auto matches = regex.globalMatch(content);
        while (matches.hasNext()) {
            auto match = matches.next();
            QString expanded;
            for (int i = 0; i < replacement.size(); ++i) {
                QChar ch = replacement[i];
                if (ch == '&') expanded += match.captured(0);
                else if (ch == '\\' && i + 1 < replacement.size()) {
                    ch = replacement[++i];
                    if (ch.isDigit()) expanded += match.captured(ch.digitValue());
                    else if (ch == 'r') expanded += '\n';
                    else expanded += ch;
                } else expanded += ch;
            }
            edits.push_back({start + static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), expanded});
            last_changed_line = line;
            if (!flags.contains('g')) break;
        }
    }
    last_search_state = SearchState{FindDirection::Forward, pattern};
    if (!edits.empty()) {
        push_current_history_state();
        int cursor = starts[last_changed_line];
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            if (it->start < starts[last_changed_line]) cursor += it->text.size() - it->length;
            insert_text(it->text, it->start, it->start + it->length);
        }
        QString updated = adapter->get_text();
        while (cursor < updated.size() && (updated[cursor] == ' ' || updated[cursor] == '\t')) ++cursor;
        set_cursor_position(cursor);
    }
    return true;
}

void VimEditor::handle_text_command(QString text){
    if (handle_substitute(text)) return;

    VimLineEdit* line_edit = dynamic_cast<VimLineEdit*>(editor_widget);
    VimTextEdit* text_edit = dynamic_cast<VimTextEdit*>(editor_widget);
    bool handled = false;
    if (text == "w" || text == "wq" || text == "write"){
        emit_save();
        handled = true;
    }

    if (text == "q" || text == "quit" || text == "wq"){
        emit_quit();
        handled = true;
    }
    if (text == "q!" || text == "quit!"){
        if (line_edit){
            emit line_edit->forceQuitCommand();
        }
        if (text_edit){
            emit text_edit->forceQuitCommand();
        }
        handled = true;
    }

    if (!handled) {
        std::cerr << "Unknown command: " << text.toStdString();
    }
}

int VimEditor::get_cursor_position() const {
    return adapter->get_cursor_position();
}

void VimEditor::delete_selection() {
    int selection_begin, selection_end;
    QString selected_text = get_current_selection(selection_begin, selection_end);
    remove_text(selection_begin, selection_end - selection_begin);
    set_cursor_position(selection_begin);
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
    text_edit->viewport()->update();
}

void QTextEditAdapter::set_extra_selections(const QList<QTextEdit::ExtraSelection> &selections) {
    text_edit->setExtraSelections(selections);

}

void QLineEditAdapter::set_extra_selections(const QList<QTextEdit::ExtraSelection> &selections) {
}

void QTextEditAdapter::set_text(QString text) {
    if (text_edit->isReadOnly()) {
        return;
    }
    QScrollBar *vertical_scroll_bar = text_edit->verticalScrollBar();
    QScrollBar *horizontal_scroll_bar = text_edit->horizontalScrollBar();
    int vertical_scroll_value = vertical_scroll_bar->value();
    int horizontal_scroll_value = horizontal_scroll_bar->value();

    text_edit->setPlainText(text);

    vertical_scroll_bar->setValue(vertical_scroll_value);
    horizontal_scroll_bar->setValue(horizontal_scroll_value);
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
    QTextCursor cursor = text_edit->textCursor();
    auto extra_selections = text_edit->extraSelections();
    for (auto selection : extra_selections){
        if (selection.format.property(QTextFormat::UserProperty).toInt() != SEARCH_HIGHLIGHT_PROPERTY_INDEX){
            cursor = selection.cursor;
            break;
        }
    }


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
    for (QObject *child : children()) {
        if (child->inherits("QWidgetLineControl")) {
            child->installEventFilter(this);
            break;
        }
    }
    editor = new VimEditor(this);
}

bool VimLineEdit::eventFilter(QObject *watched, QEvent *event) {
    if (vim_enabled && event->type() == QEvent::Timer) {
        return true;
    }
    return QLineEdit::eventFilter(watched, event);
}

void VimLineEdit::keyPressEvent(QKeyEvent *event) {
    if (!vim_enabled){
        return QLineEdit::keyPressEvent(event);
    }

    bool should_propagate = false;
    bool should_handle_with_qlineedit = editor->key_press_event(event, should_propagate);
    if (should_handle_with_qlineedit){
        return QLineEdit::keyPressEvent(event);
    }

    if (should_propagate){
        event->ignore();
    }
    else {
        event->accept();
    }
}

void VimLineEdit::resizeEvent(QResizeEvent *event) {
    editor->command_line_edit->resize(event->size().width(), editor->command_line_edit->height());
    editor->command_line_edit->move(0, height() - editor->command_line_edit->height());
    QLineEdit::resizeEvent(event);
}

LineNumberArea::LineNumberArea(VimTextEdit *editor) : QWidget(editor), text_edit(editor) {
    setStyleSheet("background-color: transparent;");
}

QSize LineNumberArea::sizeHint() const {
    return QSize(text_edit->line_number_area_width(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent *event) {
    text_edit->line_number_area_paint_event(event);
}

VimTextEdit::VimTextEdit(QWidget *parent) : QTextEdit(parent) {
    for (QObject *child : children()) {
        if (child->inherits("QWidgetTextControl")) {
            child->installEventFilter(this);
            break;
        }
    }
    editor = new VimEditor(this);
    line_number_area = new LineNumberArea(this);
    line_number_area->hide();

    QObject::connect(document(), &QTextDocument::blockCountChanged, this, [this]() {
        update_line_number_area_width();
        update_line_number_area();
    });
    QObject::connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        update_line_number_area();
    });
    QObject::connect(this, &QTextEdit::textChanged, this, [this]() {
        update_line_number_area();
    });
    QObject::connect(this, &QTextEdit::cursorPositionChanged, this, [this]() {
        update_line_number_area();
    });
    update_line_number_area_width();
}

bool VimTextEdit::eventFilter(QObject *watched, QEvent *event) {
    if (vim_enabled && event->type() == QEvent::Timer) {
        return true;
    }
    return QTextEdit::eventFilter(watched, event);
}

QList<QTextCursor> VimTextEdit::selection_cursors() const {
    if (!vim_enabled) {
        return {textCursor()};
    }
    const auto mode = editor->get_mode();
    if (mode != VimMode::Visual && mode != VimMode::VisualLine && mode != VimMode::VisualBlock) {
        return {};
    }
    QList<QTextCursor> cursors;
    for (const auto& selection : extraSelections()) {
        if (selection.format.property(QTextFormat::UserProperty).toInt() != SEARCH_HIGHLIGHT_PROPERTY_INDEX) {
            cursors.append(selection.cursor);
        }
    }
    return cursors;
}

void VimTextEdit::keyPressEvent(QKeyEvent *event) {

    if (!vim_enabled) {
        using Key = QKeySequence::StandardKey;
        struct Motion {
            Key move;
            Key select;
            QTextCursor::MoveOperation operation;
            bool forward;
        };
        static const Motion motions[] = {
            {Key::MoveToNextChar, Key::SelectNextChar, QTextCursor::Right, true},
            {Key::MoveToPreviousChar, Key::SelectPreviousChar, QTextCursor::Left, false},
            {Key::MoveToNextWord, Key::SelectNextWord, QTextCursor::WordRight, true},
            {Key::MoveToPreviousWord, Key::SelectPreviousWord, QTextCursor::WordLeft, false},
            {Key::MoveToNextLine, Key::SelectNextLine, QTextCursor::Down, true},
            {Key::MoveToPreviousLine, Key::SelectPreviousLine, QTextCursor::Up, false},
            {Key::MoveToNextPage, Key::SelectNextPage, QTextCursor::Down, true},
            {Key::MoveToPreviousPage, Key::SelectPreviousPage, QTextCursor::Up, false},
        };
        for (const auto& motion : motions) {
            if (event->matches(motion.move) || event->matches(motion.select)) {
                auto cursor = textCursor();
                if (!cursor.movePosition(motion.operation)) {
                    emit textBoundaryRequested(motion.forward);
                }
                break;
            }
        }
        return QTextEdit::keyPressEvent(event);
    }

    bool should_propagate = false;
    bool should_handle_with_qlineedit = editor->key_press_event(event, should_propagate);
    if (should_handle_with_qlineedit){
        return QTextEdit::keyPressEvent(event);
    }

    if (should_propagate){
        event->ignore();
    }
    else {
        event->accept();
    }
}

void VimTextEdit::resizeEvent(QResizeEvent *event) {
    editor->command_line_edit->resize(event->size().width(), editor->command_line_edit->height());
    editor->command_line_edit->move(0, height() - editor->command_line_edit->height());
    QTextEdit::resizeEvent(event);
    update_line_number_area();
}


void VimLineEdit::set_vim_enabled(bool enabled){
    if (vim_enabled == enabled) {
        return;
    }
    vim_enabled = enabled;
    if (editor) {
        editor->update_cursor_style();
    }
}

bool VimLineEdit::get_vim_enabled(){
    return vim_enabled;
}

void VimTextEdit::set_vim_enabled(bool enabled){
    if (vim_enabled == enabled) {
        return;
    }
    vim_enabled = enabled;
    if (editor) {
        editor->update_cursor_style();
    }
}

bool VimTextEdit::get_vim_enabled(){
    return vim_enabled;
}

void VimTextEdit::set_line_numbers_visible(bool visible){
    if (line_numbers_visible == visible){
        return;
    }

    line_numbers_visible = visible;
    line_number_area->setVisible(visible);
    update_line_number_area_width();
    update_line_number_area();
}

bool VimTextEdit::get_line_numbers_visible() const{
    return line_numbers_visible;
}

int VimTextEdit::line_number_area_width() const{
    if (!line_numbers_visible){
        return 0;
    }

    int digits = 1;
    int max_line = std::max(1, document()->blockCount());
    while (max_line >= 10) {
        max_line /= 10;
        ++digits;
    }

    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void VimTextEdit::update_line_number_area_width(){
    setViewportMargins(line_number_area_width(), 0, 0, 0);
}

void VimTextEdit::update_line_number_area(){
    if (!line_number_area){
        return;
    }

    QRect cr = contentsRect();
    line_number_area->setGeometry(QRect(cr.left(), cr.top(), line_number_area_width(), cr.height()));
    if (line_numbers_visible){
        line_number_area->update();
    }
}

void VimTextEdit::line_number_area_paint_event(QPaintEvent *event){
    if (!line_numbers_visible){
        return;
    }

    QPainter painter(line_number_area);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::Mid));

    QTextBlock block = document()->firstBlock();
    int block_number = 1;
    QAbstractTextDocumentLayout *layout = document()->documentLayout();
    QPointF offset(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    QColor highlight_color = get_darker_color(palette().color(QPalette::Window));

    while (block.isValid()) {
        QRectF block_rect = layout->blockBoundingRect(block).translated(offset);
        int top = qRound(block_rect.top());
        int bottom = qRound(block_rect.bottom());

        // highlight the focused line number
        if (textCursor().blockNumber() == block_number - 1) {
            painter.fillRect(0, top, line_number_area->width(), fontMetrics().height(), highlight_color);
        }
        if (bottom >= event->rect().top() && top <= event->rect().bottom()) {
            painter.drawText(0, top, line_number_area->width() - 4, fontMetrics().height(),
                             Qt::AlignRight, QString::number(block_number));
        }

        if (top > event->rect().bottom()) {
            break;
        }

        if (textCursor().blockNumber() == block_number - 1) {
            QString current_line_text = block.text();
            custom_current_line_painter(block_number, current_line_text, &painter, QRect(0, top, line_number_area->width(), fontMetrics().height()));
        }

        block = block.next();
        ++block_number;
    }
}

void VimTextEdit::custom_current_line_painter(int line_number, QString line_text, QPainter *painter, const QRect &rect){

}

void VimEditor::goto_line(int line_number){
    int pos = get_ith_line_start_position(line_number);
    adapter->set_cursor_position(pos);
}

void VimEditor::center_on_cursor(){
    // try to center the view on the cursor position
    QTextEdit* text_edit = dynamic_cast<QTextEdit*>(editor_widget);
    if (text_edit) {
        QScrollBar *v_bar = text_edit->verticalScrollBar();
        if (v_bar) {
            int cursor_y = text_edit->cursorRect().top();
            int viewport_h = text_edit->viewport()->height();
            int diff = cursor_y - viewport_h / 2;
            v_bar->setValue(v_bar->value() + diff);
        }
    }
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
        if (!showing_suggestion_menu){
            emit focusLost();
        }
    }
    return QTextEdit::focusOutEvent(event);
}

void VimEditor::push_current_history_state() {
    push_history({adapter->get_text(), get_cursor_position(), marks});
}
void VimEditor::add_jump(int pos) {
    if (jump_index >= 0 && jump_index < static_cast<int>(jumps.size()) - 1) {
        jumps.erase(jumps.begin() + jump_index + 1, jumps.end());
    }
    if (jumps.empty() || jumps.back() != pos) {
        jumps.push_back(pos);
        if (jumps.size() > 100) {
            jumps.erase(jumps.begin());
        }
    }
    jump_index = static_cast<int>(jumps.size());
}
bool is_separator(QChar ch){
    return ch.isSpace() || ch == ';' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '(' || ch == ')';
}

QString VimEditor::get_word_under_cursor(){
    // get the word under the cursor
    QString text = adapter->get_text();
    int cursor_pos = get_cursor_position();

    if (text.isEmpty()){
        return "";
    }

    if (cursor_pos >= text.length()){
        cursor_pos = text.length() - 1;
    }

    if (is_separator(text[cursor_pos])){
        return "";
    }

    int pos = cursor_pos;
    while (pos >= 0 && !is_separator(text[pos])){
        pos--;
    }

    int word_begin = pos + 1;
    if (pos == 0 && !is_separator(text[0])){
        word_begin = 0;
    }

    pos = cursor_pos;
    while (pos < text.length() && !is_separator(text[pos])){
        pos++;
    }

    int word_end = pos;

    return text.mid(word_begin, word_end - word_begin);

}

QString VimEditor::get_previous_word(){
    // get the word before the cursor
    QString text = adapter->get_text();
    int cursor_pos = get_cursor_position();

    if (cursor_pos == 0){
        return "";
    }
    int pos = cursor_pos - 1;
    // skip any whitespace before the word
    while (pos >= 0 && !is_separator(text[pos])){
        pos--;
    }

    int word_begin = pos + 1;
    if (pos == 0 && !is_separator(text[0])){
        word_begin = 0;
    }

    int word_end = cursor_pos;

    return text.mid(word_begin, word_end - word_begin);

}

class SuggestionMenu : public QMenu {
public:
    explicit SuggestionMenu(QWidget *parent = nullptr) : QMenu(parent) {}

protected:
    void keyPressEvent(QKeyEvent *event) override {
        bool is_navigation_modifier = event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier);
        if (is_navigation_modifier) {
            if (event->key() == Qt::Key_N) {
                move_selection(1);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_P) {
                move_selection(-1);
                event->accept();
                return;
            }
        }

        QMenu::keyPressEvent(event);
    }

private:
    void move_selection(int delta) {
        QList<QAction *> actions = this->actions();
        if (actions.isEmpty()) {
            return;
        }

        int current_index = actions.indexOf(activeAction());
        if (current_index < 0) {
            current_index = 0;
        }

        int next_index = current_index + delta;
        if (next_index >= static_cast<int>(actions.size())) {
            next_index = 0;
        }
        else if (next_index < 0) {
            next_index = static_cast<int>(actions.size()) - 1;
        }
        setActiveAction(actions[next_index]);
    }
};

QStringList VimTextEdit::get_autocomplete_suggestions(const QString &current_word){
    return {};
}

void VimTextEdit::show_documentation(const QString &word){
}


void VimTextEdit::show_autocomplete_suggestions(const QStringList &suggestions){
    if (suggestions.isEmpty()) {
        return;
    }

    SuggestionMenu popup(this);
    QList<QAction *> actions;
    actions.reserve(suggestions.size());
    for (const QString &suggestion : suggestions) {
        QAction *action = popup.addAction(suggestion);
        actions.append(action);
        connect(action, &QAction::triggered, this, [this, suggestion]() {
            QTextCursor cursor = textCursor();
            int cursor_pos = cursor.position();
            QString text = toPlainText();
            int start = cursor_pos;

            while (start > 0 && (text[start - 1].isLetterOrNumber() || text[start - 1] == '_')) {
                --start;
            }

            QTextCursor replacement_cursor = textCursor();
            replacement_cursor.setPosition(start);
            replacement_cursor.setPosition(cursor_pos, QTextCursor::KeepAnchor);
            replacement_cursor.insertText(suggestion);
            setTextCursor(replacement_cursor);
        });
    }

    if (!actions.isEmpty()) {
        popup.setActiveAction(actions.first());
    }

    QColor highlight_color = palette().color(QPalette::Highlight);
    popup.setStyleSheet(QString("QMenu::item:selected { background-color: %1; }").arg(highlight_color.name()));

    QRect cursor_rect = cursorRect();
    showing_suggestion_menu = true;
    popup.exec(mapToGlobal(cursor_rect.bottomLeft()));
    showing_suggestion_menu = false;
}

}
