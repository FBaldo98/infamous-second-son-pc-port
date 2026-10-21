"""Build an offline analysis executable; does not relink or modify the game DLL."""
from pathlib import Path
import re
import shlex
import subprocess
from build_gpu import environment, ROOT, WORKSPACE, SOURCE, CLANG


if __name__ == '__main__':
    out = ROOT / 'out/gcn-decode-build'
    out.mkdir(exist_ok=True)
    ninja = (WORKSPACE / 'build-opt/build.ninja').read_text()
    defs = shlex.split(re.search(r'^  DEFINES = (.+)$', ninja, re.M)[1])
    includes = shlex.split(re.search(r'^  INCLUDES = (.+)$', ninja, re.M)[1].replace('\\', '/'))
    sources = [ROOT / 'tools/gcn_decode.cpp', SOURCE / 'shader_recompiler/frontend/decode.cpp',
               SOURCE / 'shader_recompiler/frontend/format.cpp']
    args = [str(CLANG), '/nologo', '/std:c++latest', '/EHsc', '/MD', '/O2', *defs, *includes,
            *map(str, sources), str(WORKSPACE / 'build-opt/_deps/fmt-build/fmt.lib'),
            '/Fe:' + str(out / 'gcn-decode.exe')]
    raise SystemExit(subprocess.run(args, cwd=out, env=environment()).returncode)
