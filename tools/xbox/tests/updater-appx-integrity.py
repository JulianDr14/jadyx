# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publication verifier must reject altered payload, signature and ZIP metadata."""
import pathlib
import subprocess
import sys
import zipfile

package = pathlib.Path(sys.argv[1])
folder = pathlib.Path('build-uwp/test-results/updater/tampered')
folder.mkdir(parents=True, exist_ok=True)
original = package.read_bytes()
with zipfile.ZipFile(package) as archive:
    targets = {
        'payload': archive.getinfo('jadyx.exe').header_offset,
        'signature': archive.getinfo('AppxSignature.p7x').header_offset,
        'directory': archive.start_dir,
    }
for name, offset in targets.items():
    altered = bytearray(original)
    if name != 'directory':
        name_size = int.from_bytes(altered[offset+26:offset+28], 'little')
        extra_size = int.from_bytes(altered[offset+28:offset+30], 'little')
        offset += 30 + name_size + extra_size + 16
    altered[offset] ^= 1
    path = folder / (name + '.appx')
    path.write_bytes(altered)
    result = subprocess.run(['pwsh', '-NoProfile', '-File', 'tools/xbox/verify-appx.ps1',
                             '-Package', str(path)], capture_output=True)
    assert result.returncode != 0, f'Accepted tampered {name}'
    print('Rejected altered ' + name)
print('Publication APPX negative integrity gates: PASS')
