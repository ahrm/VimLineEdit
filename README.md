# VimLineEdit

A drop-in replacement for QLineEdit and QTextEdit that supports Vim-like keybindings.

#### Usage
Simply add `VimLineEdit.cpp` and `VimLineEdit.h` to your project, and use `QVimEditor::VimLineEdit` or `QVimEditor::VimTextEdit` in place of `QLineEdit` or `QTextEdit`.

### Goals
This is a simple implementation intended to be used in my PDF viewer, sioyek, to provide Vim-like keybindings for text input (e.g. when editing annotations).
### Non-goals
- To be a full implementation of all Vim features (it is mainly used to edit simple text, such as annotations so more advanced vim features are not needed).
- To be fast (again, it is mainly used to edit simple, short text, so performance is not a priority).

### Bug Reporting
If you find a discrepancy between the Vim behavior and this implementation, you can use the `test_generator/vim_test_generator.py` script which opens a vim instance (using the `vim` commandline program, if you don't have a `vim` program you need to change the script or define an alias). Then you can enter the sequence of keys you want to test, and it will generate a test case for you which involves two files: a `.txt` file and a `.keystrokes.txt` file. Please provide these files if you open a behaviour-related issue.