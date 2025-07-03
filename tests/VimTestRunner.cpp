#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QKeyEvent>
#include <QDebug>
#include <iostream>

#include "../VimLineEdit.h" // Assuming VimLineEdit.h is in the parent directory

// Function to simulate keystrokes on VimLineEdit
void simulate_keystrokes(VimLineEdit *lineEdit, const QString &keystrokes) {
    int index = 0;
    int BACKSPACE_KEY = 0xfffd;
    while (index < keystrokes.length()) {
        QChar c = keystrokes.at(index);
        // qDebug() << "Processing keystroke at index" << index << ":" << c;
        Qt::Key key = Qt::Key_unknown;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        QString text_val = "";

        if (c.isPrint() && (c.unicode() != BACKSPACE_KEY)) {
            key = static_cast<Qt::Key>(c.toUpper().unicode());
            if (c.isUpper()) {
                modifiers |= Qt::ShiftModifier;
            }
            text_val = QString(c);
        }
        else if (c == '\e') { // Escape key
            key = Qt::Key_Escape;
        }
        else if (c == ':') { // Colon for command mode
            key = Qt::Key_Colon;
            modifiers |= Qt::ShiftModifier; // Shift for colon
            text_val = ":";                 // Set text for colon
        }
        else if (c == ' ') {
            key = Qt::Key_Space;
        }
        else if (c == '\n') {
            key = Qt::Key_Return;
        }
        else if (c == '\b') { // Backspace
            key = Qt::Key_Backspace;
        }
        else if ((int)c.unicode() == 0xfffd) { // Backspace
        // else if ((int)c.unicode() == 0x0080) { // Backspace
            key = Qt::Key_Backspace;
            // the vim script represents backspace as \ufffd followed by kb
            // so we skip over the kb here
            index+=2; 
        }
        else if ((int)c.unicode() == 0x17) {
            key = Qt::Key_W;
            #ifdef Q_OS_MACOS
            modifiers |= Qt::MetaModifier;
            #else
            modifiers |= Qt::ControlModifier;
            #endif
        }
        else if ((int)c.unicode() == 0x1) {
            key = Qt::Key_A;
            #ifdef Q_OS_MACOS
            modifiers |= Qt::MetaModifier;
            #else
            modifiers |= Qt::ControlModifier;
            #endif
        }

        // Add more key mappings as needed

        if (key != Qt::Key_unknown) {
            // qDebug() << "Simulating key:" << c << "(Unicode:" << c.unicode() << ")" << "Key:" <<
            // key << "Modifiers:" << modifiers << "Text:" << text_val;

            QKeyEvent press_event(QEvent::KeyPress, key, modifiers, text_val);
            QApplication::sendEvent(lineEdit, &press_event);
            QApplication::processEvents(); // Process events immediately

            QKeyEvent release_event(QEvent::KeyRelease, key, modifiers, text_val);
            QApplication::sendEvent(lineEdit, &release_event);
            QApplication::processEvents(); // Process events immediately
        }
        index++;
    }
}

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    VimLineEdit line_edit;

    QString test_cases_path = "/Users/ali/projects/vim_lineedit/test_generator/test_cases";

    QDir test_dir(test_cases_path);
    if (!test_dir.exists()) {
        std::cerr << "Test cases directory not found: " << test_cases_path.toStdString() << std::endl;
        return 1;
    }

    QStringList filters;
    filters << "test_case_*.keystrokes.txt";
    QFileInfoList keystrokes_file = test_dir.entryInfoList(filters, QDir::Files, QDir::Name);

    int num_passed_tests = 0;
    int num_failed_tests = 0;
    int only_index = 16;
    int current_test_index = 0;
    if (argc > 1) {
        bool ok;
        only_index = QString(argv[1]).toInt(&ok);
        if (!ok || only_index < 0) {
            std::cerr << "Invalid test index provided. Please provide a valid integer." << std::endl;
            return 1;
        }
    }


    for (const QFileInfo& keystrokeFile : keystrokes_file) {

        current_test_index++;
        QString base_name = keystrokeFile.baseName(); // e.g., "test_case_0.keystrokes"
        QString index_str = base_name.split("_").last(); // e.g., "0"
        QString test_name = "test_case_" + index_str;


        QString keystrokes_file_path = keystrokeFile.absoluteFilePath();
        QString exptected_output_file_path = test_cases_path + "/test_case_" + index_str + ".txt";

        if (only_index != -1) {
            base_name = "test_case_" + QString::number(only_index);
            index_str = QString::number(only_index);
            test_name = base_name;
            keystrokes_file_path = test_cases_path + "/" + base_name + ".keystrokes.txt";
            exptected_output_file_path = test_cases_path + "/" + base_name + ".txt";
        }

        QFile keystrokes_file(keystrokes_file_path);
        QFile expected_output_file(exptected_output_file_path);

        if (!keystrokes_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Could not open keystrokes file: " << keystrokes_file_path.toStdString() << std::endl;
            continue;
        }
        if (!expected_output_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Could not open expected output file: " << exptected_output_file_path.toStdString() << std::endl;
            keystrokes_file.close();
            continue;
        }

        // QByteArray keystrokes = keystrokesFile.readAll();
        QTextStream keystrokes_stream(&keystrokes_file);
        QString keystrokes = keystrokes_stream.readAll();
        keystrokes_file.close();

        QTextStream expected_output_stream(&expected_output_file);
        QString expected_output = expected_output_stream.readAll().trimmed();
        expected_output_file.close();

        // No cleaning of keystrokes; VimLineEdit should handle all of them.

        line_edit.clear(); // Clear previous test data
        line_edit.setFocus(); // Ensure the widget has focus for events

        simulate_keystrokes(&line_edit, keystrokes);

        QString actual_output = line_edit.toPlainText();

        if (actual_output.trimmed() == expected_output.trimmed()) {
            std::cout << "PASS: " << test_name.toStdString() << std::endl;
            std::cout << "  Value: '" << actual_output.toStdString() << "'" << std::endl;
            num_passed_tests++;
        } else {
            std::cout << "FAIL: " << test_name.toStdString() << std::endl;
            std::cout << "  Expected: \n" << expected_output.toStdString() << "" << std::endl;
            std::cout << "  Actual: \n" << actual_output.toStdString() << "" << std::endl;
            std::cout << "  Keystrokes: '" << keystrokes.toStdString() << "'" << std::endl;
            num_failed_tests++;
        }
        if (only_index != -1){
            break;
        }
    }

    std::cout << std::endl;
    std::cout << "Tests run: " << (num_passed_tests + num_failed_tests) << std::endl;
    std::cout << "Failures: " << num_failed_tests << std::endl;
    std::cout << "Passed: " << num_passed_tests << std::endl;

    return (num_failed_tests == 0) ? 0 : 1;
}
