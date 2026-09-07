# gem5 QEMU JIT backend

This directory contains the narrow C adapter, public ABI, and standalone smoke
test used by gem5's `RiscvJitCPU`. Together with the RISC-V translation hook
and Meson target in the `gem5-jit` branch, it turns QEMU's RISC-V TCG engine
into a dynamically loaded execution backend.

The version-1 programmable HPM snapshot API transfers the implemented bank,
counter samples, raw event/filter/OF state and HPM inhibition bits. Restoration
requires a stopped RV64 M-mode hart and validates the full snapshot before
mutation. It rebases backend sources and reconstructs timer scheduling without
guest CSR-write side effects; fixed cycle/instret, their inhibition bits,
counter-access gates and LCOFIP remain separate state owners. The profile smoke
checks two-hart frozen-bank round trips, untouched pending/fixed inhibition,
and atomic rejection of malformed snapshots. gem5 integration and active
counter continuity are not yet qualified. Duplicate nonzero event selectors
are rejected because the current QEMU event map selects only one counter per
event; supporting multiple counters per event remains required work.

The separate version-1 fixed-counter snapshot carries cycle/instret samples,
CY/IR inhibition and optional filter controls. Its trusted setter rebases
sources without executing guest writes and leaves HPM/interrupt state alone.
Two-hart frozen round trips and rejection atomicity are covered by the profile
smoke. The gem5 integration restores fixed samples at startup/takeover and
imports samples after batch retirement accounting; active continuity needs
the corresponding gem5 checkpoint and handoff tests, not just this smoke.

RV64 retired-instruction CSR reads now return the pre-retirement sample:
guest read helpers carry a return address through the CSR layer, while host
reads retain the unadjusted source sample. Guest writes no longer need the
old extra RV64 baseline increment, and xRET no longer receives the old RV64
mode correction. ECALL/EBREAK are removed from the PMU instruction source
before trap-mode accounting. Other fault classes and multicore source isolation
still require qualification. These source changes do not redefine the run
result's raw TCG execution count as a fully qualified retired-instruction count.

The adapter supports multiple gem5 JitCPU objects in one process. The first
adapter initialization fixes the instance count and creates one QEMU RISC-V
vCPU per instance. Each vCPU receives a unique instance ID, gem5 hart ID, and
callback set. The vCPUs keep independent architectural and transient state
while sharing one QEMU physical address space and executing serially through
`tcg,thread=single`.

The embedded CPU deliberately disables SSTC. QEMU's SSTC implementation uses
its own asynchronous virtual timer, whereas gem5 owns simulated time and the
CLINT interrupt path. Linux therefore uses OpenSBI's timer service, which
keeps timer interrupts synchronized with gem5 before and after a CPU switch.

The register API explicitly transfers FFLAGS and FRM even when
`mstatus.FS=Off`; ordinary architectural CSR helpers reject that valid switch
state. Translation invalidation flushes both the vCPU TLB and the shared TCG
translation-block cache before reverse takeover resumes.

The additive version-1 vector migration-state API transfers all 32 registers
as little-endian bytes at configured VLENB, plus VL, VSTART, VTYPE, VILL,
VXRM and VXSAT. It preserves dormant state even with V/VS disabled. Import
requires an exact version/size/VLEN match, valid small control fields and
zero unused register bytes; rejection makes no state changes. The caller
must be between runs and invalidate translations after restoration. This
is an internal migration interface, not a validator for untrusted snapshots.
The smoke test exercises two independent harts with VS Off, different
register/control patterns and rejected malformed inputs. Vector execution,
profile selection and gem5 consumption of this API are separate work; the
default embedded CPU remains scalar.

The version-1 configuration initializer can alternatively select QEMU's
`rva23s64` CPU and VLENB=16/32/64/128. Profile and width are process-wide;
mixed requests are rejected before registering another hart. The original
initializer continues to select the unchanged legacy configuration. New
harts receive unique internal IDs before realization because QEMU keys its
implied-extension initialization by hart ID.

`gem5-qemu-jit-smoke --rva23-vlenb 32` tests two configured harts, invalid
and mixed configurations, dormant-state round trips, and vector execution
after importing VSTART=3. The execution oracle checks preserved leading
lanes, resumed VID, VSTART clearing, VADD and a vector store. The same test
runs with VLENB 16, 64 and 128. This standalone profile path is not yet wired
into gem5: full H/VS state transfer and gem5-owned SSTC deadlines remain
required before full-profile CPU switching or Linux use is qualified.

The mode migration API explicitly reads/writes privilege and virtualization.
Version-1 state-enable snapshots contain raw M/H/S banks and the implemented
bit masks. Unlike architectural H/S CSR reads, snapshots retain state hidden
by ancestor gates. Imports check version, size, all masks and reserved bits
before any mutation. The mask definitions are shared with QEMU's architectural
CSR handlers. The two-hart smoke checks hidden-state round trips, isolation,
and rejection without partial updates. A consumer must compare masks before
claiming feature compatibility; it must not silently discard unknown state.

Version-1 `restore_mstatus` is a trusted host operation, separate from guest
CSR writes. It restores MPV/GVA trap fields as well as normally writable
status fields, retaining WARL legalization. Call it only with M mode and V=0;
invalid versions, modes, harts, or H-only state without H are rejected before
mutation. A changed trap field invalidates the local software TLB. The smoke
checks all four MPV/GVA patterns on both harts, rejected versions and modes,
and ordinary guest CSR writes of those trap fields, needed for nested M-mode
trap-frame restoration.

The embedded RV64 backend fixes UXL to 64 bits in both MSTATUS/SSTATUS and
VSSTATUS, matching gem5's U/VU execution widths. Optional RV32 user execution
is not exposed: allowing it only inside QEMU would lose state at batch and
CPU-switch boundaries. This uses the existing gem5 embedding flag and does
not change standalone QEMU's WARL choices.

Changing V swaps QEMU's HS/VS banks before updating execution flags, following
its debugger restore path. It does not emulate trap entry or xRET. Invalid
privilege 2, nonboolean V, virtual M mode, missing H, and invalid hart IDs
are rejected before state changes. Legacy `set_priv` selects V=0 through
this same path. The profile smoke tests S/VS status, trap vector, scratch,
EPC, cause, trap value and SATP banks, guest updates across VS/VU transitions,
and legacy M-mode borrowing; invalid requests must preserve the active mode.
Host mode setters preserve LR address/value state: borrowing privilege for
CSR synchronization is not a guest trap or xRET. Explicit translation
invalidation at takeover still clears the reservation. The two-hart smoke
splits LR and SC across execution calls and host mode changes (including
VS/VU in profile mode), checks successful SC without interference, and checks
failed SC after explicit invalidation or a different-value store by the other
hart. This does not qualify all reservation-granule or eventual-progress rules.

The version-1 `gem5_qemu_jit_set_wrs_exit_mode` API enables host-owned WRS
handling per stopped hart. It is opt-in so existing callers retain QEMU's
immediate-return policy. Enabled NTO/STO instructions produce distinct run
exit reasons, retire exactly once, leave PC at the successor, and preserve
the reservation token. QEMU still applies its NTO privilege interception
before exiting; the host must decide whether to wait, own the STO timer and
wakeups, and prevent further guest execution while waiting. This setting is
host policy, not a guest CSR or migrated wait state. The two-hart smoke checks
both instructions with live/empty reservations, one-instruction and larger
budgets, malformed mode requests, subsequent SC and opting back out. The ABI
does not by itself implement gem5 waiting or cross-model monitor transfer.

The version-2 reservation snapshot API exports/imports a stopped RV64 hart's
virtual address, expected value, physical LR address, and access width (4/8).
Version 1 is rejected because it lacks physical monitor identity. Import validates all
fields before mutation; an invalid token must have zero payload fields. It
does not access memory, raise interrupts, or perform a guest operation. Import
must follow any translation invalidation and use the matching memory and
translation snapshot. The smoke checks restoration after invalidation,
explicit clearing, ten malformed-token rejection cases, hart isolation, and
same-value peer writes after restoring a token. O3 monitor reconstruction
requires further integration. Neither full JIT checkpoint restoration nor cross-model LR/SC
preservation is established by the backend snapshot test.

Physical monitor work in progress: embedded LR records its translated physical
address and width; ordinary scalar stores, AMOs and successful SC operations
notify matching 64-byte physical reservation blocks, including direct-mapped
RAM writes. Non-embedded QEMU translation does not emit these hooks. This
repairs the scalar same-value peer-store continuation probe. Physical metadata
is now included in the version-2 migration token for cold restoration.
FSH/FSW/FSD (and compressed stores delegating to these translators) use the
same post-store notification generator as scalar stores and AMOs. Directed
same-value peer FP stores pass JIT/O3 checkpoint continuation and cold restore
for 16/32/64-bit widths; this does not establish all alias/fault cases.
Vector stores notify after each completed slow-path element and after each
completed direct-memory page chunk, including the memcpy path. Fully masked
stores issue no notification. Unit-stride, strided, indexed, and fully masked
peer probes pass JIT/O3 checkpoint continuation and cold restore. Partial
masks, fault/restart boundaries, translated aliases and all segmented/whole
register variants still need directed qualification. Other helper, CMO and device
write paths and general alias tests remain open.

CBO.ZERO notifies after direct RAM memset and after each completed byte of
its I/O fallback. A same-value peer zeroing probe (initial block all zeros)
passes continuation and cold restore on JIT and O3. The I/O fallback's
partial-fault behavior is implemented but not yet directly qualified; general
external/device writes still require host-side invalidation integration.

The version-1 physical-write notification API is the host integration entry
point: after a completed external write and before resuming any hart, submit
its physical byte range with all harts stopped. It changes monitors only,
using the same overlap policy as guest writes. Empty/wrapping ranges, bad
versions/sizes, and calls during execution are rejected before mutation.
Smokes cover same-value external write followed by failed SC, boundary-crossing
ranges, unaffected-hart preservation, and malformed-event rejection atomicity.
gem5 device/DMA callbacks are not yet connected: the API alone does not qualify
platform device coherence or writes arriving during a backend run.

SC revalidates its translated physical address and access width against the
original LR metadata before attempting the comparison/store. The model
permits success only for an exact address/width match; mismatch takes the
ordinary failed-SC path, including its permission probe. Backend tests cover
imported physical/width mismatch, while the gem5 Sv39 remap fixture checks
same-VA/different-PA SC across checkpoints. Two-stage translation and fault
cases remain unqualified.

Embedded S/VS timers use an external-timer hook instead of QEMU's ACLINT
timebase cast and native timer queue.

The versioned PMP table migration API bypasses architectural lock checks only
for trusted host snapshots. It validates version, size, region count, reserved
fields and unused entries before mutation, installs all raw address/config
entries, recomputes TOR/NAPOT bounds and active-rule counts, and invalidates
translation caches. `mseccfg` is separate state and is not transferred by this
API. RVA23 backend CPUs explicitly enable PMP, including secondary harts:
the profile CPU's defaults otherwise leave PMP CSR accesses unavailable.
The smoke checks two-hart locked-TOR replacement, continued architectural
write rejection, malformed-input rejection without mutation and restoration
of the original table. A 32-case execution matrix covers both harts, S/M
loads and stores, locked TOR allow/deny/allow transitions, and moving the
lower bound past the accessed address into a lower-priority NAPOT region.
It checks access-fault cause, EPC, trap address, load destination and memory
effects without explicit test-side invalidation between permission changes.
Instruction-fetch permissions, other PMP modes and a gem5-side consumer
remain to be qualified.

`gem5_qemu_jit_refresh_timers` samples the host's `read_time`, updates hardware
pending levels and returns versioned
time/compare/offset/gate metadata. The host must schedule its next deadline
and cap execution batches accordingly. Timer-control writes request an early
batch boundary so a newly programmed deadline can be scheduled before more
guest instructions run. This is the backend half of scheduling; gem5 event
integration is still pending. Other QEMU timers such as PMU timers are not
covered by this hook.

The profile smoke tests threshold equality, STCE gates, unsigned time and
offset wrap, software STIP with SSTC disabled, and independent software
HVIP versus hardware VS timer sources. The fork fixes two existing paths
that suppressed HVIP writes while SSTC was active. An executed STIMECMP
write must return after one instruction of a two-instruction budget; the
following instruction executes only on the next run.

The shared library is a default build target whenever `riscv64-softmmu` is
configured. A direct build can use:

```sh
mkdir build
cd build
../configure --target-list=riscv64-softmmu -Db_staticpic=true
ninja
ninja gem5-qemu-jit-smoke
./gem5-qemu-jit-smoke
```

On Ubuntu, the backend's direct build dependencies are:

```sh
sudo apt install \
  build-essential git tar meson ninja-build flex bison pkg-config \
  python3-dev libglib2.0-dev libpixman-1-dev libfdt-dev libffi-dev
```

On Apple Silicon macOS with Homebrew, install:

```sh
brew install \
  meson ninja pkgconf glib pixman dtc \
  riscv-gnu-toolchain isl libmpc mpfr
```

The gem5 repository pins a compatible commit of this branch as a submodule.
Its helper builds a source snapshot and runs the smoke test:

```sh
git submodule update --init --depth 1 ext/qemu/repo
util/jitcpu/build-qemu-jit.sh
```

The helper also runs `gem5-qemu-jit-smoke`. It initializes two harts with
different `mhartid` values and checks independent PC/GPR state, execution,
FFLAGS/FRM transfer while FS is Off, and translation invalidation. The final
backend path ends in `.so` on Linux and `.dylib` on macOS.

The public `qemu-jit.h` ABI header is dual-licensed under BSD-3-Clause or
GPL-2.0-or-later. The adapter implementation, smoke test, and QEMU integration
changes are GPL-2.0-or-later. Dynamic loading is an engineering boundary, not a
guarantee that distributing gem5 with the QEMU-derived backend avoids GPL
obligations.
