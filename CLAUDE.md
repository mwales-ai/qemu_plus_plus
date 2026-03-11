# CLAUDE.md - QEMU++ Project Guide

## Project Overview

This is QEMU++ (qemu_plus_plus): an incremental port of QEMU from C to C++17.
Branch `cpp-port` is based on QEMU v10.2.1 (stable release).
GitHub: github.com/mwales-ai/qemu_plus_plus

## Repository Layout

```
qemu/
  target/           # ISA-specific CPU emulation (i386, arm, ppc, riscv, etc.)
  hw/               # Hardware device models (per-arch machines + shared devices)
  softmmu/          # System emulation core
  include/          # All headers
    qom/            # QEMU Object Model (key porting target)
    qemu/           # Core QEMU utilities
    hw/             # Hardware model headers
  util/             # Utility functions
  block/            # Block device layer
  net/              # Networking
  migration/        # Live migration
  qobject/          # QObject serialization (JSON, etc.)
  tests/
    functional/     # Full machine boot tests (Python)
    tcg/            # TCG instruction tests (per-arch)
    unit/           # Unit tests
    cpp-port/       # Our custom test artifacts
  scripts/
    cpp-port/       # Our build/test/install scripts
  roms/             # Firmware submodules (seabios, edk2, opensbi, etc.)
  meson.build       # Build system (Meson + Ninja)
  configure         # Configuration wrapper
```

## Target ISAs

We are porting these 5 ISAs first:
- **x86_64**: target/i386/, hw/i386/  (machine: q35)
- **aarch64**: target/arm/, hw/arm/   (machine: virt)
- **arm**: target/arm/, hw/arm/       (machine: virt, shares code with aarch64)
- **ppc64**: target/ppc/, hw/ppc/     (machine: pseries)
- **riscv64**: target/riscv/, hw/riscv/ (machine: virt)

## Build Commands

```bash
# Install dependencies (Ubuntu 24.04)
sudo ./scripts/cpp-port/install-deps-ubuntu.sh

# Build QEMU for our 5 targets
./scripts/cpp-port/build-qemu.sh

# Or manually:
mkdir build && cd build
../configure --target-list=x86_64-softmmu,aarch64-softmmu,arm-softmmu,ppc64-softmmu,riscv64-softmmu
make -j$(nproc)
```

## Testing

```bash
# Download test images
./scripts/cpp-port/download-test-images.sh

# Run smoke tests
./scripts/cpp-port/run-smoke-tests.sh

# Run QEMU's built-in tests
cd build && make check
```

## C++ Coding Style

- **C++17 standard** - no newer features
- **"C with classes"** style: explicit types, simple inheritance, STL containers
- Keep `printf`/`qemu_log` for output; do not use C++ streams
- Do not use `auto` for anything other than iterator declarations or obviously
  long template types. Prefer explicit types.
- Do not use heavy template metaprogramming
- Do not use exceptions; QEMU uses error propagation via Error** parameters
- Use `static_cast<>` instead of C-style casts
- Use `enum class` instead of plain enums
- Use RAII for resource management where it simplifies cleanup
- STL containers are encouraged: `std::vector`, `std::string`, `std::map`,
  `std::unordered_map`, `std::array`
- Use `extern "C" { }` blocks when including C headers from C++ files
- Keep variable names from the original C code unless they conflict with
  C++ reserved words

## File Porting Checklist

When converting a `.c` file to `.cpp`:
1. `git mv file.c file.cpp`
2. Add `extern "C"` wrappers around C header includes
3. Fix implicit void* casts (add `static_cast`)
4. Rename variables that clash with C++ keywords (new, class, template, etc.)
5. Fix designated initializer ordering if needed
6. Replace compound literals with brace initialization
7. Update the file reference in `meson.build`
8. Build and run smoke tests

## Key Files for QOM Understanding

- `include/qom/object.h` - Object model core (TypeInfo, ObjectClass, Object)
- `qom/object.c` - Object model implementation
- `include/hw/qdev-core.h` - Device model base
- `include/hw/qdev-properties.h` - Device properties
- `hw/core/qdev.c` - Device model implementation

## Common Pitfalls

- GLib functions (g_malloc, g_free, g_strdup, etc.) need `extern "C"` context
- QEMU macros like OBJECT_CHECK, DECLARE_INSTANCE_CHECKER use typeof and casts
  that may not compile directly in C++
- The TCG (Tiny Code Generator) in tcg/ uses heavy macros and may be better
  left as C with extern "C" wrappers
- Coroutine code (util/coroutine-*) uses setjmp/longjmp; keep as C
- QEMU's coding style uses snake_case; keep that convention

## Git Workflow

- All work on the `cpp-port` branch
- One logical change per commit
- Run smoke tests before committing
- Reference the PLANNING.md phase in commit messages
