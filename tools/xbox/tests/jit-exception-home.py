# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production SEH thunk with all four legal Windows home spills."""

from pathlib import Path

from jit_test_support import BACKEND, run_cpp

source = (BACKEND / "exception_handler_windows.cpp").read_text()
wrapper = source[source.index("        code.lea(code.rsp") :]
wrapper = wrapper[: wrapper.index("        code.ret();") + len("        code.ret();")]
fixture = Path(__file__).with_suffix(".cpp").with_name("jit-exception.cpp").read_text()
run_cpp("#define SCENARIOS 2\n" + fixture.replace("// PRODUCTION", wrapper))
print("PASS: two handled AVs, four home spills, stack/PC sentinels")
