# Emacs compatibility tests

Fixtures use the same two-file format as the Vim tests:

- `emacs_test_case_N.keystrokes.txt`: raw UTF-8 typing and key bytes.
- `emacs_test_case_N.txt`: expected UTF-8 buffer contents from real Emacs.

Every case starts empty and types its own setup text. Movement and selection
are tested through subsequent edits, just as in the Vim fixtures. Comparisons
preserve all whitespace, including final newlines.

Control keys use their actual bytes (`C-a` = `0x01`, `C-SPC` = `0x00`,
`C-_`/undo = `0x1f`). Meta uses an Escape prefix (`M-f` = `0x1b` then `f`).
Return is CR or LF; Backspace is DEL (`0x7f`). The decoder also accepts Vim's
three-byte codes for Backspace, arrows, Home, End, and Delete. Unsupported
special codes, invalid UTF-8, and incomplete Meta prefixes fail explicitly.
Don't append a newline to a keystroke file unless it is part of the input.

From the repository root:

```sh
python3 test_generator/emacs_test_generator.py generate
python3 test_generator/emacs_test_generator.py verify
python3 test_generator/emacs_test_generator.py compare --runner build/tests/emacs_lineedit_tests
python3 test_generator/emacs_test_generator.py compare --live --runner build/tests/emacs_lineedit_tests
ctest --test-dir build -L emacs --output-on-failure
```

Pass individual `.keystrokes.txt` paths after the action to select cases.
`--emacs PATH` selects the reference executable. Saved comparisons do not need
Emacs installed. `--widget line` selects the single-line widget; the default
is multiline, matching the Vim runner.

The reference uses a fresh `emacs --batch -Q` Fundamental mode buffer with
Transient Mark mode enabled, electric indentation and clipboard sharing
disabled, and a fresh kill ring. Consecutive typed characters share an undo
unit; other commands create undo boundaries. Only use trusted editing input,
not file/process/exit commands. Finish searches with Return or `C-g`.

Select Emacs bindings with `set_keybinding_mode(KeybindingMode::Emacs)` on
either widget. Cases 0–2 also run in standard Qt mode to check the harness.

| Cases | Coverage |
| --- | --- |
| 0–2 | Unicode, whitespace, newlines, arrows, Backspace |
| 3–5 | Character, word, and vertical motion |
| 6–7 | Kill/yank and kill-ring rotation |
| 8–10 | Forward/backward regions and region kill/yank |
| 11–12 | Exchange point/mark and cancellation |
| 13–14 | Undo and incremental search |
| 15–19 | Word boundaries, retained vertical column, Unicode, region insertion, prefix cancellation |
| 20–24 | Consecutive kills, backward word kill, region copy, undo grouping |
| 25–30 | Reverse/repeated search, cancellation, query deletion, failing search, smart case |
