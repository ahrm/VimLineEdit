
#include <QApplication>
#include "VimLineEdit.h"
#include <QLayout>
#include <QLabel>
#include <QComboBox>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QWidget *main_widget = new QWidget;
    main_widget->setLayout(new QVBoxLayout);
    main_widget->layout()->setContentsMargins(10, 10, 10, 10);
    main_widget->layout()->setSpacing(10);
    main_widget->setWindowTitle("Text Editor Example");

    QVimEditor::VimTextEdit vim_text_edit;
    QVimEditor::VimLineEdit vim_line_edit;

    auto *mode_selector = new QComboBox;
    mode_selector->addItem("Vim", static_cast<int>(QVimEditor::KeybindingMode::Vim));
    mode_selector->addItem("Emacs", static_cast<int>(QVimEditor::KeybindingMode::Emacs));
    auto *mode_label = new QLabel("&Keybindings:");
    mode_label->setBuddy(mode_selector);
    auto *mode_row = new QHBoxLayout;
    mode_row->addWidget(mode_label);
    mode_row->addWidget(mode_selector);
    mode_row->addStretch();
    static_cast<QVBoxLayout *>(main_widget->layout())->addLayout(mode_row);

    QObject::connect(mode_selector, &QComboBox::currentIndexChanged, main_widget, [&](int) {
        const auto mode = static_cast<QVimEditor::KeybindingMode>(mode_selector->currentData().toInt());
        vim_line_edit.set_keybinding_mode(mode);
        vim_text_edit.set_keybinding_mode(mode);
    });

    main_widget->layout()->addWidget(new QLabel("Line Edit:"));
    main_widget->layout()->addWidget(&vim_line_edit);
    main_widget->layout()->addWidget(new QLabel("Text Edit:"));
    main_widget->layout()->addWidget(&vim_text_edit);
    main_widget->show();

    vim_text_edit.setFocus();

    QObject::connect(&vim_text_edit, &QVimEditor::VimTextEdit::quitCommand, [&]() {
        a.quit();
    });

    return a.exec();
}
