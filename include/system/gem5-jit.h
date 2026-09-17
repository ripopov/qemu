/*
 * gem5 RiscvJitCPU hooks into target-independent TCG code
 *
 * Copyright (c) 2026 Roman Popov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_GEM5_JIT_H
#define SYSTEM_GEM5_JIT_H

#include "exec/cpu-common.h"

/*
 * Called from the not-dirty store slow path for every store to a RAM page
 * that is clean for some dirty-memory client, before the store is
 * performed. Returning true keeps the page clean, so the next store to it
 * takes the slow path again. NULL, and hence never called, unless the gem5
 * adapter is loaded.
 */
extern bool (*gem5_jit_store_hook)(ram_addr_t ram_addr, unsigned size);

#endif
