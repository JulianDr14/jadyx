# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import tarfile
from pathlib import Path

root = Path(sys.argv[1])
expected = {'log'}
for path in (root / 'log').rglob('*'):
    expected.add('log/' + path.relative_to(root / 'log').as_posix())
with tarfile.open(root / 'download.tar', mode='r:') as archive:
    names = set()
    for member in archive:
        name = member.name.rstrip('/')
        assert name in expected, f'Unexpected entry {name}'
        assert name not in names, f'Duplicate entry {name}'
        names.add(name)
        source = root / name
        assert member.isdir() == source.is_dir(), name
        if member.isfile():
            with archive.extractfile(member) as content:
                assert content.read() == source.read_bytes(), name
    assert names == expected, (names, expected)
assert (root / 'download.tar.zst').stat().st_size < (root / 'download.tar').stat().st_size
print('PASS: standard PAX tar, exact binary/text/empty bytes, long UTF-8 names, empty directories, fixed log root only')
