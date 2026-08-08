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
