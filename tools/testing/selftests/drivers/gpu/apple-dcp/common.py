# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""Shared public-source extraction and strict host compilation."""
from pathlib import Path
import hashlib, json, os, re, shlex, subprocess

def block(source, marker):
    start = source.index(marker) if isinstance(marker, str) else marker
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def source_defines(source):
    lines = iter(source.splitlines())
    definitions = []
    for line in lines:
        if not line.startswith('#define '):
            continue
        definition = [line]
        while definition[-1].endswith(chr(92)):
            definition.append(next(lines))
        definitions.append('\n'.join(definition))
    return definitions

def run_unit(name, unit, output, cases=((),), includes=(), extra=()):
    work = output / name
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'host.c'
    source.write_text(unit)
    command = shlex.split(os.environ.get('HOSTCC', 'clang')) + [
        '-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    ] + list(extra) + ['-I' + str(p) for p in includes] + [str(source), '-o', str(work / 'host')]
    (work / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (work / 'compile.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    for index, case in enumerate(cases):
        result = subprocess.run([str(work / 'host'), *case], capture_output=True, text=True, timeout=120)
        (work / ('case-' + str(index) + '.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
    return hashlib.sha256(unit.encode()).hexdigest()
