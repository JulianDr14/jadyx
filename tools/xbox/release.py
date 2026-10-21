# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release helpers for .github/workflows/release.yml.

  release.py version 0.3.0 --manifest dist/uwp/AppxManifest.xml --releases releases.json
      Checks that 0.3.0 can be published and prints package_version=0.3.0.0 and tag=v0.3.0.
  release.py bump 0.3.0.0 --manifest dist/uwp/AppxManifest.xml [--issue-template bug_report.yml]
      Writes the package version into the manifest, keeping the rest of the file byte for byte,
      and lists it first in the bug report's version dropdown.
  release.py notes --tag v0.3.0 [--previous v0.2.0] [--repo owner/name]
      Prints the release notes: the conventional commits on the first-parent history since the
      previous release, grouped by kind.

A release accepts three or four components; the tag and package use four canonical components.
The fourth component publishes incremental fixes. A release must be newer than
the version in the manifest as well as every published release: the console only updates upwards.
"""

import argparse
import json
import re
import subprocess
import sys

VERSION_ATTR = re.compile(r'(<Identity\b[^>]*?\bVersion=")([0-9.]+)(")', re.S)
# The "- 0.3.0.0" options of the bug report's "Jadyx version" dropdown.
ISSUE_VERSIONS = re.compile(r"(label: Jadyx version\n(?:[ \t]+(?!options:)\S.*\n)*?[ \t]+options:\n)((?:[ \t]+- .*\n)+)")
CONVENTIONAL = re.compile(r"^(?P<kind>[a-z]+)(?:\((?P<scope>[^)]*)\))?(?P<breaking>!)?:\s*(?P<text>.+)$")
SECTIONS = (
    ("feat", "New"),
    ("fix", "Fixes"),
    ("perf", "Performance"),
)
# Kinds that do not change what a player gets.
HIDDEN = {"chore", "ci", "build", "docs", "test", "style", "refactor", "revert"}


def parse_release(text):
    if not re.fullmatch(r"v?(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*))?", text):
        raise ValueError(f"'{text}' is not MAJOR.MINOR.PATCH")
    parts = tuple(int(part) for part in text.lstrip("v").split("."))
    if any(part > 65535 for part in parts):
        raise ValueError("version components must be <= 65535")
    return parts


def parse_package(text):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", text):
        raise ValueError(f"'{text}' is not MAJOR.MINOR.BUILD.REVISION")
    return tuple(int(part) for part in text.split("."))


def read_manifest(path):
    with open(path, "rb") as source:
        raw = source.read()
    return raw, raw.decode("utf-8-sig")


def manifest_version(path):
    _, text = read_manifest(path)
    match = VERSION_ATTR.search(text)
    if not match:
        raise ValueError(f"{path} has no Identity Version")
    return parse_package(match.group(2))


def validate(version, manifest, releases):
    """Returns the package version for the release `version`, or raises ValueError."""
    release = parse_release(version)
    package = release + (0,) if len(release) == 3 else release
    current = manifest_version(manifest)
    if package < current:
        raise ValueError(
            f"{'.'.join(map(str, package))} is older than the manifest's "
            f"{'.'.join(map(str, current))}: consoles would not update")
    for page in releases:
        for published in page if isinstance(page, list) else [page]:
            tag = published["tag_name"]
            if tag == f"v{version.lstrip('v')}":
                raise ValueError(f"{tag} is already published")
            if published.get("draft"):
                continue
            try:
                previous = parse_release(tag)
            except ValueError:
                continue
            previous = previous + (0,) if len(previous) == 3 else previous
            if package <= previous:
                raise ValueError(f"v{version.lstrip('v')} is not newer than the published {tag}")
    return ".".join(map(str, package))


def bump(package_version, manifest):
    parse_package(package_version)
    raw, text = read_manifest(manifest)
    updated, count = VERSION_ATTR.subn(lambda m: m.group(1) + package_version + m.group(3), text, 1)
    if count != 1:
        raise ValueError(f"{manifest} has no Identity Version")
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    with open(manifest, "wb") as output:
        output.write(bom + updated.encode("utf-8"))


def issue_versions(path):
    with open(path, encoding="utf-8", newline="") as source:
        text = source.read()
    match = ISSUE_VERSIONS.search(text)
    if not match:
        raise ValueError(f"{path} has no Jadyx version dropdown")
    return text, match, [line.strip()[2:] for line in match.group(2).splitlines()]


def list_issue_version(package_version, template):
    """Lists the version first in the bug report dropdown; issue forms only take fixed options."""
    parse_package(package_version)
    text, match, versions = issue_versions(template)
    if package_version in versions:
        return
    options = match.group(2)
    indent = options[:len(options) - len(options.lstrip())]
    updated = text[:match.end(1)] + f"{indent}- {package_version}\n" + text[match.end(1):]
    with open(template, "w", encoding="utf-8", newline="") as output:
        output.write(updated)


def entries(log):
    """Yields (kind, scope, text) for each first-parent commit of `git log` output."""
    for record in log.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        subject, _, body = record.partition("\x1f")
        if subject.startswith("Merge pull request"):
            # A merged PR: its title is the first line of the merge commit's body.
            subject = body.strip().splitlines()[0] if body.strip() else ""
        elif subject.startswith("Merge "):
            continue  # upstream syncs and branch merges
        if not subject:
            continue
        match = CONVENTIONAL.match(subject)
        if not match:
            yield "other", None, subject
            continue
        kind = match.group("kind")
        if kind in HIDDEN:
            continue
        yield kind, match.group("scope"), match.group("text")


def notes(log, tag, previous, repo):
    grouped = {}
    for kind, scope, text in entries(log):
        line = f"- **{scope}:** {text}" if scope else f"- {text}"
        grouped.setdefault(kind if kind in dict(SECTIONS) else "other", []).append(line)
    out = []
    for kind, title in SECTIONS + (("other", "Other changes"),):
        if grouped.get(kind):
            out += [f"## {title}", ""] + grouped[kind] + [""]
    if not out:
        out = ["No player-facing changes since the previous release.", ""]
    if repo:
        out.append(f"**Full changelog:** https://github.com/{repo}/compare/{previous}...{tag}")
    return "\n".join(out).rstrip() + "\n"


def git_log(previous):
    return subprocess.run(
        ["git", "log", "--first-parent", "--format=%s%x1f%b%x1e", f"{previous}..HEAD"],
        check=True, capture_output=True, text=True, encoding="utf-8").stdout


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    v = commands.add_parser("version")
    v.add_argument("version")
    v.add_argument("--manifest", required=True)
    v.add_argument("--match-manifest", action="store_true", help="Require the committed source version to match the package")
    v.add_argument("--releases", required=True, help="gh api --paginate --slurp .../releases")
    v.add_argument("--issue-template", help="Require the version in this bug report's version dropdown")
    b = commands.add_parser("bump")
    b.add_argument("package_version")
    b.add_argument("--manifest", required=True)
    b.add_argument("--issue-template", help="Also list the version in this bug report's version dropdown")
    n = commands.add_parser("notes")
    n.add_argument("--tag", required=True)
    n.add_argument("--previous", default="")
    n.add_argument("--repo", default="")
    args = parser.parse_args(argv)

    try:
        if args.command == "version":
            with open(args.releases, encoding="utf-8") as source:
                package = validate(args.version, args.manifest, json.load(source))
            if args.match_manifest and parse_package(package) != manifest_version(args.manifest):
                raise ValueError("Commit the exact release version in AppxManifest.xml before publishing")
            if args.issue_template and package not in issue_versions(args.issue_template)[2]:
                raise ValueError(
                    f"List {package} in {args.issue_template} before publishing: release.py bump "
                    f"{package} --manifest {args.manifest} --issue-template {args.issue_template}")
            print(f"package_version={package}")
            print(f"tag=v{package}")
        elif args.command == "bump":
            bump(args.package_version, args.manifest)
            if args.issue_template:
                list_issue_version(args.package_version, args.issue_template)
        elif args.previous:
            sys.stdout.write(notes(git_log(args.previous), args.tag, args.previous, args.repo))
        else:
            # The first release: the whole history is the upstream emulator's, not a changelog.
            sys.stdout.write("First public build.\n")
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
