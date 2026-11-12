# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

"""Exercise native OSS HTTP initialization from fresh, statically linked C processes.

Build lance-c first, then run on Linux:
    python3 tests/static_oss_transport_test.py target/release/liblance_c.a

No OSS account is needed. A local HTTP server returns 404 for a missing manifest.
The assertion is that an HTTP request reaches it, not merely that opening fails.
Unlike a Rust test binary, the C executable must pull initialization from the archive.
"""

import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("this static-link regression test currently supports Linux")
    library = args.library.resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def missing(self):
            requests.append((self.command, self.path))
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()

        do_HEAD = missing
        do_GET = missing

        def log_message(self, *_args):
            pass

    with tempfile.TemporaryDirectory(prefix="lance-static-oss-") as directory:
        executable = Path(directory) / "test_oss_transport"
        # Pass the archive explicitly; -llance_c could silently select the shared library.
        # Do not use --whole-archive: ordinary native linking must retain initialization.
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wl,--gc-sections",
               "-I", str(root / "include"), str(root / "tests/cpp/test_oss_transport.c"),
               str(library), "-lgcc_s", "-lutil", "-lrt", "-lpthread", "-lm", "-ldl",
               "-o", str(executable)],
            check=True,
        )
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("AWS_", "OSS_", "ALIBABA_CLOUD_"))
            and key.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")
        }
        environment["NO_PROXY"] = "127.0.0.1,localhost"
        # Each mode starts a new process so an earlier call cannot hide missing initialization.
        for mode in ("ordinary", "shared"):
            requests.clear()
            with HTTPServer(("127.0.0.1", 0), Handler) as server:
                thread = threading.Thread(
                    target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
                )
                thread.start()
                try:
                    result = subprocess.run(
                        [str(executable), f"http://127.0.0.1:{server.server_port}", mode],
                        env=environment, capture_output=True, text=True, timeout=30,
                    )
                finally:
                    server.shutdown()
                    thread.join()
            assert result.returncode == 0, f"{mode}: {result.stderr}"
            assert any("/_versions/" in path for _, path in requests), (
                f"{mode}: no manifest HTTP request reached the server: {result.stderr}"
            )
            print(f"PASS: {mode} OSS open reached the local HTTP server")


if __name__ == "__main__":
    main()
