#include <algorithm>
#include <climits>
#include "VimLineEdit.h"
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QKeySequence>
#include <QPointer>
#include <QScopedValueRollback>
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
#include <QLabel>

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
        resume_insert_mode();
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
    bool was_temporary_normal = resume_insert_after_command;
    if (replaying_change) {
        bool result = process_key_press_event(event, should_propagate);
        if (was_temporary_normal) resume_insert_mode();
        return result;
    }
    bool was_normal = current_mode == VimMode::Normal;
    if (!recording_change && was_normal) {
        recording_change = true;
        change_start_text = adapter->get_text();
        change_keys.clear();
    }
    if (recording_change) change_keys.emplace_back(event->clone());
    bool result = process_key_press_event(event, should_propagate);
    if (was_temporary_normal) resume_insert_mode();
    if (pending_text_command) {
        // Preserve an operator's keys; its search motion is recorded on submission.
        if (!action_waiting_for_motion) {
            recording_change = false;
            change_keys.clear();
        }
    } else if (current_mode == VimMode::Normal && !resume_insert_after_command && !action_waiting_for_motion &&
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

void VimEditor::resume_insert_mode() {
    if (!resume_insert_after_command || action_waiting_for_motion || pending_symbol_command ||
        pending_text_command || current_node || !current_command_repeat_number.isEmpty() || current_paste_register) return;
    resume_insert_after_command = false;
    if (current_mode == VimMode::Normal) {
        current_mode = VimMode::Insert;
        set_style_for_mode(current_mode);
    }
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
                handle_command(command, event->text().at(0));
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
                case Qt::Key_S:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Sentence;
                    break;
                case Qt::Key_P:
                    action_waiting_for_motion->surrounding_kind = SurroundingKind::Paragraph;
                    break;
                case Qt::Key_W:
                    action_waiting_for_motion->surrounding_kind = event->text() == "W" ? SurroundingKind::WORD : SurroundingKind::Word;
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

        if (!is_keypress_a_modifier && waiting_for_insert_register) {
            waiting_for_insert_register = false;
            if (!event->text().isEmpty()) {
                auto contents = get_last_deleted_text(event->text().at(0).toLatin1());
                if (contents) {
                    QString text = contents->text + (contents->is_line ? "\n" : "");
                    int cursor = get_cursor_position();
                    insert_text(text, cursor);
                    set_cursor_position(cursor + text.size());
                    current_insert_mode_text += text;
                }
            }
            return false;
        }

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
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"e", {}}}, VimLineEditCommand::MoveToPreviousWordEnd},
        KeyBinding{{KeyChord{"g", {}}, KeyChord{"E", {}}}, VimLineEditCommand::MoveToPreviousWORDEnd},
        KeyBinding{{KeyChord{"B", {}}}, VimLineEditCommand::MoveWordBackwardWithSymbols},
        KeyBinding{{KeyChord{"t", {}}}, VimLineEditCommand::FindForwardTo},
        KeyBinding{{KeyChord{"T", {}}}, VimLineEditCommand::FindBackwardTo},
        KeyBinding{{KeyChord{"x", {}}}, VimLineEditCommand::DeleteChar},
        KeyBinding{{KeyChord{"X", {}}}, VimLineEditCommand::DeleteCharBackward},
        KeyBinding{{KeyChord{"S", {}}}, VimLineEditCommand::ChangeCurrentLine},
        KeyBinding{{KeyChord{"Y", {}}}, VimLineEditCommand::YankCurrentLine},
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
        KeyBinding{{KeyChord{"'", {}}}, VimLineEditCommand::GotoMarkLine},
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
        KeyBinding{{KeyChord{"u", {}}}, VimLineEditCommand::Lowercase},
        KeyBinding{{KeyChord{"o", {}}}, VimLineEditCommand::ToggleVisualCursor},
        KeyBinding{{KeyChord{"U", {}}}, VimLineEditCommand::Uppercasify},
        KeyBinding{{KeyChord{"~", {}}}, VimLineEditCommand::SwapCaseSelection},
    };

    for (const auto &binding : visual_mode_keybindings) {
        visual_mode_input_tree.add_keybinding(binding.key_chords, 0, binding.command);
    }

    std::vector<KeyBinding> insert_mode_keybindings = {
        KeyBinding{{KeyChord{Qt::Key_O, CONTROL}}, VimLineEditCommand::NormalCommandFromInsert},
        KeyBinding{{KeyChord{Qt::Key_R, CONTROL}}, VimLineEditCommand::InsertRegister},
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
    case VimLineEditCommand::DeleteCharBackward:
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
    case VimLineEditCommand::GotoMarkLine:
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
    case VimLineEditCommand::GotoMarkLine:
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
    case VimLineEditCommand::GotoMarkLine:
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

void VimEditor::handle_command(VimLineEditCommand cmd, std::optional<QChar> symbol) {

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
        case VimLineEditCommand::DeleteCharBackward:
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
            new_macro.name = symbol->unicode();
            current_macro = std::move(new_macro);
        }
        break;
    case VimLineEditCommand::SelectPasteRegister:
        current_paste_register = symbol->toLatin1();
        break;
    case VimLineEditCommand::RepeatMacro: {
        int macro_symbol = symbol->unicode();

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
            handle_search(false, num_repeats);
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
        new_pos = get_cursor_position();
        for (int i = 1; i < num_repeats && get_line_end_position(new_pos) < current_state.text.size(); ++i) {
            new_pos = get_line_end_position(new_pos) + 1;
        }
        new_pos = std::max(get_line_start_position(new_pos), get_line_end_position(new_pos) - 1);
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
    case VimLineEditCommand::NormalCommandFromInsert:
        // Keep the insert session intact while one normal command is pending.
        current_mode = VimMode::Normal;
        set_style_for_mode(current_mode);
        resume_insert_after_command = true;
        new_pos = std::min(old_pos, std::max(get_line_start_position(old_pos), get_line_end_position(old_pos) - 1));
        break;
    case VimLineEditCommand::InsertRegister:
        waiting_for_insert_register = true;
        break;
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
            QString unit = (repeat_insert_on_new_lines ? "\n" : "") + current_insert_mode_text;
            QString repeated = unit.repeated(insert_repeat_count - 1);
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
        size_t motion_count_begin = last_change_keys.size(), motion_count_end = motion_count_begin;
        if (has_explicit_count && first < last_change_keys.size()) {
            size_t command = first;
            if (last_change_keys[command]->text() == "\"") command += 2;
            if (command < last_change_keys.size()) {
                QString key = last_change_keys[command]->text();
                if (key.size() == 1 && QString("dcy<>=").contains(key)) motion_count_begin = command + 1;
                if (key == "g" && command + 1 < last_change_keys.size() &&
                    (last_change_keys[command + 1]->text() == "u" || last_change_keys[command + 1]->text() == "U")) {
                    motion_count_begin = command + 2;
                }
            }
            motion_count_end = motion_count_begin;
            while (motion_count_end < last_change_keys.size() && last_change_keys[motion_count_end]->text().size() == 1 &&
                   last_change_keys[motion_count_end]->text().at(0).isDigit()) ++motion_count_end;
        }
        for (size_t i = first; i < last_change_keys.size(); ++i) {
            if (i < motion_count_begin || i >= motion_count_end) events.emplace_back(last_change_keys[i]->clone());
        }
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
        for (int i = 0; i < num_repeats && history.current_index >= 0; ++i) undo();
        break;
    case VimLineEditCommand::Redo:
        for (int i = 0; i < num_repeats && history.current_index + 1 < static_cast<int>(history.states.size()); ++i) redo();
        break;
    case VimLineEditCommand::MoveToPreviousWordEnd:
    case VimLineEditCommand::MoveToPreviousWORDEnd: {
        const QString &text = current_state.text;
        auto category = [&](int pos) {
            if (pos >= text.size() || text[pos].isSpace()) return 0;
            return cmd == VimLineEditCommand::MoveToPreviousWORDEnd || text[pos].isLetterOrNumber() || text[pos] == '_' ? 1 : 2;
        };
        new_pos = old_pos;
        for (int i = 0; i < num_repeats && new_pos > 0; ++i) {
            int type = category(new_pos);
            --new_pos;
            while (new_pos > 0 && type != 0 && category(new_pos) == type) --new_pos;
            while (new_pos > 0 && text[new_pos].isSpace()) --new_pos;
        }
        if (action_waiting_for_motion && action_waiting_for_motion->kind != ActionWaitingForMotionKind::Visual) {
            handle_action_waiting_for_motion(new_pos, old_pos, 1);
            new_pos = -1;
        }
        break;
    }
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
        if (current_mode == VimMode::VisualBlock) {
            apply_transform_to_visual_block([&](const QString &text) {
                return cmd == VimLineEditCommand::Lowercase ? text.toLower() : text.toUpper();
            });
            action_waiting_for_motion = {};
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            pending_operator_count = 1;
        } else if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine) {
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
        if (!symbol) break;
        if (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock) {
            push_history(current_state);
            auto replace = [&](QString text) {
                for (QChar &ch : text) if (ch != '\n') ch = QChar(*symbol);
                return text;
            };
            if (current_mode == VimMode::VisualBlock) {
                apply_transform_to_visual_block(replace);
                auto bounds = get_visual_block_bounds();
                new_pos = bounds.line_starts[bounds.min_line] + bounds.min_col;
            } else {
                int begin, end;
                QString text = get_current_selection(begin, end);
                insert_text(replace(text), begin, end);
                new_pos = begin;
            }
            action_waiting_for_motion = {};
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            break;
        }
        int end = get_line_end_position(old_pos);
        if (!symbol || num_repeats > end - old_pos) break;
        push_history(current_state);
        insert_text(QString(num_repeats, QChar(*symbol)), old_pos, old_pos + num_repeats);
        new_pos = old_pos + num_repeats - 1;
        break;
    }
    case VimLineEditCommand::DeleteCharAndEnterInsertMode:
    case VimLineEditCommand::DeleteCharBackward:
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
            int begin = cmd == VimLineEditCommand::DeleteCharBackward ?
                std::max(get_line_start_position(old_pos), old_pos - num_repeats) : old_pos;
            int end = cmd == VimLineEditCommand::DeleteCharBackward ? old_pos :
                old_pos + std::min(get_line_end_position(old_pos) - old_pos, num_repeats);
            if (end > begin) {
                set_last_deleted_text(current_state.text.mid(begin, end - begin), current_paste_register);
                remove_text(begin, end - begin);
                int cursor = begin;
                if (cmd != VimLineEditCommand::DeleteCharAndEnterInsertMode) {
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
        for (int i = 1; i < num_repeats && line_end < current_state.text.size(); ++i) {
            line_end = get_line_end_position(line_end + 1);
        }
        if (num_repeats > 1 && cmd == VimLineEditCommand::DeleteToEndOfLine &&
            cursor_pos == get_line_start_position(cursor_pos)) {
            action_waiting_for_motion = {ActionWaitingForMotionKind::Delete};
            handle_action_waiting_for_motion(cursor_pos, std::max(cursor_pos, line_end - 1), 0, true);
            break;
        }
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
        mark.name = symbol->unicode();
        mark.position = get_cursor_position();
        marks[symbol->unicode()] = mark;
        break;
    }
    case VimLineEditCommand::GotoMarkLine:
    case VimLineEditCommand::GotoMark: {
        if (marks.find(symbol->unicode()) != marks.end()) {
            Mark mark = marks[symbol->unicode()];
            // move the cursor to the mark location
            new_pos = mark.position;
            if (cmd == VimLineEditCommand::GotoMarkLine) {
                new_pos = get_line_start_position(new_pos);
                while (new_pos < get_line_end_position(new_pos) && current_state.text[new_pos].isSpace()) ++new_pos;
            }
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
        handle_search(false, num_repeats);
        break;
    }
    case VimLineEditCommand::RepeatSearchReverse: {
        handle_search(true, num_repeats);
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
            if (last_deleted_text->block_width > 0) {
                paste_block(*last_deleted_text, cmd == VimLineEditCommand::PasteForward, num_repeats);
                break;
            }
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
            if (last_deleted_text->block_width > 0) {
                paste_block(*last_deleted_text, cmd == VimLineEditCommand::PasteForward, num_repeats);
                break;
            }
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
        insert_repeat_count = num_repeats;
        repeat_insert_on_new_lines = true;
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
        insert_repeat_count = num_repeats;
        repeat_insert_on_new_lines = true;
        break;
    }
    case VimLineEditCommand::SearchCommand:
    case VimLineEditCommand::ReverseSearchCommand:
    case VimLineEditCommand::CommandCommand: {
        pending_text_command = cmd;
        pending_search_count = num_repeats;
        QString initial = get_initial_command_text(cmd);
        if (cmd == VimLineEditCommand::CommandCommand &&
            (current_mode == VimMode::Visual || current_mode == VimMode::VisualLine || current_mode == VimMode::VisualBlock)) {
            marks['<'] = {std::min(visual_mode_anchor, old_pos), '<'};
            marks['>'] = {std::max(visual_mode_anchor, old_pos), '>'};
            initial += "'<,'>";
            action_waiting_for_motion = {};
            set_mode(VimMode::Normal);
            adapter->set_extra_selections({});
            visual_line_selection_begin = visual_line_selection_end = -1;
        }
        show_command_line_edit(initial);
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
            if (cmd != VimLineEditCommand::Yank) {
                if (cmd == VimLineEditCommand::Change && end > start && current_state.text[end - 1] == '\n') {
                    --end;
                }
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
                            cmd == VimLineEditCommand::TransformCurrentLine || cmd == VimLineEditCommand::GotoMarkLine;
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

void VimEditor::apply_text_object(int begin, int end, bool linewise) {
    if (action_waiting_for_motion->kind == ActionWaitingForMotionKind::Visual) {
        visual_mode_anchor = begin;
        set_cursor_position(std::max(begin, end - 1));
        set_visual_selection(begin, end - begin);
        action_waiting_for_motion = {};
    } else {
        handle_action_waiting_for_motion(begin, linewise ? std::max(begin, end - 1) : end, 0, linewise);
    }
    current_command_repeat_number.clear();
    pending_operator_count = 1;
    current_paste_register = {};
}

bool VimEditor::handle_surrounding_motion_action() {
    if (action_waiting_for_motion.has_value()) {
        if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Sentence) {
            QString text = adapter->get_text();
            static const QRegularExpression ending(R"vim([.!?][)"'\]]*(?=\s|$)|\n(?=\n))vim");
            std::vector<std::pair<int, int>> parts;
            for (int begin = 0; begin < text.size();) {
                int end = begin;
                if (text[begin].isSpace()) {
                    while (end < text.size() && text[end].isSpace()) ++end;
                } else {
                    auto match = ending.match(text, begin);
                    end = match.hasMatch() ? match.capturedEnd() : text.size();
                    while (end > begin && text[end - 1].isSpace()) --end;
                }
                parts.emplace_back(begin, end);
                begin = end;
            }
            if (parts.empty()) { action_waiting_for_motion = {}; return true; }
            int first = 0;
            while (first + 1 < static_cast<int>(parts.size()) && parts[first].second <= get_cursor_position()) ++first;
            int last = first;
            bool around = action_waiting_for_motion->surrounding_scope == SurroundingScope::Around;
            int count = std::min<qint64>(parts.size(),
                static_cast<qint64>(std::max(1, current_command_repeat_number.toInt())) * pending_operator_count);
            for (int i = 0; i < count && last < static_cast<int>(parts.size()); ++i) {
                bool whitespace = text[parts[last].first].isSpace();
                ++last;
                if (around && last < static_cast<int>(parts.size()) &&
                    text[parts[last].first].isSpace() != whitespace) ++last;
            }
            if (around && last == static_cast<int>(parts.size()) && !text[parts[last - 1].first].isSpace() &&
                first > 0 && text[parts[first - 1].first].isSpace()) --first;
            apply_text_object(parts[first].first, parts[last - 1].second);
            return true;
        }
        if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Paragraph) {
            QString text = adapter->get_text();
            auto starts = get_line_starts(text);
            int line, column;
            get_line_and_col(get_cursor_position(), line, column);
            auto blank = [&](int i) { return starts[i] == get_line_end_position(starts[i]); };
            int first = line, last = line;
            while (first > 0 && blank(first - 1) == blank(line)) --first;
            bool around = action_waiting_for_motion->surrounding_scope == SurroundingScope::Around;
            int count = std::min<qint64>(starts.size(),
                static_cast<qint64>(std::max(1, current_command_repeat_number.toInt())) * pending_operator_count);
            for (int i = 0; i < count && last < static_cast<int>(starts.size()); ++i) {
                bool empty = blank(last);
                do { ++last; } while (last < static_cast<int>(starts.size()) && blank(last) == empty);
                if (around) {
                    while (last < static_cast<int>(starts.size()) && blank(last) != empty) ++last;
                }
            }
            if (around && last == static_cast<int>(starts.size()) && !blank(last - 1)) {
                while (first > 0 && blank(first - 1)) --first;
            }
            apply_text_object(starts[first], last < static_cast<int>(starts.size()) ? starts[last] : text.size(), true);
            return true;
        }
        if (action_waiting_for_motion->surrounding_kind == SurroundingKind::Word ||
            action_waiting_for_motion->surrounding_kind == SurroundingKind::WORD) {
            QString text = adapter->get_text();
            int start = get_cursor_position();
            if (start >= text.size()) {
                action_waiting_for_motion = {};
                return true;
            }
            bool big_word = action_waiting_for_motion->surrounding_kind == SurroundingKind::WORD;
            bool around = action_waiting_for_motion->surrounding_scope == SurroundingScope::Around;
            auto category = [&](int pos) {
                QChar ch = text[pos];
                return ch.isSpace() ? 0 : (big_word || ch.isLetterOrNumber() || ch == '_' ? 1 : 2);
            };
            int type = category(start);
            while (start > 0 && text[start - 1] != '\n' && category(start - 1) == type) --start;
            int end = start;
            int count = std::max(1, current_command_repeat_number.toInt()) * pending_operator_count;
            for (int i = 0; i < count && end < text.size(); ++i) {
                type = category(end);
                do { ++end; } while (end < text.size() && category(end) == type);
                if (around && type == 0 && end < text.size()) {
                    type = category(end);
                    do { ++end; } while (end < text.size() && category(end) == type);
                }
                if (around && i + 1 < count) {
                    while (end < text.size() && text[end].isSpace()) ++end;
                }
            }
            if (around && category(get_cursor_position()) != 0) {
                int before_spaces = end;
                while (end < text.size() && text[end] != '\n' && text[end].isSpace()) ++end;
                if (end == before_spaces) {
                    while (start > 0 && text[start - 1] != '\n' && text[start - 1].isSpace()) --start;
                }
            }
            apply_text_object(start, end);
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
            if (begin_symbol == end_symbol) {
                int opening = -1;
                int line_start = get_line_start_position(cursor_pos);
                int line_end = get_line_end_position(cursor_pos);
                for (int i = line_start; i < line_end; ++i) {
                    if (current_text[i] == '\\') { ++i; continue; }
                    if (current_text[i] != begin_symbol) continue;
                    if (opening == -1) { opening = i; continue; }
                    if (i >= cursor_pos) {
                        int begin = opening, end = i + 1;
                        if (action_waiting_for_motion->surrounding_scope == SurroundingScope::Inside) {
                            ++begin;
                            --end;
                        } else {
                            while (end < line_end && current_text[end].isSpace()) ++end;
                            if (end == i + 1) {
                                while (begin > line_start && current_text[begin - 1].isSpace()) --begin;
                            }
                        }
                        apply_text_object(begin, end);
                        return true;
                    }
                    opening = -1;
                }
                action_waiting_for_motion = {};
                return true;
            }
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

            if (found_begin && found_end) {
                apply_text_object(start, end);
                return true;
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

void VimEditor::paste_block(const LastDeletedTextState &contents, bool after, int count) {
    int line, column;
    get_line_and_col(get_cursor_position(), line, column);
    if (after && get_cursor_position() < get_line_end_position(get_cursor_position())) ++column;
    QStringList rows = contents.text.split('\n');
    int first_position = 0;
    for (int i = 0; i < rows.size(); ++i) {
        auto starts = get_line_starts(adapter->get_text());
        if (line + i >= static_cast<int>(starts.size())) {
            insert_text("\n", adapter->get_text().size());
            starts = get_line_starts(adapter->get_text());
        }
        int start = starts[line + i];
        int length = get_line_end_position(start) - start;
        if (column > length) insert_text(QString(column - length, ' '), start + length);
        insert_text(rows[i].leftJustified(contents.block_width, ' ').repeated(count), start + column);
        if (i == 0) first_position = start + column;
    }
    set_cursor_position(first_position);
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
    set_last_deleted_text(yanked_block.join("\n"), current_paste_register, false, false,
                          bounds.max_col - bounds.min_col + 1);

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
            handle_search(false, pending_search_count);
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

void VimEditor::handle_search(bool reverse, int count){
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
        int match = std::find(found_indices.begin(), found_indices.end(), target_index) - found_indices.begin();
        int size = found_indices.size();
        int offset = (count - 1) % size;
        target_index = found_indices[(match + (is_reversed ? -offset : offset) + size) % size];
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

void VimEditor::set_last_deleted_text(QString text, std::optional<char> reg, bool is_line, bool is_yank, int block_width) {
    if (reg == '_') return;
    LastDeletedTextState state{text, is_line, block_width};
    is_yank = is_yank || (action_waiting_for_motion && action_waiting_for_motion->kind == ActionWaitingForMotionKind::Yank);
    if (is_yank && (!reg || reg == '"')) paste_registers['0'] = state;
    if (!is_yank && !text.isEmpty()) {
        if (is_line || text.contains('\n')) {
            for (int name = '9'; name > '1'; --name) {
                auto previous = paste_registers.find(name - 1);
                if (previous != paste_registers.end()) paste_registers[name] = previous->second;
                else paste_registers.erase(name);
            }
            paste_registers['1'] = state;
        } else if (!reg || reg == '"') {
            paste_registers['-'] = state;
        }
    }
    if (reg && *reg >= 'A' && *reg <= 'Z') {
        int name = *reg - 'A' + 'a';
        auto previous = paste_registers.find(name);
        if (previous != paste_registers.end()) {
            state.text = previous->second.text + (previous->second.is_line ? "\n" : "") + state.text;
            state.is_line = previous->second.is_line || state.is_line;
            state.block_width = 0;
        }
        reg = static_cast<char>(name);
    }
    last_deleted_text_ = state;
    if (reg && *reg != '"') {
        if (*reg == '+' || *reg == '*') QApplication::clipboard()->setText(text);
        else paste_registers[*reg] = state;
    }
}

std::optional<LastDeletedTextState> VimEditor::get_last_deleted_text(std::optional<char> reg){
    if (reg && *reg >= 'A' && *reg <= 'Z') reg = static_cast<char>(*reg - 'A' + 'a');
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
        waiting_for_insert_register = false;
        replacing = false;
        replace_steps.clear();
    }
    if (current_mode == VimMode::Insert){
        last_insert_mode_text = current_insert_mode_text;
    }

    current_mode = mode;

    if (mode == VimMode::Insert){
        repeat_insert_on_new_lines = false;
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
    static const QRegularExpression prefix(R"(^(%|(?:[.$0-9]+|'[<>])(?:,(?:[.$0-9]+|'[<>]))?)?s([^\w\s\\]))");
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
        if (value == "'<" || value == "'>") {
            auto mark = marks.find(value[1].unicode());
            int line = current_line, column = 0;
            if (mark != marks.end()) get_line_and_col(mark->second.position, line, column);
            return line;
        }
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

// Emacs state is independent of Vim's modal commands and registers. Edits use
// the native widgets so their undo history, signals, and read-only flags work.
class EmacsEditor : public QObject {
    QLineEdit *line;
    QTextEdit *multi;
    bool meta_pending = false;
    std::optional<int> argument;
    bool argument_digits = false;
    bool argument_negative = false;

    void clear_argument() {
        argument.reset();
        argument_digits = argument_negative = false;
    }
    int character_target(int count) const {
        const auto value = text();
        int target = position();
        while (count > 0 && target < value.size()) { target = next_character(value, target); --count; }
        while (count < 0 && target > 0) { target = previous_character(value, target); ++count; }
        return target;
    }
    std::optional<int> desired_column;
    int mark = -1;
    std::deque<int> mark_ring;
    bool mark_active = false;
    bool setting_selection = false;
    bool control_x_pending = false;
    bool rectangle_active = false, register_prefix = false;
    bool register_argument = false;
    std::function<void(const QString &)> register_callback;
    struct Register { QString text; QPointer<EmacsEditor> owner; int point = -1; };
    static QMap<QString, Register> &registers() { static QMap<QString, Register> values; return values; }

    void register_command(int key) {
        bool remove = register_argument;
        if (key != Qt::Key_S && key != Qt::Key_I && key != Qt::Key_Space && key != Qt::Key_J) return;
        show_prompt("Register: ");
        register_callback = [this, key, remove](const QString &name) {
            int pos = position();
            if (key == Qt::Key_S && mark >= 0) {
                int begin = std::min(pos, mark), end = std::max(pos, mark);
                registers()[name] = Register{text().mid(begin, end - begin), nullptr, -1};
                if (remove) replace(begin, end, ""); else deactivate_mark();
            } else if (key == Qt::Key_Space) registers()[name] = Register{QString(), this, pos};
            else if (registers().contains(name)) {
                const auto saved = registers().value(name);
                if (key == Qt::Key_I && saved.point < 0) {
                    push_mark(pos); replace(pos, pos, saved.text);
                } else if (key == Qt::Key_J && saved.owner) {
                    saved.owner->push_mark(saved.owner->position());
                    saved.owner->move_to(saved.point);
                    static_cast<QWidget *>(saved.owner->parent())->setFocus();
                }
            }
        };
    }
    static QStringList &killed_rectangle() { static QStringList rows; return rows; }

    static int column_at(const QString &value, int pos) {
        int column = 0;
        for (int i = line_start(value, pos); i < pos; i = next_character(value, i))
            column += value[i] == '\t' ? 8 - column % 8 : 1;
        return column;
    }
    static QString column_slice(const QString &row, int begin, int end, bool pad = false) {
        QString result;
        int column = 0;
        for (int i = 0; i < row.size(); i = next_character(row, i)) {
            int width = row[i] == '\t' ? 8 - column % 8 : 1;
            if (column + width > begin && column < end) {
                if (row[i] == '\t') result += QString(std::min(end, column + width) - std::max(begin, column), ' ');
                else result += row.mid(i, next_character(row, i) - i);
            }
            column += width;
        }
        if (pad && column < end) result += QString(end - std::max(begin, column), ' ');
        return result;
    }
    void rectangle_edit(bool copy, bool remove, const QString &inserted = QString()) {
        if (mark < 0 || (read_only() && remove)) return;
        const auto value = text();
        int pos = position();
        int begin = line_start(value, std::min(pos, mark)), end = line_end(value, std::max(pos, mark));
        int left = std::min(column_at(value, pos), column_at(value, mark));
        int right = std::max(column_at(value, pos), column_at(value, mark));
        QStringList rows = value.mid(begin, end - begin).split('\n'), copied;
        for (auto &row : rows) {
            copied << column_slice(row, left, right, true);
            if (remove) {
                QString prefix = column_slice(row, 0, left, !inserted.isNull());
                row = prefix + inserted + column_slice(row, right, INT_MAX);
            }
        }
        if (copy) killed_rectangle() = copied;
        if (remove) {
            int target_row = value.mid(begin, pos - begin).count('\n');
            replace(begin, end, rows.join("\n"));
            int target = begin;
            for (int i = 0; i < target_row; ++i) target += rows[i].size() + 1;
            target += column_slice(rows[target_row], 0, left).size();
            move_to(target);
        } else deactivate_mark();
    }
    void yank_rectangle() {
        if (read_only() || killed_rectangle().isEmpty()) return;
        int pos = position(), column = column_at(text(), pos);
        push_mark(pos);
        int target = pos;
        for (int i = 0; i < killed_rectangle().size(); ++i) {
            auto value = text();
            int begin = line_start(value, target), end = line_end(value, target);
            QString row = value.mid(begin, end - begin);
            QString prefix = column_slice(row, 0, column, true);
            QString suffix = column_slice(row, column, INT_MAX);
            replace(begin, end, prefix + killed_rectangle()[i] + suffix);
            target = begin + prefix.size() + killed_rectangle()[i].size();
            if (i + 1 < killed_rectangle().size()) {
                end = line_end(text(), target);
                if (end == text().size()) replace(end, end, "\n");
                target = end + 1;
            }
        }
        move_to(target);
    }
    bool rectangle_command(int key, bool meta) {
        switch (key) {
        case Qt::Key_K: rectangle_edit(true, true); return true;
        case Qt::Key_W: if (meta) { rectangle_edit(true, false); return true; } return false;
        case Qt::Key_D: rectangle_edit(false, true); return true;
        case Qt::Key_C: {
            if (mark >= 0) rectangle_edit(false, true, QString(std::abs(column_at(text(), mark) - column_at(text(), position())), ' '));
            return true;
        }
        case Qt::Key_Y: yank_rectangle(); return true;
        case Qt::Key_T:
            request_text("String rectangle: ", [this](const QString &input) { rectangle_edit(false, true, input); });
            return true;
        default: return false;
        }
    }
    int quoted_count = 0;
    int spacing_begin = -1, spacing_end = -1, spacing_step = 0;
    QString original_spacing;
    struct RecordedCommand {
        int key;
        Qt::KeyboardModifiers modifiers;
        QString text;
        bool control_x;
        std::optional<int> argument;
    };
    std::optional<RecordedCommand> repeated_command;
    bool repeating = false, repeat_pending = false;
    struct MacroKey { int key; Qt::KeyboardModifiers modifiers; QString text; };
    std::vector<MacroKey> macro, recording;
    bool recording_macro = false, playing_macro = false;

    void start_macro(bool append) {
        if (playing_macro) return;
        recording = append ? macro : std::vector<MacroKey>{};
        recording_macro = true;
    }
    void end_macro(int prefix_keys) {
        if (!recording_macro) return;
        for (int i = 0; i < prefix_keys && !recording.empty(); ++i) recording.pop_back();
        macro = recording;
        recording.clear();
        recording_macro = false;
    }
    void play_macro(int count) {
        if (playing_macro || recording_macro || macro.empty()) return;
        playing_macro = true;
        const auto keys = macro;
        count = std::max(0, std::min(count, 10000 / std::max(1, int(keys.size()))));
        auto cursor = multi ? multi->textCursor() : QTextCursor();
        if (multi) cursor.beginEditBlock();
        for (int i = 0; i < count; ++i) for (const auto &key : keys) {
            QKeyEvent event(QEvent::KeyPress, key.key, key.modifiers, key.text);
            QApplication::sendEvent(parent(), &event);
        }
        if (multi) cursor.endEditBlock();
        playing_macro = false;
    }

    void undo(int count, bool redo = false) {
        if (read_only()) return;
        deactivate_mark();
        for (int i = 0; i < std::abs(count); ++i) {
            if (line) { if (redo) line->redo(); else line->undo(); }
            else { if (redo) multi->redo(); else multi->undo(); }
        }
    }
    void repeat_command(std::optional<int> count) {
        if (!repeated_command || repeating) return;
        const auto command = *repeated_command;
        QKeyEvent event(QEvent::KeyPress, command.key, command.modifiers, command.text);
        argument = count ? count : command.argument;
        control_x_pending = command.control_x;
        repeating = true;
        key_press_event(&event);
        repeating = false;
        repeat_pending = true;
    }
    QString observed_text;
    std::optional<std::pair<int, int>> pending_edit;
    enum class LastAction { Other, Kill, Yank, Completion };
    LastAction last_action = LastAction::Other;
    static std::deque<QString> &kill_ring() {
        static std::deque<QString> entries;
        return entries;
    }
    static QString &last_clipboard() {
        static QString contents;
        return contents;
    }
    int yank_index = 0;
    int yank_begin = -1, yank_end = -1;
    QString yanked_text;
    int completion_begin = -1, completion_end = -1, completion_index = 0;
    QStringList completion_candidates;
    QString completion_text;

    void complete_word(bool cycle, bool host) {
        if (read_only()) return;
        const auto value = text();
        int pos = position(), begin = pos;
        while (begin > 0 && (word_character(value, previous_character(value, begin)) || value[begin - 1] == '_')) begin = previous_character(value, begin);
        QString prefix = value.mid(begin, pos - begin);
        if (host) {
            if (auto *widget = qobject_cast<VimTextEdit *>(multi)) {
                const auto suggestions = widget->get_autocomplete_suggestions(prefix);
                if (suggestions.size() == 1) replace(begin, pos, suggestions.front());
                else widget->show_autocomplete_suggestions(suggestions);
            }
            return;
        }
        if (cycle && pos == completion_end && value.mid(completion_begin, completion_end - completion_begin) == completion_text) {
            completion_index = (completion_index + 1) % completion_candidates.size();
        } else {
            if (prefix.isEmpty()) return;
            completion_begin = begin; completion_end = pos; completion_index = 0;
            completion_candidates.clear();
            QStringList before, after;
            auto matches = QRegularExpression("[\\p{L}\\p{N}_]+").globalMatch(value);
            while (matches.hasNext()) {
                auto match = matches.next();
                if (match.capturedStart() == begin || !match.captured().startsWith(prefix) || match.captured() == prefix) continue;
                if (match.capturedStart() < begin) before.prepend(match.captured()); else after.append(match.captured());
            }
            completion_candidates = before + after;
            completion_candidates.removeDuplicates();
            if (completion_candidates.isEmpty()) return;
            completion_candidates << prefix;
        }
        completion_text = completion_candidates[completion_index];
        replace(completion_begin, completion_end, completion_text);
        completion_end = position();
        last_action = LastAction::Completion;
    }
    struct IncrementalSearch {
        int origin, start, point;
        QString query;
        bool forward, failed, regexp;
    };
    std::optional<IncrementalSearch> search;
    std::vector<IncrementalSearch> search_steps;
    QString previous_search;
    QStringList search_history;
    int search_history_index = -1;
    struct Replacement {
        QRegularExpression expression;
        QString replacement;
        int limit, next, begin, end;
        QRegularExpressionMatch match;
    };
    std::optional<Replacement> replacement;

    static QRegularExpression expression(const QString &query, bool regexp) {
        QString pattern;
        if (!regexp) pattern = QRegularExpression::escape(query);
        else {
            // Emacs basic regexp grouping/alternation uses escaped delimiters.
            for (int i = 0; i < query.size(); ++i) {
                QChar c = query[i];
                if (c == '\\' && i + 1 < query.size()) {
                    QChar next = query[++i];
                    if (QString("()|{}").contains(next)) pattern += next;
                    else if (next == '<' || next == '>') pattern += "\\b";
                    else { pattern += '\\'; pattern += next; }
                } else {
                    if (QString("()|{}").contains(c)) pattern += '\\';
                    pattern += c;
                }
            }
        }
        bool uppercase = false;
        for (QChar c : query) uppercase |= c.isUpper();
        return QRegularExpression(pattern, uppercase ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    }
    void next_replacement() {
        if (!replacement) return;
        auto &r = *replacement;
        r.match = r.expression.match(text(), r.next);
        if (!r.match.hasMatch() || r.match.capturedEnd() > r.limit) {
            replacement.reset();
            if (search_label) search_label->hide();
            return;
        }
        r.begin = r.match.capturedStart(); r.end = r.match.capturedEnd();
        move_to(r.end);
        show_prompt("Replace with " + r.replacement + "? (y/n/!/q)");
    }
    void replace_match() {
        auto &r = *replacement;
        QString inserted;
        for (int i = 0; i < r.replacement.size(); ++i) {
            QChar c = r.replacement[i];
            if (c == '\\' && i + 1 < r.replacement.size()) {
                c = r.replacement[++i];
                if (c.isDigit()) inserted += r.match.captured(c.digitValue());
                else if (c == '&') inserted += r.match.captured();
                else inserted += c;
            } else inserted += c;
        }
        const auto matched = r.match.captured();
        if (inserted == inserted.toLower() && !matched.isEmpty()) {
            if (matched == matched.toUpper()) inserted = inserted.toUpper();
            else if (matched[0].isUpper()) inserted[0] = inserted[0].toUpper();
        }
        const int begin = r.begin, end = r.end;
        replace(begin, end, inserted);
        r.limit += inserted.size() - (end - begin);
        r.next = begin + inserted.size();
        if (begin == end) r.next = next_character(text(), r.next);
    }
    bool replacement_key(QKeyEvent *event, const KeyboardModifierState &modifiers) {
        int key = event->key();
        if (key == Qt::Key_Q || key == Qt::Key_Return || (modifiers.control && key == Qt::Key_G)) {
            replacement.reset(); search_label->hide(); return true;
        }
        if (key == Qt::Key_Exclam) {
            while (replacement) {
                int old = replacement->next;
                replace_match();
                if (replacement->next <= old && replacement->begin == replacement->end) {
                    replacement.reset(); search_label->hide(); break;
                }
                next_replacement();
            }
        } else if (key == Qt::Key_Y || key == Qt::Key_Space || key == Qt::Key_Period) {
            replace_match();
            if (key == Qt::Key_Period) { replacement.reset(); search_label->hide(); }
            else next_replacement();
        } else if (key == Qt::Key_N || key == Qt::Key_Backspace) {
            replacement->next = replacement->end > replacement->begin ? replacement->end : next_character(text(), replacement->end);
            next_replacement();
        }
        return true;
    }
    void query_replace(bool regexp, bool all = false) {
        if (read_only()) return;
        int begin = mark_active ? std::min(mark, position()) : position();
        int limit = mark_active ? std::max(mark, position()) : text().size();
        request_text(regexp ? "Query replace regexp: " : "Query replace: ", [this, regexp, all, begin, limit](const QString &query) {
            if (query.isEmpty()) return;
            const auto pattern = expression(query, regexp);
            if (!pattern.isValid()) return;
            request_text("Replace " + query + " with: ", [this, pattern, all, begin, limit](const QString &inserted) {
                deactivate_mark();
                replacement = Replacement{pattern, inserted, limit, begin, begin, begin, {}};
                next_replacement();
                if (all && replacement) {
                    QKeyEvent event(QEvent::KeyPress, Qt::Key_Exclam, Qt::NoModifier);
                    replacement_key(&event, KeyboardModifierState::from_qt_modifiers(Qt::NoModifier));
                }
            });
        });
    }
    QLabel *search_label = nullptr;
    QString prompt_title, prompt_input;
    std::function<void(const QString &)> prompt_callback;
    bool goto_pending = false;
    int recenter_step = 0;
    int fill_column = 70;

    void fill_paragraph() {
        if (!multi || read_only()) return;
        auto value = text();
        int pos = position();
        int begin = line_start(value, pos);
        while (begin > 0) {
            int previous = line_start(value, begin - 1);
            if (value.mid(previous, begin - previous).trimmed().isEmpty()) break;
            begin = previous;
        }
        int end = paragraph_target(begin, true);
        bool newline = end > begin && value[end - 1] == '\n';
        QString contents = value.mid(begin, end - begin);
        auto words = contents.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        int indent_end = begin;
        while (indent_end < end && (value[indent_end] == ' ' || value[indent_end] == '\t')) ++indent_end;
        QString indent = value.mid(begin, indent_end - begin), output = indent;
        int column = indent.size();
        for (const auto &word : words) {
            if (column > indent.size()) {
                if (column + 1 + word.size() > fill_column) { output += "\n" + indent; column = indent.size(); }
                else { output += ' '; ++column; }
            }
            output += word; column += word.size();
        }
        if (newline) output += '\n';
        replace(begin, end, output);
        move_to(pos >= end ? pos + output.size() - (end - begin) : std::min(pos, begin + int(output.size())));
    }
    void indent(bool region) {
        if (read_only()) return;
        const auto value = text();
        int pos = position();
        if (region && mark >= 0) {
            int begin = line_start(value, std::min(pos, mark)), end = std::max(pos, mark);
            QString contents = value.mid(begin, end - begin);
            contents.replace(QRegularExpression("(?m)^[ \t]+"), "");
            replace(begin, end, contents);
            return;
        }
        int start = line_start(value, pos), column = pos - start;
        int target_column = ((column / 8) + 1) * 8;
        if (start > 0) {
            int previous = line_start(value, start - 1);
            for (int i = previous; i < start - 1; ++i) {
                if (i - previous > column && !value[i].isSpace() && (i == previous || value[i - 1].isSpace())) {
                    target_column = i - previous; break;
                }
            }
        }
        replace(pos, pos, QString(target_column - column, ' '));
    }

    void host_command(int key) {
        auto *single = qobject_cast<VimLineEdit *>(line);
        auto *multiple = qobject_cast<VimTextEdit *>(multi);
        if (key == Qt::Key_S) {
            if (single) emit single->writeCommand();
            else emit multiple->writeCommand();
        } else if (key == Qt::Key_C) {
            if (single) emit single->quitCommand();
            else emit multiple->quitCommand();
        } else if (key == Qt::Key_F && multiple) emit multiple->openFile();
        else if (key == Qt::Key_Semicolon && !read_only()) {
            int begin = mark_active ? std::min(mark, position()) : line_start(text(), position());
            int end = mark_active ? std::max(mark, position()) : line_end(text(), position());
            if (single) emit single->commentRequested(begin, end);
            else emit multiple->commentRequested(begin, end);
        }
    }
    struct NamedCommand { const char *name; int key; bool control, meta, prefix; };
    static const std::vector<NamedCommand> &commands() {
        static const std::vector<NamedCommand> bindings = {
            {"forward-char", Qt::Key_F, true, false, false}, {"backward-char", Qt::Key_B, true, false, false},
            {"forward-word", Qt::Key_F, false, true, false}, {"backward-word", Qt::Key_B, false, true, false},
            {"beginning-of-line", Qt::Key_A, true, false, false}, {"end-of-line", Qt::Key_E, true, false, false},
            {"beginning-of-buffer", Qt::Key_Less, false, true, false}, {"end-of-buffer", Qt::Key_Greater, false, true, false},
            {"next-line", Qt::Key_N, true, false, false}, {"previous-line", Qt::Key_P, true, false, false},
            {"kill-line", Qt::Key_K, true, false, false}, {"kill-word", Qt::Key_D, false, true, false},
            {"backward-kill-word", Qt::Key_Backspace, false, true, false}, {"kill-region", Qt::Key_W, true, false, false},
            {"kill-ring-save", Qt::Key_W, false, true, false}, {"yank", Qt::Key_Y, true, false, false},
            {"mark-whole-buffer", Qt::Key_H, false, false, true}, {"mark-word", Qt::Key_At, false, true, false},
            {"mark-paragraph", Qt::Key_H, false, true, false}, {"exchange-point-and-mark", Qt::Key_X, true, false, true},
            {"upcase-word", Qt::Key_U, false, true, false}, {"downcase-word", Qt::Key_L, false, true, false},
            {"capitalize-word", Qt::Key_C, false, true, false}, {"upcase-region", Qt::Key_U, true, false, true},
            {"downcase-region", Qt::Key_L, true, false, true}, {"transpose-chars", Qt::Key_T, true, false, false},
            {"transpose-words", Qt::Key_T, false, true, false}, {"transpose-lines", Qt::Key_T, true, false, true},
            {"open-line", Qt::Key_O, true, false, false}, {"delete-indentation", Qt::Key_AsciiCircum, false, true, false},
            {"delete-horizontal-space", Qt::Key_Backslash, false, true, false}, {"cycle-spacing", Qt::Key_Space, false, true, false},
            {"delete-blank-lines", Qt::Key_O, true, false, true}, {"back-to-indentation", Qt::Key_M, false, true, false},
            {"forward-sentence", Qt::Key_E, false, true, false}, {"backward-sentence", Qt::Key_A, false, true, false},
            {"forward-paragraph", Qt::Key_BraceRight, false, true, false}, {"backward-paragraph", Qt::Key_BraceLeft, false, true, false},
            {"kill-sentence", Qt::Key_K, false, true, false}, {"scroll-up-command", Qt::Key_V, true, false, false},
            {"scroll-down-command", Qt::Key_V, false, true, false}, {"recenter-top-bottom", Qt::Key_L, true, false, false},
            {"undo", Qt::Key_Underscore, true, false, false}, {"undo-redo", Qt::Key_Question, true, false, false},
            {"repeat", Qt::Key_Z, false, false, true}, {"isearch-forward", Qt::Key_S, true, false, false},
            {"isearch-backward", Qt::Key_R, true, false, false}, {"isearch-forward-regexp", Qt::Key_S, true, true, false},
            {"isearch-backward-regexp", Qt::Key_R, true, true, false}, {"query-replace", Qt::Key_Percent, false, true, false},
            {"query-replace-regexp", Qt::Key_Percent, true, true, false}, {"fill-paragraph", Qt::Key_Q, false, true, false},
            {"set-fill-column", Qt::Key_F, false, false, true}, {"indent-region", Qt::Key_Backslash, true, true, false},
            {"indent-for-tab-command", Qt::Key_Tab, false, false, false},
            {"forward-sexp", Qt::Key_F, true, true, false}, {"backward-sexp", Qt::Key_B, true, true, false},
            {"forward-list", Qt::Key_N, true, true, false}, {"backward-list", Qt::Key_P, true, true, false},
            {"backward-up-list", Qt::Key_U, true, true, false}, {"down-list", Qt::Key_D, true, true, false},
            {"kill-sexp", Qt::Key_K, true, true, false}, {"mark-sexp", Qt::Key_Space, true, true, false},
            {"dabbrev-expand", Qt::Key_Slash, false, true, false}, {"completion-at-point", Qt::Key_Tab, false, true, false},
            {"save-buffer", Qt::Key_S, true, false, true}, {"find-file", Qt::Key_F, true, false, true},
            {"save-buffers-kill-terminal", Qt::Key_C, true, false, true}, {"kill-buffer", Qt::Key_K, false, false, true},
            {"comment-dwim", Qt::Key_Semicolon, false, true, false},
            {"start-kbd-macro", Qt::Key_ParenLeft, false, false, true},
            {"end-kbd-macro", Qt::Key_ParenRight, false, false, true},
            {"call-last-kbd-macro", Qt::Key_E, false, false, true},
        };
        return bindings;
    }
    QStringList command_names() const {
        QStringList names{"goto-line", "replace-string", "replace-regexp", "delete-trailing-whitespace", "describe-bindings"};
        for (const auto &command : commands()) names << command.name;
        names.sort();
        return names;
    }
    void execute_named(const QString &name, std::optional<int> count) {
        if (name == "goto-line") {
            if (count) goto_line(QString::number(*count));
            else request_text("Goto line: ", [this](const QString &input) { goto_line(input); });
            return;
        }
        if (name == "replace-string" || name == "replace-regexp") { query_replace(name == "replace-regexp", true); return; }
        if (name == "delete-trailing-whitespace") {
            auto value = text();
            int begin = mark_active ? std::min(mark, position()) : 0;
            int end = mark_active ? std::max(mark, position()) : value.size();
            for (int i = end; i > begin;) {
                int stop = i;
                while (i > begin && (value[i - 1] == ' ' || value[i - 1] == '\t')) --i;
                if (i < stop && (stop == value.size() || value[stop] == '\n')) replace(i, stop, "");
                --i;
            }
            return;
        }
        if (name == "describe-bindings") {
            QStringList bindings;
            for (const auto &command : commands()) {
                QString key = QKeySequence(command.key).toString();
                bindings << (QString(command.prefix ? "C-x " : "") + (command.control ? "C-" : "") + (command.meta ? "M-" : "") + key + "  " + command.name);
            }
            show_prompt(bindings.join("\n"));
            return;
        }
        for (const auto &command : commands()) if (name == command.name) {
            Qt::KeyboardModifiers modifiers = command.meta ? Qt::AltModifier : Qt::NoModifier;
#ifdef Q_OS_MACOS
            if (command.control) modifiers |= Qt::MetaModifier;
#else
            if (command.control) modifiers |= Qt::ControlModifier;
#endif
            QKeyEvent event(QEvent::KeyPress, command.key, modifiers);
            control_x_pending = command.prefix;
            argument = count;
            key_press_event(&event);
            return;
        }
        show_prompt("Unknown command: " + name);
    }
    void complete_command() {
        QStringList matches;
        for (const auto &name : command_names()) if (name.startsWith(prompt_input)) matches << name;
        if (!matches.isEmpty()) {
            QString common = matches.front();
            for (const auto &match : matches) while (!match.startsWith(common)) common.chop(1);
            prompt_input = common;
        }
        show_prompt(prompt_title + prompt_input + (matches.size() > 1 ? "\n" + matches.join("  ") : ""));
    }
    void show_prompt(const QString &message) {
        auto *widget = static_cast<QWidget *>(parent());
        if (!search_label) {
            search_label = new QLabel(widget);
            search_label->setObjectName("emacsSearchPrompt");
            search_label->setTextFormat(Qt::PlainText);
            search_label->setAutoFillBackground(true);
            search_label->setFrameStyle(QFrame::StyledPanel);
        }
        search_label->setFont(widget->font());
        search_label->setText(message);
        const int height = std::min(widget->height(), search_label->sizeHint().height());
        search_label->setGeometry(0, widget->height() - height, widget->width(), height);
        search_label->show();
        search_label->raise();
    }
    void request_text(const QString &title, std::function<void(const QString &)> callback) {
        prompt_title = title;
        prompt_input.clear();
        prompt_callback = std::move(callback);
        show_prompt(title);
    }
    bool prompt_key(QKeyEvent *event, const KeyboardModifierState &modifiers) {
        int key = event->key();
        if (modifiers.control && key == Qt::Key_G) {
            prompt_callback = nullptr;
            search_label->hide();
        } else if (key == Qt::Key_Tab && prompt_title == "M-x ") {
            complete_command();
        } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            auto callback = std::move(prompt_callback);
            auto input = prompt_input;
            prompt_callback = nullptr;
            search_label->hide();
            callback(input);
        } else {
            if (key == Qt::Key_Backspace && !prompt_input.isEmpty()) prompt_input.chop(prompt_input.size() - previous_character(prompt_input, prompt_input.size()));
            else if (modifiers.control && key == Qt::Key_U) prompt_input.clear();
            else if (!modifiers.control && !modifiers.alt && !modifiers.command) prompt_input += event->text();
            show_prompt(prompt_title + prompt_input);
        }
        return true;
    }
    void goto_line(const QString &input) {
        bool valid;
        int number = input.toInt(&valid);
        if (!valid || number < 1) return;
        push_mark(position());
        const auto value = text();
        int target = 0;
        while (--number && target < value.size()) target = std::min(int(value.size()), line_end(value, target) + 1);
        move_to(target);
    }
    int sentence_target(int count) const {
        const auto value = text();
        const QRegularExpression boundary(R"(([.!?]["')\]]*)(?:[ \t]{2,}|\n|$))");
        int target = position();
        for (int i = 0; i < std::abs(count); ++i) {
            auto matches = boundary.globalMatch(value);
            int next = count > 0 ? value.size() : 0;
            while (matches.hasNext()) {
                const auto match = matches.next();
                int end = match.capturedEnd(1);
                if (count > 0 && end > target) { next = end; break; }
                if (count < 0) {
                    while (end < value.size() && value[end].isSpace()) ++end;
                    if (end < target) next = end;
                }
            }
            target = next;
        }
        return target;
    }
    void scroll_page(int count) {
        if (!multi) return;
        auto *bar = multi->verticalScrollBar();
        int distance = std::max(1, multi->viewport()->height() - 2 * multi->fontMetrics().height());
        int old = bar->value();
        bar->setValue(old + count * distance);
        if (bar->value() != old) {
            const int target_scroll = bar->value();
            const int y = count > 0 ? 0 : multi->viewport()->height() - multi->fontMetrics().height();
            move_to(multi->cursorForPosition(QPoint(0, y)).position());
            bar->setValue(target_scroll);
        }
    }
    void recenter() {
        if (!multi) return;
        const int height = multi->viewport()->height();
        int target = recenter_step == 0 ? height / 2 : recenter_step == 1 ? multi->fontMetrics().height() : height - multi->fontMetrics().height();
        auto *bar = multi->verticalScrollBar();
        bar->setValue(bar->value() + multi->cursorRect().center().y() - target);
        recenter_step = (recenter_step + 1) % 3;
    }

    QString text() const { return line ? line->text() : multi->toPlainText(); }
    int position() const { return line ? line->cursorPosition() : multi->textCursor().position(); }
    bool read_only() const { return line ? line->isReadOnly() : multi->isReadOnly(); }

    static int next_character(const QString &value, int pos) {
        if (pos >= value.size()) return value.size();
        return pos + (value[pos].isHighSurrogate() && pos + 1 < value.size() &&
                      value[pos + 1].isLowSurrogate() ? 2 : 1);
    }
    static int previous_character(const QString &value, int pos) {
        if (pos <= 0) return 0;
        return pos - (value[pos - 1].isLowSurrogate() && pos > 1 &&
                      value[pos - 2].isHighSurrogate() ? 2 : 1);
    }
    static int line_start(const QString &value, int pos) {
        return pos == 0 ? 0 : value.lastIndexOf('\n', pos - 1) + 1;
    }
    static int line_end(const QString &value, int pos) {
        const int end = value.indexOf('\n', pos);
        return end < 0 ? value.size() : end;
    }
    static bool word_character(const QString &value, int pos) {
        const uint character = value.mid(pos, next_character(value, pos) - pos).toUcs4().value(0);
        const auto category = QChar::category(character);
        return QChar::isLetterOrNumber(character) || category == QChar::Mark_NonSpacing ||
               category == QChar::Mark_SpacingCombining || category == QChar::Mark_Enclosing;
    }
    int word_target(bool forward, int pos = -1) const {
        const auto value = text();
        if (pos < 0) pos = position();
        if (forward) {
            while (pos < value.size() && !word_character(value, pos)) pos = next_character(value, pos);
            while (pos < value.size() && word_character(value, pos)) pos = next_character(value, pos);
        } else {
            while (pos > 0 && !word_character(value, previous_character(value, pos))) pos = previous_character(value, pos);
            while (pos > 0 && word_character(value, previous_character(value, pos))) pos = previous_character(value, pos);
        }
        return pos;
    }
    int words_target(int count) const {
        int target = position();
        for (int i = 0; i < std::abs(count); ++i) target = word_target(count > 0, target);
        return target;
    }
    void move_vertical(bool forward) {
        if (multi && multi->lineWrapMode() != QTextEdit::NoWrap && !rectangle_active) {
            auto cursor = multi->textCursor();
            if (cursor.movePosition(forward ? QTextCursor::Down : QTextCursor::Up,
                                    mark_active ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor)) {
                QScopedValueRollback<bool> guard(setting_selection, true);
                multi->setTextCursor(cursor);
                return;
            }
        }
        auto value = text();
        int pos = position();
        int start = line_start(value, pos);
        if (!desired_column) {
            int column = 0;
            for (int i = start; i < pos; i = next_character(value, i))
                column += value[i] == '\t' ? 8 - column % 8 : 1;
            desired_column = column;
        }
        if ((forward && line_end(value, pos) == value.size()) || (!forward && start == 0)) {
            if (auto *widget = qobject_cast<VimTextEdit *>(multi)) emit widget->textBoundaryRequested(forward);
            value = text();
            pos = position();
            start = line_start(value, pos);
        }
        const int end = line_end(value, pos);
        if ((forward && end == value.size()) || (!forward && start == 0)) return;
        int target = forward ? end + 1 : line_start(value, start - 1);
        int column = 0;
        while (target < value.size() && value[target] != '\n' && column < *desired_column) {
            const int width = value[target] == '\t' ? 8 - column % 8 : 1;
            if (column + width > *desired_column) break;
            column += width;
            target = next_character(value, target);
        }
        move_to(target);
    }
    void move_to(int pos) {
        QScopedValueRollback<bool> guard(setting_selection, true);
        pos = std::max(0, std::min(pos, static_cast<int>(text().size())));
        if (line) {
            if (mark_active && mark >= 0 && mark != pos) line->setSelection(mark, pos - mark);
            else line->setCursorPosition(pos);
        }
        else {
            auto cursor = multi->textCursor();
            if (mark_active && mark >= 0) {
                cursor.setPosition(mark);
                cursor.setPosition(pos, QTextCursor::KeepAnchor);
            } else cursor.setPosition(pos);
            if (rectangle_active && mark >= 0) {
                cursor.clearSelection();
                QList<QTextEdit::ExtraSelection> selections;
                const auto value = text();
                int begin = line_start(value, std::min(pos, mark)), end = line_end(value, std::max(pos, mark));
                int left = std::min(column_at(value, pos), column_at(value, mark));
                int right = std::max(column_at(value, pos), column_at(value, mark));
                for (int row = begin; row <= end;) {
                    QString contents = value.mid(row, line_end(value, row) - row);
                    int a = 0, b = 0;
                    while (a < contents.size() && column_at(contents, a) < left) a = next_character(contents, a);
                    b = a;
                    while (b < contents.size() && column_at(contents, b) < right) b = next_character(contents, b);
                    QTextEdit::ExtraSelection selection;
                    selection.cursor = multi->textCursor();
                    selection.cursor.setPosition(row + a);
                    selection.cursor.setPosition(row + b, QTextCursor::KeepAnchor);
                    selection.format.setBackground(multi->palette().highlight());
                    selection.format.setForeground(multi->palette().highlightedText());
                    selections << selection;
                    int next = line_end(value, row) + 1;
                    if (next > value.size()) break;
                    row = next;
                }
                multi->setExtraSelections(selections);
            }
            multi->setTextCursor(cursor);
        }
    }
    void push_mark(int pos) {
        if (mark >= 0) {
            mark_ring.push_front(mark);
            if (mark_ring.size() > 16) mark_ring.pop_back();
        }
        mark = pos;
    }
    int paragraph_target(int pos, bool forward) const {
        const auto value = text();
        auto blank = [&](int p) { return value.mid(p, line_end(value, p) - p).trimmed().isEmpty(); };
        int target = line_start(value, pos);
        if (forward) {
            while (target < value.size() && blank(target)) target = std::min(int(value.size()), line_end(value, target) + 1);
            while (target < value.size() && !blank(target)) target = std::min(int(value.size()), line_end(value, target) + 1);
        } else {
            if (target > 0) target = line_start(value, target - 1);
            while (target > 0 && blank(target)) target = line_start(value, target - 1);
            while (target > 0 && !blank(line_start(value, target - 1))) target = line_start(value, target - 1);
        }
        return target;
    }
    void deactivate_mark() {
        mark_active = false;
        rectangle_active = false;
        if (multi) multi->setExtraSelections({});
        move_to(position());
    }
    void replace(int begin, int end, const QString &replacement) {
        if (read_only()) return;
        pending_edit = std::make_pair(begin, end);
        if (line) {
            line->setSelection(begin, end - begin);
            line->insert(replacement);
        } else {
            auto cursor = multi->textCursor();
            cursor.beginEditBlock();
            cursor.setPosition(begin);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            cursor.insertText(replacement);
            cursor.endEditBlock();
            multi->setTextCursor(cursor);
        }
        pending_edit.reset();
        deactivate_mark();
    }
    struct ExpressionSpan { int begin, end; bool list; };
    std::vector<ExpressionSpan> expression_spans() const {
        const auto value = text();
        std::vector<ExpressionSpan> spans;
        std::function<int(int)> scan = [&](int begin) -> int {
            int pos = begin;
            QChar c = value[pos++];
            int opening = QString("([{ ").indexOf(c);
            bool list = opening >= 0 && opening < 3;
            if (list) {
                QChar closing = QString(")]}")[opening];
                while (pos < value.size()) {
                    if (value[pos] == closing) { ++pos; break; }
                    if (value[pos].isSpace()) { ++pos; continue; }
                    if (QString(")]}").contains(value[pos])) return begin;
                    int end = scan(pos);
                    if (end <= pos) return begin;
                    pos = end;
                }
                if (pos > value.size() || value[pos - 1] != closing) return begin;
            } else if (c == '"') {
                while (pos < value.size() && value[pos] != '"') {
                    if (value[pos] == '\\' && pos + 1 < value.size()) ++pos;
                    ++pos;
                }
                if (pos == value.size()) return begin;
                ++pos;
            } else {
                if (QString(")]}").contains(c)) return begin;
                while (pos < value.size() && !value[pos].isSpace() && !QString("()[]{}\"").contains(value[pos])) {
                    if (value[pos] == '\\' && pos + 1 < value.size()) ++pos;
                    ++pos;
                }
            }
            spans.push_back({begin, pos, list});
            return pos;
        };
        for (int pos = 0; pos < value.size();) {
            if (value[pos].isSpace()) { ++pos; continue; }
            int end = scan(pos);
            pos = end > pos ? end : pos + 1;
        }
        return spans;
    }
    int sexp_target(int count, int origin = -1, bool lists_only = false) const {
        int target = origin < 0 ? position() : origin;
        const auto spans = expression_spans();
        const auto value = text();
        for (int i = 0; i < std::abs(count); ++i) {
            int best = -1, result = target;
            for (const auto &span : spans) {
                if (lists_only && !span.list) continue;
                if (count > 0 && span.end > target && (!span.list || span.begin >= target)) {
                    int begin = std::max(span.begin, target);
                    if (best < 0 || begin < best || (begin == best && span.end > result)) { best = begin; result = span.end; }
                } else if (count < 0 && span.begin < target && (!span.list || span.end <= target)) {
                    int end = std::min(span.end, target);
                    if (end > best || (end == best && span.begin < result)) { best = end; result = span.begin; }
                }
            }
            if (best < 0) break;
            target = result;
        }
        return target;
    }
    void expression_command(int key, int count, LastAction previous) {
        int pos = position();
        if (key == Qt::Key_U || key == Qt::Key_D) {
            int target = -1;
            for (const auto &span : expression_spans()) if (span.list) {
                if (key == Qt::Key_U && span.begin < pos && span.end >= pos && span.begin > target) target = span.begin;
                if (key == Qt::Key_D && span.begin >= pos && (target < 0 || span.begin + 1 < target)) target = span.begin + 1;
            }
            if (target >= 0) move_to(target);
            return;
        }
        int target = sexp_target((key == Qt::Key_B || key == Qt::Key_P) ? -count : count, -1, key == Qt::Key_N || key == Qt::Key_P);
        if (key == Qt::Key_K) kill(std::min(pos, target), std::max(pos, target), target < pos, previous == LastAction::Kill);
        else if (key == Qt::Key_Space || key == Qt::Key_At) {
            if (mark_active) mark = sexp_target(count, mark); else push_mark(target);
            mark_active = true; move_to(pos);
        } else move_to(target);
    }
    void horizontal_space(bool cycle, int count) {
        const auto value = text();
        int pos = position(), begin = pos, end = pos;
        while (begin > 0 && (value[begin - 1] == ' ' || value[begin - 1] == '\t')) --begin;
        while (end < value.size() && (value[end] == ' ' || value[end] == '\t')) ++end;
        QString replacement;
        if (cycle) {
            if (spacing_begin != begin || spacing_end != end) {
                original_spacing = value.mid(begin, end - begin);
                spacing_step = 0;
            }
            replacement = spacing_step == 0 ? QString(std::max(0, count), ' ') : spacing_step == 1 ? "" : original_spacing;
            spacing_step = (spacing_step + 1) % 3;
        }
        replace(begin, end, replacement);
        spacing_begin = begin; spacing_end = begin + replacement.size();
    }
    void join_line(bool following) {
        const auto value = text();
        int begin = following ? line_end(value, position()) : line_start(value, position()) - 1;
        if (begin < 0 || begin >= value.size()) return;
        int end = begin + 1;
        while (begin > 0 && (value[begin - 1] == ' ' || value[begin - 1] == '\t')) --begin;
        while (end < value.size() && (value[end] == ' ' || value[end] == '\t')) ++end;
        QString separator = begin > 0 && end < value.size() && value[begin - 1] != '(' && value[end] != ')' ? " " : "";
        replace(begin, end, separator);
        move_to(begin);
    }
    void delete_blank_lines() {
        const auto value = text();
        auto blank = [&](int p) { return value.mid(p, line_end(value, p) - p).trimmed().isEmpty(); };
        int begin = line_start(value, position()), end = begin;
        bool on_blank = blank(begin);
        if (on_blank) {
            while (begin > 0 && blank(line_start(value, begin - 1))) begin = line_start(value, begin - 1);
        } else begin = end = std::min(int(value.size()), line_end(value, begin) + 1);
        while (end < value.size() && blank(end)) end = std::min(int(value.size()), line_end(value, end) + 1);
        int first_end = std::min(int(value.size()), line_end(value, begin) + 1);
        replace(begin, end, on_blank && end > first_end ? "\n" : "");
        move_to(on_blank ? begin : std::min(position(), begin));
    }
    void change_case(int key, int count, bool region = false) {
        if (region && mark < 0) return;
        int pos = position();
        int target = region ? mark : words_target(count);
        int begin = std::min(pos, target), end = std::max(pos, target);
        QString converted = text().mid(begin, end - begin);
        if (key == Qt::Key_U) converted = converted.toUpper();
        else if (key == Qt::Key_L) converted = converted.toLower();
        else {
            bool in_word = false;
            for (int i = 0; i < converted.size(); i = next_character(converted, i)) {
                bool word = word_character(converted, i);
                converted[i] = word && !in_word ? converted[i].toUpper() : converted[i].toLower();
                in_word = word;
            }
        }
        replace(begin, end, converted);
        if (region || count < 0) move_to(pos);
    }
    void transpose(int kind, int count) {
        if (read_only() || count == 0) return;
        for (int i = 0; i < std::abs(count); ++i) {
            auto value = text();
            int pos = position(), a, b, c, d;
            if (kind == 0) {
                if (count > 0 && pos == value.size()) pos = previous_character(value, pos);
                a = previous_character(value, pos); b = pos;
                c = count > 0 ? pos : previous_character(value, a);
                d = count > 0 ? next_character(value, c) : a;
            } else if (kind == 1) {
                b = word_target(true, word_target(false, pos));
                a = word_target(false, b);
                d = count > 0 ? word_target(true, b) : word_target(false, a);
                c = count > 0 ? word_target(false, d) : word_target(true, d);
                if (count < 0) std::swap(c, d);
            } else {
                b = line_start(value, pos);
                a = line_start(value, std::max(0, b - 1));
                c = b;
                d = std::min(int(value.size()), line_end(value, c) + 1);
                if (d == value.size() && d > c && value[d - 1] != '\n') {
                    replace(d, d, "\n");
                    value = text(); ++d;
                }
            }
            bool backward = c < a;
            if (backward) { std::swap(a, c); std::swap(b, d); }
            if (a == b || c == d || b > c) return;
            replace(a, d, value.mid(c, d - c) + value.mid(b, c - b) + value.mid(a, b - a));
            if (backward) move_to(a + d - c);
        }
    }
    void save_kill(const QString &contents, bool append, bool backward) {
        if (contents.isEmpty()) return;
        if (append && !kill_ring().empty()) {
            if (backward) kill_ring().front().prepend(contents);
            else kill_ring().front().append(contents);
        } else {
            kill_ring().push_front(contents);
            if (kill_ring().size() > 60) kill_ring().pop_back();
        }
    }
    void publish_kill() {
        if (kill_ring().empty()) return;
        last_clipboard() = kill_ring().front();
        QApplication::clipboard()->setText(last_clipboard());
    }
    void kill(int begin, int end, bool backward, bool append) {
        if (read_only() || begin == end) return;
        save_kill(text().mid(begin, end - begin), append, backward);
        publish_kill();
        replace(begin, end, "");
        last_action = LastAction::Kill;
    }
    void yank(bool cycle) {
        if (read_only()) return;
        if (!cycle) {
            const auto clipboard = QApplication::clipboard()->text();
            if (!clipboard.isEmpty() && clipboard != last_clipboard()) {
                save_kill(clipboard, false, false);
                last_clipboard() = clipboard;
            }
        }
        if (kill_ring().empty()) return;
        if (cycle) {
            if (position() != yank_end || text().mid(yank_begin, yank_end - yank_begin) != yanked_text) return;
            yank_index = (yank_index + 1) % kill_ring().size();
        } else {
            yank_index = 0;
            yank_begin = yank_end = position();
        }
        mark = yank_begin;
        replace(yank_begin, yank_end, kill_ring()[yank_index]);
        yank_end = position();
        yanked_text = text().mid(yank_begin, yank_end - yank_begin);
        last_action = LastAction::Yank;
    }
    void show_search() {
        show_prompt(QString(search->failed ? "Failing " : "") +
            (search->forward ? "I-search: " : "I-search backward: ") + search->query);
    }
    void update_search() {
        if (search->query.isEmpty()) {
            search->point = search->start;
            search->failed = false;
        } else {
            const auto value = text();
            const auto pattern = expression(search->query, search->regexp);
            auto matches = pattern.globalMatch(value);
            int found = -1, end = -1;
            while (matches.hasNext()) {
                auto match = matches.next();
                if (search->forward && match.capturedStart() >= search->start) {
                    found = match.capturedStart(); end = match.capturedEnd(); break;
                }
                if (!search->forward && match.capturedEnd() <= search->start) {
                    found = match.capturedStart(); end = match.capturedEnd();
                }
            }
            search->failed = found < 0;
            if (found >= 0) search->point = search->forward ? end : found;
        }
        move_to(search->point);
        show_search();
    }
    void begin_search(bool forward, bool regexp = false) {
        deactivate_mark();
        search = IncrementalSearch{position(), position(), position(), "", forward, false, regexp};
        search_history_index = -1;
        search_steps.clear();
        show_search();
    }
    void finish_search(bool cancel) {
        if (!search) return;
        if (cancel) move_to(search->origin);
        else {
            if (!search->query.isEmpty()) {
                previous_search = search->query;
                search_history.removeAll(previous_search);
                search_history.prepend(previous_search);
                if (search_history.size() > 60) search_history.removeLast();
            }
            push_mark(search->origin);
        }
        search.reset();
        search_steps.clear();
        if (search_label) search_label->hide();
    }
    bool search_key(QKeyEvent *event, const KeyboardModifierState &modifiers) {
        const int key = event->key();
        if (modifiers.control && key == Qt::Key_G) {
            if (search->failed) {
                while (search->failed && !search_steps.empty()) {
                    search = search_steps.back();
                    search_steps.pop_back();
                }
                move_to(search->point);
                show_search();
            } else finish_search(true);
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { finish_search(false); return true; }
        if (key == Qt::Key_Backspace && !modifiers.control && !modifiers.alt) {
            if (!search_steps.empty()) {
                search = search_steps.back();
                search_steps.pop_back();
                move_to(search->point);
                show_search();
            }
            return true;
        }
        if (modifiers.control && (key == Qt::Key_S || key == Qt::Key_R)) {
            search_steps.push_back(*search);
            const bool forward = key == Qt::Key_S;
            search->start = search->failed ? (forward ? 0 : text().size()) : search->point;
            search->forward = forward;
            if (search->query.isEmpty()) search->query = previous_search;
            update_search();
            return true;
        }
        if (modifiers.alt && (key == Qt::Key_P || key == Qt::Key_N) && !search_history.isEmpty()) {
            search_steps.push_back(*search);
            search_history_index = std::max(0, std::min(int(search_history.size()) - 1, search_history_index + (key == Qt::Key_P ? 1 : -1)));
            request_text("Edit search: ", [this](const QString &query) {
                if (search) { search->query = query; update_search(); }
            });
            prompt_input = search_history[search_history_index];
            show_prompt(prompt_title + prompt_input);
            return true;
        }
        if (modifiers.control && (key == Qt::Key_W || key == Qt::Key_Y)) {
            search_steps.push_back(*search);
            QString added = key == Qt::Key_W ? text().mid(search->point, word_target(true, search->point) - search->point) : kill_ring().empty() ? "" : kill_ring().front();
            search->query += search->regexp ? QRegularExpression::escape(added) : added;
            update_search(); return true;
        }
        if (!modifiers.control && !modifiers.alt && !modifiers.command && !event->text().isEmpty()) {
            search_steps.push_back(*search);
            search->query += event->text();
            update_search();
            return true;
        }
        finish_search(false);
        return false; // Apply the command that ended the search normally.
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Resize && search) show_search();
        return QObject::eventFilter(watched, event);
    }

  public:
    explicit EmacsEditor(QWidget *widget) : QObject(widget),
        line(qobject_cast<QLineEdit *>(widget)), multi(qobject_cast<QTextEdit *>(widget)), observed_text(text()) {
        widget->installEventFilter(this);
        auto changed = [this]() {
            // External edits invalidate the search's saved offsets.
            finish_search(false);
            const auto current = text();
            int begin, old_end, new_end;
            if (pending_edit) {
                // A text diff cannot locate an insertion among repeated characters.
                begin = pending_edit->first;
                old_end = pending_edit->second;
                new_end = old_end + current.size() - observed_text.size();
            } else {
                begin = 0;
                while (begin < observed_text.size() && begin < current.size() && observed_text[begin] == current[begin]) ++begin;
                old_end = observed_text.size();
                new_end = current.size();
                while (old_end > begin && new_end > begin && observed_text[old_end - 1] == current[new_end - 1]) {
                    --old_end; --new_end;
                }
            }
            auto rebase = [&](int &point) {
                if (point > begin) point = point <= old_end ? begin : point + new_end - old_end;
            };
            rebase(mark);
            for (int &point : mark_ring) rebase(point);
            for (auto it = registers().begin(); it != registers().end(); ++it)
                if (it->owner == this) rebase(it->point);
            observed_text = current;
            last_action = LastAction::Other;
        };
        auto selection_changed = [this]() {
            const auto mode = line ? static_cast<VimLineEdit *>(line)->get_keybinding_mode() : static_cast<VimTextEdit *>(multi)->get_keybinding_mode();
            if (mode != KeybindingMode::Emacs || setting_selection || pending_edit) return;
            int anchor = position();
            if (line && line->hasSelectedText()) {
                anchor = line->selectionStart();
                if (anchor == position()) anchor += line->selectedText().size();
            } else if (multi) anchor = multi->textCursor().anchor();
            mark_active = anchor != position();
            if (mark_active) mark = anchor;
            rectangle_active = false;
            if (multi) multi->setExtraSelections({});
        };
        if (line) {
            connect(line, &QLineEdit::textChanged, this, changed);
            connect(line, &QLineEdit::selectionChanged, this, selection_changed);
        } else {
            connect(multi, &QTextEdit::textChanged, this, changed);
            connect(multi, &QTextEdit::selectionChanged, this, selection_changed);
        }
    }

    void reset() {
        finish_search(false);
        prompt_callback = nullptr;
        replacement.reset();
        goto_pending = false;
        if (search_label) search_label->hide();
        meta_pending = false;
        clear_argument();
        desired_column.reset();
        control_x_pending = false;
        register_prefix = rectangle_active = false;
        register_callback = nullptr;
        if (multi) multi->setExtraSelections({});
        quoted_count = 0;
        repeated_command.reset();
        repeat_pending = false;
        recording_macro = false;
        recording.clear();
        mark = -1;
        mark_ring.clear();
        mark_active = false;
        last_action = LastAction::Other;
        move_to(position());
    }

    void finish_key_event() { pending_edit.reset(); }

    bool handles_shortcut(QKeyEvent *event) const {
        auto modifiers = KeyboardModifierState::from_qt_modifiers(event->modifiers());
        if (register_callback || register_prefix || replacement || prompt_callback || goto_pending || quoted_count || argument || search || meta_pending || control_x_pending || event->key() == Qt::Key_Escape) return true;
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_F3 || event->key() == Qt::Key_F4) return true;
        if (modifiers.alt && modifiers.control && (event->key() == Qt::Key_S || event->key() == Qt::Key_R || event->key() == Qt::Key_Percent || event->key() == Qt::Key_Backslash ||
            event->key() == Qt::Key_F || event->key() == Qt::Key_B || event->key() == Qt::Key_N || event->key() == Qt::Key_P ||
            event->key() == Qt::Key_U || event->key() == Qt::Key_D || event->key() == Qt::Key_K || event->key() == Qt::Key_Space || event->key() == Qt::Key_At)) return true;
        if (modifiers.alt && !modifiers.control && !modifiers.command)
            return event->key() == Qt::Key_Semicolon || event->key() == Qt::Key_Slash || event->key() == Qt::Key_X || event->key() == Qt::Key_Q || event->key() == Qt::Key_Percent || event->key() == Qt::Key_A || event->key() == Qt::Key_E || event->key() == Qt::Key_K ||
                   event->key() == Qt::Key_BraceLeft || event->key() == Qt::Key_BraceRight || event->key() == Qt::Key_G || event->key() == Qt::Key_V ||
                   event->key() == Qt::Key_M || event->key() == Qt::Key_AsciiCircum || event->key() == Qt::Key_Space || event->key() == Qt::Key_Backslash ||
                   event->key() == Qt::Key_U || event->key() == Qt::Key_L || event->key() == Qt::Key_C ||
                   event->key() == Qt::Key_T || event->key() == Qt::Key_At || event->key() == Qt::Key_H ||
                   event->key() == Qt::Key_B || event->key() == Qt::Key_F ||
                   event->key() == Qt::Key_Less || event->key() == Qt::Key_Greater ||
                   event->key() == Qt::Key_D || event->key() == Qt::Key_Backspace ||
                   event->key() == Qt::Key_W || event->key() == Qt::Key_Y ||
                   event->key() == Qt::Key_Minus || (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9);
        return modifiers.control && !modifiers.alt && !modifiers.command &&
            (event->key() == Qt::Key_Question || event->key() == Qt::Key_V || event->key() == Qt::Key_L || event->key() == Qt::Key_O || event->key() == Qt::Key_Q || event->key() == Qt::Key_T || event->key() == Qt::Key_U || event->key() == Qt::Key_A || event->key() == Qt::Key_E ||
             event->key() == Qt::Key_B || event->key() == Qt::Key_F || event->key() == Qt::Key_D ||
             event->key() == Qt::Key_N || event->key() == Qt::Key_P ||
             event->key() == Qt::Key_Space || event->key() == Qt::Key_X || event->key() == Qt::Key_G ||
             event->key() == Qt::Key_K || event->key() == Qt::Key_W || event->key() == Qt::Key_Y ||
             event->key() == Qt::Key_Underscore || event->key() == Qt::Key_Slash ||
             event->key() == Qt::Key_S || event->key() == Qt::Key_R);
    }

    bool key_press_event(QKeyEvent *event) {
        if (recording_macro && !playing_macro && event->key() != Qt::Key_Control && event->key() != Qt::Key_Shift && event->key() != Qt::Key_Alt && event->key() != Qt::Key_Meta)
            recording.push_back({event->key(), event->modifiers(), event->text()});
        const auto before = text();
        const auto modifiers = KeyboardModifierState::from_qt_modifiers(event->modifiers());
        RecordedCommand command{event->key(), event->modifiers() | (meta_pending ? Qt::AltModifier : Qt::NoModifier), event->text(), control_x_pending, argument};
        const bool eligible = !repeating && !prompt_callback && !search &&
            (modifiers.control || modifiers.alt || meta_pending || event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) &&
            event->key() != Qt::Key_Underscore && event->key() != Qt::Key_Slash && event->key() != Qt::Key_Question &&
            !(control_x_pending && (event->key() == Qt::Key_U || event->key() == Qt::Key_Z));
        bool handled = execute_key(event);
        if (eligible && text() != before) repeated_command = command;
        return handled;
    }
    bool execute_key(QKeyEvent *event) {
        pending_edit.reset();
        const int key = event->key();
        if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta)
            return false;
        auto modifiers = KeyboardModifierState::from_qt_modifiers(event->modifiers());
        if (repeat_pending && key == Qt::Key_Z && !modifiers.control && !modifiers.alt) {
            repeat_command(std::nullopt); return true;
        }
        repeat_pending = false;
        if (register_callback) {
            auto callback = std::move(register_callback);
            register_callback = nullptr;
            if (search_label) search_label->hide();
            if (!(modifiers.control && key == Qt::Key_G) && !event->text().isEmpty()) callback(event->text());
            return true;
        }
        if (replacement) return replacement_key(event, modifiers);
        if (prompt_callback) return prompt_key(event, modifiers);
        if (quoted_count) {
            QString inserted = event->text();
            if (modifiers.control && key >= Qt::Key_A && key <= Qt::Key_Z) inserted = QChar(key - Qt::Key_A + 1);
            else if (key == Qt::Key_Tab) inserted = "\t";
            else if (key == Qt::Key_Return) inserted = "\n";
            else if (key == Qt::Key_Escape) inserted = QChar(27);
            replace(position(), position(), inserted.repeated(quoted_count));
            quoted_count = 0;
            return true;
        }
        if (search && search_key(event, modifiers)) return true;
        if (modifiers.control && !modifiers.command && key == Qt::Key_G) {
            meta_pending = control_x_pending = false;
            clear_argument();
            goto_pending = register_prefix = false;
            recording_macro = false;
            recording.clear();
            if (search_label) search_label->hide();
            desired_column.reset();
            deactivate_mark();
            last_action = LastAction::Other;
            return true;
        }
        if (key == Qt::Key_Escape) { meta_pending = true; return true; }
        const bool escape_prefix = meta_pending;
        const bool meta = modifiers.alt || escape_prefix;
        meta_pending = false;
        if (!control_x_pending && !modifiers.command) {
            if (modifiers.control && !meta && key == Qt::Key_U) {
                argument = argument ? std::min(10000, *argument * 4) : 4;
                return true;
            }
            if ((meta || argument) && key == Qt::Key_Minus && !argument_digits) {
                argument = -1;
                argument_negative = true;
                return true;
            }
            if ((meta || argument) && key >= Qt::Key_0 && key <= Qt::Key_9) {
                int magnitude = argument_digits ? std::abs(*argument) : 0;
                magnitude = std::min(10000, magnitude * 10 + key - Qt::Key_0);
                argument = argument_negative ? -magnitude : magnitude;
                argument_digits = true;
                return true;
            }
        }
        if (modifiers.control && !meta && key == Qt::Key_X && !control_x_pending) {
            control_x_pending = true;
            return true;
        }
        const bool explicit_argument = argument.has_value();
        const int count = argument.value_or(1);
        clear_argument();
        const LastAction previous_action = last_action;
        last_action = LastAction::Other;
        if (register_prefix) {
            register_prefix = false;
            if (!rectangle_command(key, meta)) register_command(key);
            return true;
        }
        if (key == Qt::Key_F3) { start_macro(explicit_argument); return true; }
        if (key == Qt::Key_F4) {
            if (recording_macro) end_macro(1); else play_macro(count);
            return true;
        }
        if (goto_pending) {
            goto_pending = false;
            if (key == Qt::Key_G) request_text("Goto line: ", [this](const QString &input) { goto_line(input); });
            return true;
        }
        if (!(modifiers.control && key == Qt::Key_L)) recenter_step = 0;
        if (!(meta && key == Qt::Key_Space)) spacing_begin = -1;
        const bool vertical = modifiers.control && !meta && (key == Qt::Key_N || key == Qt::Key_P);
        if (!vertical) desired_column.reset();
        if (modifiers.command) return false;
        const auto value = text();
        const int pos = position();
        if (mark_active && !modifiers.control && !meta) {
            switch (key) {
            case Qt::Key_Left: move_to(previous_character(value, pos)); return true;
            case Qt::Key_Right: move_to(next_character(value, pos)); return true;
            case Qt::Key_Up: move_vertical(false); return true;
            case Qt::Key_Down: move_vertical(true); return true;
            case Qt::Key_Home: move_to(line_start(value, pos)); return true;
            case Qt::Key_End: move_to(line_end(value, pos)); return true;
            default: break;
            }
        }
        if (control_x_pending) {
            control_x_pending = false;
            if (modifiers.control && (key == Qt::Key_S || key == Qt::Key_F || key == Qt::Key_C)) { host_command(key); return true; }
            if (!modifiers.control && key == Qt::Key_K) { host_command(Qt::Key_C); return true; }
            if (!modifiers.control && key == Qt::Key_R) { register_prefix = true; register_argument = explicit_argument; return true; }
            if (!modifiers.control && key == Qt::Key_Space) {
                rectangle_active = !rectangle_active;
                if (rectangle_active) { push_mark(pos); mark_active = true; } else deactivate_mark();
                move_to(pos); return true;
            }
            if (!modifiers.control && key == Qt::Key_ParenLeft) { start_macro(explicit_argument); return true; }
            if (!modifiers.control && key == Qt::Key_ParenRight) { end_macro(2); return true; }
            if (!modifiers.control && key == Qt::Key_E) { play_macro(count); return true; }
            if (!modifiers.control && key == Qt::Key_F) {
                if (explicit_argument) fill_column = std::max(1, count);
                else request_text("Fill column: ", [this](const QString &input) {
                    bool valid; int column = input.toInt(&valid);
                    if (valid && column > 0) fill_column = column;
                });
                return true;
            }
            if (!modifiers.control && key == Qt::Key_U) { undo(count); return true; }
            if (!modifiers.control && key == Qt::Key_Z) { repeat_command(explicit_argument ? std::optional<int>(count) : std::nullopt); return true; }
            if (modifiers.control && key == Qt::Key_O) { delete_blank_lines(); return true; }
            if (modifiers.control && key == Qt::Key_T) { transpose(2, count); return true; }
            if (modifiers.control && (key == Qt::Key_U || key == Qt::Key_L)) { change_case(key, count, true); return true; }
            if (!modifiers.control && key == Qt::Key_H) {
                push_mark(value.size());
                mark_active = true;
                move_to(0);
            }
            if (modifiers.control && !meta && key == Qt::Key_X && mark >= 0) {
                const int target = mark;
                mark = pos;
                mark_active = true;
                move_to(target);
            }
            return true;
        }
        if (meta && modifiers.control) {
            switch (key) {
            case Qt::Key_F: case Qt::Key_B: case Qt::Key_N: case Qt::Key_P:
            case Qt::Key_U: case Qt::Key_D: case Qt::Key_K: case Qt::Key_Space: case Qt::Key_At:
                expression_command(key, count, previous_action); return true;
            default: break;
            }
            if (key == Qt::Key_S || key == Qt::Key_R) { begin_search(key == Qt::Key_S, true); return true; }
            if (key == Qt::Key_Percent) { query_replace(true); return true; }
            if (key == Qt::Key_Backslash) { indent(true); return true; }
        }
        if (meta && !modifiers.control) {
            switch (key) {
            case Qt::Key_Semicolon: host_command(key); return true;
            case Qt::Key_Slash: complete_word(previous_action == LastAction::Completion, false); return true;
            case Qt::Key_Tab: complete_word(false, true); return true;
            case Qt::Key_X:
                request_text("M-x ", [this, count, explicit_argument](const QString &name) { execute_named(name, explicit_argument ? std::optional<int>(count) : std::nullopt); });
                return true;
            case Qt::Key_Q: fill_paragraph(); return true;
            case Qt::Key_Percent: query_replace(false); return true;
            case Qt::Key_A: move_to(sentence_target(-count)); return true;
            case Qt::Key_E: move_to(sentence_target(count)); return true;
            case Qt::Key_K: {
                int target = sentence_target(count);
                kill(std::min(pos, target), std::max(pos, target), target < pos, previous_action == LastAction::Kill);
                return true;
            }
            case Qt::Key_BraceLeft:
            case Qt::Key_BraceRight: {
                int target = pos;
                for (int i = 0; i < std::abs(count); ++i) target = paragraph_target(target, (key == Qt::Key_BraceRight) == (count > 0));
                move_to(target); return true;
            }
            case Qt::Key_G: goto_pending = true; return true;
            case Qt::Key_V: scroll_page(-count); return true;
            case Qt::Key_M: {
                int target = line_start(value, pos);
                while (target < value.size() && (value[target] == ' ' || value[target] == '\t')) ++target;
                move_to(target); return true;
            }
            case Qt::Key_AsciiCircum: join_line(explicit_argument); return true;
            case Qt::Key_Space: horizontal_space(true, count); return true;
            case Qt::Key_Backslash: horizontal_space(false, count); return true;
            case Qt::Key_U:
            case Qt::Key_L:
            case Qt::Key_C: change_case(key, count); return true;
            case Qt::Key_T: transpose(1, count); return true;
            case Qt::Key_B: move_to(words_target(-count)); return true;
            case Qt::Key_F: move_to(words_target(count)); return true;
            case Qt::Key_Less: push_mark(pos); move_to(0); return true;
            case Qt::Key_Greater: push_mark(pos); move_to(value.size()); return true;
            case Qt::Key_At: {
                int target = mark_active ? mark : pos;
                for (int i = 0; i < std::abs(count); ++i) target = word_target(count > 0, target);
                if (!mark_active) push_mark(target); else mark = target;
                mark_active = true;
                move_to(pos);
                return true;
            }
            case Qt::Key_H: {
                int begin = paragraph_target(pos, false);
                int end = paragraph_target(begin, true);
                push_mark(end);
                mark_active = true;
                move_to(begin);
                return true;
            }
            case Qt::Key_D:
            case Qt::Key_Backspace: {
                int target = words_target(key == Qt::Key_D ? count : -count);
                kill(std::min(pos, target), std::max(pos, target), target < pos, previous_action == LastAction::Kill);
                return true;
            }
            case Qt::Key_W:
                if (rectangle_active) { rectangle_edit(true, false); return true; }
                if (mark >= 0) {
                    save_kill(value.mid(std::min(mark, pos), std::abs(pos - mark)), false, false);
                    publish_kill();
                }
                deactivate_mark();
                return true;
            case Qt::Key_Y:
                if (previous_action == LastAction::Yank) yank(true);
                return true;
            default: break;
            }
        }
        if (modifiers.control && !meta) {
            switch (key) {
            case Qt::Key_V: scroll_page(count); return true;
            case Qt::Key_L: recenter(); return true;
            case Qt::Key_Q: quoted_count = std::max(1, count); return true;
            case Qt::Key_O:
                if (multi) { replace(pos, pos, QString(std::max(0, count), '\n')); move_to(pos); }
                return true;
            case Qt::Key_T: transpose(0, count); return true;
            case Qt::Key_A:
                move_to(line_start(value, pos)); return true;
            case Qt::Key_E: move_to(line_end(value, pos)); return true;
            case Qt::Key_B: move_to(character_target(-count)); return true;
            case Qt::Key_F: move_to(character_target(count)); return true;
            case Qt::Key_D: {
                int target = character_target(count);
                if (explicit_argument) kill(std::min(pos, target), std::max(pos, target), count < 0, previous_action == LastAction::Kill);
                else replace(pos, target, "");
                return true;
            }
            case Qt::Key_N:
            case Qt::Key_P:
                for (int i = 0; i < std::abs(count); ++i) move_vertical((key == Qt::Key_N) == (count > 0));
                return true;
            case Qt::Key_Space:
                if (explicit_argument && mark >= 0) {
                    int target = mark;
                    if (!mark_ring.empty()) {
                        mark_ring.push_back(mark);
                        mark = mark_ring.front();
                        mark_ring.pop_front();
                    }
                    mark_active = false;
                    move_to(target);
                    return true;
                }
                mark_active = !(mark_active && mark == pos);
                push_mark(pos);
                move_to(pos);
                return true;
            case Qt::Key_X: control_x_pending = true; return true;
            case Qt::Key_K: {
                int end = line_end(value, pos);
                if (explicit_argument) {
                    end = pos;
                    if (count <= 0) end = line_start(value, end);
                    for (int i = 0; i < std::abs(count); ++i)
                        end = count > 0 ? std::min(line_end(value, end) + 1, int(value.size())) : line_start(value, std::max(0, end - 1));
                } else if (end < value.size() && value.mid(pos, end - pos).trimmed().isEmpty()) ++end;
                kill(std::min(pos, end), std::max(pos, end), end < pos, previous_action == LastAction::Kill);
                return true;
            }
            case Qt::Key_W:
                if (rectangle_active) { rectangle_edit(true, true); return true; }
                if (mark >= 0) kill(std::min(mark, pos), std::max(mark, pos), pos < mark,
                                    previous_action == LastAction::Kill);
                return true;
            case Qt::Key_Y: yank(false); return true;
            case Qt::Key_S: begin_search(true); return true;
            case Qt::Key_R: begin_search(false); return true;
            case Qt::Key_Question: undo(count, true); return true;
            case Qt::Key_Underscore:
            case Qt::Key_Slash: undo(count); return true;
            default: break;
            }
        }
        if (!modifiers.control && !meta && key == Qt::Key_Tab) { indent(mark_active); return true; }
        if (!modifiers.control && !meta &&
            (!event->text().isEmpty() || key == Qt::Key_Backspace || key == Qt::Key_Delete)) {
            // Fundamental-mode insertion leaves the region's text intact.
            deactivate_mark();
            if (key == Qt::Key_Backspace) { int target = character_target(-count); replace(std::min(pos, target), std::max(pos, target), ""); return true; }
            if (key == Qt::Key_Delete) { int target = character_target(count); replace(std::min(pos, target), std::max(pos, target), ""); return true; }
            if (explicit_argument) {
                QString inserted = event->text();
                if (key == Qt::Key_Return || key == Qt::Key_Enter) inserted = multi ? "\n" : "";
                if (key == Qt::Key_Tab) inserted = "\t";
                replace(pos, pos, inserted.repeated(std::max(0, count)));
                return true;
            }
            pending_edit = std::make_pair(pos, pos);
        }
        // An unknown Escape-prefixed command must not insert its final key.
        return escape_prefix; // Otherwise let Qt handle typing and native shortcuts.
    }
};

bool VimLineEdit::event(QEvent *event) {
    if (keybinding_mode == KeybindingMode::Emacs && emacs_editor &&
        event->type() == QEvent::ShortcutOverride &&
        emacs_editor->handles_shortcut(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    return QLineEdit::event(event);
}

bool VimTextEdit::event(QEvent *event) {
    if (keybinding_mode == KeybindingMode::Emacs && emacs_editor &&
        event->type() == QEvent::ShortcutOverride &&
        emacs_editor->handles_shortcut(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    return QTextEdit::event(event);
}

VimLineEdit::VimLineEdit(QWidget *parent) : QLineEdit(parent) {
    for (QObject *child : children()) {
        if (child->inherits("QWidgetLineControl")) {
            child->installEventFilter(this);
            break;
        }
    }
    editor = new VimEditor(this);
    emacs_editor = new EmacsEditor(this);
}

bool VimLineEdit::eventFilter(QObject *watched, QEvent *event) {
    if (get_vim_enabled() && event->type() == QEvent::Timer) {
        return true;
    }
    return QLineEdit::eventFilter(watched, event);
}

void VimLineEdit::keyPressEvent(QKeyEvent *event) {
    if (keybinding_mode == KeybindingMode::Emacs && emacs_editor->key_press_event(event)) {
        event->accept();
        return;
    }

    if (!get_vim_enabled()){
        QLineEdit::keyPressEvent(event);
        emacs_editor->finish_key_event();
        return;
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
    emacs_editor = new EmacsEditor(this);
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
    if (get_vim_enabled() && event->type() == QEvent::Timer) {
        return true;
    }
    return QTextEdit::eventFilter(watched, event);
}

QList<QTextCursor> VimTextEdit::selection_cursors() const {
    if (!get_vim_enabled()) {
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
    if (keybinding_mode == KeybindingMode::Emacs && emacs_editor->key_press_event(event)) {
        event->accept();
        return;
    }


    if (!get_vim_enabled()) {
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
        QTextEdit::keyPressEvent(event);
        emacs_editor->finish_key_event();
        return;
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


void VimLineEdit::set_vim_enabled(bool enabled) {
    set_keybinding_mode(enabled ? KeybindingMode::Vim : KeybindingMode::Standard);
}

bool VimLineEdit::get_vim_enabled() const {
    return keybinding_mode == KeybindingMode::Vim;
}

void VimLineEdit::set_keybinding_mode(KeybindingMode mode) {
    if (keybinding_mode == mode) return;
    keybinding_mode = mode;
    emacs_editor->reset();
    editor->command_line_edit->hide();
    editor->update_cursor_style();
}

void VimTextEdit::set_vim_enabled(bool enabled) {
    set_keybinding_mode(enabled ? KeybindingMode::Vim : KeybindingMode::Standard);
}

bool VimTextEdit::get_vim_enabled() const {
    return keybinding_mode == KeybindingMode::Vim;
}

void VimTextEdit::set_keybinding_mode(KeybindingMode mode) {
    if (keybinding_mode == mode) return;
    keybinding_mode = mode;
    emacs_editor->reset();
    setExtraSelections({});
    editor->command_line_edit->hide();
    editor->update_cursor_style();
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
