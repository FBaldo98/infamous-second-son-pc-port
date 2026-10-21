# SPDX-License-Identifier: GPL-2.0-or-later
"""Locate vcvars64.bat in Visual Studio 2022 or 18 (2026)."""
from pathlib import Path

_BASE = Path(r'C:\Program Files\Microsoft Visual Studio')
_EDITIONS = ('Community', 'Professional', 'Enterprise', 'BuildTools')
VARS = next((p for p in (_BASE/v/e/'VC/Auxiliary/Build/vcvars64.bat' for v in ('2022', '18') for e in _EDITIONS) if p.exists()),
            _BASE/'2022/Community/VC/Auxiliary/Build/vcvars64.bat')
# VS 18 defaults to MSVC 14.5x, whose STL requires Clang 20+. Select the VS 2022 (v143)
# toolset when it is installed, the one tools/llvm (Clang 19) is built against.
_V143 = VARS.parent/'Microsoft.VCToolsVersion.v143.default.txt'
ARGS = f' -vcvars_ver={_V143.read_text().strip()}' if VARS.parts[3] == '18' and _V143.exists() else ''
