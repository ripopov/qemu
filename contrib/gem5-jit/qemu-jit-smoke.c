/*
 * Standalone execution smoke test for the gem5 QEMU JIT adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu-jit.h"

#include <stdio.h>
#include <string.h>

static uint8_t memory[4096];

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

int
main(void)
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
        },
        {
            .instance_id = 1,
            .instance_count = 2,
            .hart_id = 11,
            .memory_read = memory_read,
            .memory_write = memory_write,
            .memory_map = memory_map,
        },
    };
    Gem5QemuJitRunResult results[2];
    unsigned instance;

    memcpy(memory, program, sizeof(program));
    for (instance = 0; instance < 2; ++instance) {
        uint64_t value;
        const uint64_t fflags = 1u << instance;
        const uint64_t frm = instance + 1;

        if (gem5_qemu_jit_init(&callbacks[instance]) != 0) {
            fprintf(stderr, "adapter instance %u initialization failed\n",
                    instance);
            return 1;
        }
        /*
         * SSTC needs a QEMU timer to post STIP asynchronously, but gem5 owns
         * the event queue and CLINT interrupts. The embedded CPU must not
         * advertise an unsynchronized internal timer implementation.
         */
        if (gem5_qemu_jit_get_csr(instance, 0x14d, &value) == 0) {
            fprintf(stderr, "adapter instance %u unexpectedly exposes sstc\n",
                    instance);
            return 1;
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

    if (vector_state_smoke()) {
        fprintf(stderr, "vector migration state smoke failed\n");
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

    puts("gem5 QEMU JIT two-hart smoke test passed");
    return 0;
}
