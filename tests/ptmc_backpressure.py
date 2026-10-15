#!/usr/bin/env python3
"""Compile production atomic reservation helpers and stress their hard limits."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'PlayTools/MetalCapture/PTMetalCapture.inc').read_text()
helpers = source.split('// PTMC_ATOMIC_HELPERS_BEGIN\n')[1].split('// PTMC_ATOMIC_HELPERS_END')[0]
harness = r'''
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
typedef bool BOOL;
#define YES true
#define NO false
HELPERS
static atomic_uint_fast64_t count, bytes, high;
static void *worker(void *unused) {
    (void)unused;
    for (int i=0; i<10000; i++) {
        if (ptmc_reserve_bounded(&count,1,64)) {
            assert(ptmc_reserve_bounded(&bytes,1024*1024,64*1024*1024));
        }
        ptmc_atomic_max(&high, i);
    }
    return 0;
}
int main(void) {
    pthread_t t[8];
    for (int i=0;i<8;i++) assert(!pthread_create(&t[i],0,worker,0));
    for (int i=0;i<8;i++) assert(!pthread_join(t[i],0));
    assert(atomic_load(&count)==64);
    assert(atomic_load(&bytes)==64*1024*1024);
    assert(atomic_load(&high)==9999);
    assert(!ptmc_reserve_bounded(&count, UINT64_MAX,64));
    atomic_fetch_sub(&count,1);
    assert(ptmc_reserve_bounded(&count,1,64));
    assert(!ptmc_reserve_bounded(&count,1,64));
}
'''.replace('HELPERS',helpers)
with tempfile.TemporaryDirectory() as temp:
    p=Path(temp); (p/'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
# Stop/finalize may run at every callback handoff boundary. The production callback
# must keep at least one ownership count live until the compressed frame is queued.
callback = source.split('static void ptmc_vt_output_callback(')[1].split('static CMVideoCodecType')[0]
assert callback.index('ptmc_reserve_bounded(&session->_pendingWrites') < callback.index('atomic_fetch_sub_explicit(&session->_inFlight')
assert callback.index('CFRetain(sampleBuffer)') < callback.index('ptmc_release_slot(slot)')
assert 'MIN(objc_getClassList(classes, capacity), capacity)' in source
assert 'if (!atomic_load_explicit(&ptmc_manager()->_requested, memory_order_acquire)) return;' in source
assert source.count('ptmc_present_depth++') == 7
assert '[fm removeItemAtURL:finalURL' not in source
assert 'fileExistsAtPath:url.path' in source
print('PTMC bounded backlog concurrency + final-callback handoff + lifecycle contracts: PASS')
