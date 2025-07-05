
#include <QApplication>
#include "VimLineEdit.h"
#include <QLayout>
#include <QLabel>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QWidget *main_widget = new QWidget;
    main_widget->setLayout(new QVBoxLayout);
    main_widget->layout()->setContentsMargins(10, 10, 10, 10);
    main_widget->layout()->setSpacing(10);
    main_widget->setWindowTitle("Vim Line Edit Example");

    QVimEditor::VimTextEdit vim_text_edit;
    QVimEditor::VimLineEdit vim_line_edit;

    main_widget->layout()->addWidget(new QLabel("Vim Line Edit:"));
    main_widget->layout()->addWidget(&vim_line_edit);
    main_widget->layout()->addWidget(new QLabel("Vim Text Edit:"));
    main_widget->layout()->addWidget(&vim_text_edit);
    main_widget->show();

    vim_text_edit.setFocus();

    QObject::connect(&vim_text_edit, &QVimEditor::VimTextEdit::quitCommand, [&]() {
        a.quit();
    });

    return a.exec();
}
