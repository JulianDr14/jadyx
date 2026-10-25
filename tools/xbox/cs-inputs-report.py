# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads the cs_inputs_<hash>_<dispatch>_<binding>_<before|after>.bin dumps ("cs_inputs=" in
boot.cfg) and the matching jadyx_log.txt lines, and prints what each dispatch got.

Every binding gets its size, how many words are not zero and its first words. With --jobs, the
dispatch is read as a job list: binding 0 holds the job count in its first word and one entry
of --entry-stride bytes per group from --entry-base, whose first word indexes the records of
binding 1 (--record-stride bytes each); the record word at --count-offset is the trip count of
the group's stride loop (64 lanes), and the whole record is printed so the other fields can be
read. The offsets come from the shader's IR dump (dump_shader=).

usage: cs-inputs-report.py LOG_DIR [--hash H] [--jobs] [--words N]
"""
import argparse
import collections
import pathlib
import re
import struct
import sys

NAME = re.compile(r"cs_inputs_([0-9a-f]{16})_(\d+)_(\d+)_(before|after)\.bin$")
LINE = re.compile(r"cs_inputs: ([0-9a-f]{16}) dispatch (\d+) groups (\d+)x(\d+)x(\d+) binding (\d+) "
                  r"(before|after) @([0-9a-f]+)\+(\d+) size (\d+)")


def words(data):
    return struct.unpack(f"<{len(data) // 4}I", data[:len(data) // 4 * 4])


def word(table, offset):
    index = offset // 4
    return table[index] if 0 <= index < len(table) else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log_dir", type=pathlib.Path)
    parser.add_argument("--hash", help="only this compute shader")
    parser.add_argument("--jobs", action="store_true", help="read binding 0/1 as a job list")
    parser.add_argument("--words", type=int, default=16, help="leading words shown per binding")
    parser.add_argument("--entry-base", type=int, default=32)
    parser.add_argument("--entry-stride", type=int, default=32)
    parser.add_argument("--record-stride", type=int, default=128)
    parser.add_argument("--count-offset", type=int, default=108)
    parser.add_argument("--lanes", type=int, default=64)
    args = parser.parse_args()

    dumps = collections.defaultdict(dict)
    for path in sorted(args.log_dir.glob("cs_inputs_*.bin")):
        m = NAME.search(path.name)
        if not m or (args.hash and m.group(1) != args.hash.lower()):
            continue
        dumps[(m.group(1), int(m.group(2)))][(int(m.group(3)), m.group(4))] = path.read_bytes()
    groups, places = {}, {}
    log = args.log_dir / "jadyx_log.txt"
    if log.exists():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            m = LINE.search(line)
            if m:
                key = (m.group(1), int(m.group(2)))
                groups[key] = tuple(int(m.group(i)) for i in (3, 4, 5))
                places[key + (int(m.group(6)), m.group(7))] = (int(m.group(8), 16),
                                                                int(m.group(10)))
    if not dumps:
        sys.exit(f"no cs_inputs dumps in {args.log_dir}")

    for (shader, dispatch), bindings in sorted(dumps.items()):
        dim = groups.get((shader, dispatch))
        print(f"# CS {shader} dispatch {dispatch}"
              + (f", {dim[0]}x{dim[1]}x{dim[2]} groups" if dim else ""))
        for (binding, when), data in sorted(bindings.items()):
            table = words(data)
            address, size = places.get((shader, dispatch, binding, when), (None, len(data)))
            where = f" @{address:x} (address % 16 = {address % 16})" if address is not None else ""
            kept = "" if size == len(data) else f" of {size}"
            nonzero = sum(1 for w in table if w)
            head = " ".join(f"{w:08x}" for w in table[:args.words])
            print(f"- binding {binding} {when}: {len(data)} bytes{kept}{where}, "
                  f"{nonzero} words not zero\n  {head}")
        if not args.jobs:
            continue
        b0, b1 = bindings.get((0, "before")), bindings.get((1, "before"))
        if b0 is None or b1 is None:
            print("  (no binding 0/1 dumped before the dispatch)")
            continue
        b0, b1 = words(b0), words(b1)
        jobs = b0[0] if b0 else 0
        count_groups = dim[0] if dim else jobs
        print(f"\n  jobs {jobs}, groups {count_groups}\n")
        print("  | group | record | trip count | iterations per lane | record words |")
        print("  |---|---|---|---|---|")
        total = worst = 0
        for group in range(min(count_groups, jobs)):
            record = word(b0, args.entry_base + group * args.entry_stride)
            if record is None:
                print(f"  | {group} | out of the dump | | | |")
                continue
            base = record * args.record_stride
            count = word(b1, base + args.count_offset)
            fields = b1[base // 4:(base + args.record_stride) // 4]
            if count is None:
                print(f"  | {group} | {record} | out of the dump | | |")
                continue
            per_lane = -(-count // args.lanes)
            total += count
            worst = max(worst, per_lane)
            print(f"  | {group} | {record} | {count} | {per_lane} | "
                  + " ".join(f"{w:x}" for w in fields) + " |")
        print(f"\n  items {total}, longest group {worst} iterations per lane\n")


if __name__ == "__main__":
    main()
