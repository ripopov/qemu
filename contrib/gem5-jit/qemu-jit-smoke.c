/*
 * Standalone execution smoke test for the gem5 QEMU JIT adapter
 *
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu-jit.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static uint8_t memory[8192];
static unsigned writes;
static uint64_t last_write_count[4];
static uint64_t reservation_address[4];
static bool reserved[4];
static uint64_t host_cycles;
static volatile sig_atomic_t term_seen;

static void host_term(int signal_number)
{
    term_seen = signal_number;
}

static uint64_t read_cycles(void *opaque)
{
    uint64_t executed = gem5_qemu_jit_executed(*(uint32_t *)opaque);
    return host_cycles + (executed ? executed - 1 : 0);
}

static int run(uint32_t instance, uint64_t budget, Gem5QemuJitRunResult *result)
{
    int status = gem5_qemu_jit_run(instance, budget, result);
    if (!status) {
        for (unsigned reg = 0; reg < 32; ++reg) {
            if (result->gprs[reg] != gem5_qemu_jit_get_gpr(instance, reg)) {
                fprintf(stderr, "batch GPR snapshot mismatch\n");
                return -1;
            }
        }
        if (result->pc != gem5_qemu_jit_get_pc(instance) ||
            result->privilege != gem5_qemu_jit_get_priv(instance)) {
            fprintf(stderr, "batch PC/privilege snapshot mismatch\n");
            return -1;
        }
        host_cycles += result->instructions;
    }
    return status;
}

static int check_pmu(uint64_t cycles, uint64_t instructions, bool pending)
{
    uint64_t actual_cycles, actual_instructions;
    if (gem5_qemu_jit_pmu_poll(0, &actual_cycles, &actual_instructions) ||
        actual_cycles != cycles || actual_instructions != instructions ||
        !!(gem5_qemu_jit_get_mip(0) & (1u << 13)) != pending) {
        fprintf(stderr, "PMU deadline/interrupt mismatch\n");
        return -1;
    }
    return 0;
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
    if (opaque) {
        unsigned hart = *(uint32_t *)opaque;
        last_write_count[hart] = gem5_qemu_jit_executed(hart);
        if (!last_write_count[hart] || last_write_count[hart] == UINT64_MAX) {
            return -1;
        }
    }
    if (address > sizeof(memory) || size > sizeof(memory) - address) {
        return -1;
    }
    memcpy(memory + address, data, size);
    ++writes;
    for (unsigned i = 0; i < 4; ++i) {
        if (reserved[i] && address < reservation_address[i] + 64 &&
            address + size > reservation_address[i]) {
            reserved[i] = false;
        }
    }
    return 0;
}

static uint64_t read_time(void *opaque)
{
    return 1000000 + gem5_qemu_jit_executed(*(uint32_t *)opaque);
}

static int reservation(void *opaque, uint64_t address, uint64_t value,
                       unsigned size, int store, uint64_t *result)
{
    unsigned hart = *(uint32_t *)opaque;
    uint8_t data[8];

    if (address > sizeof(memory) || size > sizeof(memory) - address) {
        return -1;
    }
    if (!store) {
        *result = 0;
        for (unsigned i = 0; i < size; ++i) {
            *result |= (uint64_t)memory[address + i] << (8 * i);
        }
        reserved[hart] = true;
        reservation_address[hart] = address & ~UINT64_C(63);
        return 0;
    }
    *result = !(reserved[hart] &&
                reservation_address[hart] == (address & ~UINT64_C(63)));
    reserved[hart] = false;
    if (*result == 0) {
        for (unsigned i = 0; i < size; ++i) {
            data[i] = value >> (8 * i);
        }
        return memory_write(opaque, address, data, size);
    }
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

static bool
has_extension(const char *isa, const char *extension)
{
    size_t length = strlen(extension);
    const char *token = isa;

    while ((token = strchr(token, '_'))) {
        ++token;
        if (!strncmp(token, extension, length) &&
            (token[length] == '_' || token[length] == '\0')) {
            return true;
        }
    }
    return false;
}

int
main(int argc, char **argv)
{
    const unsigned count = argc == 1 ? 2 :
        argc == 2 && !strcmp(argv[1], "1") ? 1 :
        argc == 2 && !strcmp(argv[1], "2") ? 2 :
        argc == 2 && !strcmp(argv[1], "4") ? 4 : 0;
    /* addi x1,x0,5; addi x2,x1,7; csrr x3,mhartid; sw x2,256(x0) */
    const uint32_t program[] = {
        0x00500093, 0x00708113, 0xf14021f3, 0x10202023
    };
    Gem5QemuJitCallbacks callbacks[4];
    Gem5QemuJitRunResult results[4];
    unsigned instance;
    sigset_t signals_before, signals_after;
    struct sigaction action = { .sa_handler = host_term };

    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) ||
        sigprocmask(SIG_SETMASK, NULL, &signals_before)) {
        return 1;
    }

    if (count != 1 && count != 2 && count != 4) {
        fprintf(stderr, "usage: %s [1|2|4]\n", argv[0]);
        return 1;
    }
    memcpy(memory, program, sizeof(program));
    for (instance = 0; instance < count; ++instance) {
        uint64_t value;
        const uint64_t fflags = 1u << instance;
        const uint64_t frm = instance + 1;

        callbacks[instance] = (Gem5QemuJitCallbacks) {
            .abi_version = GEM5_QEMU_JIT_ABI_VERSION,
            .instance_id = instance,
            .instance_count = count,
            .hart_id = instance,
            .memory_read = memory_read,
            .memory_write = memory_write,
            .memory_map = memory_map,
            .reservation = reservation,
            .read_time = read_time,
            .read_cycles = read_cycles,
            .opaque = &callbacks[instance].instance_id,
        };
        if (gem5_qemu_jit_init(&callbacks[instance]) != 0) {
            fprintf(stderr, "adapter instance %u initialization failed\n",
                    instance);
            return 1;
        }
        printf("hart %u resolved ISA: %s\n", instance,
               gem5_qemu_jit_get_isa(instance));
        {
            const char *isa = gem5_qemu_jit_get_isa(instance);
            const char *required[] = {
                "zic64b", "zicbom", "zicbop", "zicboz", "ziccamoa",
                "ziccif", "zicclsm", "ziccrse", "zicntr", "zicond",
                "zicsr", "zifencei", "zihintntl", "zihintpause", "zihpm",
                "zimop", "za64rs", "zfa", "zfh", "zfhmin", "zcb",
                "zcmop", "zba", "zbb", "zbc", "zbs", "zbkb", "zbkc",
                "zbkx", "zkne", "zknd", "zknh", "zks", "zvbc",
                "ssccptr", "sscofpmf", "ssu64xl", "svade",
                /* Dependencies expanded in the modern DT extension list. */
                "zmmul", "zaamo", "zalrsc", "zca", "zcd", "zksed",
                "zksh", "zve32f", "zve32x", "zve64f", "zve64d", "zve64x",
            };
            if (strncmp(isa, "rv64imafdcv_", 12) ||
                strcmp(isa, gem5_qemu_jit_get_isa(0))) {
                fprintf(stderr, "hart %u base ISA mismatch\n", instance);
                return 1;
            }
            for (size_t i = 0; i < sizeof(required) / sizeof(required[0]);
                 ++i) {
                if (!has_extension(isa, required[i])) {
                    fprintf(stderr, "hart %u missing %s\n", instance,
                            required[i]);
                    return 1;
                }
            }
            /* QEMU omits Svbare from its string. Check satp's WARL modes. */
            const unsigned modes[] = { 8, 9, 10, 0 };
            for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
                uint64_t satp = (uint64_t)modes[i] << 60;
                if (gem5_qemu_jit_set_csr(instance, 0x180, satp) ||
                    gem5_qemu_jit_get_csr(instance, 0x180, &value) ||
                    value != satp) {
                    fprintf(stderr, "hart %u satp mode %u failed\n",
                            instance, modes[i]);
                    return 1;
                }
            }
            if (gem5_qemu_jit_translate_debug(instance, 0x1abc, &value) ||
                value != 0x1abc ||
                gem5_qemu_jit_translate_debug(instance, 0, NULL) != -1 ||
                gem5_qemu_jit_executed(instance) != 0) {
                fprintf(stderr, "hart %u bare debug translation failed\n",
                        instance);
                return 1;
            }
        }
        /* Every hart must resolve V's implied extensions and use VLEN=256. */
        if (gem5_qemu_jit_get_csr(instance, 0x301, &value) ||
            !(value & (1ULL << ('V' - 'A'))) ||
            gem5_qemu_jit_set_csr(instance, 0x300, 0x200) ||
            gem5_qemu_jit_get_csr(instance, 0xc22, &value) || value != 32) {
            fprintf(stderr, "hart %u vector configuration failed\n", instance);
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
         * Host state inspection must preserve FP control state even while
         * mstatus.FS is Off, when an architectural CSR would trap.
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

    for (instance = 0; instance < count; ++instance) {
        unsigned other = (instance + 1) % count;

        if (run(instance, 4, &results[instance]) != 0) {
            fprintf(stderr, "adapter instance %u execution failed\n",
                    instance);
            return 1;
        }

        if (gem5_qemu_jit_get_pc(other) != (other > instance ? 0 : 16) ||
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

    for (instance = 0; instance < count; ++instance) {
        if (results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            results[instance].instructions != 4 ||
            gem5_qemu_jit_get_pc(instance) != 16 ||
            gem5_qemu_jit_get_gpr(instance, 1) != 5 ||
            gem5_qemu_jit_get_gpr(instance, 2) != 12 ||
            gem5_qemu_jit_get_gpr(instance, 3) !=
                callbacks[instance].hart_id ||
            gem5_qemu_jit_get_gpr(instance, 4) != 0x44 + instance ||
            last_write_count[instance] != 4) {
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
    if (memory[0x100] != 12 || writes != count) {
        fprintf(stderr, "shared memory: value=%u callback writes=%u\n",
                memory[0x100], writes);
        return 1;
    }

    /*
     * An external writer may change executable memory without changing
     * SATP. Explicit invalidation must discard an already translated block.
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
        if (run(0, 1, &results[0]) != 0 ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            gem5_qemu_jit_get_gpr(0, 1) != 9 ||
            gem5_qemu_jit_get_pc(0) != 4) {
            fprintf(stderr, "translation invalidation failed\n");
            return 1;
        }
    }

    for (instance = 0; instance < count; ++instance) {
        const uint32_t exits[] = { 0x4200007b, 0x10500073 };
        const uint32_t atomic[] = {
            0x10000293, /* li t0,256 */
            0x00300313, /* li t1,3 */
            0x0062a3af, /* amoadd.w t2,t1,(t0) */
            0x0002ae03, /* lw t3,0(t0) */
        };
        const uint32_t vector[] = {
            0x0c0072d7, /* vsetvli t0,zero,e8,m1,ta,ma */
            0x5e03b0d7, /* vmv.v.i v1,7 */
            0x42102357, /* vmv.x.s t1,v1 */
        };

        memcpy(memory + 0x400, atomic, sizeof(atomic));
        gem5_qemu_jit_invalidate_translations(instance);
        gem5_qemu_jit_set_pc(instance, 0x400);
        if (run(instance, 4, &results[instance]) ||
            results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            results[instance].instructions != 4 ||
            gem5_qemu_jit_get_gpr(instance, 7) != 12 + instance * 3 ||
            gem5_qemu_jit_get_gpr(instance, 28) != 15 + instance * 3 ||
            writes != count + instance + 1 ||
            last_write_count[instance] != 3) {
            fprintf(stderr, "hart %u callback AMO failed\n", instance);
            return 1;
        }

        memcpy(memory + 0x300, vector, sizeof(vector));
        gem5_qemu_jit_invalidate_translations(instance);
        gem5_qemu_jit_set_pc(instance, 0x300);
        if (run(instance, 3, &results[instance]) ||
            results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            results[instance].instructions != 3 ||
            gem5_qemu_jit_get_gpr(instance, 5) != 32 ||
            gem5_qemu_jit_get_gpr(instance, 6) != 7 ||
            gem5_qemu_jit_get_pc(instance) != 0x30c) {
            fprintf(stderr, "hart %u vector execution failed\n", instance);
            return 1;
        }

        /* m5 exit followed by WFI: env.bins must not repeat the m5 op. */
        memcpy(memory + 0x200, exits, sizeof(exits));
        gem5_qemu_jit_invalidate_translations(instance);
        gem5_qemu_jit_set_pc(instance, 0x200);
        if (run(instance, 4, &results[instance]) ||
            results[instance].reason != GEM5_QEMU_JIT_EXIT_M5OP ||
            results[instance].instructions != 1 ||
            results[instance].m5_function != 0x21) {
            fprintf(stderr, "hart %u m5 exit failed\n", instance);
            return 1;
        }
        gem5_qemu_jit_set_pc(instance, 0x204);
        if (run(instance, 4, &results[instance]) ||
            results[instance].reason != GEM5_QEMU_JIT_EXIT_HALTED ||
            gem5_qemu_jit_get_pc(instance) != 0x208) {
            fprintf(stderr, "hart %u WFI after m5 exit failed\n", instance);
            return 1;
        }
    }

    for (instance = 0; instance < count; ++instance) {
        const uint32_t lrsc[] = {
            0x1002a3af, /* lr.w t2,(t0) */
            0x1862ae2f, /* sc.w t3,t1,(t0) */
        };
        memcpy(memory + 0x500, lrsc, sizeof(lrsc));
        gem5_qemu_jit_invalidate_translations(instance);
        gem5_qemu_jit_set_gpr(instance, 5, 0x100);
        gem5_qemu_jit_set_gpr(instance, 6, 0x55);

        for (unsigned mode = 0; mode < 5; ++mode) {
            uint8_t saved[4];
            memcpy(saved, memory + 0x100, sizeof(saved));
            gem5_qemu_jit_set_pc(instance, 0x500);
            if (run(instance, 1, &results[instance]) ||
                results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
                !reserved[instance]) {
                fprintf(stderr, "hart %u LR failed\n", instance);
                return 1;
            }
            if (mode == 1) { /* same-value write */
                memory_write(NULL, 0x100, saved, sizeof(saved));
            } else if (mode == 2) { /* ABA write */
                uint8_t changed[4] = { saved[0] ^ 1, 0, 0, 0 };
                memory_write(NULL, 0x100, changed, sizeof(changed));
                memory_write(NULL, 0x100, saved, sizeof(saved));
            } else if (mode == 3) { /* another word in the 64-byte granule */
                memory_write(NULL, 0x104, saved, sizeof(saved));
            } else if (mode == 4) { /* unrelated reservation granule */
                memory_write(NULL, 0x180, saved, sizeof(saved));
            }
            if (run(instance, 1, &results[instance]) ||
                results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
                gem5_qemu_jit_get_gpr(instance, 28) !=
                    (mode > 0 && mode < 4) || reserved[instance]) {
                fprintf(stderr, "hart %u SC mode %u failed\n", instance, mode);
                return 1;
            }
            /* An SC consumes the reservation even when it fails. */
            gem5_qemu_jit_set_pc(instance, 0x504);
            if (run(instance, 1, &results[instance]) ||
                gem5_qemu_jit_get_gpr(instance, 28) != 1) {
                fprintf(stderr, "hart %u repeated SC succeeded\n", instance);
                return 1;
            }
        }
    }

    for (instance = 0; instance < count; ++instance) {
        const uint32_t timer[] = {
            0x00000013, 0x00000013, 0x00000013, /* three nops */
            0xc0102373, /* csrr t1,time */
        };
        memcpy(memory + 0x600, timer, sizeof(timer));
        gem5_qemu_jit_invalidate_translations(instance);
        gem5_qemu_jit_set_pc(instance, 0x600);
        if (run(instance, 4, &results[instance]) ||
            results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            gem5_qemu_jit_get_gpr(instance, 6) != 1000004 ||
            gem5_qemu_jit_executed(instance) != 0) {
            fprintf(stderr, "hart %u callback time failed\n", instance);
            return 1;
        }
    }

    /* instret is per hart, even though TCG's scheduling icount is global. */
    for (instance = 0; instance < count; ++instance) {
        if (gem5_qemu_jit_set_csr(instance, 0x320, 0) ||
            gem5_qemu_jit_set_csr(instance, 0xb02, 0)) {
            fprintf(stderr, "hart %u counter setup failed\n", instance);
            return 1;
        }
    }
    memcpy(memory + 0x700, program, sizeof(program));
    gem5_qemu_jit_invalidate_translations(0);
    gem5_qemu_jit_set_pc(0, 0x700);
    if (run(0, 4, &results[0]) ||
        results[0].instructions != 4) {
        return 1;
    }
    for (instance = 0; instance < count; ++instance) {
        uint64_t retired;
        if (gem5_qemu_jit_get_csr(instance, 0xb02, &retired) ||
            retired != (instance == 0 ? 4 : 0)) {
            fprintf(stderr, "hart %u instret isolation failed: %llu\n",
                    instance, (unsigned long long)retired);
            return 1;
        }
    }

    /* Reads exclude themselves; a counter write replaces its increment. */
    {
        const uint32_t counters[] = {
            0xb02022f3, 0x00000013, 0xb0202373, /* read, nop, read */
            0xb0201073, 0xb02023f3, /* write zero, read */
        };
        uint64_t retired;
        memcpy(memory + 0x780, counters, sizeof(counters));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0x780);
        if (gem5_qemu_jit_set_csr(0, 0xb02, 0) ||
            run(0, 5, &results[0]) ||
            gem5_qemu_jit_get_gpr(0, 5) != 0 ||
            gem5_qemu_jit_get_gpr(0, 6) != 2 ||
            gem5_qemu_jit_get_gpr(0, 7) != 0 ||
            gem5_qemu_jit_get_csr(0, 0xb02, &retired) || retired != 1) {
            fprintf(stderr, "instret CSR read/write boundary failed\n");
            return 1;
        }
    }

    /* A synchronous exception does not retire the faulting instruction. */
    {
        const uint32_t fault[] = { 0xb02022f3, 0x00000000 };
        const uint32_t handler = 0xb0202373;
        memcpy(memory + 0x800, fault, sizeof(fault));
        memcpy(memory + 0x820, &handler, sizeof(handler));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0x800);
        if (gem5_qemu_jit_set_csr(0, 0x305, 0x820) ||
            run(0, 3, &results[0]) ||
            results[0].retired != 2 ||
            gem5_qemu_jit_get_gpr(0, 6) - gem5_qemu_jit_get_gpr(0, 5) != 1) {
            fprintf(stderr, "faulting instruction retired: %llu -> %llu\n",
                    (unsigned long long)gem5_qemu_jit_get_gpr(0, 5),
                    (unsigned long long)gem5_qemu_jit_get_gpr(0, 6));
            return 1;
        }
    }

    /*
     * Unlike a decoded illegal opcode, a CSR helper fault restores icount
     * to before the faulting instruction. It still costs one host cycle.
     */
    {
        const uint32_t fault[] = { 0xb02022f3, 0xfff023f3 };
        memcpy(memory + 0x840, fault, sizeof(fault));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0x840);
        if (run(0, 3, &results[0]) || results[0].instructions != 2 ||
            results[0].retired != 1 ||
            gem5_qemu_jit_get_pc(0) != 0x820 ||
            run(0, 1, &results[0]) || results[0].retired != 1 ||
            gem5_qemu_jit_get_gpr(0, 6) - gem5_qemu_jit_get_gpr(0, 5) != 1) {
            fprintf(stderr, "CSR helper fault accounting failed\n");
            return 1;
        }
    }

    /* Cycles follow host time even when no hart executes. */
    for (instance = 0; instance < count; ++instance) {
        if (gem5_qemu_jit_set_csr(instance, 0x320, 0) ||
            gem5_qemu_jit_set_csr(instance, 0xb00, 0)) {
            return 1;
        }
    }
    host_cycles += 123;
    for (instance = 0; instance < count; ++instance) {
        uint64_t cycles;
        if (gem5_qemu_jit_get_csr(instance, 0xb00, &cycles) || cycles != 123) {
            fprintf(stderr, "hart %u did not follow host cycles\n", instance);
            return 1;
        }
    }
    if (gem5_qemu_jit_set_csr(count - 1, 0x320, 1)) {
        return 1;
    }
    host_cycles += 7;
    for (instance = 0; instance < count; ++instance) {
        uint64_t cycles;
        uint64_t expected = instance == count - 1 ? 123 : 130;
        if (gem5_qemu_jit_get_csr(instance, 0xb00, &cycles) ||
            cycles != expected) {
            fprintf(stderr, "hart %u cycle inhibition failed\n", instance);
            return 1;
        }
    }
    {
        const uint32_t cycles[] = { 0xb00022f3, 0x00000013, 0xb0002373 };
        uint64_t value;
        memcpy(memory + 0x900, cycles, sizeof(cycles));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0x900);
        if (gem5_qemu_jit_set_csr(0, 0x320, 0) ||
            gem5_qemu_jit_set_csr(0, 0xb00, 20) ||
            run(0, 3, &results[0]) ||
            gem5_qemu_jit_get_gpr(0, 5) != 20 ||
            gem5_qemu_jit_get_gpr(0, 6) != 22 ||
            gem5_qemu_jit_get_csr(0, 0xb00, &value) || value != 23) {
            fprintf(stderr, "cycle callback instruction timing failed\n");
            return 1;
        }
    }

    /* Host-cycle overflow must fire even with every hart idle. */
    if (gem5_qemu_jit_set_csr(0, 0x323, 1) ||
        gem5_qemu_jit_set_csr(0, 0xb03, UINT64_MAX - 2) ||
        check_pmu(3, UINT64_MAX, false)) {
        return 1;
    }
    host_cycles += 2;
    if (check_pmu(1, UINT64_MAX, false)) {
        return 1;
    }
    ++host_cycles;
    if (check_pmu(UINT64_MAX, UINT64_MAX, true)) {
        return 1;
    }
    gem5_qemu_jit_set_mip(0, 0);
    if (!(gem5_qemu_jit_get_mip(0) & (1u << 13))) {
        fprintf(stderr, "device synchronization lost PMU interrupt\n");
        return 1;
    }
    /* Mode filtering must suppress counting without changing the value. */
    if (gem5_qemu_jit_set_csr(0, 0x344, 0) ||
        gem5_qemu_jit_set_csr(0, 0x323, (UINT64_C(1) << 62) | 1) ||
        gem5_qemu_jit_set_csr(0, 0xb03, UINT64_MAX - 2) ||
        check_pmu(UINT64_MAX, UINT64_MAX, false)) {
        return 1;
    }
    host_cycles += 100;
    if (check_pmu(UINT64_MAX, UINT64_MAX, false) ||
        gem5_qemu_jit_set_csr(0, 0x323, 1) ||
        check_pmu(3, UINT64_MAX, false)) {
        return 1;
    }
    /*
     * Re-select instruction counting. Host time and peer execution cannot
     * bring an instruction overflow closer on the idle owning hart.
     */
    if (gem5_qemu_jit_set_csr(0, 0x323, 2) ||
        gem5_qemu_jit_set_csr(0, 0xb03, UINT64_MAX - 2) ||
        check_pmu(UINT64_MAX, 3, false)) {
        return 1;
    }
    {
        const uint32_t nops[] = { 0x13, 0x13, 0x13, 0x13 };
        memcpy(memory + 0x980, nops, sizeof(nops));
        for (instance = 0; instance < count; ++instance) {
            gem5_qemu_jit_invalidate_translations(instance);
            gem5_qemu_jit_set_pc(instance, 0x980);
        }
        if (count > 1 && run(1, 2, &results[1])) {
            return 1;
        }
        host_cycles += 100;
        if (check_pmu(UINT64_MAX, 3, false) || run(0, 2, &results[0]) ||
            check_pmu(UINT64_MAX, 1, false) || run(0, 1, &results[0]) ||
            check_pmu(UINT64_MAX, UINT64_MAX, true)) {
            return 1;
        }
    }
    /* Two programmable counters can select the same event independently. */
    if (gem5_qemu_jit_set_csr(0, 0x344, 0) ||
        gem5_qemu_jit_set_csr(0, 0x323, 1) ||
        gem5_qemu_jit_set_csr(0, 0x324, 1) ||
        gem5_qemu_jit_set_csr(0, 0xb03, UINT64_MAX - 2) ||
        gem5_qemu_jit_set_csr(0, 0xb04, UINT64_MAX - 4) ||
        check_pmu(3, UINT64_MAX, false)) {
        return 1;
    }
    host_cycles += 3;
    if (check_pmu(2, UINT64_MAX, true) ||
        gem5_qemu_jit_set_csr(0, 0x344, 0)) {
        return 1;
    }
    host_cycles += 2;
    if (check_pmu(UINT64_MAX, UINT64_MAX, true)) {
        return 1;
    }

    /* Count a cold data-TLB fill, including its overflow interrupt. */
    {
        const uint32_t load[] = { 0x0000b283, 0x00000013 };
        memcpy(memory + 0x9c0, load, sizeof(load));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0x9c0);
        gem5_qemu_jit_set_gpr(0, 1, 0x1000);
        if (gem5_qemu_jit_set_csr(0, 0x344, 0) ||
            gem5_qemu_jit_set_csr(0, 0x304, 1u << 13) ||
            gem5_qemu_jit_set_csr(0, 0x324, 0) ||
            gem5_qemu_jit_set_csr(0, 0x323, 0x10019) ||
            gem5_qemu_jit_set_csr(0, 0xb03, UINT64_MAX) ||
            run(0, 2, &results[0]) ||
            check_pmu(UINT64_MAX, UINT64_MAX, true)) {
            fprintf(stderr, "TLB event overflow failed\n");
            return 1;
        }
    }

    /*
     * Install an event inside already translated code, stop before it,
     * resume exactly one instruction, and trigger again on revisiting it.
     */
    {
        const uint32_t code[] = { 0x00128293, 0x00128293, 0x00128293 };
        const uint64_t event_pc = 0xa44;
        memcpy(memory + 0xa40, code, sizeof(code));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0xa40);
        if (run(0, 3, &results[0])) {
            return 1;
        }
        gem5_qemu_jit_set_pc(0, 0xa40);
        gem5_qemu_jit_set_gpr(0, 5, 0);
        if (gem5_qemu_jit_set_pc_events(0, &event_pc, 1) ||
            run(0, 8, &results[0]) ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_PC_EVENT ||
            results[0].instructions != 1 || results[0].retired != 1 ||
            gem5_qemu_jit_get_pc(0) != event_pc ||
            gem5_qemu_jit_get_gpr(0, 5) != 1 ||
            run(0, 8, &results[0]) || results[0].instructions != 1 ||
            gem5_qemu_jit_get_gpr(0, 5) != 2 ||
            run(0, 1, &results[0]) ||
            gem5_qemu_jit_get_gpr(0, 5) != 3) {
            fprintf(stderr, "PC event boundary/resume failed\n");
            return 1;
        }
        gem5_qemu_jit_set_pc(0, event_pc);
        if (run(0, 8, &results[0]) || results[0].instructions != 0 ||
            results[0].retired != 0 ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_PC_EVENT ||
            gem5_qemu_jit_set_pc_events(0, NULL, 0) ||
            run(0, 1, &results[0]) ||
            gem5_qemu_jit_get_gpr(0, 5) != 4) {
            fprintf(stderr, "PC event revisit/removal failed\n");
            return 1;
        }
    }

    /*
     * A handler can redirect through another event before resuming. The
     * final destination must execute once without duplicate delivery.
     */
    {
        const uint64_t event_pcs[] = { 0xa40, 0xa48 };
        gem5_qemu_jit_set_pc(0, 0xa40);
        gem5_qemu_jit_set_gpr(0, 5, 0);
        if (gem5_qemu_jit_pc_event_serviced(0) != -1 ||
            gem5_qemu_jit_set_pc_events(0, event_pcs, 2) ||
            run(0, 8, &results[0]) ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_PC_EVENT ||
            results[0].instructions != 0) {
            return 1;
        }
        gem5_qemu_jit_set_pc(0, 0xa48);
        if (gem5_qemu_jit_pc_event_serviced(0) ||
            run(0, 8, &results[0]) || results[0].instructions != 1 ||
            results[0].retired != 1 ||
            gem5_qemu_jit_get_gpr(0, 5) != 1 ||
            gem5_qemu_jit_get_pc(0) != 0xa4c) {
            fprintf(stderr, "redirected PC event delivered twice\n");
            return 1;
        }
        gem5_qemu_jit_set_pc(0, 0xa48);
        if (run(0, 8, &results[0]) || results[0].instructions != 0 ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_PC_EVENT ||
            gem5_qemu_jit_set_pc_events(0, NULL, 0)) {
            fprintf(stderr, "redirected PC event lost on revisit\n");
            return 1;
        }
    }

    /*
     * Host/device writes bypass QEMU's store callbacks. Guest FENCE.I must
     * make such writes visible even when the old code is already translated.
     */
    {
        const uint32_t old_code = 0x01100293;
        const uint32_t new_code = 0x01d00293;
        const uint32_t fence_i = 0x0000100f;
        memcpy(memory + 0xb00, &old_code, sizeof(old_code));
        memcpy(memory + 0xb40, &fence_i, sizeof(fence_i));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0xb00);
        if (run(0, 1, &results[0]) ||
            gem5_qemu_jit_get_gpr(0, 5) != 17) {
            return 1;
        }
        memcpy(memory + 0xb00, &new_code, sizeof(new_code));
        gem5_qemu_jit_set_pc(0, 0xb40);
        if (run(0, 1, &results[0]) || results[0].instructions != 1 ||
            results[0].retired != 1) {
            return 1;
        }
        gem5_qemu_jit_set_pc(0, 0xb00);
        if (run(0, 1, &results[0]) ||
            gem5_qemu_jit_get_gpr(0, 5) != 29) {
            fprintf(stderr, "FENCE.I missed external code publication\n");
            return 1;
        }
    }

    /*
     * A non-returning WFI helper must end translation as well as execution:
     * trailing instructions cannot be charged to icount or instret.
     */
    {
        const uint32_t sleep[] = { 0x10500073, 0x00100293, 0x0000006f };
        memcpy(memory + 0xa00, sleep, sizeof(sleep));
        gem5_qemu_jit_invalidate_translations(0);
        gem5_qemu_jit_set_pc(0, 0xa00);
        gem5_qemu_jit_set_gpr(0, 5, 123);
        if (gem5_qemu_jit_set_csr(0, 0x304, 0) ||
            run(0, 16, &results[0]) || results[0].instructions != 1 ||
            results[0].retired != 1 ||
            results[0].reason != GEM5_QEMU_JIT_EXIT_HALTED ||
            gem5_qemu_jit_get_gpr(0, 5) != 123 ||
            gem5_qemu_jit_get_pc(0) != 0xa04) {
            fprintf(stderr, "WFI charged unexecuted instructions\n");
            return 1;
        }
    }

    if (sigprocmask(SIG_SETMASK, NULL, &signals_after)) {
        return 1;
    }
    for (int sig = 1; sig < NSIG; ++sig) {
        if (sigismember(&signals_before, sig) !=
            sigismember(&signals_after, sig)) {
            fprintf(stderr, "adapter changed host signal mask: %d\n", sig);
            return 1;
        }
    }
    raise(SIGTERM);
    if (term_seen != SIGTERM) {
        fprintf(stderr, "adapter replaced host termination handler\n");
        return 1;
    }

    printf("gem5 QEMU JIT %u-hart smoke test passed\n", count);
    return 0;
}
