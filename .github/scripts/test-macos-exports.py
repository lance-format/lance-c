#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

"""Exercise compiler argument forwarding without requiring a macOS linker."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ExportShimTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lance exports ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exports = self.root / "exports"
        self.original = "_lance_fixture\n_crc_fast_fixture\n"
        self.exports.write_text(self.original)
        self.capture = self.root / "arguments.json"
        self.linker = self.root / "linker"
        self.linker.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "Path(os.environ['LINKER_CAPTURE']).write_text(json.dumps(sys.argv[1:]))\n"
            "sys.exit(int(os.environ.get('LINKER_STATUS', '0')))\n"
        )
        self.linker.chmod(0o755)
        self.env = dict(os.environ, LANCE_C_MACOS_LINKER=str(self.linker),
                        LINKER_CAPTURE=str(self.capture))
        self.shim = Path(__file__).with_name("limit-macos-exports.sh")

    def run_link(self, flags, output="liblance_c.dylib"):
        args = ["-o", str(self.root / output), *flags]
        result = subprocess.run(["bash", str(self.shim), *args], env=self.env,
                                text=True, capture_output=True)
        if self.capture.exists():
            self.assertEqual(json.loads(self.capture.read_text()), args)
        return result

    def test_forwarding_forms(self):
        for flags in (
            ["-Wl,-exported_symbols_list", f"-Wl,{self.exports}"],
            [f"-Wl,-exported_symbols_list,{self.exports}"],
            ["-Xlinker", "-exported_symbols_list", "-Xlinker", str(self.exports)],
        ):
            with self.subTest(flags=flags):
                self.exports.write_text(self.original)
                result = self.run_link(flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.exports.read_text(), "_lance_*\n")
                self.assertTrue(self.capture.exists())

    def test_other_outputs_are_untouched(self):
        for output in ("libother.dylib", "build-script", "test-executable"):
            result = self.run_link(
                ["-Wl,-exported_symbols_list", f"-Wl,{self.exports}"], output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.exports.read_text(), self.original)

    def test_missing_export_list_fails_before_linking(self):
        for flags in ([], ["-Wl,-exported_symbols_list"],
                      ["-Wl,-exported_symbols_list", f"-Wl,{self.root / 'absent'}"]):
            result = self.run_link(flags)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exported-symbol list", result.stderr)
            self.assertFalse(self.capture.exists())
            self.assertEqual(self.exports.read_text(), self.original)

    def test_linker_failure_is_preserved(self):
        self.env["LINKER_STATUS"] = "23"
        result = self.run_link(["-Wl,-exported_symbols_list", f"-Wl,{self.exports}"])
        self.assertEqual(result.returncode, 23)
        self.assertTrue(self.capture.exists())


if __name__ == "__main__":
    unittest.main()
