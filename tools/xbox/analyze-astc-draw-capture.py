# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate bounded ASTC draw snapshots; compare hashes without claiming GPU causality."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def inspect(path):
    text = path.read_text()
    result = {"manifest": str(path), "prepared": "status=PREPARED" in text,
              "files": [], "errors": []}
    for label, size in re.findall(r"^(\S+) bytes=(\d+)$", text, re.M):
        file = path.with_name(path.stem + "-" + label + ".bin")
        if not file.exists() or file.stat().st_size != int(size):
            result["errors"].append(f"missing/incorrect size {label}")
        else:
            result["files"].append({"label": label, "captured": int(size),
                                    "sha256": hashlib.sha256(file.read_bytes()).hexdigest()})
    for match in re.finditer(r"^(\S+) offset=(\d+) requested=(\d+) captured=(\d+) truncated=(true|false) fnv1a64=([0-9a-f]+)$", text, re.M):
        label, offset, requested, captured, truncated, expected = match.groups()
        file = path.with_name(path.stem + "-" + label + ".bin")
        item = {"label": label, "offset": int(offset), "requested": int(requested),
                "captured": int(captured), "truncated": truncated == "true"}
        if not file.exists():
            result["errors"].append(f"missing {file.name}")
            continue
        digest = hashlib.sha256()
        fnv = 14695981039346656037
        with file.open("rb") as stream:
            while chunk := stream.read(512 * 1024):
                digest.update(chunk)
                for byte in chunk:
                    fnv = ((fnv ^ byte) * 1099511628211) & 0xffffffffffffffff
        item["sha256"] = digest.hexdigest()
        if file.stat().st_size != int(captured) or fnv != int(expected, 16):
            result["errors"].append(f"size/hash mismatch {label}")
        if label.startswith("raw-") and int(captured) >= 16:
            with file.open("rb") as stream:
                levels, header_blocks, payload_blocks, reserved = struct.unpack("<4I", stream.read(16))
            item["header"] = dict(levels=levels, header_blocks=header_blocks,
                                  payload_blocks=payload_blocks, reserved=reserved)
            if reserved != 0x41535443 or not 1 <= levels <= 32 or header_blocks * 16 < 16 + levels * 48:
                result["errors"].append(f"invalid raw header {label}")
            if not item["truncated"] and (header_blocks + payload_blocks) * 16 > int(captured):
                result["errors"].append(f"payload outside raw resource {label}")
        result["files"].append(item)
    if not result["prepared"]:
        result["errors"].append("capture incomplete")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--shader", default="*")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    captures = [inspect(p) for p in sorted(args.directory.glob(f"astc-capture-{args.shader}-draw*.txt"))]
    comparisons = []
    for previous, current in zip(captures, captures[1:]):
        old = {f["label"]: f.get("sha256") for f in previous["files"]}
        new = {f["label"]: f.get("sha256") for f in current["files"]}
        comparisons.append({"from": previous["manifest"], "to": current["manifest"],
                            "changed": sorted(k for k in old.keys() & new.keys() if old[k] != new[k]),
                            "same": sorted(k for k in old.keys() & new.keys() if old[k] == new[k]),
                            "added": sorted(new.keys() - old.keys()),
                            "removed": sorted(old.keys() - new.keys())})
    report = {"captures": captures, "count": len(captures),
              "comparisons": comparisons,
              "valid": bool(captures) and all(not c["errors"] for c in captures),
              "limitation": "Input snapshots only; matching hashes do not prove matching interpolated samples or render targets."}
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded)
    raise SystemExit(0 if report["valid"] else 1)


if __name__ == "__main__":
    main()
