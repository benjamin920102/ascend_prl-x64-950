#!/usr/bin/env python3
"""Source/protocol consistency checks requiring only Python 3 (NOT NPU validation)."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
mix = (root / 'kernel/pearl_hlc2_mix.asc').read_text()
refc = (root / 'kernel/pearl_hlc2_cube.asc').read_text()
refv = (root / 'kernel/pearl_hlc2_vec.asc').read_text()
cmake = (root / 'kernel/CMakeLists.txt').read_text()
assert '__global__ __mix__(1, 2) void pearl_hlc2_mix' in mix
assert '__kfc_workspace__ __gm__ uint8_t* workspace' in mix
assert 'ENABLE_CV_COMM_VIA_SSBUF=true' in cmake
assert 'GetSsbufBaseAddr()' in mix
assert 'CrossCoreSetFlag<4, PIPE_S>' in mix
assert 'CrossCoreWaitFlag<4, PIPE_S>' in mix
assert 'static_assert(VEC2X == 2' in mix
assert 'g_prodF' not in mix and 'g_consF' not in mix
assert 'g_sV' not in mix and 'dcci(' in mix # hit DCache writeback remains
assert 'g_sV' in refc and 'g_prodF' in refc

# Fold/XOR and BLAKE3 implementation should be copied without changes:
start='__aicore__ inline void XorVec('
end='// 3KB SSBuffer is shared only within this one MIX(1,2) task.'
# Ignore the comments before XorVec and BLAKE3 helper; verify full section.
def strip_comments(t):
    t = re.sub(r'//[^\n]*', '', t)
    return ''.join(t.split())
assert strip_comments(mix[mix.index(start):mix.index(end)]) == strip_comments(refv[refv.index(start):refv.index('__global__ __vector__ void')]), 'Original SIMD/BLAKE3 helpers changed unexpectedly'

# Symbolic protocol simulation: each AIC emits one band per iteration,
# vec 0 handles even, vec 1 handles odd; ACK is emitted after all reads.
# Bounded ring reuse must follow the matched ACK and all sends have matching waits.
for grid in (1, 2, 5, 20, 31):
    for n in range(1, 260):
        produced = set()
        consumed = set()
        for lc in range(grid):
            issued = 0
            pending = {0: [], 1: []}
            for band in range(lc, n, grid):
                sub, slot = issued % 2, issued % 4
                if issued >= 4:
                    prev = issued - 4
                    assert pending[sub][0] == prev
                    pending[sub].pop(0)
                pending[sub].append(issued)
                produced.add(band)
                assert (band-lc)//grid == issued
                assert sub == issued % 2 and slot == issued % 4
                issued += 1
            # All last <4 outstanding acknowledgements are drained.
            for idx in range(max(0,issued-4),issued):
                sub=idx%2
                assert pending[sub][0] == idx
                pending[sub].pop(0)
            assert not pending[0] and not pending[1]
            for sub in range(2):
                for i in range(sub,issued,2):
                    band=lc+i*grid
                    assert band < n
                    assert band not in consumed
                    consumed.add(band)
        assert produced == consumed == set(range(n)), (grid, n)

print('PASS: source invariants, original BLAKE3/SIMD helper equivalence, and 5*259 symbolic ring/ACK cases')
print('NOTE: No CANN compile, device execution, or SSBuffer collision validation performed.')
