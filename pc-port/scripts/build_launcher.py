# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the launcher (launcher/launcher.cpp) with the toolchain of build.py:
python scripts/build_launcher.py [--output PATH]"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
from vcvars import VARS, ARGS as VARS_ARGS
CLANG = ROOT.parent / 'tools/llvm/bin/clang-cl.exe'


def build(output):
    result = subprocess.run(f'cmd.exe /d /s /c ""{VARS}"{VARS_ARGS} >nul && set"', capture_output=True,
                            text=True, check=True)
    env = os.environ.copy()
    for line in result.stdout.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            env[key] = value
    env['PATH'] = str(CLANG.parent) + os.pathsep + env.get('Path', env.get('PATH', ''))
    env.pop('Path', None)
    output.parent.mkdir(parents=True, exist_ok=True)
    work = ROOT / 'out' / 'launcher-build'
    work.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG), '/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/DUNICODE', '/D_UNICODE',
           '/D_CRT_SECURE_NO_WARNINGS', str(ROOT / 'launcher' / 'launcher.cpp'),
           str(ROOT / 'launcher' / 'prepare_game.cpp'), f'/Fo:{work}\\',
           f'/Fe:{output}', '/clang:-fuse-ld=lld', '/link', '/SUBSYSTEM:WINDOWS',
           'user32.lib', 'gdi32.lib', 'gdiplus.lib', 'shell32.lib', 'ole32.lib', 'dwmapi.lib',
           'shlwapi.lib', 'uuid.lib', 'bcrypt.lib']
    subprocess.run(cmd, cwd=work, env=env, check=True)
    print(f'Built {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'out' / 'launcher-staging' / 'inFAMOUS Second Son Launcher.exe')
    args = parser.parse_args()
    try:
        build(args.output.resolve())
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f'Build failed: {e}')
