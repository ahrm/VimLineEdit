"""End-to-end checks that the reference/comparison harness fails honestly."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "test_generator" / "emacs_test_generator.py"
spec = importlib.util.spec_from_file_location("emacs_generator", GENERATOR)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class InfrastructureTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "emacs_test_case_0.keystrokes.txt"
        self.path.write_bytes(" A😀 ".encode("utf-8"))

    def save_reference(self):
        text = generator.reference(generator.read_keys(self.path), os.environ["REFERENCE_EMACS"])
        generator.reference_path(self.path).write_bytes(text.encode("utf-8"))
        return text

    def run_compare(self, *options):
        return subprocess.run([sys.executable, str(GENERATOR), "compare", str(self.path),
                               "--runner", os.environ["REFERENCE_QT_RUNNER"], *options],
                              text=True, capture_output=True, timeout=30)

    def test_live_unicode_and_whitespace(self):
        self.assertEqual(self.save_reference(), " A😀 ")
        result = self.run_compare("--mode", "standard", "--live", "--emacs", os.environ["REFERENCE_EMACS"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_comparison_rejects_whitespace_changes(self):
        self.save_reference()
        for text in (" A😀", " A😀 \n"):
            generator.reference_path(self.path).write_bytes(text.encode("utf-8"))
            result = self.run_compare("--mode", "standard")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("--- expected", result.stdout)

    def test_changed_sequence_fails_verification(self):
        self.save_reference()
        self.path.write_bytes(b"changed")
        result = subprocess.run([sys.executable, str(GENERATOR), "verify", str(self.path),
                                 "--emacs", os.environ["REFERENCE_EMACS"]],
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_missing_reference_is_failure(self):
        result = self.run_compare("--mode", "standard")
        self.assertEqual(result.returncode, 1)

    def test_emacs_mode_replays_input(self):
        self.save_reference()
        result = self.run_compare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_control_meta_and_mark_are_replayed_in_emacs(self):
        self.path.write_bytes(b"one two\x01\x00\x1bf\x17\x05\x19")
        self.assertEqual(self.save_reference(), " twoone")

    def test_raw_special_keys_and_unicode_continuation_bytes(self):
        # U+0100 contains 0x80 inside UTF-8; it must not become a special key.
        self.path.write_bytes("Āb".encode("utf-8") + b"\x80klX")
        self.assertEqual(self.save_reference(), "ĀXb")
        result = self.run_compare("--mode", "standard")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_interactive_reference_records_and_replays_macros(self):
        self.path.write_bytes(b"\x80k3hello\x80k4\x80k4")
        self.assertEqual(self.save_reference(), "hellohello")

    def test_interactive_command_errors_are_failures(self):
        self.path.write_bytes(b"\x80k4")  # No macro has been defined.
        with self.assertRaises(ValueError):
            self.save_reference()

    def test_malformed_input_is_rejected(self):
        for keys in (b"\x80zz", b"\x80k", b"\xff", b"\x1b"):
            self.path.write_bytes(keys)
            with self.assertRaises(ValueError):
                generator.read_keys(self.path)


if __name__ == "__main__":
    unittest.main()
