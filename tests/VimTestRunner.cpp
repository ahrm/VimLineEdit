#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QKeyEvent>
#include <QDebug>
#include <iostream>

#include "../VimLineEdit.h" // Assuming VimLineEdit.h is in the parent directory

// Function to simulate keystrokes on VimLineEdit
void simulateKeystrokes(VimLineEdit *lineEdit, const QString &keystrokes) {
    int index = 0;
    int BACKSPACE_KEY = 0xfffd;
    while (index < keystrokes.length()) {
        QChar c = keystrokes.at(index);
        // qDebug() << "Processing keystroke:" << c;
        // Convert QChar to Qt::Key
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
            key = Qt::Key_Backspace;
            // the vim script represents backspace as \ufffd followed by kb
            // so we skip over the kb here
            index+=2; 
        }
        // Add more key mappings as needed

        if (key != Qt::Key_unknown) {
            // qDebug() << "Simulating key:" << c << "(Unicode:" << c.unicode() << ")" << "Key:" <<
            // key << "Modifiers:" << modifiers << "Text:" << text_val;

            QKeyEvent pressEvent(QEvent::KeyPress, key, modifiers, text_val);
            QApplication::sendEvent(lineEdit, &pressEvent);
            QApplication::processEvents(); // Process events immediately

            QKeyEvent releaseEvent(QEvent::KeyRelease, key, modifiers, text_val);
            QApplication::sendEvent(lineEdit, &releaseEvent);
            QApplication::processEvents(); // Process events immediately
        }
        index++;
    }
}

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    VimLineEdit lineEdit;

    QString testCasesPath = "/Users/ali/projects/vim_lineedit/test_generator/test_cases";

    QDir testDir(testCasesPath);
    if (!testDir.exists()) {
        std::cerr << "Test cases directory not found: " << testCasesPath.toStdString() << std::endl;
        return 1;
    }

    QStringList filters;
    filters << "test_case_*.keystrokes.txt";
    QFileInfoList keystrokeFiles = testDir.entryInfoList(filters, QDir::Files, QDir::Name);

    int passedTests = 0;
    int failedTests = 0;

    for (const QFileInfo& keystrokeFile : keystrokeFiles) {
        QString baseName = keystrokeFile.baseName(); // e.g., "test_case_0.keystrokes"
        QString indexStr = baseName.split("_").last(); // e.g., "0"
        QString testName = "test_case_" + indexStr;

        QString keystrokesFilePath = keystrokeFile.absoluteFilePath();
        QString expectedOutputFilePath = testCasesPath + "/test_case_" + indexStr + ".txt";

        QFile keystrokesFile(keystrokesFilePath);
        QFile expectedOutputFile(expectedOutputFilePath);

        if (!keystrokesFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Could not open keystrokes file: " << keystrokesFilePath.toStdString() << std::endl;
            continue;
        }
        if (!expectedOutputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Could not open expected output file: " << expectedOutputFilePath.toStdString() << std::endl;
            keystrokesFile.close();
            continue;
        }

        QTextStream keystrokesStream(&keystrokesFile);
        QString keystrokes = keystrokesStream.readAll();
        keystrokesFile.close();

        QTextStream expectedOutputStream(&expectedOutputFile);
        QString expectedOutput = expectedOutputStream.readAll().trimmed();
        expectedOutputFile.close();

        // No cleaning of keystrokes; VimLineEdit should handle all of them.

        lineEdit.clear(); // Clear previous test data
        lineEdit.setFocus(); // Ensure the widget has focus for events

        simulateKeystrokes(&lineEdit, keystrokes);

        QString actualOutput = lineEdit.toPlainText();

        if (actualOutput == expectedOutput) {
            std::cout << "PASS: " << testName.toStdString() << std::endl;
            passedTests++;
        } else {
            std::cout << "FAIL: " << testName.toStdString() << std::endl;
            std::cout << "  Expected: '" << expectedOutput.toStdString() << "'" << std::endl;
            std::cout << "  Actual: '" << actualOutput.toStdString() << "'" << std::endl;
            std::cout << "  Keystrokes: '" << keystrokes.toStdString() << "'" << std::endl;
            failedTests++;
        }
    }

    std::cout << std::endl;
    std::cout << "Tests run: " << (passedTests + failedTests) << std::endl;
    std::cout << "Failures: " << failedTests << std::endl;
    std::cout << "Passed: " << passedTests << std::endl;

    return (failedTests == 0) ? 0 : 1;
}
