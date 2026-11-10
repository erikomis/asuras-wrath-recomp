#!/usr/bin/env python3
"""Lift raw SPURS jobs captured by SPU_DUMP_MISS (generated output only)."""
import argparse
import pathlib
import re
import subprocess
import sys


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolkit', type=pathlib.Path, required=True)
    parser.add_argument('--dumps', type=pathlib.Path, default=root / 'cache/probe')
    args = parser.parse_args()
    kit = args.toolkit.resolve() / 'tools'
    sys.path.insert(0, str(kit))
    from wrap_spu_elf import wrap

    out = root / 'port/spu'
    out.mkdir(parents=True, exist_ok=True)
    declarations, registrations = [], []
    jobs = sorted(args.dumps.glob('spujob_*.bin'))
    for index, path in enumerate(jobs, 28):
        match = re.fullmatch(r'spujob_([0-9A-Fa-f]{16})_(\d+)\.bin', path.name)
        if not match:
            raise ValueError(f'Invalid dump filename: {path}')
        data = path.read_bytes()
        fp = 1469598103934665603
        for byte in data:
            fp = ((fp ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        if len(data) != int(match[2]) or fp != int(match[1], 16):
            raise ValueError(f'Dump size/fingerprint mismatch: {path}')
        name = f'spursjob_{fp:016X}'
        dest = out / name
        dest.mkdir(exist_ok=True)
        elf = dest / 'wrapped.elf'
        # spu_run_spurs_job stages raw job binaries and enters their CRT at LS 0.
        elf.write_bytes(wrap(data, base=0, entry=0, symbols=[]))
        command = [
            sys.executable, str(kit / 'spu_lifter.py'), '--auto-functions', str(elf),
            '--symbol-prefix', name + '_', '--return-entries', '--merge-chunks',
            '--source-name', 'spu_recomp.c', '--header-name', 'spu_recomp.h',
            '-o', str(dest),
        ]
        # This exact Edge Geometry binary ends at 0x7834; the following
        # string table otherwise seeds bogus functions when disassembled.
        if fp == 0x42E1FC8CEE0BE246:
            command.extend(['--code-end', '0x7834'])
        subprocess.run(command, check=True)
        for source in [dest / 'spu_recomp.c', dest / 'spu_recomp.h']:
            text = source.read_text()
            text = re.sub(r'"(?:\.\./)+runtime/spu/([^"/]+)"', r'"\1"', text)
            source.write_text(text)
        declarations.extend([
            f'extern void {name}_spu_func_00000000(spu_context*);',
            f'extern void {name}_spu_recomp_register(void);',
        ])
        registrations.extend([
            f'    spu_begin_image({index});',
            f'    {name}_spu_recomp_register();',
            f'    spu_workload_register_img(0x{fp:016X}ULL, '
            f'{name}_spu_func_00000000, {index}, "{name}");',
        ])
    registry = '\n'.join([
        '/* Generated from local SPU_DUMP_MISS captures; do not commit. */',
        '#include "spu_workload.h"', 'extern void spu_begin_image(int);',
        *declarations,
        '__attribute__((constructor)) static void register_spurs_jobs(void) {',
        *registrations, '}', '',
    ])
    (out / 'spurs_jobs_register.c').write_text(registry)
    print(f'Registered {len(jobs)} captured SPURS job(s)')


if __name__ == '__main__':
    main()
