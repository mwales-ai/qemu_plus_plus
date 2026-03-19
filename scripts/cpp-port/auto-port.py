#!/usr/bin/env python3
"""
auto-port.py - Automated C-to-C++ porting for QEMU files

This script:
1. Renames .c -> .cpp and updates meson.build
2. Applies mechanical C++ fixes (void* casts, PRI macros, keywords, etc.)
3. Attempts to compile and parses errors
4. Applies targeted fixes for known error patterns
5. Repeats until clean or gives up with a report

Usage:
    ./scripts/cpp-port/auto-port.py file1.c [file2.c ...]
    ./scripts/cpp-port/auto-port.py --dir hw/timer/
    ./scripts/cpp-port/auto-port.py --dry-run file.c
    ./scripts/cpp-port/auto-port.py --analyze-only file.c  # just show what would need fixing
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

QEMU_ROOT = Path(__file__).resolve().parent.parent.parent
MAX_FIX_ITERATIONS = 5

# Files that must stay as .c (poisoned macros, target-specific)
SKIP_PATTERNS = [
    'os-win32.c', 'os-wasm.c',
    # These are in specific_ss and use poisoned macros
]

@dataclass
class CompileError:
    file: str
    line: int
    col: int
    error_type: str  # "error" or "warning"
    message: str
    raw: str

@dataclass
class PortResult:
    file: str
    success: bool
    errors_remaining: List[CompileError] = field(default_factory=list)
    fixes_applied: List[str] = field(default_factory=list)
    needs_manual: List[str] = field(default_factory=list)


##############################################################################
# Phase 1: Mechanical text transforms (pre-compile)
##############################################################################

def fix_pri_macros(content: str) -> Tuple[str, int]:
    """Fix PRI macro literal-suffix errors: "%"PRIu64 -> "%" PRIu64"""
    count = 0
    for macro in ['PRI[diouxX]\\d+', 'VADDR_PRI[xXdou]', 'TARGET_FMT_\\w+',
                   'HWADDR_PRI[xXdou]', 'RAM_ADDR_FMT', 'TYPE_FMT_\\w+']:
        # Add space after closing quote before macro
        new, n = re.subn(rf'"({macro})', r'" \1', content)
        count += n
        content = new
        # Add space before opening quote after macro
        new, n = re.subn(rf'({macro})"', r'\1 "', content)
        count += n
        content = new
    # Remove double spaces
    content = re.sub(r'"  (PRI|VADDR|TARGET_FMT|HWADDR|RAM_ADDR|TYPE_FMT)', r'" \1', content)
    content = re.sub(r'(PRI[diouxX]\d+|VADDR_PRI\w+|HWADDR_PRI\w+|RAM_ADDR_FMT|TYPE_FMT_\w+)  "', r'\1 "', content)
    return content, count


def fix_void_star_params(content: str) -> Tuple[str, int]:
    """Fix void* parameter assignments: TYPE *var = opaque -> static_cast"""
    count = 0
    void_params = r'opaque|pv|user_data|userdata|data|arg|cb_arg|cb_opaque|timer_opaque'
    # TYPE *var = param;
    pattern = re.compile(
        rf'^(\s*)((?:const\s+)?(?:struct\s+)?(?:unsigned\s+)?\w+)\s+\*(\w+)\s*=\s*({void_params})\s*;',
        re.MULTILINE
    )
    def repl(m):
        nonlocal count
        count += 1
        indent, typ, var, param = m.group(1), m.group(2), m.group(3), m.group(4)
        return f'{indent}{typ} *{var} = static_cast<{typ} *>({param});'
    content = pattern.sub(repl, content)

    # TYPE **var = param;
    pattern2 = re.compile(
        rf'^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*\*(\w+)\s*=\s*({void_params})\s*;',
        re.MULTILINE
    )
    def repl2(m):
        nonlocal count
        count += 1
        indent, typ, var, param = m.group(1), m.group(2), m.group(3), m.group(4)
        return f'{indent}{typ} **{var} = static_cast<{typ} **>({param});'
    content = pattern2.sub(repl2, content)
    return content, count


def fix_gmalloc_casts(content: str) -> Tuple[str, int]:
    """Fix g_malloc/g_malloc0/g_realloc/g_memdup2 void* returns"""
    count = 0
    funcs = r'g_(?:try_)?malloc0?|g_realloc|g_memdup2|g_try_malloc0?_n'
    pattern = re.compile(
        rf'^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*({funcs})\(',
        re.MULTILINE
    )
    lines = content.split('\n')
    new_lines = []
    for line in lines:
        m = pattern.match(line)
        if m:
            indent, typ, var, func = m.group(1), m.group(2), m.group(3), m.group(4)
            rest = line[m.end():]
            new_line = f'{indent}{typ} *{var} = static_cast<{typ} *>({func}({rest}'
            # If line ends with ); add the extra closing paren
            if new_line.rstrip().endswith(');'):
                new_line = new_line.rstrip()[:-2] + '));'
            new_lines.append(new_line)
            count += 1
        else:
            new_lines.append(line)
    return '\n'.join(new_lines), count


def fix_cpp_keywords(content: str) -> Tuple[str, int]:
    """Rename C variables that clash with C++ keywords"""
    count = 0

    # Only rename 'new' if it's used as a variable name (declared with a type)
    if re.search(r'^\s*\w+\s+\*?new\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(
            r'\bnew\b(?!_|ly|line|->)',
            'new_val',
            content,
            flags=re.MULTILINE
        )
        if new_content != content:
            count += 1
            content = new_content

    # 'class' as variable -> 'klass'
    if re.search(r'^\s*\w+\s+\*?class\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(r'\bclass\b(?!_|es|ify|ic)', 'klass', content)
        if new_content != content:
            count += 1
            content = new_content

    # 'template' as variable -> 'tmpl'
    if re.search(r'^\s*\w+\s+\*?template\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(r'\btemplate\b(?!_|s\b)', 'tmpl', content)
        if new_content != content:
            count += 1
            content = new_content

    # 'typename' as variable -> 'type_name'
    if re.search(r'^\s*\w+\s+\*?typename\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(r'\btypename\b(?!_)', 'type_name', content)
        if new_content != content:
            count += 1
            content = new_content

    return content, count


def fix_void_ptr_arithmetic(content: str) -> Tuple[str, int]:
    """Fix void* pointer arithmetic: (void*)ptr + n -> (char*)ptr + n"""
    count = 0
    new, n = re.subn(r'\(void\s*\*\)\s*(\w+)\s*\+', r'static_cast<char *>(\1) +', content)
    count += n
    content = new
    new, n = re.subn(r'\(const\s+void\s*\*\)\s*(\w+)\s*\+', r'static_cast<const char *>(\1) +', content)
    count += n
    content = new
    return content, count


def apply_pre_compile_fixes(filepath: Path) -> List[str]:
    """Apply all mechanical fixes before attempting compilation."""
    content = filepath.read_text()
    fixes = []

    content, n = fix_pri_macros(content)
    if n: fixes.append(f"PRI macro spacing ({n})")

    content, n = fix_void_star_params(content)
    if n: fixes.append(f"void* param casts ({n})")

    content, n = fix_gmalloc_casts(content)
    if n: fixes.append(f"g_malloc casts ({n})")

    content, n = fix_cpp_keywords(content)
    if n: fixes.append(f"C++ keyword renames ({n})")

    content, n = fix_void_ptr_arithmetic(content)
    if n: fixes.append(f"void* arithmetic ({n})")

    filepath.write_text(content)
    return fixes


##############################################################################
# Phase 2: Compile and parse errors
##############################################################################

def compile_file(cpp_file: Path) -> List[CompileError]:
    """Try to build and return list of errors for our file."""
    result = subprocess.run(
        ['ninja', '-C', str(QEMU_ROOT / 'build'), '-j1'],
        capture_output=True, text=True, timeout=600
    )
    errors = []
    # Match: ../path/file.cpp:LINE:COL: error: message
    err_pattern = re.compile(
        r'^\.\./(.+?):(\d+):(\d+): (error|warning): (.+)$',
        re.MULTILINE
    )
    output = result.stdout + result.stderr
    for m in err_pattern.finditer(output):
        fpath, line, col, etype, msg = m.groups()
        # Only care about errors in our file
        if fpath.endswith(cpp_file.name):
            errors.append(CompileError(
                file=fpath, line=int(line), col=int(col),
                error_type=etype, message=msg, raw=m.group(0)
            ))
    return errors


def try_compile_file_only(cpp_file: Path) -> List[CompileError]:
    """Try building and return errors only for the specific file."""
    result = subprocess.run(
        ['ninja', '-C', str(QEMU_ROOT / 'build')],
        capture_output=True, text=True, timeout=600
    )
    errors = []
    err_pattern = re.compile(
        r'^\.\./(.+?):(\d+):(\d+): (error): (.+)$',
        re.MULTILINE
    )
    output = result.stdout + result.stderr
    basename = cpp_file.name
    for m in err_pattern.finditer(output):
        fpath, line, col, etype, msg = m.groups()
        if basename in fpath:
            errors.append(CompileError(
                file=fpath, line=int(line), col=int(col),
                error_type=etype, message=msg, raw=m.group(0)
            ))
    return errors


##############################################################################
# Phase 3: Error-driven fixes (post-compile)
##############################################################################

def fix_invalid_conversion(filepath: Path, error: CompileError) -> Optional[str]:
    """Fix 'invalid conversion from void*/gpointer to TYPE*'"""
    m = re.search(r"invalid conversion from '(?:void\*|gpointer.*?)' to '(.+?)'", error.message)
    if not m:
        m = re.search(r"invalid conversion from 'gconstpointer.*?' to '(.+?)'", error.message)
    if not m:
        return None

    target_type = m.group(1)
    lines = filepath.read_text().split('\n')
    line_idx = error.line - 1
    if line_idx >= len(lines):
        return None

    line = lines[line_idx]

    # Already fixed?
    if 'static_cast' in line:
        return None

    # Common patterns: TYPE *var = func_call(...);
    # Try wrapping the RHS in static_cast
    # Pattern: = something_that_returns_void_star(...)
    cast_pattern = re.compile(r'=\s*(\w+\()')
    cm = cast_pattern.search(line)
    if cm:
        func_name = cm.group(1)
        # Wrap the function call in static_cast
        start = line.index(func_name)
        depth = 0
        end = start
        for i in range(start + len(func_name) - 1, len(line)):
            if line[i] == '(':
                depth += 1
            elif line[i] == ')':
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        call = line[start:end]
        new_call = f'static_cast<{target_type}>({call})'
        lines[line_idx] = line[:start] + new_call + line[end:]
        filepath.write_text('\n'.join(lines))
        return f"void* cast at line {error.line}"

    # Simple assignment: TYPE *var = param;
    assign_pattern = re.compile(r'=\s*(\w+)\s*;')
    am = assign_pattern.search(line)
    if am:
        param = am.group(1)
        lines[line_idx] = line.replace(f'= {param};', f'= static_cast<{target_type}>({param});')
        filepath.write_text('\n'.join(lines))
        return f"void* param cast at line {error.line}"

    return None


def fix_enum_conversion(filepath: Path, error: CompileError) -> Optional[str]:
    """Fix int-to-enum implicit conversions."""
    m = re.search(r"invalid conversion from 'int' to '(\w+)'", error.message)
    if not m:
        return None

    enum_type = m.group(1)
    lines = filepath.read_text().split('\n')
    line_idx = error.line - 1
    if line_idx >= len(lines):
        return None

    line = lines[line_idx]
    if 'static_cast' in line:
        return None

    # = 0; pattern
    zero_pattern = re.compile(r'(\w+)\s*=\s*0\s*;')
    zm = zero_pattern.search(line)
    if zm:
        var = zm.group(1)
        lines[line_idx] = line.replace(f'{var} = 0;', f'{var} = static_cast<{enum_type}>(0);')
        filepath.write_text('\n'.join(lines))
        return f"enum cast at line {error.line}"

    return None


def fix_undefined_reference(error: CompileError) -> Optional[Tuple[str, str]]:
    """Parse undefined reference errors to identify missing extern "C".

    Returns (mangled_name, header_file) if fixable, None otherwise.
    """
    m = re.search(r"undefined reference to `(.+?)'", error.message)
    if not m:
        return None
    symbol = m.group(1)
    # C++ mangled names start with _Z
    if symbol.startswith('_Z'):
        return symbol, None
    return None


def add_extern_c_to_header(header_path: Path) -> Optional[str]:
    """Add extern "C" guards to a header file if not already present."""
    if not header_path.exists():
        return None

    content = header_path.read_text()

    # Already has guards?
    if '__cplusplus' in content:
        return None

    # Find the #ifndef / #define guard
    lines = content.split('\n')
    insert_after = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('#define') and i > 0 and lines[i-1].strip().startswith('#ifndef'):
            insert_after = i
            break

    if insert_after < 0:
        return None

    # Find the #endif at the end
    endif_line = -1
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == '#endif':
            endif_line = i
            break

    if endif_line < 0:
        return None

    # Insert guards
    lines.insert(insert_after + 1, '')
    lines.insert(insert_after + 2, '#ifdef __cplusplus')
    lines.insert(insert_after + 3, 'extern "C" {')
    lines.insert(insert_after + 4, '#endif')

    # Adjust endif_line for insertions
    endif_line += 4

    lines.insert(endif_line, '')
    lines.insert(endif_line + 1, '#ifdef __cplusplus')
    lines.insert(endif_line + 2, '}')
    lines.insert(endif_line + 3, '#endif')

    header_path.write_text('\n'.join(lines))
    return f"Added extern C guards to {header_path}"


def find_header_for_function(func_name: str) -> Optional[Path]:
    """Try to find the header that declares a function."""
    # Search include/ directory for function declaration
    result = subprocess.run(
        ['grep', '-rl', f'{func_name}(', 'include/'],
        capture_output=True, text=True, cwd=QEMU_ROOT
    )
    if result.returncode == 0:
        candidates = result.stdout.strip().split('\n')
        for c in candidates:
            c = c.strip()
            if c.endswith('.h'):
                return QEMU_ROOT / c
    return None


def add_extern_c_to_func_def(filepath: Path, func_name: str) -> Optional[str]:
    """Add extern "C" to a function definition in a .cpp file."""
    content = filepath.read_text()

    # Find the function definition (not declaration)
    # Pattern: type funcname(... at start of line (not indented = definition)
    pattern = re.compile(
        rf'^(\w[\w\s\*]+?)\b({re.escape(func_name)})\s*\(',
        re.MULTILINE
    )
    m = pattern.search(content)
    if not m:
        return None

    # Check if already has extern "C"
    line_start = content.rfind('\n', 0, m.start()) + 1
    prefix = content[line_start:m.start()]
    if 'extern "C"' in prefix:
        return None

    # Add extern "C" before the function
    content = content[:m.start()] + 'extern "C"\n' + content[m.start():]
    filepath.write_text(content)
    return f"Added extern \"C\" to {func_name} definition"


def fix_linker_errors(filepath: Path, build_output: str) -> List[str]:
    """Fix undefined reference errors by adding extern "C" to function defs
    or adding guards to headers."""
    fixes = []

    # Find undefined references in our file's object
    basename = filepath.stem
    # Look for undefined references related to our file
    undef_pattern = re.compile(
        rf'{re.escape(basename)}\.cpp\.o:.*undefined reference to `(\w+)\('
    )

    for m in undef_pattern.finditer(build_output):
        func_name = m.group(1)

        # Try adding extern "C" to the function definition in our file
        fix = add_extern_c_to_func_def(filepath, func_name)
        if fix:
            fixes.append(fix)
            continue

        # Try finding and fixing the header
        header = find_header_for_function(func_name)
        if header:
            fix = add_extern_c_to_header(header)
            if fix:
                fixes.append(fix)

    return fixes


def apply_error_fixes(filepath: Path, errors: List[CompileError]) -> List[str]:
    """Try to fix errors based on compiler output."""
    fixes = []
    for err in errors:
        fix = None
        if 'invalid conversion' in err.message and ('void' in err.message or 'gpointer' in err.message):
            fix = fix_invalid_conversion(filepath, err)
        elif 'invalid conversion' in err.message and "from 'int'" in err.message:
            fix = fix_enum_conversion(filepath, err)
        if fix:
            fixes.append(fix)
    return fixes


##############################################################################
# Phase 4: Analysis / reporting
##############################################################################

def analyze_file(c_file: Path) -> dict:
    """Analyze a .c file for potential C++ issues without modifying it."""
    content = c_file.read_text()
    issues = {
        'compound_literals': [],
        'designator_inits': [],
        'void_star_casts': [],
        'pri_macros': [],
        'cpp_keywords': [],
        'void_ptr_arithmetic': [],
        'typeof_usage': [],
        'gnu_extensions': [],
    }

    for i, line in enumerate(content.split('\n'), 1):
        # Compound literals: (Type) { ... }
        if re.search(r'\(\w+\)\s*\{', line):
            issues['compound_literals'].append((i, line.strip()))

        # Out-of-order or complex designated initializers
        if re.search(r'\.\w+\s*=', line) and not re.search(r'^\s*\.', line):
            # Nested designators like .u.field = ...
            if re.search(r'\.\w+\.\w+\s*=', line):
                issues['designator_inits'].append((i, line.strip()))

        # void* implicit casts
        if re.search(r'=\s*(opaque|pv|user_data|userdata|data|arg)\s*;', line):
            if not 'static_cast' in line and not 'void' in line.split('=')[0]:
                issues['void_star_casts'].append((i, line.strip()))

        # PRI macros without space
        if re.search(r'"PRI[diouxX]|PRI[diouxX]\d+"', line):
            issues['pri_macros'].append((i, line.strip()))

        # C++ keywords as identifiers
        for kw in ['new', 'class', 'template', 'typename', 'namespace', 'this', 'delete', 'export']:
            if re.search(rf'^\s*\w+\s+\*?{kw}\s*[=,;)]', line):
                issues['cpp_keywords'].append((i, f'{kw}: {line.strip()}'))

        # typeof (not available in standard C++)
        if re.search(r'\btypeof\b', line) and 'typeof_strip_qual' not in line:
            issues['typeof_usage'].append((i, line.strip()))

        # GNU range designators [0 ... N]
        if re.search(r'\[\d+\s*\.\.\.\s*\d+\]', line):
            issues['gnu_extensions'].append((i, line.strip()))

    return issues


def classify_difficulty(issues: dict) -> str:
    """Classify porting difficulty based on issues found."""
    blockers = len(issues['compound_literals']) + len(issues['gnu_extensions']) + len(issues['typeof_usage'])
    manual = len(issues['designator_inits'])
    auto = (len(issues['void_star_casts']) + len(issues['pri_macros']) +
            len(issues['cpp_keywords']) + len(issues['void_ptr_arithmetic']))

    if blockers > 5:
        return "HARD"
    elif blockers > 0 or manual > 3:
        return "MEDIUM"
    elif auto > 0:
        return "EASY"
    else:
        return "TRIVIAL"


##############################################################################
# Main porting logic
##############################################################################

def rename_and_update_meson(c_file: Path) -> Path:
    """git mv .c -> .cpp and update meson.build"""
    cpp_file = c_file.with_suffix('.cpp')

    # git mv
    subprocess.run(['git', 'mv', str(c_file), str(cpp_file)],
                   cwd=QEMU_ROOT, check=True)

    # Update meson.build
    meson_dir = c_file.parent
    meson_file = meson_dir / 'meson.build'
    if not meson_file.exists():
        meson_file = QEMU_ROOT / 'meson.build'

    if meson_file.exists():
        content = meson_file.read_text()
        old_name = c_file.name
        new_name = cpp_file.name
        new_content = content.replace(f"'{old_name}'", f"'{new_name}'")
        if new_content == content:
            # Try with path prefix
            old_path = str(c_file)
            new_path = str(cpp_file)
            new_content = content.replace(f"'{old_path}'", f"'{new_path}'")
        if new_content != content:
            meson_file.write_text(new_content)

    return cpp_file


def port_file(c_file: Path, dry_run: bool = False, analyze_only: bool = False) -> PortResult:
    """Port a single .c file to .cpp."""
    result = PortResult(file=str(c_file), success=False)

    # Skip known problematic files
    if c_file.name in SKIP_PATTERNS:
        result.needs_manual.append(f"Skipped: known problematic file")
        return result

    # Analyze first
    issues = analyze_file(c_file)
    difficulty = classify_difficulty(issues)

    if analyze_only:
        result.needs_manual.append(f"Difficulty: {difficulty}")
        for category, items in issues.items():
            if items:
                result.needs_manual.append(f"  {category}: {len(items)} issues")
                for line_no, text in items[:3]:
                    result.needs_manual.append(f"    L{line_no}: {text[:80]}")
                if len(items) > 3:
                    result.needs_manual.append(f"    ... and {len(items)-3} more")
        return result

    if dry_run:
        result.needs_manual.append(f"[DRY RUN] Would port {c_file} (difficulty: {difficulty})")
        return result

    # Phase 1: Rename
    print(f"  Renaming {c_file} -> {c_file.with_suffix('.cpp')}")
    cpp_file = rename_and_update_meson(c_file)

    # Phase 2: Apply mechanical fixes
    print(f"  Applying pre-compile fixes...")
    fixes = apply_pre_compile_fixes(cpp_file)
    result.fixes_applied.extend(fixes)
    if fixes:
        print(f"    Applied: {', '.join(fixes)}")

    # Phase 3: Compile-fix loop
    for iteration in range(MAX_FIX_ITERATIONS):
        print(f"  Compile attempt {iteration + 1}...")

        # Build and capture full output for linker error analysis
        build_result = subprocess.run(
            ['ninja', '-C', str(QEMU_ROOT / 'build')],
            capture_output=True, text=True, timeout=600
        )
        build_output = build_result.stdout + build_result.stderr

        errors = []
        err_pattern = re.compile(
            r'^\.\./(.+?):(\d+):(\d+): (error): (.+)$',
            re.MULTILINE
        )
        basename = cpp_file.name
        for m in err_pattern.finditer(build_output):
            fpath, line, col, etype, msg = m.groups()
            if basename in fpath:
                errors.append(CompileError(
                    file=fpath, line=int(line), col=int(col),
                    error_type=etype, message=msg, raw=m.group(0)
                ))

        if not errors and build_result.returncode == 0:
            result.success = True
            print(f"  CLEAN BUILD!")
            break

        # Check for linker errors (undefined reference)
        if not errors and 'undefined reference' in build_output:
            linker_fixes = fix_linker_errors(cpp_file, build_output)
            if linker_fixes:
                result.fixes_applied.extend(linker_fixes)
                print(f"    Linker fixes: {', '.join(linker_fixes)}")
                continue

        if not errors:
            # Build failed but not our file - might be pre-existing
            print(f"    Build failed but no errors in {basename}")
            result.success = True  # Our file is probably fine
            break

        print(f"    {len(errors)} errors found, attempting fixes...")
        new_fixes = apply_error_fixes(cpp_file, errors)
        result.fixes_applied.extend(new_fixes)

        if not new_fixes:
            # No more automated fixes possible
            result.errors_remaining = errors
            for err in errors:
                result.needs_manual.append(f"  L{err.line}: {err.message[:100]}")
            print(f"    No automated fixes available for remaining {len(errors)} errors")
            break
        else:
            print(f"    Applied: {', '.join(new_fixes)}")

    return result


def port_directory(dir_path: Path, **kwargs) -> List[PortResult]:
    """Port all .c files in a directory."""
    c_files = sorted(dir_path.glob('*.c'))
    c_files = [f for f in c_files if not f.name.endswith('.c.inc')]
    results = []
    for c_file in c_files:
        print(f"\n{'='*60}")
        print(f"  {c_file}")
        print(f"{'='*60}")
        result = port_file(c_file, **kwargs)
        results.append(result)
    return results


def print_report(results: List[PortResult]):
    """Print a summary report."""
    print(f"\n{'='*60}")
    print(f"  PORTING REPORT")
    print(f"{'='*60}")

    success = [r for r in results if r.success]
    failed = [r for r in results if not r.success and r.errors_remaining]
    skipped = [r for r in results if not r.success and not r.errors_remaining]

    print(f"\n  Total files: {len(results)}")
    print(f"  Succeeded:   {len(success)}")
    print(f"  Failed:      {len(failed)}")
    print(f"  Skipped:     {len(skipped)}")

    if failed:
        print(f"\n  FAILED FILES (need manual fixes):")
        for r in failed:
            print(f"    {r.file}:")
            for note in r.needs_manual[:5]:
                print(f"      {note}")

    if success:
        print(f"\n  SUCCEEDED:")
        for r in success:
            fixes_str = ', '.join(r.fixes_applied) if r.fixes_applied else 'no fixes needed'
            print(f"    {r.file} ({fixes_str})")


def main():
    parser = argparse.ArgumentParser(description='Auto-port QEMU C files to C++')
    parser.add_argument('files', nargs='*', help='C files to port')
    parser.add_argument('--dir', help='Port all .c files in directory')
    parser.add_argument('--dry-run', action='store_true', help='Show what would be done')
    parser.add_argument('--analyze-only', action='store_true',
                        help='Analyze files for issues without modifying')
    parser.add_argument('--scan-dir', help='Scan directory and sort files by difficulty')
    args = parser.parse_args()

    os.chdir(QEMU_ROOT)

    if args.scan_dir:
        # Scan mode: analyze all files and sort by difficulty
        scan_path = Path(args.scan_dir)
        c_files = sorted(scan_path.rglob('*.c'))
        c_files = [f for f in c_files if not f.name.endswith('.c.inc')
                    and 'build' not in str(f) and 'roms' not in str(f)]

        by_difficulty = {'TRIVIAL': [], 'EASY': [], 'MEDIUM': [], 'HARD': []}
        for f in c_files:
            issues = analyze_file(f)
            diff = classify_difficulty(issues)
            total = sum(len(v) for v in issues.values())
            by_difficulty[diff].append((f, total, issues))

        for diff in ['TRIVIAL', 'EASY', 'MEDIUM', 'HARD']:
            files = by_difficulty[diff]
            if files:
                print(f"\n{diff} ({len(files)} files):")
                for f, total, issues in sorted(files, key=lambda x: x[1]):
                    detail = ', '.join(f'{k}={len(v)}' for k, v in issues.items() if v)
                    print(f"  {f} ({detail or 'clean'})")
        return

    files = []
    if args.dir:
        files = sorted(Path(args.dir).glob('*.c'))
        files = [f for f in files if not f.name.endswith('.c.inc')]
    elif args.files:
        files = [Path(f) for f in args.files]
    else:
        parser.print_help()
        return

    results = []
    for f in files:
        print(f"\n{'='*60}")
        print(f"  {f}")
        print(f"{'='*60}")
        result = port_file(f, dry_run=args.dry_run, analyze_only=args.analyze_only)
        results.append(result)

    print_report(results)


if __name__ == '__main__':
    main()
