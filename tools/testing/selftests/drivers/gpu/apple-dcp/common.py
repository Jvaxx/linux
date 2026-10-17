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


# Asahi 7.2 moved the generation-specific setup after the tunables into
# per-SoC atcphy_hw callbacks; atcphy_configure() calls them and compares
# against the all-modes one for the RCAL wait.
ATC_POST_TUNABLE = ('atcphy_configure_post_tunable_t8103', 'atcphy_wake_common_t8122',
                    'atcphy_configure_post_tunable_t8122',
                    'atcphy_configure_post_tunable_all_modes')


def atc_mux_without_pm(source):
    """Lane/teardown fixtures omit USB PM, keeping mode mapping and lane work real.

    The PM fixtures replace only this adapter with the actual mux update/callback.
    """
    mapping = block(source, 'static enum atcphy_mode atcphy_typec_target_mode(')
    locked = block(source, 'static int atcphy_mux_set_locked(')
    return mapping + '\n' + locked + r'''
static int atcphy_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
    struct apple_atcphy *atcphy = typec_mux_get_drvdata(mux);
    return atcphy_mux_set_locked(atcphy, atcphy_typec_target_mode(atcphy, state));
}
'''
