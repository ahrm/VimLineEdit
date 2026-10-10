#!/usr/bin/env python3
"""Generate, verify, or compare Emacs .keystrokes.txt / .txt test pairs."""

import argparse
import difflib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
CASES = HERE / "emacs_test_cases"
# Vim's three-byte special-key encoding, also accepted by these fixtures.
SPECIAL_KEYS = {b"kb": "DEL", b"kl": "LEFT", b"kr": "RIGHT", b"ku": "UP",
                b"kd": "DOWN", b"kh": "HOME", b"@7": "END", b"kD": "DELETE"}


def read_keys(path):
    """Decode UTF-8 typing, control bytes, and Escape-prefixed Meta keys."""
    raw = path.read_bytes()
    events = []
    index = 0
    meta = False
    while index < len(raw):
        byte = raw[index]
        if byte == 0x1b:
            if meta:
                raise ValueError("Repeated Escape prefix is not supported")
            meta = True
            index += 1
            continue
        if byte == 0x80:
            key = SPECIAL_KEYS.get(raw[index + 1:index + 3])
            if key is None:
                raise ValueError(f"Unsupported special key at byte {index}")
            index += 3
            event = {"key": key, "modifiers": [], "text": ""}
        else:
            length = 1 if byte < 0x80 else 2 if byte < 0xe0 else 3 if byte < 0xf0 else 4
            char = raw[index:index + length].decode("utf-8", errors="strict")
            index += length
            if byte in (9, 10, 13, 127):
                event = {"key": {9: "TAB", 10: "RET", 13: "RET", 127: "DEL"}[byte],
                         "modifiers": [], "text": ""}
            elif byte < 32:
                key = " " if byte == 0 else chr(byte + 96) if byte <= 26 else chr(byte + 64)
                event = {"key": key, "modifiers": ["control"], "text": ""}
            else:
                event = {"key": char, "modifiers": ["shift"] if char.isupper() else [], "text": char}
        if meta:
            event["modifiers"].append("meta")
            event["text"] = ""
            meta = False
        events.append(event)
    if meta:
        raise ValueError("Incomplete Escape/Meta prefix at end of keystrokes")
    return events


def reference(events, emacs):
    with tempfile.TemporaryDirectory(prefix="emacs-reference-") as directory:
        input_path = Path(directory) / "input.json"
        output_path = Path(directory) / "output.txt"
        input_path.write_text(json.dumps(events, ensure_ascii=False), encoding="utf-8")
        env = dict(os.environ, EMACS_FIXTURE_INPUT=str(input_path),
                   EMACS_FIXTURE_OUTPUT=str(output_path))
        result = subprocess.run([emacs, "--batch", "-Q", "--load", str(HERE / "emacs_reference.el")],
                                env=env, capture_output=True, text=True, timeout=20)
        if result.returncode or not output_path.exists():
            raise ValueError(f"Emacs failed\n{result.stdout}{result.stderr}")
        return output_path.read_bytes().decode("utf-8")


def reference_path(path):
    if not path.name.endswith(".keystrokes.txt"):
        raise ValueError("Fixture filenames must end with .keystrokes.txt")
    return path.with_name(path.name.removesuffix(".keystrokes.txt") + ".txt")


def compare(expected, actual):
    if expected == actual:
        return True
    # JSON escaping here makes whitespace and missing final newlines visible.
    print("".join(difflib.unified_diff(
        [json.dumps(expected, ensure_ascii=False) + "\n"],
        [json.dumps(actual, ensure_ascii=False) + "\n"],
        fromfile="expected", tofile="actual")))
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("generate", "verify", "compare"))
    parser.add_argument("cases", nargs="*", type=Path, help="*.keystrokes.txt files; defaults to all fixtures")
    parser.add_argument("--emacs", default=os.environ.get("EMACS", "emacs"))
    parser.add_argument("--runner", type=Path, help="built emacs_lineedit_tests executable")
    parser.add_argument("--mode", choices=("emacs", "standard"), default="emacs")
    parser.add_argument("--widget", choices=("line", "text"), default="text")
    parser.add_argument("--live", action="store_true", help="compare against a fresh Emacs run")
    args = parser.parse_args()
    if args.action == "compare" and not args.runner:
        parser.error("compare requires --runner")
    paths = args.cases or sorted(CASES.glob("emacs_test_case_*.keystrokes.txt"))
    if not paths:
        parser.error("no fixtures found")
    failures = 0
    for path in paths:
        try:
            output_path = reference_path(path)
            events = read_keys(path)
            if args.action == "generate":
                output_path.write_bytes(reference(events, args.emacs).encode("utf-8"))
            elif args.action == "verify":
                if not compare(output_path.read_bytes().decode("utf-8"), reference(events, args.emacs)):
                    print(f"FAIL {path.name}")
                    failures += 1
                    continue
            else:
                expected = reference(events, args.emacs) if args.live else output_path.read_bytes().decode("utf-8")
                env = dict(os.environ)
                env.setdefault("QT_QPA_PLATFORM", "offscreen")
                replay = {"widget": args.widget, "events": events}
                result = subprocess.run([str(args.runner.resolve()), "--mode", args.mode],
                                        input=json.dumps(replay, ensure_ascii=False),
                                        capture_output=True, text=True, env=env, timeout=20)
                if result.returncode:
                    raise ValueError(f"Qt replay failed: {result.stdout}{result.stderr}")
                if not compare(expected, json.loads(result.stdout)):
                    print(f"FAIL {path.name}")
                    failures += 1
                    continue
            print(f"PASS {path.name}")
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            print(f"FAIL {path.name}: {error}", file=sys.stderr)
            failures += 1
    print(f"{len(paths)} fixtures; {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
