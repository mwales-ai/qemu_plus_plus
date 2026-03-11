# QEMU++ : QEMU Ported to C++

QEMU++ is a fork of [QEMU](https://www.qemu.org/) that incrementally ports the
codebase from C to C++17. The goal is to make QEMU's complex object-oriented
architecture (the QOM - QEMU Object Model) more natural by using a language
with native OO support, while preserving QEMU's performance and compatibility.

**Base version**: QEMU v10.2.1 (stable release)
**Branch**: `cpp-port`

## Why?

QEMU has a sophisticated object-oriented architecture implemented entirely in C
using macros, function pointers, and manual type-casting. The QOM (QEMU Object
Model) provides inheritance, interfaces, properties, and type registration --
all through preprocessor macros like `OBJECT_CHECK`, `DECLARE_INSTANCE_CHECKER`,
and `OBJECT_CLASS_CHECK`.

This works, but it's:
- **Hard to read**: The actual type relationships are hidden behind macros
- **Error-prone**: Type casts are unchecked, wrong casts cause silent corruption
- **Hard to navigate**: IDEs can't follow the macro-based inheritance
- **Verbose**: Registering a new device type requires extensive boilerplate

C++ gives us classes, inheritance, `static_cast`, and virtual methods -- exactly
what QOM is reimplementing by hand.

## C++ Style

This is not a rewrite using modern C++ idioms. We use a conservative
"C with classes" approach:

- **C++17 standard**
- STL containers (`std::vector`, `std::map`, `std::string`, etc.)
- Classes with RAII for resource management
- `static_cast` instead of C-style casts
- `enum class` instead of plain enums
- Simple inheritance to replace QOM macro hierarchies
- Explicit types -- no `auto` everywhere
- No exceptions -- QEMU's `Error**` error propagation is kept
- No C++ streams -- `printf` and `qemu_log` are fine
- No heavy template metaprogramming

## Target Architectures

We're starting with 5 ISAs:

| Architecture | QEMU Binary              | Test Machine |
|-------------|--------------------------|--------------|
| x86_64      | `qemu-system-x86_64`    | q35          |
| AArch64     | `qemu-system-aarch64`   | virt         |
| ARM (32-bit)| `qemu-system-arm`       | virt         |
| PowerPC 64  | `qemu-system-ppc64`     | pseries      |
| RISC-V 64   | `qemu-system-riscv64`   | virt         |

## Quick Start

### Install dependencies (Ubuntu 24.04)

```bash
sudo ./scripts/cpp-port/install-deps-ubuntu.sh
```

### Build

```bash
./scripts/cpp-port/build-qemu.sh

# Or manually:
mkdir build && cd build
../configure --target-list=x86_64-softmmu,aarch64-softmmu,arm-softmmu,ppc64-softmmu,riscv64-softmmu
make -j$(nproc)
```

### Test

```bash
# Download small test images (Alpine Linux, Debian netboot)
./scripts/cpp-port/download-test-images.sh

# Run smoke tests against all 5 ISAs
./scripts/cpp-port/run-smoke-tests.sh
```

## Project Documentation

- [PLANNING.md](PLANNING.md) - Detailed porting plan, phases, and strategy
- [CLAUDE.md](CLAUDE.md) - AI assistant project guide and coding conventions
- [README.rst](README.rst) - Original QEMU README

## Porting Status

| Phase | Description                  | Status      |
|-------|------------------------------|-------------|
| 0     | Infrastructure & test setup  | Complete    |
| 1     | Build system C++ support     | In Progress |
| 2     | Common infrastructure        | Not Started |
| 3     | Target-independent code      | Not Started |
| 4     | Target-specific code         | Not Started |
| 5     | Hardware models              | Not Started |
| 6     | QOM redesign                 | Not Started |

## License

QEMU as a whole is released under the GNU General Public License, version 2.
See the [LICENSE](LICENSE) file for details. The C++ porting work in this fork
is released under the same license.

## Upstream QEMU

This fork preserves all upstream QEMU branches and tags. The original QEMU
project is at:
- Website: https://www.qemu.org/
- Source: https://gitlab.com/qemu-project/qemu
