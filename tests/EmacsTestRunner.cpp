#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <variant>

#include "../VimLineEdit.h"

// The only integration point needed when Emacs bindings are implemented.
// Return true after selecting the real Emacs mode on the requested widget.
static bool enable_emacs_bindings(QWidget *) {
    return false;
}

static int key_code(const QString &key) {
    static const QMap<QString, int> special = {
        {"RET", Qt::Key_Return}, {"TAB", Qt::Key_Tab}, {"ESC", Qt::Key_Escape},
        {"DEL", Qt::Key_Backspace}, {"DELETE", Qt::Key_Delete},
        {"LEFT", Qt::Key_Left}, {"RIGHT", Qt::Key_Right},
        {"UP", Qt::Key_Up}, {"DOWN", Qt::Key_Down},
        {"HOME", Qt::Key_Home}, {"END", Qt::Key_End},
    };
    if (special.contains(key)) return special.value(key);
    const auto points = key.toUpper().toUcs4();
    if (key.toUcs4().size() != 1) return -1;
    return points.size() == 1 ? static_cast<int>(points[0]) : Qt::Key_unknown;
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 3 || arguments[1] != "--mode" ||
        (arguments[2] != "emacs" && arguments[2] != "standard")) {
        std::cerr << "Usage: emacs_lineedit_tests --mode emacs|standard < replay.json\n";
        return 2;
    }
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) {
        std::cerr << "Cannot read replay from stdin\n";
        return 2;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        std::cerr << "Invalid replay JSON\n";
        return 2;
    }
    const auto replay = document.object();
    const QString kind = replay["widget"].toString();
    if ((kind != "line" && kind != "text") || !replay["events"].isArray()) {
        std::cerr << "Replay requires widget and events\n";
        return 2;
    }
    std::unique_ptr<QWidget> widget;
    QVimEditor::VimLineEdit *line = nullptr;
    QVimEditor::VimTextEdit *multi = nullptr;
    if (kind == "line") {
        line = new QVimEditor::VimLineEdit;
        widget.reset(line);
        line->set_vim_enabled(false);
    } else {
        multi = new QVimEditor::VimTextEdit;
        widget.reset(multi);
        multi->set_vim_enabled(false);
        multi->setLineWrapMode(QTextEdit::NoWrap);
    }
    if (arguments[2] == "emacs" && !enable_emacs_bindings(widget.get())) {
        std::cerr << "Emacs bindings are not implemented; connect enable_emacs_bindings() when available.\n";
        return 77;
    }
    widget->resize(800, 600);
    widget->show();
    widget->setFocus();
    application.processEvents();
    for (const auto &value : replay["events"].toArray()) {
        const auto event = value.toObject();
        const int key = key_code(event["key"].toString());
        if (key < 0 || !event["modifiers"].isArray() || !event["text"].isString()) {
            std::cerr << "Invalid replay event\n";
            return 2;
        }
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        for (const auto &modifier : event["modifiers"].toArray()) {
            const QString name = modifier.toString();
            if (name == "shift") modifiers |= Qt::ShiftModifier;
            else if (name == "meta") modifiers |= Qt::AltModifier;
            else if (name == "control") {
#ifdef Q_OS_MACOS
                modifiers |= Qt::MetaModifier; // Physical Control in Qt on macOS.
#else
                modifiers |= Qt::ControlModifier;
#endif
            } else {
                std::cerr << "Unsupported modifier\n";
                return 2;
            }
        }
        QString event_text = event["text"].toString();
        if (key == Qt::Key_Return) event_text = "\r";
        if (key == Qt::Key_Tab) event_text = "\t";
        QKeyEvent press(QEvent::KeyPress, key, modifiers, event_text);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers, event_text);
        QApplication::sendEvent(widget.get(), &press);
        QApplication::sendEvent(widget.get(), &release);
    }
    const QString actual = line ? line->text() : multi->toPlainText();
    // JSON is only the internal process transport, never the fixture format.
    const QByteArray encoded = QJsonDocument(QJsonArray{actual}).toJson(QJsonDocument::Compact);
    std::cout << encoded.mid(1, encoded.size() - 2).constData() << '\n';
    return 0;
}
