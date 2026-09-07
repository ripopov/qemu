/*
 * Standalone execution smoke test for the gem5 QEMU JIT adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu-jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static uint8_t memory[4096];
static uint64_t host_time[2];

static uint64_t
read_time(void *opaque)
{
    return *(uint64_t *)opaque;
}

static int
memory_read(void *opaque, uint64_t address, uint8_t *data, size_t size)
{
    if (address > sizeof(memory) || size > sizeof(memory) - address) {
        return -1;
    }
    memcpy(data, memory + address, size);
    return 0;
}

static int
memory_write(void *opaque, uint64_t address, const uint8_t *data, size_t size)
{
    if (address > sizeof(memory) || size > sizeof(memory) - address) {
        return -1;
    }
    memcpy(memory + address, data, size);
    return 0;
}

static int
memory_map(void *opaque, size_t index, uint64_t *guest_address, uint64_t *size,
           uint8_t **host_address, int *writable)
{
    if (index != 0) {
        return -1;
    }
    *guest_address = 0;
    *size = sizeof(memory);
    *host_address = memory;
    *writable = 1;
    return 0;
}

static int
hpm_transfer_smoke(void)
{
    Gem5QemuJitHpmState saved[2], state[2], observed, bad;
    uint64_t pending[2], inhibit[2];
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_set_mode(hart, 3, 0) ||
            gem5_qemu_jit_get_hpm_state(hart, &saved[hart], sizeof(saved[hart])) ||
            gem5_qemu_jit_get_csr(hart, 0x320, &inhibit[hart])) {
            return -1;
        }
        pending[hart] = gem5_qemu_jit_get_mip(hart);
        state[hart] = saved[hart];
        state[hart].inhibited = state[hart].implemented;
        for (unsigned i = 3; i < 32; i++) {
            if (state[hart].implemented & (UINT32_C(1) << i)) {
                state[hart].counter[i] = UINT64_C(0x1234567800000000) +
                    hart * 256 + i;
                state[hart].event[i] = (UINT64_C(1) << 63) |
                    ((uint64_t)(i & 31) << 58);
            }
        }
        if (gem5_qemu_jit_set_hpm_state(hart, &state[hart], sizeof(state[hart]))) {
            return -1;
        }
    }
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_hpm_state(hart, &observed, sizeof(observed)) ||
            memcmp(&state[hart], &observed, sizeof(observed)) ||
            gem5_qemu_jit_get_mip(hart) != pending[hart]) {
            return -1;
        }
        for (unsigned trial = 0; trial < 6; trial++) {
            bad = state[hart];
            switch (trial) {
            case 0: bad.version++; break;
            case 1: bad.size--; break;
            case 2: bad.implemented ^= 8; break;
            case 3: bad.counter[1] = 1; break;
            case 4: bad.inhibited |= 1; break;
            case 5: bad.event[3] = bad.event[4] = 2; break;
            }
            if (!gem5_qemu_jit_set_hpm_state(hart, &bad, sizeof(bad)) ||
                gem5_qemu_jit_get_hpm_state(hart, &observed, sizeof(observed)) ||
                memcmp(&state[hart], &observed, sizeof(observed)) ||
                gem5_qemu_jit_get_mip(hart) != pending[hart]) {
                return -1;
            }
        }
        uint64_t current_inhibit;
        if (gem5_qemu_jit_get_csr(hart, 0x320, &current_inhibit) ||
            (current_inhibit & 7) != (inhibit[hart] & 7) ||
            gem5_qemu_jit_set_hpm_state(hart, &saved[hart], sizeof(saved[hart]))) {
            return -1;
        }
    }
    puts("HPM migration: two-hart frozen bank, OF and rejection atomicity passed");
    return 0;
}

static int
fixed_counter_transfer_smoke(void)
{
    Gem5QemuJitFixedCounterState saved[2], state[2], observed, bad;
    Gem5QemuJitHpmState hpm[2], hpm_after;
    uint64_t pending[2];
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_set_mode(hart, 3, 0) ||
            gem5_qemu_jit_get_fixed_counters(hart, &saved[hart], sizeof(saved[hart])) ||
            gem5_qemu_jit_get_hpm_state(hart, &hpm[hart], sizeof(hpm[hart]))) {
            return -1;
        }
        pending[hart] = gem5_qemu_jit_get_mip(hart);
        state[hart] = saved[hart];
        state[hart].inhibited = 5;
        state[hart].cycle = UINT64_C(0x1234567800000000) + hart;
        state[hart].instret = UINT64_C(0x2345678900000000) + hart;
        if (gem5_qemu_jit_set_fixed_counters(hart, &state[hart], sizeof(state[hart]))) {
            return -1;
        }
    }
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_fixed_counters(hart, &observed, sizeof(observed)) ||
            memcmp(&observed, &state[hart], sizeof(observed))) {
            return -1;
        }
        for (unsigned trial = 0; trial < 4; trial++) {
            bad = state[hart];
            switch (trial) {
            case 0: bad.version++; break;
            case 1: bad.size--; break;
            case 2: bad.inhibited |= 2; break;
            case 3: bad.reserved = 1; break;
            }
            if (!gem5_qemu_jit_set_fixed_counters(hart, &bad, sizeof(bad)) ||
                gem5_qemu_jit_get_fixed_counters(hart, &observed, sizeof(observed)) ||
                memcmp(&observed, &state[hart], sizeof(observed))) {
                return -1;
            }
        }
        if (gem5_qemu_jit_get_hpm_state(hart, &hpm_after, sizeof(hpm_after)) ||
            memcmp(&hpm[hart], &hpm_after, sizeof(hpm_after)) ||
            gem5_qemu_jit_get_mip(hart) != pending[hart] ||
            gem5_qemu_jit_set_fixed_counters(hart, &saved[hart], sizeof(saved[hart]))) {
            return -1;
        }
    }
    puts("Fixed counters: two-hart frozen samples and rejection atomicity passed");
    return 0;
}

static int
counter_isolation_smoke(void)
{
    Gem5QemuJitFixedCounterState saved_fixed[2], fixed[2], observed_fixed;
    Gem5QemuJitHpmState saved_hpm[2], hpm[2], observed_hpm;
    uint64_t saved_pc[2], saved_pending[2], executed[2] = {0, 0};
    uint32_t nops[32];
    for (unsigned i = 0; i < 32; i++) {
        nops[i] = 0x00000013;
    }
    memcpy(memory + 3072, nops, sizeof(nops));
    for (unsigned hart = 0; hart < 2; hart++) {
        saved_pc[hart] = gem5_qemu_jit_get_pc(hart);
        saved_pending[hart] = gem5_qemu_jit_get_mip(hart);
        gem5_qemu_jit_set_mip(hart, saved_pending[hart] & ~(UINT64_C(1) << 13));
        if (gem5_qemu_jit_set_mode(hart, 3, 0) ||
            gem5_qemu_jit_get_fixed_counters(hart, &saved_fixed[hart], sizeof(fixed[hart])) ||
            gem5_qemu_jit_get_hpm_state(hart, &saved_hpm[hart], sizeof(hpm[hart]))) {
            return -1;
        }
        fixed[hart] = saved_fixed[hart];
        fixed[hart].inhibited = 0;
        fixed[hart].cycle = 1000 + hart;
        fixed[hart].instret = 2000 + hart;
        hpm[hart] = saved_hpm[hart];
        hpm[hart].inhibited = hpm[hart].implemented & ~UINT32_C(8);
        memset(hpm[hart].counter, 0, sizeof(hpm[hart].counter));
        memset(hpm[hart].event, 0, sizeof(hpm[hart].event));
        hpm[hart].counter[3] = hart ? UINT64_MAX - 7 : 3000;
        /* Hart 1 uses a noncurrent-mode filter to exercise mode snapshots. */
        hpm[hart].event[3] = 2 | (hart ? UINT64_C(1) << 61 : 0);
        if (gem5_qemu_jit_set_fixed_counters(hart, &fixed[hart], sizeof(fixed[hart])) ||
            gem5_qemu_jit_set_hpm_state(hart, &hpm[hart], sizeof(hpm[hart]))) {
            return -1;
        }
    }
    for (unsigned round = 0; round < 4; round++) {
        unsigned active = round & 1;
        Gem5QemuJitRunResult result;
        gem5_qemu_jit_set_pc(active, 3072);
        gem5_qemu_jit_invalidate_translations(active);
        unsigned remaining = 17;
        for (unsigned attempt = 0; remaining && attempt < 64; attempt++) {
            int rc = gem5_qemu_jit_run(active, remaining, &result);
            if (rc || result.instructions > remaining ||
                (result.reason != GEM5_QEMU_JIT_EXIT_BUDGET &&
                 result.reason != GEM5_QEMU_JIT_EXIT_INTERRUPT)) {
                fprintf(stderr, "counter isolation run failed: round %u rc %d "
                        "reason %d instructions %" PRIu64 "\n",
                        round, rc, result.reason, result.instructions);
                return -1;
            }
            remaining -= result.instructions;
        }
        if (remaining) {
            fprintf(stderr, "counter isolation stalled: round %u remaining %u\n",
                    round, remaining);
            return -1;
        }
        executed[active] += 17;
        for (unsigned hart = 0; hart < 2; hart++) {
            if (gem5_qemu_jit_get_fixed_counters(hart, &observed_fixed, sizeof(observed_fixed)) ||
                gem5_qemu_jit_get_hpm_state(hart, &observed_hpm, sizeof(observed_hpm)) ||
                observed_fixed.cycle != fixed[hart].cycle + executed[hart] ||
                observed_fixed.instret != fixed[hart].instret + executed[hart] ||
                observed_hpm.counter[3] != hpm[hart].counter[3] + executed[hart] ||
                !!(observed_hpm.event[3] >> 63) != (hart && executed[hart] >= 8) ||
                !!(gem5_qemu_jit_get_mip(hart) & (UINT64_C(1) << 13)) !=
                    (hart && executed[hart] >= 8)) {
                fprintf(stderr, "counter isolation failed: round %u hart %u\n", round, hart);
                return -1;
            }
        }
    }
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_set_fixed_counters(hart, &saved_fixed[hart], sizeof(saved_fixed[hart])) ||
            gem5_qemu_jit_set_hpm_state(hart, &saved_hpm[hart], sizeof(saved_hpm[hart]))) {
            return -1;
        }
        gem5_qemu_jit_set_pc(hart, saved_pc[hart]);
        gem5_qemu_jit_set_mip(hart, saved_pending[hart]);
    }
    puts("Counter isolation: alternating harts preserve inactive sources");
    return 0;
}

static int
counter_restore_smoke(void)
{
    for (unsigned hart = 0; hart < 2; hart++) {
        uint64_t inhibit, saved, observed;
        uint64_t value = UINT64_C(0x1234567800000000) + hart;
        if (gem5_qemu_jit_set_mode(hart, 3, 0) ||
            gem5_qemu_jit_get_csr(hart, 0x320, &inhibit) ||
            gem5_qemu_jit_get_csr(hart, 0xb02, &saved) ||
            gem5_qemu_jit_set_csr(hart, 0x320, inhibit & ~UINT64_C(4)) ||
            gem5_qemu_jit_set_csr(hart, 0xb02, value) ||
            gem5_qemu_jit_get_csr(hart, 0xb02, &observed) || observed != value ||
            gem5_qemu_jit_set_csr(hart, 0xb02, saved) ||
            gem5_qemu_jit_set_csr(hart, 0x320, inhibit)) {
            return -1;
        }
    }
    return 0;
}

static int
mstatus_restore_smoke(void)
{
    const uint64_t mask = UINT64_C(3) << 38; /* GVA, MPV */
    uint64_t saved[2], observed;

    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_set_mode(hart, 3, 0) ||
            gem5_qemu_jit_get_csr(hart, 0x300, &saved[hart])) {
            return -1;
        }
    }
    for (unsigned pattern = 0; pattern < 4; pattern++) {
        for (unsigned hart = 0; hart < 2; hart++) {
            uint64_t expected = (saved[hart] & ~mask) |
                ((uint64_t)(pattern ^ hart) << 38);
            if (gem5_qemu_jit_restore_mstatus(hart, 1, expected) ||
                gem5_qemu_jit_get_csr(hart, 0x300, &observed) ||
                observed != expected ||
                gem5_qemu_jit_restore_mstatus(hart, 2, expected ^ mask) == 0 ||
                gem5_qemu_jit_get_csr(hart, 0x300, &observed) ||
                observed != expected ||
                /* Guest accessors must still leave trap fields unchanged. */
                gem5_qemu_jit_set_csr(hart, 0x300, expected ^ mask) ||
                gem5_qemu_jit_get_csr(hart, 0x300, &observed) ||
                observed != expected) {
                return -1;
            }
        }
    }
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_set_mode(hart, 1, 1) ||
            gem5_qemu_jit_restore_mstatus(hart, 1, saved[hart]) == 0 ||
            gem5_qemu_jit_set_mode(hart, 3, 0) ||
            gem5_qemu_jit_restore_mstatus(hart, 1, saved[hart])) {
            return -1;
        }
    }
    return 0;
}

static int
stateen_reserved_smoke(void)
{
    for (unsigned hart = 0; hart < 2; hart++) {
        for (unsigned index = 1; index < 4; index++) {
            uint64_t saved, observed;
            if (gem5_qemu_jit_get_csr(hart, 0x30c + index, &saved) ||
                gem5_qemu_jit_set_csr(hart, 0x30c + index, UINT64_MAX) ||
                gem5_qemu_jit_set_csr(hart, 0x10c + index, UINT64_MAX) ||
                gem5_qemu_jit_get_csr(hart, 0x10c + index, &observed) ||
                observed != 0 ||
                gem5_qemu_jit_set_csr(hart, 0x30c + index, saved)) {
                return -1;
            }
        }
    }
    return 0;
}

static int
stateen_transfer_smoke(void)
{
    Gem5QemuJitStateenState saved[2], expected[2], desired, observed, bad;
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_stateen(hart, &saved[hart], sizeof(saved[hart]))) {
            return -1;
        }
        desired = saved[hart];
        for (unsigned index = 0; index < 4; index++) {
            desired.value[0][index] = 0;
            desired.value[1][index] = hart ? 0 : desired.mask[1][index];
        }
        expected[hart] = desired;
        if (gem5_qemu_jit_set_stateen(hart, &desired, sizeof(desired)) ||
            gem5_qemu_jit_get_stateen(hart, &observed, sizeof(observed)) ||
            memcmp(&desired, &observed, sizeof(desired))) {
            return -1;
        }
        uint64_t visible;
        if (gem5_qemu_jit_get_csr(hart, 0x60c, &visible) || visible) {
            return -1;
        }
        for (unsigned test = 0; test < 4; test++) {
            bad = desired;
            bad.value[0][0] = bad.mask[0][0];
            switch (test) {
            case 0: bad.version++; break;
            case 1: bad.size--; break;
            case 2: bad.mask[1][3] ^= 1; break;
            case 3: bad.value[2][3] = 1; break;
            }
            if (gem5_qemu_jit_set_stateen(hart, &bad, sizeof(bad)) == 0 ||
                gem5_qemu_jit_get_stateen(hart, &observed, sizeof(observed)) ||
                memcmp(&desired, &observed, sizeof(desired))) {
                return -1;
            }
        }
    }
    for (unsigned hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_stateen(hart, &observed, sizeof(observed)) ||
            memcmp(&expected[hart], &observed, sizeof(observed)) ||
            gem5_qemu_jit_set_stateen(hart, &saved[hart], sizeof(saved[hart]))) {
            return -1;
        }
    }
    return 0;
}

static int
vector_state_smoke(void)
{
    Gem5QemuJitVectorState saved[2], expected[2], observed, bad;
    unsigned hart, reg, byte, test;

    for (hart = 0; hart < 2; hart++) {
        uint64_t status;
        if (gem5_qemu_jit_get_csr(hart, 0x300, &status) ||
            (status & (3u << 9)) ||
            gem5_qemu_jit_get_vector_state(hart, &saved[hart],
                                            sizeof(saved[hart]))) {
            return -1;
        }
        expected[hart] = saved[hart];
        expected[hart].vl = hart ? 0 : 7;
        expected[hart].vstart = hart ? 0 : 3;
        expected[hart].vtype = 0;
        expected[hart].vill = hart;
        expected[hart].vxrm = hart + 1;
        expected[hart].vxsat = hart;
        for (reg = 0; reg < 32; reg++) {
            for (byte = 0; byte < expected[hart].vlenb; byte++) {
                expected[hart].registers[reg][byte] =
                    1 + hart * 71 + reg * 13 + byte;
            }
        }
        if (gem5_qemu_jit_set_vector_state(hart, &expected[hart],
                                            sizeof(expected[hart]))) {
            return -1;
        }
    }
    for (hart = 0; hart < 2; hart++) {
        for (test = 0; test < 8; test++) {
            size_t size = sizeof(bad);
            bad = expected[hart];
            switch (test) {
            case 0: bad.version++; break;
            case 1: bad.size--; break;
            case 2: bad.vlenb *= 2; break;
            case 3: bad.vxrm = 4; break;
            case 4: bad.vxsat = 2; break;
            case 5: bad.vill = 2; break;
            case 6: size--; break;
            case 7:
                if (bad.vlenb == GEM5_QEMU_JIT_MAX_VLENB) {
                    continue;
                }
                bad.registers[31][bad.vlenb] = 1;
                break;
            }
            if (gem5_qemu_jit_set_vector_state(hart, &bad, size) == 0 ||
                gem5_qemu_jit_get_vector_state(hart, &observed,
                                                sizeof(observed)) ||
                memcmp(&observed, &expected[hart], sizeof(observed))) {
                return -1;
            }
        }
        if (gem5_qemu_jit_get_vector_state(hart, NULL, sizeof(observed)) == 0 ||
            gem5_qemu_jit_set_vector_state(hart, NULL, sizeof(observed)) == 0 ||
            gem5_qemu_jit_get_vector_state(2, &observed,
                                            sizeof(observed)) == 0 ||
            gem5_qemu_jit_set_vector_state(2, &expected[hart],
                                            sizeof(observed)) == 0 ||
            gem5_qemu_jit_set_vector_state(hart, &saved[hart],
                                            sizeof(saved[hart]))) {
            return -1;
        }
        gem5_qemu_jit_invalidate_translations(hart);
    }
    puts("vector migration state: two-hart VS-Off round-trip passed");
    return 0;
}

static int
vector_execution_smoke(void)
{
    /* vid.v v8; vadd.vi v9,v8,3; vse8.v v9,(x7) */
    const uint32_t program[] = {0x5208a457, 0x0281b4d7, 0x020384a7};
    Gem5QemuJitVectorState state;
    Gem5QemuJitRunResult result;
    uint64_t status;
    unsigned hart, byte;

    memcpy(memory + 64, program, sizeof(program));
    for (hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_vector_state(hart, &state, sizeof(state)) ||
            gem5_qemu_jit_get_csr(hart, 0x300, &status)) {
            return -1;
        }
        state.vtype = 0; /* e8/m1, tail/mask undisturbed */
        state.vill = 0;
        state.vl = 8;
        state.vstart = 3;
        memset(state.registers[8], 0x55, state.vlenb);
        if (gem5_qemu_jit_set_vector_state(hart, &state, sizeof(state)) ||
            gem5_qemu_jit_set_csr(hart, 0x300, status | (1u << 9)) ||
            gem5_qemu_jit_set_gpr(hart, 7, 512 + hart * 16)) {
            return -1;
        }
        gem5_qemu_jit_set_pc(hart, 64);
        gem5_qemu_jit_invalidate_translations(hart);
        if (gem5_qemu_jit_run(hart, 1, &result) ||
            result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            result.instructions != 1 || gem5_qemu_jit_get_pc(hart) != 68 ||
            gem5_qemu_jit_get_vector_state(hart, &state, sizeof(state)) ||
            state.vstart != 0) {
            return -1;
        }
        for (byte = 0; byte < 8; byte++) {
            if (state.registers[8][byte] != (byte < 3 ? 0x55 : byte)) {
                return -1;
            }
        }
        if (gem5_qemu_jit_run(hart, 2, &result) ||
            result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            result.instructions != 2 || gem5_qemu_jit_get_pc(hart) != 76) {
            return -1;
        }
        for (byte = 0; byte < 8; byte++) {
            if (memory[512 + hart * 16 + byte] !=
                (byte < 3 ? 0x58 : byte + 3)) {
                return -1;
            }
        }
    }
    puts("RVA23 vector execution from imported VSTART state passed");
    return 0;
}

static int
virtual_mode_smoke(void)
{
    const unsigned host[] = {0x105, 0x140, 0x141, 0x142, 0x143, 0x180, 0x100};
    const unsigned guest[] = {0x205, 0x240, 0x241, 0x242, 0x243, 0x280, 0x200};
    uint64_t saved_host[7], saved_guest[7], hs[7], vs[7], updated[7], value;
    unsigned hart, i, privilege, virt;

    for (hart = 0; hart < 2; hart++) {
        for (i = 0; i < 7; i++) {
            hs[i] = i == 5 ? (UINT64_C(8) << 60) | 0x10 :
                            0x100 + hart * 64 + i * 4;
            vs[i] = i == 5 ? (UINT64_C(8) << 60) | 0x20 :
                            0x200 + hart * 64 + i * 4;
            if (i == 6) {
                hs[i] = (UINT64_C(2) << 32) | (1u << 18); /* UXL/SUM */
                vs[i] = (UINT64_C(2) << 32) | (1u << 19); /* UXL/MXR */
            }
            updated[i] = i == 6 ? vs[i] | 2 : vs[i] + 0x40;
            if (gem5_qemu_jit_get_csr(hart, host[i], &saved_host[i]) ||
                gem5_qemu_jit_get_csr(hart, guest[i], &saved_guest[i]) ||
                gem5_qemu_jit_set_csr(hart, host[i], hs[i]) ||
                gem5_qemu_jit_set_csr(hart, guest[i], vs[i])) {
                return -1;
            }
        }
        if (gem5_qemu_jit_set_mode(hart, 1, 1) ||
            gem5_qemu_jit_get_mode(hart, &privilege, &virt) ||
            privilege != 1 || virt != 1) {
            return -1;
        }
        for (i = 0; i < 7; i++) {
            if (gem5_qemu_jit_get_csr(hart, host[i], &value) ||
                value != vs[i] ||
                gem5_qemu_jit_set_csr(hart, host[i], updated[i])) {
                return -1;
            }
        }
        if (gem5_qemu_jit_set_mode(hart, 0, 1) ||
            gem5_qemu_jit_get_mode(hart, &privilege, &virt) ||
            privilege != 0 || virt != 1 ||
            gem5_qemu_jit_set_mode(hart, 3, 1) == 0 ||
            gem5_qemu_jit_set_mode(hart, 1, 2) == 0 ||
            gem5_qemu_jit_set_priv(hart, 2) == 0 ||
            gem5_qemu_jit_get_mode(hart, &privilege, &virt) ||
            privilege != 0 || virt != 1 ||
            gem5_qemu_jit_set_mode(hart, 1, 1)) {
            return -1;
        }
        for (i = 0; i < 7; i++) {
            if (gem5_qemu_jit_get_csr(hart, host[i], &value) ||
                value != updated[i]) {
                return -1;
            }
        }
        /* Legacy M-mode borrowing must bank correctly too. */
        if (gem5_qemu_jit_set_priv(hart, 3) ||
            gem5_qemu_jit_get_mode(hart, &privilege, &virt) ||
            privilege != 3 || virt != 0) {
            return -1;
        }
        for (i = 0; i < 7; i++) {
            if (gem5_qemu_jit_get_csr(hart, host[i], &value) || value != hs[i] ||
                gem5_qemu_jit_get_csr(hart, guest[i], &value) ||
                value != updated[i] ||
                gem5_qemu_jit_set_csr(hart, host[i], saved_host[i]) ||
                gem5_qemu_jit_set_csr(hart, guest[i], saved_guest[i])) {
                return -1;
            }
        }
        gem5_qemu_jit_invalidate_translations(hart);
    }
    puts("virtual mode: HS/VS banks and VU transitions passed");
    return 0;
}

static int
timer_smoke(void)
{
    const unsigned csrs[] = {0x30a, 0x60a, 0x14d, 0x24d, 0x605, 0x645};
    uint64_t saved[6], value, mip;
    Gem5QemuJitTimerState state;
    Gem5QemuJitRunResult result;
    /* csrw stimecmp,x5; addi x6,x0,1 */
    const uint32_t program[] = {0x14d29073, 0x00100313};
    unsigned hart, i;
    const uint64_t stce = UINT64_C(1) << 63;
    memcpy(memory + 96, program, sizeof(program));
    for (hart = 0; hart < 2; hart++) {
        mip = gem5_qemu_jit_get_mip(hart);
        for (i = 0; i < 6; i++) {
            if (gem5_qemu_jit_get_csr(hart, csrs[i], &saved[i])) {
                fprintf(stderr, "timer save hart %u CSR %#x failed\n", hart, csrs[i]);
                return -1;
            }
        }
        /* Non-virtual M/S pending bits must not read back as HVIP. */
        gem5_qemu_jit_set_mip(hart, mip | 0xaaa);
        if (gem5_qemu_jit_get_csr(hart, 0x645, &value) || (value & 0xaaa)) {
            fprintf(stderr, "HVIP leaked physical pending bits on hart %u\n", hart);
            return -1;
        }
        gem5_qemu_jit_set_mip(hart, mip);
        host_time[hart] = 10;
        if (gem5_qemu_jit_set_csr(hart, 0x14d, 20) ||
            gem5_qemu_jit_set_csr(hart, 0x24d, 25) ||
            gem5_qemu_jit_set_csr(hart, 0x605, 5) ||
            gem5_qemu_jit_set_csr(hart, 0x30a, saved[0] | stce) ||
            gem5_qemu_jit_set_csr(hart, 0x60a, saved[1] | stce) ||
            gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.flags != 3 || state.time != 10 || state.stimecmp != 20 ||
            state.vstimecmp != 25 || state.htimedelta != 5) {
            fprintf(stderr, "timer setup hart %u flags=%u\n", hart, state.flags);
            return -1;
        }
        host_time[hart] = 20;
        if (gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.flags != 15 || !(gem5_qemu_jit_get_mip(hart) & (1u << 5)) ||
            gem5_qemu_jit_set_csr(hart, 0x645, 0) ||
            gem5_qemu_jit_get_csr(hart, 0x645, &value) ||
            (value & (1u << 6)) ||
            gem5_qemu_jit_get_csr(hart, 0x644, &value) ||
            !(value & (1u << 6)) ||
            gem5_qemu_jit_set_csr(hart, 0x645, 1u << 6) ||
            gem5_qemu_jit_set_csr(hart, 0x60a, saved[1] & ~stce) ||
            gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.flags != 5 ||
            gem5_qemu_jit_get_csr(hart, 0x645, &value) ||
            !(value & (1u << 6))) {
            fprintf(stderr, "timer gates hart %u flags=%u hvip=%llx\n", hart,
                    state.flags, (unsigned long long)value);
            return -1;
        }
        host_time[hart] = UINT64_MAX;
        if (gem5_qemu_jit_set_csr(hart, 0x14d, UINT64_MAX) ||
            gem5_qemu_jit_set_csr(hart, 0x605, 1) ||
            gem5_qemu_jit_set_csr(hart, 0x60a, saved[1] | stce) ||
            gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.flags != 7) {
            fprintf(stderr, "timer max hart %u flags=%u\n", hart, state.flags);
            return -1;
        }
        host_time[hart] = 0;
        if (gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.flags != 3 ||
            gem5_qemu_jit_set_csr(hart, 0x30a, saved[0] & ~stce) ||
            gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.flags || (gem5_qemu_jit_get_mip(hart) & (1u << 5))) {
            fprintf(stderr, "timer wrap hart %u flags=%u\n", hart, state.flags);
            return -1;
        }
        gem5_qemu_jit_set_mip(hart, mip | (1u << 5));
        if (gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            !(gem5_qemu_jit_get_mip(hart) & (1u << 5))) {
            fprintf(stderr, "timer software STIP hart %u failed\n", hart);
            return -1;
        }
        if (gem5_qemu_jit_set_csr(hart, 0x30a, saved[0] | stce) ||
            gem5_qemu_jit_set_gpr(hart, 5, 100) ||
            gem5_qemu_jit_set_gpr(hart, 6, 0)) {
            return -1;
        }
        gem5_qemu_jit_set_pc(hart, 96);
        gem5_qemu_jit_invalidate_translations(hart);
        if (gem5_qemu_jit_run(hart, 2, &result) ||
            result.reason != GEM5_QEMU_JIT_EXIT_INTERRUPT ||
            result.instructions != 1 || gem5_qemu_jit_get_pc(hart) != 100 ||
            gem5_qemu_jit_get_gpr(hart, 6) != 0 ||
            gem5_qemu_jit_refresh_timers(hart, &state, sizeof(state)) ||
            state.stimecmp != 100 ||
            gem5_qemu_jit_run(hart, 1, &result) ||
            result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            gem5_qemu_jit_get_gpr(hart, 6) != 1) {
            fprintf(stderr, "timer CSR batch boundary hart %u failed\n", hart);
            return -1;
        }
        for (i = 0; i < 6; i++) {
            if (gem5_qemu_jit_set_csr(hart, csrs[i], saved[i])) {
                fprintf(stderr, "timer restore hart %u CSR %#x failed\n", hart, csrs[i]);
                return -1;
            }
        }
        gem5_qemu_jit_set_mip(hart, mip);
    }
    puts("host timers: deadlines, gates, wrap and independent HVIP passed");
    return 0;
}

static int
pmp_state_smoke(void)
{
    Gem5QemuJitPmpState saved[2], state[2], actual, invalid;
    uint64_t address = 0;
    unsigned hart, test;
    for (hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_pmp_state(hart, &saved[hart], sizeof(actual)) ||
            saved[hart].regions < 2) {
            fprintf(stderr, "PMP hart %u regions %u\n", hart, saved[hart].regions);
            return -1;
        }
        state[hart] = saved[hart];
        state[hart].address[0] = 64 + hart * 16;
        state[hart].address[1] = 128 + hart * 16;
        state[hart].config[0] = 0;
        state[hart].config[1] = 0x8f; /* Locked TOR, RWX. */
        if (gem5_qemu_jit_set_pmp_state(hart, &state[hart], sizeof(actual)) ||
            gem5_qemu_jit_set_csr(hart, 0x3b0, 1) ||
            gem5_qemu_jit_get_csr(hart, 0x3b0, &address) ||
            address != state[hart].address[0]) {
            fprintf(stderr, "PMP CSR hart %u address %llu expected %llu priv %u\n",
                    hart, (unsigned long long)address,
                    (unsigned long long)state[hart].address[0],
                    gem5_qemu_jit_get_priv(hart));
            return -1;
        }
        /* Migration can replace even the lower bound of a locked TOR. */
        state[hart].address[0] += 8;
        state[hart].address[1] += 8;
        if (gem5_qemu_jit_set_pmp_state(hart, &state[hart], sizeof(actual))) {
            return -1;
        }
    }
    for (hart = 0; hart < 2; hart++) {
        for (test = 0; test < 7; test++) {
            invalid = state[hart];
            switch (test) {
            case 0: invalid.version++; break;
            case 1: invalid.size--; break;
            case 2: invalid.regions--; break;
            case 3: invalid.reserved = 1; break;
            case 4: invalid.regions = GEM5_QEMU_JIT_MAX_PMPS + 1; break;
            case 5:
                if (invalid.regions == GEM5_QEMU_JIT_MAX_PMPS) {
                    continue;
                }
                invalid.address[invalid.regions] = 1;
                break;
            case 6:
                if (invalid.regions == GEM5_QEMU_JIT_MAX_PMPS) {
                    continue;
                }
                invalid.config[invalid.regions] = 1;
                break;
            }
            if (gem5_qemu_jit_set_pmp_state(hart, &invalid, sizeof(invalid)) != -1 ||
                gem5_qemu_jit_get_pmp_state(hart, &actual, sizeof(actual)) ||
                memcmp(&actual, &state[hart], sizeof(actual))) {
                return -1;
            }
        }
        if (gem5_qemu_jit_set_pmp_state(hart, &saved[hart], sizeof(actual)) ||
            gem5_qemu_jit_get_pmp_state(hart, &actual, sizeof(actual)) ||
            memcmp(&actual, &saved[hart], sizeof(actual))) {
            return -1;
        }
        gem5_qemu_jit_invalidate_translations(hart);
    }
    puts("PMP migration: locked TOR replacement and rejection atomicity passed");
    return 0;
}

static int
pmp_execution_smoke(void)
{
    /* ld x5,0(x7); marker; sd x8,0(x7); marker; trap marker */
    const uint32_t program[] = {
        0x0003b283, 0x0000007b, 0x0083b023, 0x0000007b, 0x0000007b
    };
    const unsigned csrs[] = {0x300, 0x305, 0x341, 0x342, 0x343};
    Gem5QemuJitPmpState saved, state;
    Gem5QemuJitRunResult result;
    uint64_t saved_csr[5], cause, epc, tval;
    unsigned hart, mode, store, phase, i;
    memcpy(memory + 224, program, sizeof(program));
    for (hart = 0; hart < 2; hart++) {
        if (gem5_qemu_jit_get_pmp_state(hart, &saved, sizeof(saved)) ||
            saved.regions < 3) {
            return -1;
        }
        for (i = 0; i < 5; i++) {
            if (gem5_qemu_jit_get_csr(hart, csrs[i], &saved_csr[i])) {
                return -1;
            }
        }
        for (mode = 1; mode <= 3; mode += 2) {
            for (store = 0; store < 2; store++) {
                for (phase = 0; phase < 4; phase++) {
                    unsigned pc = store ? 232 : 224;
                    int denied = phase == 1;
                    state = saved;
                    memset(state.address, 0, sizeof(state.address));
                    memset(state.config, 0, sizeof(state.config));
                    state.address[0] = phase == 3 ? 258 : 256;
                    state.address[1] = 272;
                    state.config[1] = phase == 0 || phase == 2 ? 0x8b : 0x88;
                    /* Low-priority 4 KiB RWX NAPOT permits S-mode code
                     * and accesses outside the first locked TOR range. */
                    state.address[2] = 511;
                    state.config[2] = 0x1f;
                    memset(memory + 1024, 0, 8);
                    memory[1024] = 17;
                    if (gem5_qemu_jit_set_priv(hart, 3) ||
                        gem5_qemu_jit_set_csr(hart, 0x300,
                            saved_csr[0] & ~((1ULL << 17) | 0xa)) ||
                        gem5_qemu_jit_set_csr(hart, 0x305, 240) ||
                        gem5_qemu_jit_set_csr(hart, 0x342, 0) ||
                        gem5_qemu_jit_set_pmp_state(hart, &state, sizeof(state)) ||
                        gem5_qemu_jit_set_gpr(hart, 5, 99) ||
                        gem5_qemu_jit_set_gpr(hart, 7, 1024) ||
                        gem5_qemu_jit_set_gpr(hart, 8, 42) ||
                        gem5_qemu_jit_set_priv(hart, mode)) {
                        return -1;
                    }
                    gem5_qemu_jit_set_pc(hart, pc);
                    /* No explicit invalidation: the migration setter must
                     * revoke the cached permissions from the previous run. */
                    if (gem5_qemu_jit_run(hart, 8, &result) ||
                        result.reason != GEM5_QEMU_JIT_EXIT_M5OP ||
                        gem5_qemu_jit_set_priv(hart, 3) ||
                        gem5_qemu_jit_get_csr(hart, 0x342, &cause) ||
                        gem5_qemu_jit_get_csr(hart, 0x341, &epc) ||
                        gem5_qemu_jit_get_csr(hart, 0x343, &tval) ||
                        cause != (denied ? (store ? 7 : 5) : 0) ||
                        (denied && (epc != pc || tval != 1024)) ||
                        memory[1024] != (store && !denied ? 42 : 17) ||
                        (!store && gem5_qemu_jit_get_gpr(hart, 5) !=
                            (denied ? 99 : 17))) {
                        fprintf(stderr, "PMP execution hart=%u mode=%u store=%u phase=%u\n",
                                hart, mode, store, phase);
                        return -1;
                    }
                }
            }
        }
        if (gem5_qemu_jit_set_pmp_state(hart, &saved, sizeof(saved))) {
            return -1;
        }
        for (i = 0; i < 5; i++) {
            if (gem5_qemu_jit_set_csr(hart, csrs[i], saved_csr[i])) {
                return -1;
            }
        }
        gem5_qemu_jit_invalidate_translations(hart);
    }
    puts("PMP execution: S/M loads/stores, revocation and moved TOR bounds passed");
    return 0;
}

static int
reservation_smoke(int profile_test)
{
    /* lr.d x5,(x7); sc.d x6,x8,(x7) */
    const uint32_t program[] = {0x1003b2af, 0x1883b32f};
    const uint32_t interfering_store = 0x0083b023; /* sd x8,0(x7) */
    Gem5QemuJitRunResult result;
    unsigned hart, invalidate;
    memcpy(memory + 128, program, sizeof(program));
    memcpy(memory + 192, &interfering_store, sizeof(interfering_store));
    for (hart = 0; hart < 2; hart++) {
        for (invalidate = 0; invalidate < 3; invalidate++) {
            memset(memory + 768, 0, 8);
            memory[768] = 17;
            if (gem5_qemu_jit_set_gpr(hart, 7, 768) ||
                gem5_qemu_jit_set_gpr(hart, 8, 42)) {
                return -1;
            }
            gem5_qemu_jit_set_pc(hart, 128);
            gem5_qemu_jit_invalidate_translations(hart);
            if (gem5_qemu_jit_run(hart, 1, &result) ||
                result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
                gem5_qemu_jit_get_gpr(hart, 5) != 17 ||
                gem5_qemu_jit_get_pc(hart) != 132 ||
                gem5_qemu_jit_set_priv(hart, 3) ||
                gem5_qemu_jit_set_mode(hart, 1, 0) ||
                gem5_qemu_jit_set_mode(hart, 3, 0)) {
                return -1;
            }
            if (profile_test &&
                (gem5_qemu_jit_set_mode(hart, 1, 1) ||
                 gem5_qemu_jit_set_mode(hart, 0, 1) ||
                 gem5_qemu_jit_set_mode(hart, 3, 0))) {
                return -1;
            }
            if (invalidate == 1) {
                gem5_qemu_jit_invalidate_translations(hart);
            } else if (invalidate == 2) {
                unsigned other = hart ^ 1;
                if (gem5_qemu_jit_set_gpr(other, 7, 768) ||
                    gem5_qemu_jit_set_gpr(other, 8, 99)) {
                    return -1;
                }
                gem5_qemu_jit_set_pc(other, 192);
                gem5_qemu_jit_invalidate_translations(other);
                if (gem5_qemu_jit_run(other, 1, &result) ||
                    result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
                    memory[768] != 99) {
                    return -1;
                }
            }
            if (gem5_qemu_jit_run(hart, 1, &result) ||
                result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
                gem5_qemu_jit_get_pc(hart) != 136 ||
                (gem5_qemu_jit_get_gpr(hart, 6) == 0) != !invalidate ||
                memory[768] != (invalidate == 2 ? 99 : invalidate ? 17 : 42)) {
                fprintf(stderr, "reservation hart %u invalidate=%u sc=%llu\n",
                        hart, invalidate,
                        (unsigned long long)gem5_qemu_jit_get_gpr(hart, 6));
                return -1;
            }
        }
    }
    puts("LR/SC: batch/mode preservation, invalidation and interference passed");
    return 0;
}

int
main(int argc, char **argv)
{
    /* addi x1,x0,5; addi x2,x1,7; csrr x3,mhartid; sw x2,256(x0) */
    const uint32_t program[] = {
        0x00500093, 0x00708113, 0xf14021f3, 0x10202023
    };
    const Gem5QemuJitCallbacks callbacks[] = {
        {
            .instance_id = 0,
            .instance_count = 2,
            .hart_id = 7,
            .memory_read = memory_read,
            .memory_write = memory_write,
            .memory_map = memory_map,
            .read_time = read_time,
            .opaque = &host_time[0],
        },
        {
            .instance_id = 1,
            .instance_count = 2,
            .hart_id = 11,
            .memory_read = memory_read,
            .memory_write = memory_write,
            .memory_map = memory_map,
            .read_time = read_time,
            .opaque = &host_time[1],
        },
    };
    Gem5QemuJitRunResult results[2];
    unsigned instance;
    Gem5QemuJitConfig config = {
        .version = GEM5_QEMU_JIT_CONFIG_VERSION,
        .size = sizeof(config),
        .profile = GEM5_QEMU_JIT_PROFILE_RVA23S64,
        .vlenb = 32,
    };
    const int profile_test = argc == 3 && !strcmp(argv[1], "--rva23-vlenb");
    if (argc != 1 && !profile_test) {
        fprintf(stderr, "usage: %s [--rva23-vlenb N]\n", argv[0]);
        return 1;
    }
    if (profile_test) {
        char *end;
        unsigned long vlenb = strtoul(argv[2], &end, 0);
        if (!*argv[2] || *end || vlenb > GEM5_QEMU_JIT_MAX_VLENB) {
            return 1;
        }
        config.vlenb = vlenb;
    }
    /* Rejected configuration must not initialize or poison QEMU. */
    for (unsigned test = 0; test < 8; test++) {
        Gem5QemuJitConfig bad = config;
        size_t size = sizeof(bad);
        switch (test) {
        case 0: bad.version++; break;
        case 1: bad.size--; break;
        case 2: bad.profile = 2; break;
        case 3: bad.vlenb = 24; break;
        case 4: bad.vlenb = 8; break;
        case 5: bad.vlenb = 256; break;
        case 6: bad.profile = 0; bad.vlenb = 32; break;
        case 7: size--; break;
        }
        if (gem5_qemu_jit_init_config(&callbacks[0], &bad, size) == 0) {
            fprintf(stderr, "invalid configuration accepted\n");
            return 1;
        }
    }
    if (gem5_qemu_jit_init_config(&callbacks[0], NULL, sizeof(config)) == 0) {
        return 1;
    }

    memcpy(memory, program, sizeof(program));
    for (instance = 0; instance < 2; ++instance) {
        uint64_t value;
        const uint64_t fflags = 1u << instance;
        const uint64_t frm = instance + 1;

        if (instance == 1) {
            Gem5QemuJitConfig mismatch = config;
            mismatch.vlenb = config.vlenb == 16 ? 32 : 16;
            if (gem5_qemu_jit_init_config(&callbacks[instance], &mismatch,
                                            sizeof(mismatch)) == 0) {
                fprintf(stderr, "mixed hart configuration accepted\n");
                return 1;
            }
            if (profile_test && gem5_qemu_jit_init(&callbacks[instance]) == 0) {
                fprintf(stderr, "mixed legacy/profile configuration accepted\n");
                return 1;
            }
        }

        if ((profile_test ? gem5_qemu_jit_init_config(&callbacks[instance],
                              &config, sizeof(config)) :
                            gem5_qemu_jit_init(&callbacks[instance])) != 0) {
            fprintf(stderr, "adapter instance %u initialization failed\n",
                    instance);
            return 1;
        }
        /*
         * SSTC needs a QEMU timer to post STIP asynchronously, but gem5 owns
         * the event queue and CLINT interrupts. The embedded CPU must not
         * advertise an unsynchronized internal timer implementation.
         */
        if (!profile_test &&
            gem5_qemu_jit_get_csr(instance, 0x14d, &value) == 0) {
            fprintf(stderr, "adapter instance %u unexpectedly exposes sstc\n",
                    instance);
            return 1;
        }
        if (profile_test) {
            Gem5QemuJitVectorState state;
            const uint64_t vh = (1u << 21) | (1u << 7);
            if (gem5_qemu_jit_get_csr(instance, 0x301, &value) ||
                (value & vh) != vh ||
                gem5_qemu_jit_get_vector_state(instance, &state,
                                                sizeof(state)) ||
                state.vlenb != config.vlenb) {
                fprintf(stderr, "profile or VLEN mismatch\n");
                return 1;
            }
        }
        /*
         * CPU takeover must preserve FP control state even while mstatus.FS
         * is Off, when an architectural CSR instruction would trap.
         */
        if (gem5_qemu_jit_set_csr(instance, 0x001, fflags) != 0 ||
            gem5_qemu_jit_get_csr(instance, 0x001, &value) != 0 ||
            value != fflags ||
            gem5_qemu_jit_set_csr(instance, 0x002, frm) != 0 ||
            gem5_qemu_jit_get_csr(instance, 0x002, &value) != 0 ||
            value != frm) {
            fprintf(stderr,
                    "adapter instance %u FP control-state transfer failed\n",
                    instance);
            return 1;
        }
        gem5_qemu_jit_set_pc(instance, 0);
        if (gem5_qemu_jit_set_gpr(instance, 4, 0x44 + instance) != 0) {
            fprintf(stderr, "adapter instance %u register setup failed\n",
                    instance);
            return 1;
        }
    }

    if (profile_test && hpm_transfer_smoke()) {
        fprintf(stderr, "HPM bank migration failed\n");
        return 1;
    }
    if (profile_test && fixed_counter_transfer_smoke()) {
        fprintf(stderr, "fixed-counter migration failed\n");
        return 1;
    }
    if (profile_test && counter_isolation_smoke()) {
        return 1;
    }
    if (counter_restore_smoke()) {
        fprintf(stderr, "host counter restoration failed\n");
        return 1;
    }
    if (profile_test && mstatus_restore_smoke()) {
        fprintf(stderr, "machine status restoration failed\n");
        return 1;
    }
    if (profile_test && stateen_reserved_smoke()) {
        fprintf(stderr, "reserved supervisor state-enable bits writable\n");
        return 1;
    }
    if (profile_test && stateen_transfer_smoke()) {
        fprintf(stderr, "raw state-enable transfer failed\n");
        return 1;
    }
    if (vector_state_smoke()) {
        fprintf(stderr, "vector migration state smoke failed\n");
        return 1;
    }
    if ((profile_test && virtual_mode_smoke()) ||
        (!profile_test && gem5_qemu_jit_set_mode(0, 1, 1) == 0) ||
        gem5_qemu_jit_set_priv(0, 2) == 0 ||
        gem5_qemu_jit_set_mode(2, 3, 0) == 0) {
        fprintf(stderr, "virtual mode smoke failed\n");
        return 1;
    }

    for (instance = 0; instance < 2; ++instance) {
        unsigned other = instance ^ 1;

        if (gem5_qemu_jit_run(instance, 4, &results[instance]) != 0) {
            fprintf(stderr, "adapter instance %u execution failed\n",
                    instance);
            return 1;
        }

        if (gem5_qemu_jit_get_pc(other) != (instance == 0 ? 0 : 16) ||
            gem5_qemu_jit_get_gpr(other, 4) != 0x44 + other) {
            fprintf(stderr,
                    "instance %u execution corrupted instance %u: "
                    "pc=0x%llx x4=0x%llx\n",
                    instance, other,
                    (unsigned long long)gem5_qemu_jit_get_pc(other),
                    (unsigned long long)gem5_qemu_jit_get_gpr(other, 4));
            return 1;
        }
    }

    for (instance = 0; instance < 2; ++instance) {
        if (results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            results[instance].instructions != 4 ||
            gem5_qemu_jit_get_pc(instance) != 16 ||
            gem5_qemu_jit_get_gpr(instance, 1) != 5 ||
            gem5_qemu_jit_get_gpr(instance, 2) != 12 ||
            gem5_qemu_jit_get_gpr(instance, 3) !=
                callbacks[instance].hart_id ||
            gem5_qemu_jit_get_gpr(instance, 4) != 0x44 + instance) {
            fprintf(stderr,
                    "unexpected instance %u result: reason=%d insns=%llu "
                    "pc=0x%llx x1=%llu x2=%llu x3=%llu x4=0x%llx "
                    "qemu_exception=%d\n",
                    instance, results[instance].reason,
                    (unsigned long long)results[instance].instructions,
                    (unsigned long long)gem5_qemu_jit_get_pc(instance),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 1),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 2),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 3),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 4),
                    results[instance].qemu_exception);
            return 1;
        }
    }
    if (memory[0x100] != 12) {
        fprintf(stderr, "unexpected shared memory value: %u\n", memory[0x100]);
        return 1;
    }

    /*
     * Reverse takeover may change executable memory without changing SATP.
     * The backend must discard an already translated block before resuming.
     */
    {
        const uint32_t replacement = 0x00900093; /* addi x1,x0,9 */

        memcpy(memory, &replacement, sizeof(replacement));
        gem5_qemu_jit_set_pc(0, 0);
        if (gem5_qemu_jit_set_gpr(0, 1, 0) != 0) {
            fprintf(stderr, "translation invalidation setup failed\n");
            return 1;
        }
        gem5_qemu_jit_invalidate_translations(0);
        if (gem5_qemu_jit_run(0, 1, &results[0]) != 0 ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            gem5_qemu_jit_get_gpr(0, 1) != 9 ||
            gem5_qemu_jit_get_pc(0) != 4) {
            fprintf(stderr, "translation invalidation failed\n");
            return 1;
        }
    }

    if (pmp_state_smoke()) {
        fprintf(stderr, "PMP migration smoke failed\n");
        return 1;
    }
    if (pmp_execution_smoke()) {
        return 1;
    }
    if (reservation_smoke(profile_test)) {
        return 1;
    }
    if (profile_test && timer_smoke()) {
        fprintf(stderr, "host timer smoke failed\n");
        return 1;
    }
    if (profile_test && vector_execution_smoke()) {
        fprintf(stderr, "RVA23 vector execution smoke failed\n");
        return 1;
    }
    puts("gem5 QEMU JIT two-hart smoke test passed");
    return 0;
}
