# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""The scenes layer on the console, through the Device Portal REST API.

The payload is packed (package-appx.ps1 -BootNro) and installed once (deploy.ps1 -NoPackage) under a
version above the installed one: the suite never uninstalls. Each payload run then writes skip.txt and
only.txt into the package's LocalState, launches the app, waits for jadyx_diag.txt to say it exited,
and downloads results.txt, the read-back pictures and the log. The console has no D3D12 debug layer.

The installed package afterwards is the payload, not the library: install a library build after the run.
"""
import json
import os
import subprocess
import tempfile
import time
import urllib.parse
import base64

STATE = "\\LocalState\\jadyx"
SDMC = STATE + "\\sdmc\\renderer_suite"
LOG = STATE + "\\log"


class Portal:
    def __init__(self, address):
        self.base = f"https://{address}:11443"
        fd, self.jar = tempfile.mkstemp(prefix="jadyx-suite-portal-", suffix=".txt")
        os.close(fd)
        self.token = None

    def close(self):
        if os.path.exists(self.jar):
            os.remove(self.jar)

    def curl(self, args, tries=6, check=True, binary=False):
        """curl.exe with the portal's cookie jar; retries a dropped connection."""
        for attempt in range(1, tries + 1):
            out = subprocess.run(["curl.exe", "-sSk", "--max-time", "120", "-b", self.jar, "-c", self.jar] + args,
                                 capture_output=True)
            if out.returncode == 0 or not check:
                return out.stdout if binary else out.stdout.decode("utf-8", errors="replace")
            if attempt == tries:
                raise RuntimeError(f"portal request failed ({out.returncode}): {' '.join(args)}\n"
                                   + out.stderr.decode(errors="replace"))
            time.sleep(10 * attempt)

    def post_headers(self):
        # The portal answers a POST or DELETE only with the CSRF token it set as a cookie on a GET.
        self.curl([f"{self.base}/api/os/info"])
        with open(self.jar, encoding="utf-8", errors="replace") as f:
            for line in f:
                fields = line.rstrip("\n").split("\t")
                if len(fields) >= 7 and fields[5] == "CSRF-Token":
                    self.token = fields[6]
        if not self.token:
            raise RuntimeError("the portal sent no CSRF token")
        return ["-H", f"X-CSRF-Token: {self.token}"]

    def package(self):
        data = json.loads(self.curl([f"{self.base}/api/app/packagemanager/packages"]))
        found = [p for p in data["InstalledPackages"] if p["PackageFullName"].startswith("Jadyx_")]
        if not found:
            raise RuntimeError("Jadyx is not installed on the console")
        # The portal can list more than one version (one per console user): the newest is the one installed.
        version = lambda p: tuple(p["Version"][k] for k in ("Major", "Minor", "Build", "Revision"))
        return max(found, key=version)

    def query(self, package, path):
        return ("knownfolderid=LocalAppData&packagefullname=" + package["PackageFullName"]
                + "&path=" + urllib.parse.quote(path))

    def list_files(self, package, path):
        text = self.curl([f"{self.base}/api/filesystem/apps/files?{self.query(package, path)}"], check=False)
        try:
            return [item["Name"] for item in json.loads(text).get("Items", []) if item.get("Type") != 16]
        except ValueError:
            return []

    def download(self, package, path, name, dest):
        url = f"{self.base}/api/filesystem/apps/file?{self.query(package, path)}&filename={urllib.parse.quote(name)}"
        out = subprocess.run(["curl.exe", "-sSk", "--fail", "--max-time", "120", "-o", dest, url], capture_output=True)
        return out.returncode == 0

    def upload(self, package, path, source):
        self.curl(self.post_headers() + ["-F", f"file=@{source}",
                                         f"{self.base}/api/filesystem/apps/file?{self.query(package, path)}"])

    def delete(self, package, path, name):
        self.curl(self.post_headers() + ["-X", "DELETE", f"{self.base}/api/filesystem/apps/file?"
                                         f"{self.query(package, path)}&filename={urllib.parse.quote(name)}"],
                  check=False)

    def stop(self, package):
        name = b64(package["PackageFullName"])
        self.curl(self.post_headers() + ["-X", "DELETE", f"{self.base}/api/taskmanager/app?package={name}"],
                  check=False)

    def start(self, package):
        self.curl(self.post_headers() + ["-X", "POST", "-H", "Content-Length: 0",
                                         f"{self.base}/api/taskmanager/app?appid={b64(package['PackageRelativeId'])}"
                                         f"&package={b64(package['PackageFullName'])}"])


def b64(text):
    return urllib.parse.quote(base64.b64encode(text.encode()).decode())


class ConsoleTarget:
    """Runs the payload on the console; the same calls as the PC target in scenes.py."""

    def __init__(self, repo, nro, args, boot_cfg, report):
        self.repo, self.args, self.report = repo, args, report
        self.portal = Portal(args.console)
        self.work = tempfile.mkdtemp(prefix="jadyx-suite-console-")
        # --no-package runs the payload package already installed under --version.
        if not args.no_package:
            self.install(nro, boot_cfg)
        self.package = self.portal.package()
        if not self.package["PackageFullName"].startswith(f"Jadyx_{args.version}_"):
            raise RuntimeError(f"installed package is {self.package['PackageFullName']}, not {args.version}")
        self.lines = []

    def install(self, nro, boot_cfg):
        powershell = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
        package = subprocess.run(powershell + [os.path.join(self.repo, "tools", "xbox", "package-appx.ps1"),
                                               "-BootNro", nro, "-RunSeconds", str(self.args.scene_seconds),
                                               "-Version", self.args.version, "-RequireShaders",
                                               "-BootCfg", ",".join(boot_cfg)],
                                 cwd=self.repo, capture_output=True, text=True, stdin=subprocess.DEVNULL)
        if package.returncode != 0:
            raise RuntimeError("package-appx.ps1 failed:\n" + (package.stdout + package.stderr)[-3000:])
        self.report(f"console: installing {self.args.version} on {self.args.console}")
        for attempt in range(1, 5):
            deploy = subprocess.run(powershell + [os.path.join(self.repo, "tools", "xbox", "deploy.ps1"),
                                                  "-Version", self.args.version, "-NoBuild", "-NoPackage",
                                                  "-Console", self.args.console],
                                    cwd=self.repo, capture_output=True, text=True, stdin=subprocess.DEVNULL)
            if deploy.returncode == 0:
                return
            self.report(f"console: install attempt {attempt} failed: {(deploy.stdout + deploy.stderr)[-500:]}")
            time.sleep(60)
        raise RuntimeError("console install failed")

    def prepare(self, skip, only):
        for name, names in (("skip.txt", skip), ("only.txt", only)):
            path = os.path.join(self.work, name)
            with open(path, "w", newline="\n") as f:
                f.write("".join(n + "\n" for n in names))
            self.portal.upload(self.package, SDMC, path)
        self.portal.delete(self.package, SDMC, "results.txt")

    def run(self):
        """One payload run; returns (exited, log text)."""
        self.portal.stop(self.package)
        time.sleep(3)
        for name in ("jadyx_diag.txt", "jadyx_log.txt"):
            self.portal.delete(self.package, LOG, name)
        self.portal.start(self.package)
        # The payload ends itself after its last scene; run_seconds is the backstop.
        deadline = time.monotonic() + self.args.scene_seconds + 120
        diag_path = os.path.join(self.work, "jadyx_diag.txt")
        exited = False
        time.sleep(20)
        while time.monotonic() < deadline:
            if self.portal.download(self.package, LOG, "jadyx_diag.txt", diag_path):
                with open(diag_path, encoding="utf-8", errors="replace") as f:
                    text = f.read()
                if "exiting" in text or "CRASH" in text:
                    exited = True
                    break
            time.sleep(10)
        time.sleep(5)
        self.portal.stop(self.package)
        time.sleep(3)
        log = ""
        log_path = os.path.join(self.work, "jadyx_log.txt")
        if self.portal.download(self.package, LOG, "jadyx_log.txt", log_path):
            with open(log_path, encoding="utf-8", errors="replace") as f:
                log = f.read()
        self.lines = []
        results_path = os.path.join(self.work, "results.txt")
        if os.path.exists(results_path):
            os.remove(results_path)
        if self.portal.download(self.package, SDMC, "results.txt", results_path):
            with open(results_path, encoding="utf-8", errors="replace") as f:
                self.lines = f.read().splitlines()
        return exited, log

    def results(self):
        return self.lines

    def pictures(self, dest):
        for name in self.portal.list_files(self.package, SDMC):
            if name.endswith(".bin"):
                self.portal.download(self.package, SDMC, name, os.path.join(dest, name))

    def dumps(self, dest):
        for name in self.portal.list_files(self.package, LOG):
            if name.startswith(("shader_", "dxil_")):
                self.portal.download(self.package, LOG, name, os.path.join(dest, name))

    def close(self):
        self.portal.close()
