#!/usr/bin/env python3
"""Run the SDK wake tests through WAIT_SIGNAL with unrelated LFQueue activity."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolkit', type=Path, required=True)
    kit = parser.parse_args().toolkit.resolve()
    harness = (kit / 'runtime/spu/tests/test_spu_taskset_wake.c').read_text()
    harness = harness.replace('#include "../spu_channels.c"',
                              f'#include "{kit}/runtime/spu/spu_channels.c"')
    old = '''    spu_taskset_parked_add(w->taskset, w->task, 0);
    spu_taskset_wait_signal(w->taskset, w->task);
    spu_taskset_parked_del(w->taskset, w->task);'''
    new = '''    spu_context ctx = {0};
    ctx.ls = ctx.ls_store;
    ctx.image_id = 1;
    ctx.gpr[3]._u32[0] = 2;
    for (int i = 0; i < 4; i++) {
        ctx.ls[0x27BC + i] = w->taskset >> ((3 - i) * 8);
        ctx.ls[0x27D4 + i] = w->task >> ((3 - i) * 8);
    }
    /* A previous push in an unrelated subsystem must not satisfy this wait. */
    __atomic_store_n(&g_spu_lfq_push_gen, 1, __ATOMIC_SEQ_CST);
    spu_spurs_taskset_syscall(&ctx);'''
    if old not in harness:
        raise RuntimeError('SDK wake harness changed; update the WAIT_SIGNAL adapter')
    harness = harness.replace(old, new)
    harness = '#include <assert.h>\nextern int spu_taskset_signal_parked_force(void);\n' + harness
    harness = harness.replace('    sleep_ms(100);',
        '    sleep_ms(100);\n    assert(spu_taskset_signal_parked_force() == 0);')
    harness += '\nuint32_t g_ps3_ro_image_lo = 0, g_ps3_ro_image_hi = 0;\n'
    files = ['spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload',
             'spurs_policy', 'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap',
             'spurs_policy_blob_weak', 'spu_interp']
    with tempfile.TemporaryDirectory(prefix='asura-signal-') as tmp:
        source = Path(tmp) / 'test.c'
        binary = Path(tmp) / 'test'
        source.write_text(harness)
        command = ['clang', '-std=gnu17', '-O1', '-I', str(kit / 'include'),
                   '-I', str(kit / 'runtime/spu'), '-I', str(kit / 'runtime/platform'),
                   str(source), *[str(kit / f'runtime/spu/{f}.c') for f in files],
                   str(kit / 'runtime/platform/win32_compat.c'), '-lpthread', '-lm',
                   '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
