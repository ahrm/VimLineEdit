#include <QApplication>
#include <QKeyEvent>
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
    widget.set_vim_enabled(false);
    check(widget.get_keybinding_mode() == KeybindingMode::Standard, "Legacy disable selects standard mode");
    widget.set_vim_enabled(true);
    check(widget.get_keybinding_mode() == KeybindingMode::Vim, "Legacy enable selects Vim mode");
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    test_modes<VimLineEdit>();
    test_modes<VimTextEdit>();
    return failures ? 1 : 0;
}
