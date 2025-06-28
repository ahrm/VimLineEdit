
#include <QApplication>
#include "VimLineEdit.h"
#include <QLayout>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QWidget *main_widget = new QWidget;
    main_widget->setLayout(new QVBoxLayout);
    main_widget->layout()->setContentsMargins(10, 10, 10, 10);
    main_widget->layout()->setSpacing(10);
    main_widget->setWindowTitle("Vim Line Edit Example");

    VimLineEdit vim_line_edit;

    main_widget->layout()->addWidget(&vim_line_edit);
    main_widget->show();

    return a.exec();
}
