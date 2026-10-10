#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QClipboard>
#include <QDialog>
#include <QScrollBar>
#include <iostream>
#include <optional>
#include <variant>
#include "../VimLineEdit.h"

using namespace QVimEditor;
static int failures = 0;
static void check(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
static Qt::KeyboardModifier control_modifier() {
#ifdef Q_OS_MACOS
    return Qt::MetaModifier;
#else
    return Qt::ControlModifier;
#endif
}
static void control(QWidget &widget, int key) {
    QKeyEvent event(QEvent::KeyPress, key, control_modifier());
    QApplication::sendEvent(&widget, &event);
}
static QString contents(const VimLineEdit &widget) { return widget.text(); }
static QString contents(const VimTextEdit &widget) { return widget.toPlainText(); }
static int position(const VimLineEdit &widget) { return widget.cursorPosition(); }
static int position(const VimTextEdit &widget) { return widget.textCursor().position(); }
static QString selection(const VimLineEdit &widget) { return widget.selectedText(); }
static QString selection(const VimTextEdit &widget) { return widget.textCursor().selectedText(); }

template<class Widget> static void test_modes() {
    Widget widget;
    check(widget.get_vim_enabled(), "Default mode should remain Vim");
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    check(!widget.get_vim_enabled(), "Emacs must not use Vim dispatch");
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, "abc");
    QApplication::sendEvent(&widget, &typed);
    control(widget, Qt::Key_A);
    check(position(widget) == 0, "C-a should move to the beginning");
    widget.setReadOnly(true);
    control(widget, Qt::Key_D);
    check(contents(widget) == "abc", "Read-only Emacs deletion changed text");
    control(widget, Qt::Key_Underscore);
    check(contents(widget) == "abc", "Read-only Emacs undo changed text");
    control(widget, Qt::Key_F);
    check(position(widget) == 1, "Read-only navigation should still work");
    control(widget, Qt::Key_Space);
    control(widget, Qt::Key_F);
    check(selection(widget) == "b", "Mark should extend a region during movement");
    control(widget, Qt::Key_G);
    check(selection(widget).isEmpty() && position(widget) == 2, "C-g should deactivate the region without moving point");
    QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_A, control_modifier());
    shortcut.ignore();
    QApplication::sendEvent(&widget, &shortcut);
    check(shortcut.isAccepted(), "Emacs should claim its shortcuts before application actions");
    control(widget, Qt::Key_A);
    control(widget, Qt::Key_S);
    QKeyEvent query(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b");
    QApplication::sendEvent(&widget, &query);
    check(contents(widget) == "abc" && position(widget) == 2, "Read-only search should move point without editing");
    auto *prompt = widget.template findChild<QLabel *>("emacsSearchPrompt");
    check(prompt && !prompt->isHidden() && prompt->text().contains("b"), "Search should display its query");
    widget.setReadOnly(false);
    control(widget, Qt::Key_G);
    control(widget, Qt::Key_A);
    control(widget, Qt::Key_D);
    control(widget, Qt::Key_Underscore);
    control(widget, Qt::Key_Question);
    check(contents(widget) == "bc", "C-? should redo the deletion");
    control(widget, Qt::Key_Underscore);
    control(widget, Qt::Key_S);
    widget.set_vim_enabled(false);
    check(prompt && prompt->isHidden(), "Changing modes should close the search prompt");
    check(widget.get_keybinding_mode() == KeybindingMode::Standard, "Legacy disable selects standard mode");
    widget.set_vim_enabled(true);
    check(widget.get_keybinding_mode() == KeybindingMode::Vim, "Legacy enable selects Vim mode");
    widget.setReadOnly(false);
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QKeyEvent backward_word(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b");
    QApplication::sendEvent(&widget, &escape);
    QApplication::sendEvent(&widget, &backward_word);
    check(position(widget) == 0 && contents(widget) == "abc", "Escape prefix should work like Meta");
    QApplication::sendEvent(&widget, &escape);
    QKeyEvent unsupported(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, "q");
    QApplication::sendEvent(&widget, &unsupported);
    check(contents(widget) == "abc", "Unsupported Escape-prefixed commands must not type their final key");
}

static void test_scrolling() {
    VimTextEdit widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    widget.resize(400, 200);
    widget.setPlainText(QString("line of text\n").repeated(100));
    widget.show();
    QApplication::processEvents();
    control(widget, Qt::Key_V);
    check(widget.verticalScrollBar()->value() > 0 && position(widget) > 0, "C-v should scroll and move point into the page");
    QKeyEvent back(QEvent::KeyPress, Qt::Key_V, Qt::AltModifier);
    QApplication::sendEvent(&widget, &back);
    check(widget.verticalScrollBar()->value() == 0, "M-v should scroll back");
    auto cursor = widget.textCursor(); cursor.setPosition(600); widget.setTextCursor(cursor);
    control(widget, Qt::Key_L);
    check(widget.cursorRect().center().y() > 20 && widget.cursorRect().center().y() < 180, "C-l should recenter point");
}

static void test_shared_kill_ring() {
    VimLineEdit first;
    VimTextEdit second;
    first.set_keybinding_mode(KeybindingMode::Emacs);
    second.set_keybinding_mode(KeybindingMode::Emacs);
    first.setText("shared text");
    control(first, Qt::Key_A);
    control(first, Qt::Key_K);
    check(QApplication::clipboard()->text() == "shared text", "Emacs kills should reach the system clipboard");
    control(second, Qt::Key_Y);
    check(second.toPlainText() == "shared text", "Emacs widgets should share the kill ring");
    QApplication::clipboard()->setText("external");
    control(second, Qt::Key_Y);
    check(second.toPlainText() == "shared textexternal", "C-y should import external clipboard text");
    QKeyEvent rotate(QEvent::KeyPress, Qt::Key_Y, Qt::AltModifier);
    QApplication::sendEvent(&second, &rotate);
    check(second.toPlainText() == "shared textshared text", "M-y should rotate from external clipboard into the shared ring");
}

static void test_completion_hook() {
    class CompletingEdit : public VimTextEdit {
    public:
        QString requested;
        QStringList get_autocomplete_suggestions(const QString &word) override {
            requested = word;
            return {"completed"};
        }
    } widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    widget.setPlainText("comp");
    control(widget, Qt::Key_E);
    QKeyEvent complete(QEvent::KeyPress, Qt::Key_Tab, Qt::AltModifier);
    QApplication::sendEvent(&widget, &complete);
    check(widget.requested == "comp" && widget.toPlainText() == "completed", "M-TAB should use the host completion hook");
}

template<class Widget> static void test_host_commands() {
    Widget widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    int saves = 0, quits = 0;
    QObject::connect(&widget, &Widget::writeCommand, [&]() { ++saves; });
    QObject::connect(&widget, &Widget::quitCommand, [&]() { ++quits; });
    control(widget, Qt::Key_X); control(widget, Qt::Key_S);
    control(widget, Qt::Key_X); control(widget, Qt::Key_C);
    check(saves == 1 && quits == 1, "Emacs file commands should emit existing host signals");
    control(widget, Qt::Key_X); control(widget, Qt::Key_G);
    control(widget, Qt::Key_S);
    check(saves == 1, "Cancelling C-x must prevent a later accidental save");
}
static void test_open_command() {
    VimTextEdit widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    int opens = 0;
    int comment_begin = -1, comment_end = -1;
    QObject::connect(&widget, &VimTextEdit::commentRequested, [&](int begin, int end) { comment_begin = begin; comment_end = end; });
    QObject::connect(&widget, &VimTextEdit::openFile, [&]() { ++opens; });
    control(widget, Qt::Key_X); control(widget, Qt::Key_F);
    check(opens == 1, "C-x C-f should request an open through the host");
    widget.setPlainText("line one\nline two");
    QKeyEvent comment(QEvent::KeyPress, Qt::Key_Semicolon, Qt::AltModifier);
    QApplication::sendEvent(&widget, &comment);
    check(comment_begin == 0 && comment_end == 8, "M-; should pass the current line to the host comment hook");
}

static void test_mouse_selection_and_wrapping() {
    VimLineEdit line;
    line.set_keybinding_mode(KeybindingMode::Emacs);
    line.setText("abcd");
    line.setSelection(1, 2);
    control(line, Qt::Key_W);
    check(line.text() == "ad", "C-w should kill a native single-line selection");
    VimTextEdit multi;
    multi.set_keybinding_mode(KeybindingMode::Emacs);
    multi.setPlainText("abcd");
    auto selected = multi.textCursor();
    selected.setPosition(1); selected.setPosition(3, QTextCursor::KeepAnchor);
    multi.setTextCursor(selected);
    control(multi, Qt::Key_W);
    check(multi.toPlainText() == "ad", "C-w should kill a native multiline selection");
    multi.resize(160, 200);
    multi.setLineWrapMode(QTextEdit::WidgetWidth);
    multi.setPlainText(QString("word ").repeated(30) + "\nlast line");
    multi.show(); QApplication::processEvents();
    auto expected = multi.textCursor();
    expected.movePosition(QTextCursor::Down);
    control(multi, Qt::Key_N);
    check(position(multi) == expected.position(), "C-n should follow visual lines in a wrapped widget");
    control(multi, Qt::Key_P);
    check(position(multi) == 0, "C-p should return to the same visual column");
    control(multi, Qt::Key_X);
    multi.set_keybinding_mode(KeybindingMode::Standard);
    multi.set_keybinding_mode(KeybindingMode::Emacs);
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, "h");
    QApplication::sendEvent(&multi, &typed);
    check(multi.toPlainText().startsWith("hword"), "Mode switching should clear pending Emacs prefixes");
}

static void test_command_help() {
    VimTextEdit widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    QKeyEvent prompt(QEvent::KeyPress, Qt::Key_X, Qt::AltModifier);
    QApplication::sendEvent(&widget, &prompt);
    QKeyEvent name(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, "describe-bindings");
    QApplication::sendEvent(&widget, &name);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&widget, &enter);
    auto *help = widget.findChild<QDialog *>("emacsBindingsHelp");
    auto *contents = help ? help->findChild<QTextEdit *>() : nullptr;
    check(contents && contents->isReadOnly() && contents->toPlainText().contains("kill-rectangle"), "Emacs binding help should be scrollable and include rectangle commands");
    if (help) help->close();
}

static void test_external_edit_cancels_prompt() {
    VimTextEdit widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    QKeyEvent prompt(QEvent::KeyPress, Qt::Key_X, Qt::AltModifier);
    QApplication::sendEvent(&widget, &prompt);
    widget.setPlainText("new");
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "X");
    QApplication::sendEvent(&widget, &typed);
    check(widget.toPlainText() == "Xnew", "An external buffer replacement should cancel pending Emacs prompts");
}

static void test_replacement_dialects() {
    VimTextEdit widget;
    widget.setPlainText("ab ab");
    widget.editor->handle_command(VimLineEditCommand::CommandCommand);
    widget.editor->command_line_edit->setText(R"(:%s/\(a\)b/[&]-\1-\&-\r/g)");
    QKeyEvent submit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(widget.editor->command_line_edit, &submit);
    check(widget.toPlainText() == "[ab]-a-&-\n [ab]-a-&-\n", "Vim replacement expansion should preserve its ampersand and newline rules");
    widget.setPlainText("ab ab");
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    QKeyEvent prompt(QEvent::KeyPress, Qt::Key_X, Qt::AltModifier);
    QApplication::sendEvent(&widget, &prompt);
    for (const auto &input : {QString("replace-regexp"), QString(R"(\(a\)b)"), QString(R"([\&]-\1-&-\\)")}) {
        QKeyEvent typed(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, input);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&widget, &typed);
        QApplication::sendEvent(&widget, &enter);
    }
    check(widget.toPlainText() == "[ab]-a-&-\\ [ab]-a-&-\\", "Emacs replacement expansion should preserve its distinct escaping rules");
}

template<class Widget> static void test_named_command_repetition() {
    Widget widget;
    widget.set_keybinding_mode(KeybindingMode::Emacs);
    auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, QString text = {}) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
        QApplication::sendEvent(&widget, &event);
    };
    key(Qt::Key_unknown, Qt::NoModifier, "abcdefghij");
    control(widget, Qt::Key_A);
    control(widget, Qt::Key_U);
    key(Qt::Key_2, Qt::NoModifier, "2");
    key(Qt::Key_X, Qt::AltModifier);
    key(Qt::Key_unknown, Qt::NoModifier, "delete-char");
    key(Qt::Key_Return);
    check(contents(widget) == "cdefghij", "M-x should pass the numeric argument to the selected command");
    control(widget, Qt::Key_X); key(Qt::Key_Z);
    check(contents(widget) == "efghij", "Repeat should retain the named command's numeric argument");
    key(Qt::Key_Z);
    check(contents(widget) == "ghij", "A following z should repeat the same command again");
    control(widget, Qt::Key_U);
    key(Qt::Key_1, Qt::NoModifier, "1");
    control(widget, Qt::Key_X); key(Qt::Key_Z);
    check(contents(widget) == "hij", "An explicit repeat argument should override the original count");
    control(widget, Qt::Key_G);
    key(Qt::Key_Delete);
    control(widget, Qt::Key_X); key(Qt::Key_Z);
    check(contents(widget) == "j", "Delete should replace the previous repeat command");
    control(widget, Qt::Key_X); key(Qt::Key_R);
    control(widget, Qt::Key_G);
    key(Qt::Key_S, Qt::NoModifier, "s");
    check(contents(widget) == "sj", "Cancelling a register prefix should restore ordinary typing");
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    test_modes<VimLineEdit>();
    test_modes<VimTextEdit>();
    test_scrolling();
    test_shared_kill_ring();
    test_completion_hook();
    test_host_commands<VimLineEdit>();
    test_host_commands<VimTextEdit>();
    test_open_command();
    test_mouse_selection_and_wrapping();
    test_command_help();
    test_external_edit_cancels_prompt();
    test_replacement_dialects();
    test_named_command_repetition<VimLineEdit>();
    test_named_command_repetition<VimTextEdit>();
    return failures ? 1 : 0;
}
