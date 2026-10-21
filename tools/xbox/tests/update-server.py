# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local stand-in for the GitHub releases a test package with update_server= asks for.

Each subfolder of ROOT is one release, named by its tag (v0.3.7.2) and holding
Jadyx_<version>_x64.appx plus the update.json/update.sig that create-update.ps1 signed.
An optional notes.md becomes the release body.

    py tools/xbox/tests/update-server.py ROOT [--port 8787]
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re

REPO = "https://github.com/JulianDr14/jadyx/releases/download/"


def releases(root):
    listed = []
    for folder in sorted(root.iterdir(), reverse=True):
        if not folder.is_dir() or not re.fullmatch(r"v\d+\.\d+\.\d+\.\d+", folder.name):
            continue
        manifest = json.loads((folder / "update.json").read_bytes())
        notes = folder / "notes.md"
        listed.append({
            "tag_name": folder.name,
            "draft": False,
            "prerelease": manifest.get("channel") == "preview",
            "body": notes.read_text(encoding="utf-8") if notes.exists() else "",
            "assets": [{"name": f.name, "size": f.stat().st_size,
                        "browser_download_url": REPO + folder.name + "/" + f.name}
                       for f in sorted(folder.iterdir()) if f.is_file() and f.name != "notes.md"],
        })
    return json.dumps(listed).encode()


def handler(root):
    class Handler(http.server.BaseHTTPRequestHandler):
        def send(self, status, length, etag=None):
            self.send_response(status)
            self.send_header("Content-Length", str(length))
            if etag:
                self.send_header("ETag", etag)
            self.end_headers()

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/releases":
                # Only page 1 holds releases; the updater stops at a page under 100 entries.
                body = releases(root) if "page=1" in self.path.split("?", 1)[-1].split("&") else b"[]"
                self.send(200, len(body))
                self.wfile.write(body)
                return
            match = re.fullmatch(r"/releases/download/(v[\d.]+)/([\w.\-]+)", path)
            file = root / match[1] / match[2] if match else None
            if not file or not file.is_file() or file.name == "notes.md":
                self.send(404, 0)
                return
            stat = file.stat()
            self.send(200, stat.st_size,
                      '"' + hashlib.sha1(f"{file}{stat.st_size}{stat.st_mtime_ns}".encode()).hexdigest() + '"')
            with file.open("rb") as data:
                while chunk := data.read(1 << 20):
                    self.wfile.write(chunk)

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    print(releases(args.root).decode())
    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler(args.root.resolve()))
    print(f"serving {args.root} on port {args.port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
