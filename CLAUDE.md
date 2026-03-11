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

## Build System Notes

- Meson project declares both `['c', 'cpp']` languages
- `cpp_std=gnu++17` set in project defaults
- Meson auto-detects file language by extension: `.c` -> C, `.cpp` -> C++
- Source files are added via `files()` into source sets (e.g., `util_ss`, `system_ss`)
- C++ flags inherit from C flags + extra macros (`__STDC_LIMIT_MACROS`, etc.)
- Warning flags applied per-language via `get_supported_arguments()` (C-only
  warnings like `-Wmissing-prototypes` are silently skipped for C++)
- Mixed C/C++ linking is handled automatically by meson

## C/C++ Compatibility Header

`include/qemu/cpp_compat.h` provides:
- `QEMU_EXTERN_C_BEGIN` / `QEMU_EXTERN_C_END` - wrap C header includes in .cpp files
- `QEMU_EXTERN_C` - mark a single function as extern "C"
- `QEMU_CAST(type, expr)` - static_cast in C++, C-style cast in C

## Header Include Pattern for .cpp Files

CRITICAL: `qemu/osdep.h` MUST be included first, OUTSIDE any `extern "C"` block.
It pulls in GLib and system headers that are already C++-aware.

```cpp
// CORRECT pattern:
#include "qemu/osdep.h"       // Always first, never inside extern "C"

extern "C" {
#include "qemu/id.h"          // Simple QEMU headers go inside extern "C"
#include "qapi/error.h"
}

#include <string>              // C++ headers after everything else
```

Headers that pull in deep dependency chains (monitor.h, block/block.h, etc.)
may NOT work inside `extern "C"` because they transitively include C++
standard library headers via GLib. These headers should be included outside
`extern "C"` (they're already C++-safe via osdep.h's `extern "C"` block).

## Known C++ Header Blockers

Issues in shared headers that must be fixed before files using them can port:
- **QAPI generated headers**: Use `export` as a struct member name (C++ keyword)
  - File: `build/qapi/qapi-types-block-core.h` line 3068
  - Fix: Modify QAPI code generator to rename `export` in C++ context
- ~~**`include/qemu/lockable.h`**: Implicit void* casts~~ **FIXED** (static_cast in #ifdef __cplusplus)
- ~~**`include/qemu/cutils.h`**: Uses `restrict` keyword~~ **FIXED** (changed to `__restrict__`)
- ~~**`include/qemu/compiler.h`**: `typeof_strip_qual` C-only~~ **FIXED** (C++ version using std::remove_cv_t)
- **Trace headers**: Generated trace format strings use `"%"PRId64` (no space),
  which triggers `-Werror=literal-suffix` in C++. Blocks porting any file that
  includes `trace.h`. Fix: modify trace code generator or add `-Wno-literal-suffix`.
- **VMState compound literals**: Files using `VMStateField[]` compound literals
  (e.g., fifo8.c) can't port directly - compound literals aren't valid in C++.
  Fix: use static const arrays or brace initialization.

## File Porting Checklist

When converting a `.c` file to `.cpp`:
1. `git mv file.c file.cpp`
2. Include `qemu/osdep.h` first (outside extern "C")
3. Wrap simple QEMU C headers with `extern "C" { }`
4. For headers with deep deps (monitor.h, block.h), include outside extern "C"
5. Add `extern "C"` to function definitions called from C code
6. Fix implicit void* casts (add `static_cast`)
7. Rename variables that clash with C++ keywords (new, class, template, etc.)
8. Fix designated initializer ordering if needed
9. Replace compound literals with brace initialization
10. Update the file reference in `meson.build`
11. Build and run smoke tests

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
