# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows-side regression for capture timing and coverage in profile-report.py."""
from pathlib import Path
import subprocess
import sys
import tempfile

report = Path(__file__).resolve().parents[1] / 'profile-report.py'

def run(text):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'profile.txt'
        path.write_text(text, encoding='utf-8')
        result = subprocess.run([sys.executable, str(report), str(path), '--exe',
                                 str(Path(directory) / 'absent.exe')],
                                check=True, capture_output=True, text=True)
        return result.stdout

fixture = '''window 1 seconds 1 hz 32 ticks 32 sampler_cpu_ms 1 suspensions 1 suspended_us_avg 100 suspended_us_max 100 stacks 0 dropped_samples 0
thread test id 1 cpu_ms 10 running 1 waiting 0
window 2 seconds 1 hz 32 ticks 32 sampler_cpu_ms 1 suspensions 9 suspended_us_avg 10 suspended_us_max 10 stacks 0 dropped_samples 0
thread test id 1 cpu_ms 10 running 9 waiting 0 capture_skips 7 slow_captures 2
capture_coverage cooldown_skips 7 slow_captures 2
'''
result = run(fixture)
assert 'elapsed 19.0 us' in result  # (1*100 + 9*10) / 10, not (100+10)/2
assert 'not exact target suspension time' in result
assert '7 capture attempts skipped' in result and '2 slow captures' in result
assert 'Sampling coverage:' not in run(fixture.replace(
    'capture_coverage cooldown_skips 7 slow_captures 2\n', ''))
assert 'elapsed 0.0 us' in run('window 1 seconds 1 hz 32 ticks 1 sampler_cpu_ms 0 '
                              'suspensions 0 suspended_us_avg 0 suspended_us_max 0 '
                              'stacks 0 dropped_samples 0\n')
print('PASS: weighted capture elapsed, legacy/new records, zero captures, coverage warning')
