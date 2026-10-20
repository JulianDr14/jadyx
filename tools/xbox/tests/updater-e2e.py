# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay real signed candidate assets privately; never publish a test release."""
import hashlib
import http.server
import json
import pathlib
import subprocess
import sys
import threading
import time

assets = pathlib.Path(sys.argv[2]).resolve()
metadata = json.loads((assets / "update.json").read_text(encoding="utf-8"))
requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        requests.append((path, self.headers.get("Range")))
        if path == "/releases":
            base = metadata["url"].rsplit("/", 1)[0] + "/"
            release = [dict(tag_name=metadata["tag"], draft=False, prerelease=False, assets=[
                dict(name="update.json", browser_download_url=base + "update.json"),
                dict(name="update.sig", browser_download_url=base + "update.sig"),
                dict(name=metadata["url"].rsplit("/", 1)[1],
                     browser_download_url=metadata["url"], size=metadata["size"])])]
            data = json.dumps(release).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        file = assets / path[1:]
        if file.name not in ["update.json", "update.sig", metadata["url"].rsplit("/", 1)[1]]:
            self.send_error(404)
            return
        size = file.stat().st_size
        start = 0
        etag = '"' + metadata["sha256"] + '"'
        if self.headers.get("Range"):
            assert self.headers.get("If-Range") == etag
            start = int(self.headers["Range"][6:-1])
        self.send_response(206 if start else 200)
        self.send_header("ETag", etag)
        self.send_header("Content-Length", str(size - start))
        if start:
            self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
        self.end_headers()
        try:
            with file.open("rb") as stream:
                stream.seek(start)
                while chunk := stream.read(65536):
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    if file.suffix == ".appx":
                        time.sleep(.002)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    folder = pathlib.Path("build-uwp/test-results/updater") / ("e2e-" + str(time.monotonic_ns()))
    subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}", str(folder)],
                   check=True, timeout=90)
    downloads = [r for r in requests if r[0].endswith(".appx")]
    assert len(downloads) == 2 and downloads[1][1] is not None, downloads
    assert hashlib.sha256((folder / "jadyx-update.appx").read_bytes()).hexdigest() == metadata["sha256"]
    print("Private signed-release fixture: resumed Range, no redownload on reopen, "
          "installed-version confirmation passed")
finally:
    server.shutdown()
    server.server_close()
