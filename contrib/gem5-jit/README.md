# gem5 QEMU JIT backend

This directory contains the narrow C adapter, public ABI, and standalone smoke
test used by gem5's `RiscvJitCPU`. Together with the RISC-V translation hook
and Meson target in the `gem5-jit` branch, it turns QEMU's RISC-V TCG engine
into a dynamically loaded execution backend.

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
