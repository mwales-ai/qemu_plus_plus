# QEMU++ Porting Plan: C to C++17

## Overview

This document describes the plan for incrementally porting QEMU from C to C++17.
The goal is to leverage C++ classes, the STL, and straightforward OO patterns to
replace the hand-rolled QOM (QEMU Object Model) and simplify the codebase, while
keeping the code readable and avoiding heavy template metaprogramming.

## Branch Strategy

- **Base**: `v10.2.1` (latest stable QEMU release)
- **Branch**: `cpp-port`
- **Approach**: Incremental, file-by-file rename from `.c` to `.cpp` with
  compilation fixes. Keep the tree building at all times.

## Target ISAs (Phase 1)

We focus on 5 architectures to validate the port:

| ISA      | Machine    | Target Dir        | Why                                     |
|----------|------------|-------------------|-----------------------------------------|
| x86_64   | q35        | target/i386/      | Most-used, best test coverage           |
| aarch64  | virt       | target/arm/        | ARM 64-bit, growing importance          |
| arm      | virt       | target/arm/        | ARM 32-bit, shares code with aarch64    |
| ppc64    | pseries    | target/ppc/        | Enterprise, complex ISA, many machines  |
| riscv64  | virt       | target/riscv/      | Clean modern ISA, rising popularity     |

## C++ Style Guidelines

- **Standard**: C++17
- **Style**: "C with classes" - use C++ for what it does well, don't go overboard
- **DO use**:
  - Classes with constructors/destructors for resource management (RAII)
  - STL containers (`std::vector`, `std::map`, `std::string`, `std::unordered_map`)
  - `enum class` instead of C enums
  - References where appropriate
  - Namespaces to replace prefixed naming (e.g., `qemu::` instead of `qemu_`)
  - Simple inheritance for the object model
- **DO NOT use**:
  - `auto` everywhere - use explicit types for readability
  - Heavy template metaprogramming
  - Deep inheritance hierarchies
  - Exceptions (QEMU uses error propagation patterns; keep those)
  - C++ streams for I/O (keep printf/qemu_log patterns)
  - `std::shared_ptr` everywhere - prefer explicit ownership

## Porting Phases

### Phase 0: Infrastructure (Complete)
- [x] Set up branch from v10.2.1
- [x] Create build scripts
- [x] Create dependency installer
- [x] Create test image downloader
- [x] Create smoke test suite
- [x] Verify baseline builds and passes all smoke tests
- [x] Add `.gitignore` entries for test-images/ and build/

### Phase 1: Build System Modifications (In Progress)
- [x] Modify `meson.build` to support compiling `.cpp` files alongside `.c`
  - Changed project() languages from `['c']` to `['c', 'cpp']`
  - Changed `cpp_std` from `gnu++11` to `gnu++17`
  - Enabled C++ compiler on all platforms (was Windows-only)
  - QEMU already had C++ flag infrastructure (qemu_cxxflags, warn_flags)
- [x] C++ compiler detection - meson handles this automatically with `['c', 'cpp']`
- [x] Mixed C/C++ linking - meson handles this automatically
- [x] C++17 standard via `cpp_std=gnu++17` in project defaults
- [x] Create `include/qemu/cpp_compat.h` with `extern "C"` wrapping macros
  - `QEMU_EXTERN_C_BEGIN` / `QEMU_EXTERN_C_END` for wrapping C header includes
  - `QEMU_EXTERN_C` for single function declarations
  - `QEMU_CAST()` for C++/C compatible casting
- [x] Port proof-of-concept files to validate the toolchain
  - `util/id.cpp` - first file ported (identifier utilities)
  - `util/base64.cpp` - base64 decode wrapper
  - `util/block-helpers.cpp` - block size validation
  - `util/drm.cpp` - DRM rendernode open (with g_strdup_printf -> std::string)
- [x] Fix `ARRAY_SIZE` macro for C++ (typeof not available, added sizeof fallback)
- [x] Established include pattern: osdep.h first (unwrapped), then extern "C" for simple headers

### Phase 1.5: Header Compatibility Fixes (Needed Before Broader Porting)
These shared header issues block porting files with complex dependencies:
- [ ] **QAPI code generator**: `export` used as struct member name (C++ reserved word)
  - Affects: any file that transitively includes block-core QAPI types
  - Fix: modify scripts/qapi/ to rename `export` -> `export_` or similar
- [ ] **`include/qemu/lockable.h`**: implicit void* -> typed pointer casts
  - Fix: add `static_cast` in `#ifdef __cplusplus` blocks
- [ ] **`include/qemu/cutils.h`**: `restrict` keyword (C99, not C++)
  - Fix: use `__restrict__` or `#ifdef __cplusplus` conditional
- [ ] **`include/qemu/osdep.h`**: already mostly C++ safe (has extern "C" guards)
  - ARRAY_SIZE fixed, but QEMU_BUILD_BUG_ON_STRUCT uses anonymous struct bitfields

### Phase 2: Common Infrastructure
Port foundational code that everything depends on:
- [x] `include/qemu/osdep.h` - already C++ compatible (has extern "C" guards)
- [ ] `include/qom/object.h` - the QOM core (plan C++ class hierarchy)
- [ ] `include/qemu/typedefs.h` - type definitions
- [ ] `util/` - utility functions
- [ ] `qobject/` - QEMU object serialization

Key challenge: The QOM uses C macros extensively (`OBJECT_CHECK`, `OBJECT_CLASS_CHECK`,
type casting macros). These need C++ equivalents that use `static_cast` or
`dynamic_cast` instead of raw casts.

### Phase 3: Target-Independent Code
- [ ] `softmmu/` - system emulation core
- [ ] `hw/core/` - device model core
- [ ] `hw/misc/` - miscellaneous devices
- [ ] `migration/` - live migration
- [ ] `block/` - block layer
- [ ] `net/` - networking

### Phase 4: Target-Specific Code (per ISA)
For each ISA, port in this order:
1. `target/<arch>/cpu.h` and `cpu.c` - CPU definitions
2. `target/<arch>/helper.c` - helper functions
3. `target/<arch>/translate.c` - TCG translation
4. `target/<arch>/machine.c` - machine state
5. Remaining files in `target/<arch>/`

Order of ISAs:
1. **riscv64** - cleanest, smallest target code (~31K lines)
2. **x86_64** - most important, moderate size (~59K lines)
3. **ppc64** - complex but well-structured (~41K lines)
4. **aarch64/arm** - largest target (~108K lines), do last

### Phase 5: Hardware Models
Port machine-specific hardware:
- [ ] `hw/i386/` - x86 machines
- [ ] `hw/arm/` - ARM machines
- [ ] `hw/ppc/` - PowerPC machines
- [ ] `hw/riscv/` - RISC-V machines
- [ ] `hw/virtio/` - VirtIO devices (shared across all)

### Phase 6: QOM Redesign
Once enough code is ported, redesign the object model:
- [ ] Replace macro-based type registration with C++ class registration
- [ ] Replace `OBJECT_CHECK` macros with `static_cast`
- [ ] Replace property system with C++ member variables
- [ ] Replace signal/callback system with virtual methods or `std::function`

## File Porting Procedure

For each `.c` file being ported:

1. **Rename**: `git mv foo.c foo.cpp`
2. **Add extern "C" wrappers**: Wrap included C headers with `extern "C" { }`
3. **Fix C++ incompatibilities**:
   - Implicit `void*` casts need explicit casts
   - `new`, `class`, `template`, `namespace` etc. are reserved words
   - Designated initializers have restrictions in C++17
   - Compound literals need replacement
   - Flexible array members need handling
4. **Update meson.build**: Change filename in the build definition
5. **Build and test**: Ensure everything still compiles and smoke tests pass
6. **Commit**: One logical change per commit

## Common C-to-C++ Issues in QEMU

### Void pointer casts
```c
// C (implicit)
MyStruct *s = g_malloc(sizeof(*s));
// C++ (explicit)
MyStruct *s = static_cast<MyStruct *>(g_malloc(sizeof(*s)));
```

### Reserved words used as identifiers
QEMU uses `new`, `class`, `template`, `namespace`, `this` as variable names
in many places. These must be renamed.

### Designated initializers
```c
// C99 (works)
struct foo f = { .bar = 1, .baz = 2 };
// C++17 (works, but order must match declaration order)
struct foo f = { .bar = 1, .baz = 2 };  // OK if bar declared before baz
```

### Compound literals
```c
// C (compound literal)
return (QEMUTimer){ .cb = cb, .opaque = opaque };
// C++ (brace initialization)
return QEMUTimer{ .cb = cb, .opaque = opaque };
```

### typeof
```c
// GNU C
typeof(x) y = x;
// C++ (use decltype or just write the type)
decltype(x) y = x;  // or just: int y = x;
```

## Risk Areas

1. **GLib dependency**: GLib is a C library; all GLib calls need `extern "C"`.
   GLib headers may need wrapping.
2. **TCG (Tiny Code Generator)**: Heavy macro usage, code generation. May be
   best left as C and wrapped.
3. **KVM/hypervisor interfaces**: Kernel headers are C; need extern "C".
4. **Coroutines**: QEMU's coroutine system uses `setjmp`/`longjmp` and
   assembly. These interact poorly with C++ destructors. May need to remain C.
5. **Build time**: C++ compilation is slower. Monitor build times.

## Testing Strategy

Every porting change must pass:
1. `make` - clean compilation with no warnings-as-errors
2. Smoke tests - all 5 ISAs boot successfully
3. QEMU's built-in `make check` tests
4. Functional tests for the affected ISA

## Success Criteria

- All 5 target ISAs build and boot test images successfully
- No performance regression (boot time within 10% of C baseline)
- Code is more readable, not less
- QOM usage is simpler for developers
- Build system handles mixed C/C++ cleanly
