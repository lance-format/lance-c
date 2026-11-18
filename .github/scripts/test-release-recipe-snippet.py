#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

"""Check the publish recipe snippet against the stable and beta.1 releases.

The fixtures are the published SHA512SUMS files. The snippet must emit the
package-manager keys (without the version) and preserve each digest.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("release-recipe-snippet.sh")
OUTPUT = re.compile(r'^set\(LANCE_C_SHA512_(\S+)  "([0-9a-f]{128})"\)$')

STABLE_SUMS = """488463b062e25a100b56837613b37efd332e031a82a669eb2a882435d72d11e72dd3c966f9e53ca7197845126cecf2a64be722086f45f83deb2d8171a19d542b  lance-c-v0.1.9-aarch64-apple-darwin.tar.xz
e170c35b61dbaa237a19d59f67940b96636dddd999fd4e855cb6ccdac478f3f08860c2ce5731aa1f3731bf2b86214bcd28033aee250b84aef91cf1e1b1c0172f  lance-c-v0.1.9-aarch64-unknown-linux-gnu-debian12.tar.xz
0a8c91a07ca82a15a635a2110cd70ed85bc3eaf4009284a8509395f9da4ef9fa85510f68cddec466a3609b22c5c2c56c0e4e004279dbd8d074d2c61097f30ee9  lance-c-v0.1.9-aarch64-unknown-linux-gnu.tar.xz
5aba369dd70b244d905298eacac93a3f5ddaed4b3df43e6a9fe0184303f1ce06f60794da6ea6288f15ade20f29120a8683bc1fbfdcef65b06f1e1e7944493595  lance-c-v0.1.9-x86_64-unknown-linux-gnu-debian12.tar.xz
edcd1bb4c8097486c80aaf4b41c72bec42561370ba97cfddc68015a5df958a2b14c38d85ee5625dba59544bfee403f779e2e73f5508a29142b10c94f5ab5b5e7  lance-c-v0.1.9-x86_64-unknown-linux-gnu.tar.xz"""

BETA_SUMS = """612b1d5dd2b19a02aed90d31cae618033cbd3d3242b1008e700e31e22bcc6dadf154d557c5f872948e9f5782b98b63eecfb3b8c9a95eff6bf271a185b402c5cd  lance-c-v0.2.0-beta.1-aarch64-apple-darwin.tar.xz
745249ae61e4e7e0fd65d20eef4e03766a451ab572430745b7fea1eb67ff9d04d899ca21c992dcac3be39e5b9823eee8afe9f1047cd08a94cc9c6939ddf9b04b *lance-c-v0.2.0-beta.1-aarch64-pc-windows-msvc.tar.xz
74d671ab5b4214d1ef28b52017e9668c16b6bc989a115aa6d0b08b82649d08393261ce5667e7aae7e7b42b71d0c0a3f48aa5bf02026c6071b20c672a804dcdd5  lance-c-v0.2.0-beta.1-aarch64-unknown-linux-gnu.tar.xz
867c2f861a4369a6742b6d650b630c2fb6d9ada1bfd93ddee6dd0f9363a9c2d6cb056a0529201ed4000d1b7b06a48f893713967a3bf92e900f2acd052e27be76 *lance-c-v0.2.0-beta.1-x86_64-pc-windows-msvc.tar.xz
f03cc519ffaa6809032303e242c4c6457d0ffa71f6669028dc59f61c134b2d1146fd45f8fdb3762701279736ff72f4baf2376df52089a3fdbbd4d12f3149c2d0  lance-c-v0.2.0-beta.1-x86_64-unknown-linux-gnu.tar.xz"""


def read_sums(text):
    sums = {}
    for line in text.strip().splitlines():
        digest, name = line.split(None, 1)
        sums[name.lstrip("*")] = digest
    return sums


class ReleaseRecipeSnippetTest(unittest.TestCase):
    def run_snippet(self, sums_text, version, *, names=None, with_checksums=True):
        sums = read_sums(sums_text)
        temp = tempfile.TemporaryDirectory(prefix="release-recipe-snippet ")
        self.addCleanup(temp.cleanup)
        dist = Path(temp.name) / "dist"
        dist.mkdir()
        for name in names if names is not None else sums:
            (dist / name).touch()
            if with_checksums and name in sums:
                (dist / f"{name}.sha512").write_text(f"{sums[name]}  {name}\n")
        result = subprocess.run(
            ["bash", str(SCRIPT), str(dist), version],
            text=True,
            capture_output=True,
        )
        return result, sums

    def assert_snippet(self, result, sums, version):
        self.assertEqual(result.returncode, 0, result.stderr)
        prefix = f"lance-c-v{version}-"
        expected = {}
        for name, digest in sums.items():
            self.assertTrue(name.startswith(prefix), name)
            target = name[len(prefix):]
            self.assertTrue(target.endswith(".tar.xz"), target)
            expected[target[:-len(".tar.xz")]] = digest

        emitted = {}
        for line in result.stdout.splitlines():
            match = OUTPUT.match(line)
            self.assertIsNotNone(match, f"unexpected output line: {line}")
            target, digest = match.groups()
            self.assertNotIn(target, emitted)
            emitted[target] = digest
        self.assertEqual(emitted, expected)

    def test_stable_v0_1_9(self):
        result, sums = self.run_snippet(STABLE_SUMS, "0.1.9")
        self.assertEqual(len(sums), 5)
        self.assert_snippet(result, sums, "0.1.9")

    def test_prerelease_v0_2_0_beta_1(self):
        result, sums = self.run_snippet(BETA_SUMS, "0.2.0-beta.1")
        self.assertEqual(len(sums), 5)
        self.assert_snippet(result, sums, "0.2.0-beta.1")
        self.assertNotIn("LANCE_C_SHA512_beta.1-", result.stdout)

    def test_rejects_mismatched_version(self):
        result, _ = self.run_snippet(BETA_SUMS, "0.1.9")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("unexpected archive name for version 0.1.9", result.stderr)

    def test_rejects_missing_checksum(self):
        name = "lance-c-v0.1.9-x86_64-unknown-linux-gnu.tar.xz"
        result, _ = self.run_snippet(
            STABLE_SUMS, "0.1.9", names=[name], with_checksums=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
