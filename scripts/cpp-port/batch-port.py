#!/usr/bin/env python3
"""
batch-port.py - Batch port TRIVIAL and EASY .c files to .cpp

This script:
1. Scans directories for TRIVIAL/EASY files using source analysis
2. Renames all files in a batch (git mv + meson.build update)
3. Applies mechanical fixes for EASY files
4. Builds to verify, auto-reverts failing files
5. Iterates until build is clean

Usage:
    ./scripts/cpp-port/batch-port.py --difficulty TRIVIAL --dirs hw/
    ./scripts/cpp-port/batch-port.py --difficulty EASY --dirs hw/misc hw/char
    ./scripts/cpp-port/batch-port.py --difficulty BOTH --dirs hw/
    ./scripts/cpp-port/batch-port.py --list --dirs hw/  # just list files
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

QEMU_ROOT = Path(__file__).resolve().parent.parent.parent

# Known header-level blockers: files including these headers will fail
# regardless of source-level analysis
HEADER_BLOCKERS = {
    # header pattern -> description
    'include/hw/nvme/': 'nvme.h uses "namespace" keyword',
    'target/arm/cpu.h': 'target-specific macros',
    'target/ppc/cpu.h': 'target-specific macros',
    'target/riscv/cpu.h': 'target-specific macros',
}

# Directories where files transitively include target-specific cpu.h
# These need the target cpu.h to be C++-compatible before porting
BLOCKED_DIRS = {
    'hw/ppc',    # includes target/ppc/cpu.h via machine headers
    'hw/riscv',  # includes target/riscv/cpu.h via machine headers
}

# Files/patterns to skip entirely
SKIP_FILES = {
    'os-win32.c', 'os-wasm.c',
}

# Directories that use specific_ss (poisoned macros) or are target-specific
SKIP_DIRS = {
    'target',
    'accel/tcg',
    'accel/kvm',
}

# Known files that fail due to header issues (not detectable from source analysis)
# Updated dynamically as we discover more
KNOWN_HEADER_BLOCKED = set()


def analyze_file(c_file):
    """Analyze a C file for C++ compatibility issues."""
    content = c_file.read_text()
    issues = {
        'extractable_compounds': [],  # VMStateField[], InterfaceInfo[] - auto-fixable
        'compound_literals': [],       # other compound literals - need manual work
        'nested_designators': [],      # .valid/.impl MemoryRegionOps - auto-fixable
        'other_designator_inits': [],  # other nested designators - manual
        'void_star_casts': [],
        'pri_macros': [],
        'cpp_keywords': [],
        'void_ptr_arithmetic': [],
        'typeof_usage': [],
        'gnu_extensions': [],
        'narrowing': [],
        'const_globals': [],
        'sparse_arrays': [],      # [idx] = val array initializers - auto-fixable
    }
    for i, line in enumerate(content.split('\n'), 1):
        # Compound literals: (Type){...} or (const Type[]) {
        is_compound = False
        if re.search(r'\(const\s+VMStateField\s*\[\]', line):
            issues['extractable_compounds'].append((i, line.strip()))
            is_compound = True
        elif re.search(r'\(const\s+InterfaceInfo\s*\[\]', line):
            issues['extractable_compounds'].append((i, line.strip()))
            is_compound = True
        elif re.search(r'\(\w+\s*\*?\)\s*\{', line):
            issues['compound_literals'].append((i, line.strip()))
            is_compound = True
        elif re.search(r'\(const\s+\w+\s*\[', line):
            issues['compound_literals'].append((i, line.strip()))
            is_compound = True

        # Nested designators: .foo.bar =
        if re.search(r'\.\w+\.\w+\s*=', line):
            # Check if it's a MemoryRegionOps .valid/.impl pattern (auto-fixable)
            if re.search(r'\.(valid|impl)\.(min_access_size|max_access_size|unaligned)\s*=', line):
                issues['nested_designators'].append((i, line.strip()))
            else:
                issues['other_designator_inits'].append((i, line.strip()))

        # void* implicit casts
        if re.search(r'=\s*(opaque|pv|user_data|userdata|data|arg)\s*;', line):
            if 'static_cast' not in line and 'void' not in line.split('=')[0]:
                issues['void_star_casts'].append((i, line.strip()))
        # PRI macros without space
        if re.search(r'"PRI[diouxX]|PRI[diouxX]\d+"', line):
            issues['pri_macros'].append((i, line.strip()))
        # C++ keywords used as identifiers
        for kw in ['new', 'class', 'template', 'typename', 'namespace',
                    'this', 'delete', 'export', 'private', 'protected', 'public']:
            if re.search(rf'^\s*\w+\s+\*?{kw}\s*[=,;)\[]', line):
                issues['cpp_keywords'].append((i, f'{kw}: {line.strip()}'))
            if re.search(rf'->{kw}\b', line):
                issues['cpp_keywords'].append((i, f'->{kw}: {line.strip()}'))
        # typeof usage
        if re.search(r'\btypeof\b', line) and 'typeof_strip_qual' not in line:
            issues['typeof_usage'].append((i, line.strip()))
        # GNU range designators: [0 ... 0xFF]
        if re.search(r'\[\d+\s*\.\.\.\s*\d+\]', line):
            issues['gnu_extensions'].append((i, line.strip()))
        # C designated array initializers: [0x0000] = VALUE or [INDEX] = VALUE
        if re.search(r'\[0x[0-9a-fA-F]+\]\s*=', line) or re.search(r'\[\w+\]\s*=\s*\w', line):
            # Make sure it's not a normal array subscript assignment (has ; at end)
            if not re.search(r';\s*$', line.strip()):
                issues['sparse_arrays'].append((i, f'sparse array: {line.strip()}'))
        # const globals (C++ internal linkage)
        if re.search(r'^(static\s+)?const\s+\w+\s+\w+\s*=', line) and not line.strip().startswith('static'):
            if re.search(r'^const\s+(VMState|Property|TypeInfo)', line):
                issues['const_globals'].append((i, line.strip()))
        # RegisterAccessInfo arrays have known designator ordering issues
        if 'RegisterAccessInfo' in line and re.search(r'\[\]', line):
            issues['other_designator_inits'].append((i, f'RegisterAccessInfo: {line.strip()}'))
    return issues


def classify_difficulty(issues):
    """Classify file difficulty based on issues found.

    TRIVIAL: no issues at all
    EASY: only auto-fixable issues (void* casts, PRI macros, keywords,
          extractable compound literals, MemoryRegionOps nested designators)
    MEDIUM: some non-extractable compound literals or other nested designators (1-5)
    HARD: many compound literals, GNU extensions, or typeof usage
    """
    hard_blockers = len(issues['gnu_extensions']) + len(issues['typeof_usage'])
    manual_compounds = len(issues['compound_literals'])
    manual_designators = len(issues['other_designator_inits']) + len(issues['const_globals'])
    auto = (len(issues['void_star_casts']) + len(issues['pri_macros']) +
            len(issues['cpp_keywords']) + len(issues['void_ptr_arithmetic']) +
            len(issues['extractable_compounds']) + len(issues['nested_designators']) +
            len(issues['sparse_arrays']))

    if hard_blockers > 0 or manual_compounds > 5:
        return "HARD"
    elif manual_compounds > 0 or manual_designators > 0:
        return "MEDIUM"
    elif auto > 0:
        return "EASY"
    else:
        return "TRIVIAL"


def check_header_blockers(c_file):
    """Check if a file includes headers known to block C++ compilation."""
    rel = str(c_file.relative_to(QEMU_ROOT))

    # Check if file is in a blocked directory
    for bd in BLOCKED_DIRS:
        if rel.startswith(bd + '/'):
            return f"directory {bd}/ blocked (target cpu.h not C++-compatible)"

    try:
        content = c_file.read_text()
    except Exception:
        return None
    for pattern, desc in HEADER_BLOCKERS.items():
        if pattern in content:
            return desc
    if rel in KNOWN_HEADER_BLOCKED:
        return "previously failed compilation"
    return None


def find_c_files(dirs):
    """Find all .c files in the given directories."""
    files = []
    for d in dirs:
        p = QEMU_ROOT / d
        if not p.exists():
            print(f"WARNING: {d} does not exist, skipping")
            continue
        for f in sorted(p.rglob('*.c')):
            if f.name.endswith('.c.inc'):
                continue
            if f.name in SKIP_FILES:
                continue
            rel = f.relative_to(QEMU_ROOT)
            skip = False
            for sd in SKIP_DIRS:
                if str(rel).startswith(sd + '/'):
                    skip = True
                    break
            if skip:
                continue
            if f.with_suffix('.cpp').exists():
                continue
            files.append(f)
    return files


def classify_files(files):
    """Classify files by difficulty."""
    result = {'TRIVIAL': [], 'EASY': [], 'MEDIUM': [], 'HARD': []}
    for f in files:
        try:
            # Check header blockers first
            blocker = check_header_blockers(f)
            if blocker:
                result['HARD'].append((f, {'_blocker': blocker}))
                continue
            issues = analyze_file(f)
            diff = classify_difficulty(issues)
            result[diff].append((f, issues))
        except Exception as e:
            print(f"WARNING: Could not analyze {f}: {e}")
    return result


def update_meson_build(c_file, cpp_file):
    """Update meson.build to reference .cpp instead of .c."""
    c_name = c_file.name
    cpp_name = cpp_file.name

    # Check meson.build in same directory
    meson_file = c_file.parent / 'meson.build'
    updated = False

    if meson_file.exists():
        content = meson_file.read_text()
        new_content = content.replace(f"'{c_name}'", f"'{cpp_name}'")
        if new_content != content:
            meson_file.write_text(new_content)
            updated = True

    # Also check parent meson.build for subdirectory references
    parent_meson = c_file.parent.parent / 'meson.build'
    if parent_meson.exists():
        subdir = c_file.parent.name
        sub_c_ref = f"'{subdir}/{c_name}'"
        sub_cpp_ref = f"'{subdir}/{cpp_name}'"
        content = parent_meson.read_text()
        if sub_c_ref in content:
            new_content = content.replace(sub_c_ref, sub_cpp_ref)
            parent_meson.write_text(new_content)
            updated = True

    if not updated:
        # Try QEMU root meson.build
        root_meson = QEMU_ROOT / 'meson.build'
        if root_meson.exists():
            content = root_meson.read_text()
            rel = str(c_file.relative_to(QEMU_ROOT))
            cpp_rel = str(cpp_file.relative_to(QEMU_ROOT))
            new_content = content.replace(f"'{rel}'", f"'{cpp_rel}'")
            if new_content != content:
                root_meson.write_text(new_content)
                updated = True

    return updated


def revert_file(cpp_file):
    """Revert a .cpp file back to .c, restoring original content."""
    c_file = cpp_file.with_suffix('.c')
    try:
        subprocess.run(['git', 'mv', str(cpp_file), str(c_file)],
                      cwd=QEMU_ROOT, check=True, capture_output=True)
    except subprocess.CalledProcessError:
        subprocess.run(['git', 'checkout', '--', str(c_file)],
                      cwd=QEMU_ROOT, capture_output=True)
        if cpp_file.exists():
            cpp_file.unlink()
        return False

    # Restore original file content (undo any mechanical fixes)
    subprocess.run(['git', 'checkout', 'HEAD', '--', str(c_file)],
                  cwd=QEMU_ROOT, capture_output=True)

    # Fix meson.build back
    cpp_name = cpp_file.name
    c_name = c_file.name
    meson_file = cpp_file.parent / 'meson.build'
    if meson_file.exists():
        content = meson_file.read_text()
        if cpp_name in content:
            meson_file.write_text(content.replace(cpp_name, c_name))

    parent_meson = cpp_file.parent.parent / 'meson.build'
    if parent_meson.exists():
        subdir = cpp_file.parent.name
        sub_cpp = f"{subdir}/{cpp_name}"
        sub_c = f"{subdir}/{c_name}"
        content = parent_meson.read_text()
        if sub_cpp in content:
            parent_meson.write_text(content.replace(sub_cpp, sub_c))

    return True


def extract_vmstate_fields(content):
    """Extract VMStateField compound literal arrays to named static const arrays."""
    pattern = re.compile(
        r'(static\s+const\s+VMStateDescription\s+(\w+)\s*=\s*\{)',
        re.MULTILINE
    )
    extractions = []
    for match in pattern.finditer(content):
        vmstate_name = match.group(2)
        struct_start = match.start()
        # Find end of this struct
        brace_depth = 0
        i = content.index('{', match.start())
        struct_end = None
        for pos in range(i, len(content)):
            if content[pos] == '{': brace_depth += 1
            elif content[pos] == '}':
                brace_depth -= 1
                if brace_depth == 0:
                    struct_end = pos + 1
                    break
        if struct_end is None:
            continue
        struct_text = content[struct_start:struct_end]
        fields_match = re.search(
            r'\.fields\s*=\s*\(const\s+VMStateField\[\]\)\s*\{',
            struct_text
        )
        if not fields_match:
            continue
        # Find matching closing brace
        fields_start = fields_match.end()
        brace_depth = 1
        fields_end = None
        for pos in range(fields_start, len(struct_text)):
            if struct_text[pos] == '{': brace_depth += 1
            elif struct_text[pos] == '}':
                brace_depth -= 1
                if brace_depth == 0:
                    fields_end = pos
                    break
        if fields_end is None:
            continue
        field_entries = struct_text[fields_start:fields_end].strip()
        array_name = f'{vmstate_name}_fields'
        old_fields = struct_text[fields_match.start():fields_end + 1]
        extractions.append({
            'vmstate_name': vmstate_name,
            'array_name': array_name,
            'field_entries': field_entries,
            'struct_start': struct_start,
            'old_fields': old_fields,
        })
    for ext in reversed(extractions):
        new_ref = f'.fields = {ext["array_name"]},'
        extracted = f'static const VMStateField {ext["array_name"]}[] = {{\n{ext["field_entries"]}\n}};\n\n'
        content = content.replace(ext['old_fields'], new_ref, 1)
        line_start = content.rfind('\n', 0, content.find(ext['vmstate_name'])) + 1
        content = content[:line_start] + extracted + content[line_start:]
    return content, len(extractions)


def extract_interface_info(content):
    """Extract InterfaceInfo compound literal arrays to named static const arrays."""
    pattern = re.compile(
        r'(static\s+const\s+TypeInfo\s+(\w+)\s*=)',
        re.MULTILINE
    )
    extractions = []
    for match in pattern.finditer(content):
        type_name = match.group(2)
        brace_start = content.index('{', match.start())
        brace_depth = 0
        type_end = None
        for pos in range(brace_start, len(content)):
            if content[pos] == '{': brace_depth += 1
            elif content[pos] == '}':
                brace_depth -= 1
                if brace_depth == 0:
                    type_end = pos + 1
                    break
        if type_end is None:
            continue
        type_text = content[match.start():type_end]
        iface_match = re.search(
            r'\.interfaces\s*=\s*\(const\s+InterfaceInfo\[\]\)\s*\{',
            type_text
        )
        if not iface_match:
            continue
        iface_start = iface_match.end()
        brace_depth = 1
        iface_end = None
        for pos in range(iface_start, len(type_text)):
            if type_text[pos] == '{': brace_depth += 1
            elif type_text[pos] == '}':
                brace_depth -= 1
                if brace_depth == 0:
                    iface_end = pos
                    break
        if iface_end is None:
            continue
        iface_entries = type_text[iface_start:iface_end].strip()
        old_iface = type_text[iface_match.start():iface_end + 1]
        array_name = f'{type_name}_interfaces'
        extractions.append({
            'type_name': type_name,
            'array_name': array_name,
            'iface_entries': iface_entries,
            'search_start': match.start(),
            'old_iface': old_iface,
        })
    for ext in reversed(extractions):
        new_ref = f'.interfaces = {ext["array_name"]},'
        extracted = f'static const InterfaceInfo {ext["array_name"]}[] = {{\n{ext["iface_entries"]}\n}};\n\n'
        content = content.replace(ext['old_iface'], new_ref, 1)
        line_start = content.rfind('\n', 0, content.find(ext['type_name'])) + 1
        content = content[:line_start] + extracted + content[line_start:]
    return content, len(extractions)


def fix_memregion_ops_nested(content):
    """Convert MemoryRegionOps nested .valid/.impl to constructor init pattern."""
    pattern = re.compile(
        r'static\s+(const\s+)?MemoryRegionOps\s+(\w+)\s*=\s*\{',
        re.MULTILINE
    )
    inits = []
    for match in pattern.finditer(content):
        is_const = match.group(1) is not None
        ops_name = match.group(2)
        brace_start = content.index('{', match.start())
        brace_depth = 0
        struct_end = None
        for pos in range(brace_start, len(content)):
            if content[pos] == '{': brace_depth += 1
            elif content[pos] == '}':
                brace_depth -= 1
                if brace_depth == 0:
                    struct_end = pos + 1
                    break
        if struct_end is None:
            continue
        struct_text = content[match.start():struct_end]
        nested_fields = []
        for section in ('valid', 'impl'):
            nested_match = re.search(
                rf'\.{section}\s*=\s*\{{([^}}]*)\}}',
                struct_text
            )
            if nested_match:
                inner = nested_match.group(1).strip()
                for field_m in re.finditer(r'\.(\w+)\s*=\s*([^,}]+)', inner):
                    field_name = field_m.group(1)
                    field_val = field_m.group(2).strip()
                    nested_fields.append((section, field_name, field_val))
        if not nested_fields:
            continue
        new_struct = struct_text
        for section in ('valid', 'impl'):
            new_struct = re.sub(
                rf'\s*\.{section}\s*=\s*\{{[^}}]*\}}\s*,?',
                '', new_struct
            )
        if is_const:
            new_struct = new_struct.replace('static const MemoryRegionOps',
                                           'static MemoryRegionOps', 1)
        init_lines = [f'static void __attribute__((constructor)) init_{ops_name}(void) {{']
        for section, field_name, field_val in nested_fields:
            init_lines.append(f'    {ops_name}.{section}.{field_name} = {field_val};')
        init_lines.append('}')
        init_text = '\n'.join(init_lines) + '\n'
        inits.append({
            'old_text': struct_text,
            'new_text': new_struct,
            'init_text': init_text,
        })
    for init in reversed(inits):
        content = content.replace(init['old_text'], init['new_text'], 1)
        pos = content.find(init['new_text']) + len(init['new_text'])
        while pos < len(content) and content[pos] != '\n':
            pos += 1
        content = content[:pos + 1] + '\n' + init['init_text'] + content[pos + 1:]
    return content, len(inits)


def header_has_cpp_guards(header_include_path):
    """Check if a header already has __cplusplus / extern "C" guards.

    Args:
        header_include_path: the path as it appears in #include "..." directives,
                            e.g., "qemu/id.h", "hw/cpu/core.h"
    """
    # Try include/ directory first
    full = QEMU_ROOT / 'include' / header_include_path
    if full.exists():
        try:
            return '__cplusplus' in full.read_text()
        except:
            return False

    # Try as a relative path from QEMU root (for local headers like "monitor-internal.h")
    full = QEMU_ROOT / header_include_path
    if full.exists():
        try:
            return '__cplusplus' in full.read_text()
        except:
            return False

    return False  # Unknown headers are assumed to need wrapping


def restructure_includes(content):
    """Restructure #include directives for C++ compatibility.

    - qemu/osdep.h stays first, outside any extern "C" block
    - Headers with __cplusplus guards are included directly (no wrapping)
    - Headers without guards are wrapped in extern "C" { }
    - C++ standard headers go last

    Returns (new_content, num_changes).
    """
    lines = content.split('\n')

    # Find all include lines and their positions
    includes = []
    first_include_idx = -1
    last_include_idx = -1
    has_osdep = False
    already_has_extern_c = False

    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('#include'):
            if first_include_idx < 0:
                first_include_idx = i
            last_include_idx = i
            m = re.match(r'#include\s+[<"](.+?)[>"]', stripped)
            if m:
                header = m.group(1)
                is_system = '<' in stripped.split('#include')[1].split('>')[0] if '<' in stripped else False
                includes.append((i, header, is_system, stripped))
                if header == 'qemu/osdep.h':
                    has_osdep = True
        if 'extern "C"' in stripped:
            already_has_extern_c = True

    if not includes or already_has_extern_c or first_include_idx < 0:
        return content, 0  # Don't restructure if already has extern C or no includes

    if not has_osdep:
        return content, 0  # Non-standard file, skip

    # Classify includes
    osdep_includes = []      # qemu/osdep.h - always first, outside
    guarded_includes = []    # Headers with __cplusplus guards - outside
    unguarded_includes = []  # C headers without guards - need extern "C"
    cpp_includes = []        # C++ standard library headers
    local_includes = []      # Local relative includes (e.g., "foo.h" without path)

    for idx, header, is_system, original_line in includes:
        if header == 'qemu/osdep.h':
            osdep_includes.append(original_line)
        elif is_system and (header.startswith('c') and not header.endswith('.h')
                          or header in ('string', 'vector', 'map', 'set', 'algorithm',
                                       'memory', 'iostream', 'array', 'unordered_map',
                                       'functional', 'utility', 'cassert', 'cstring',
                                       'cstdlib', 'cstdio', 'cmath', 'limits',
                                       'type_traits', 'numeric', 'bitset')):
            cpp_includes.append(original_line)
        elif header_has_cpp_guards(header):
            guarded_includes.append(original_line)
        elif is_system:
            # System C headers like <sys/mman.h> - include outside (handled by osdep.h)
            guarded_includes.append(original_line)
        elif '/' not in header:
            # Local include like "trace.h" or "internal.h" - check relative to file
            unguarded_includes.append(original_line)
        else:
            unguarded_includes.append(original_line)

    # Only restructure if there are unguarded includes to wrap
    if not unguarded_includes:
        return content, 0

    # Build new include section
    new_includes = []
    for line in osdep_includes:
        new_includes.append(line)
    new_includes.append('')

    if guarded_includes:
        for line in guarded_includes:
            new_includes.append(line)
        new_includes.append('')

    if unguarded_includes:
        new_includes.append('extern "C" {')
        for line in unguarded_includes:
            new_includes.append(line)
        new_includes.append('}')
        new_includes.append('')

    if cpp_includes:
        for line in cpp_includes:
            new_includes.append(line)
        new_includes.append('')

    # Replace the include section in the file
    # Remove old include lines
    new_lines = lines[:first_include_idx]
    new_lines.extend(new_includes)
    # Skip old include section (everything from first to last include line)
    # but keep non-include lines between them
    skip_lines = set()
    for idx, header, is_system, original_line in includes:
        skip_lines.add(idx)
    # Also skip blank lines adjacent to removed includes and existing extern "C" { } wrapping
    for i in range(first_include_idx, last_include_idx + 1):
        if i in skip_lines:
            continue
        stripped = lines[i].strip()
        if stripped == '' or stripped == 'extern "C" {' or stripped == '}':
            continue
        new_lines.append(lines[i])

    # Add rest of file after last include
    new_lines.extend(lines[last_include_idx + 1:])

    return '\n'.join(new_lines), len(unguarded_includes)


def reorder_typeinfo_designators(content):
    """Reorder TypeInfo struct initializer fields to match declaration order.

    TypeInfo field order: name, parent, instance_size, instance_align,
    instance_init, instance_post_init, instance_finalize, is_abstract,
    class_size, class_init, class_base_init, class_data, interfaces
    """
    TYPEINFO_ORDER = [
        'name', 'parent', 'instance_size', 'instance_align',
        'instance_init', 'instance_post_init', 'instance_finalize',
        'is_abstract', 'class_size', 'class_init', 'class_base_init',
        'class_data', 'interfaces',
    ]
    field_rank = {f: i for i, f in enumerate(TYPEINFO_ORDER)}

    count = 0
    # Find TypeInfo struct initializers
    pattern = re.compile(
        r'((?:static\s+)?const\s+TypeInfo\s+\w+\s*=\s*\{)(.*?)(\};)',
        re.DOTALL
    )
    def reorder_match(m):
        nonlocal count
        prefix = m.group(1)
        body = m.group(2)
        suffix = m.group(3)

        # Parse field assignments
        fields = []
        for fm in re.finditer(r'(\s*\.(\w+)\s*=\s*)(.*?)(?=\s*\.\w+\s*=|\s*$)', body, re.DOTALL):
            indent_and_dot = fm.group(1)
            field_name = fm.group(2)
            value = fm.group(3).rstrip().rstrip(',')
            fields.append((field_name, indent_and_dot, value))

        if not fields:
            return m.group(0)

        # Check if reordering is needed
        ranks = [field_rank.get(f[0], 999) for f in fields]
        if ranks == sorted(ranks):
            return m.group(0)  # Already in order

        # Reorder
        fields.sort(key=lambda f: field_rank.get(f[0], 999))
        count += 1

        # Rebuild
        lines = []
        for i, (name, indent_dot, value) in enumerate(fields):
            # Use consistent indentation from first field
            indent = '    '
            comma = ',' if i < len(fields) - 1 else ','
            lines.append(f'{indent}.{name} = {value}{comma}')

        return prefix + '\n' + '\n'.join(lines) + '\n' + suffix

    content = pattern.sub(reorder_match, content)
    return content, count


def fix_sparse_array_initializers(content):
    """Convert C designated array initializers to __attribute__((constructor)) init functions.

    Converts patterns like:
        const uint8_t foo[] = {
            [0x0000] = REG_CTRL,
            [0x0004] = REG_MODE,
        };
    To:
        uint8_t foo[MAX_INDEX + 1];
        static void __attribute__((constructor)) init_foo(void) {
            foo[0x0000] = REG_CTRL;
            foo[0x0004] = REG_MODE;
        }
    """
    count = 0
    # Match array declarations with designated initializers
    pattern = re.compile(
        r'^((?:static\s+)?)(const\s+)?((?:unsigned\s+)?\w+)\s+(\w+)\s*\[\s*\]\s*=\s*\{'
        r'(.*?)'
        r'^\};',
        re.MULTILINE | re.DOTALL
    )

    def replace_sparse(m):
        nonlocal count
        static_kw = m.group(1)
        const_kw = m.group(2) or ''
        elem_type = m.group(3)
        arr_name = m.group(4)
        body = m.group(5)

        # Check if body contains designated array initializers [idx] = val
        entries = re.findall(r'\[([^\]]+)\]\s*=\s*([^,\n]+)', body)
        if not entries:
            return m.group(0)  # Not a sparse array

        # Find max index to size the array
        max_idx = 0
        for idx_str, val in entries:
            idx_str = idx_str.strip()
            try:
                if idx_str.startswith('0x') or idx_str.startswith('0X'):
                    idx_val = int(idx_str, 16)
                else:
                    idx_val = int(idx_str)
                if idx_val > max_idx:
                    max_idx = idx_val
            except ValueError:
                # Non-numeric index (enum constant etc.) — can't compute size
                return m.group(0)

        count += 1
        arr_size = max_idx + 1

        # Build the replacement: array declaration + constructor init function
        lines = []
        # Remove const since we initialize in constructor
        lines.append(f'{static_kw}{elem_type} {arr_name}[{arr_size}];')
        lines.append(f'')
        lines.append(f'static void __attribute__((constructor)) init_{arr_name}(void) {{')
        for idx_str, val in entries:
            val = val.strip().rstrip(',')
            lines.append(f'    {arr_name}[{idx_str.strip()}] = {val};')
        lines.append(f'}}')

        return '\n'.join(lines)

    content = pattern.sub(replace_sparse, content)
    return content, count


def apply_easy_fixes(filepath):
    """Apply mechanical fixes for EASY files."""
    content = filepath.read_text()
    fixes = []

    # Restructure includes with extern "C" wrapping
    content, n = restructure_includes(content)
    if n: fixes.append(f"include restructure ({n} wrapped)")

    # Extract VMStateField compound literals
    content, n = extract_vmstate_fields(content)
    if n: fixes.append(f"VMStateField extraction ({n})")

    # Extract InterfaceInfo compound literals
    content, n = extract_interface_info(content)
    if n: fixes.append(f"InterfaceInfo extraction ({n})")

    # Fix MemoryRegionOps nested designators
    content, n = fix_memregion_ops_nested(content)
    if n: fixes.append(f"MemoryRegionOps nested ({n})")

    # Reorder TypeInfo designators
    content, n = reorder_typeinfo_designators(content)
    if n: fixes.append(f"TypeInfo reorder ({n})")

    # Fix sparse array initializers ([idx] = val)
    content, n = fix_sparse_array_initializers(content)
    if n: fixes.append(f"sparse array init ({n})")

    # PRI macro spacing
    count = 0
    for macro in ['PRI[diouxX]\\d+', 'VADDR_PRI[xXdou]', 'TARGET_FMT_\\w+',
                   'HWADDR_PRI[xXdou]', 'RAM_ADDR_FMT', 'TYPE_FMT_\\w+']:
        new, n = re.subn(rf'"({macro})', r'" \1', content)
        count += n
        content = new
        new, n = re.subn(rf'({macro})"', r'\1 "', content)
        count += n
        content = new
    content = re.sub(r'"  (PRI|VADDR|TARGET_FMT|HWADDR|RAM_ADDR|TYPE_FMT)', r'" \1', content)
    content = re.sub(r'(PRI[diouxX]\d+|VADDR_PRI\w+|HWADDR_PRI\w+|RAM_ADDR_FMT|TYPE_FMT_\w+)  "', r'\1 "', content)
    if count:
        fixes.append(f"PRI macros ({count})")

    # void* param casts
    count = 0
    void_params = r'opaque|pv|user_data|userdata|data|arg|cb_arg|cb_opaque|timer_opaque|s|p|ptr|buf|buffer|mem|ctx|cookie|priv|private_data|handle|info|params|state|dev_opaque'
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
    if count:
        fixes.append(f"void* casts ({count})")

    # g_malloc casts
    count = 0
    funcs = r'g_(?:try_)?malloc0?|g_realloc|g_memdup2|g_try_malloc0?_n'
    gm_pattern = re.compile(
        rf'^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*({funcs})\(',
        re.MULTILINE
    )
    lines = content.split('\n')
    new_lines = []
    for line in lines:
        m = gm_pattern.match(line)
        if m:
            indent, typ, var, func = m.group(1), m.group(2), m.group(3), m.group(4)
            rest = line[m.end():]
            new_line = f'{indent}{typ} *{var} = static_cast<{typ} *>({func}({rest}'
            if new_line.rstrip().endswith(');'):
                new_line = new_line.rstrip()[:-2] + '));'
            new_lines.append(new_line)
            count += 1
        else:
            new_lines.append(line)
    content = '\n'.join(new_lines)
    if count:
        fixes.append(f"g_malloc casts ({count})")

    # C++ keyword renames
    kw_count = 0
    if re.search(r'^\s*\w+\s+\*?new\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(r'\bnew\b(?!_|ly|line|->)', 'new_val', content, flags=re.MULTILINE)
        if new_content != content:
            kw_count += 1
            content = new_content
    if re.search(r'^\s*\w+\s+\*?class\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(r'\bclass\b(?!_|es|ify|ic)', 'klass', content)
        if new_content != content:
            kw_count += 1
            content = new_content
    if re.search(r'^\s*\w+\s+\*?template\s*[=,;)]', content, re.MULTILINE):
        new_content = re.sub(r'\btemplate\b(?!_|s\b)', 'tmpl', content)
        if new_content != content:
            kw_count += 1
            content = new_content
    if kw_count:
        fixes.append(f"C++ keywords ({kw_count})")

    # void* arithmetic
    count = 0
    new, n = re.subn(r'\(void\s*\*\)\s*(\w+)\s*\+', r'static_cast<char *>(\1) +', content)
    count += n
    content = new
    new, n = re.subn(r'\(const\s+void\s*\*\)\s*(\w+)\s*\+', r'static_cast<const char *>(\1) +', content)
    count += n
    content = new
    if count:
        fixes.append(f"void* arithmetic ({count})")

    filepath.write_text(content)
    return fixes


def extract_failing_sources(build_output, ported_cpp_rels=None):
    """Extract source file paths from build errors.

    Args:
        build_output: combined stdout+stderr from ninja
        ported_cpp_rels: set of relative paths of our ported .cpp files,
                        used to resolve linker errors back to source files
    """
    failing = set()

    # Match direct source file errors: ../hw/foo/bar.cpp:123:45: error:
    for m in re.finditer(r'^\.\./(\S+\.cpp):\d+:\d+: error:', build_output, re.MULTILINE):
        failing.add(m.group(1))

    # Match header errors traced to .cpp compilation
    for m in re.finditer(r'^c\+\+.*-c \.\./(\S+\.cpp)$', build_output, re.MULTILINE):
        failing.add(m.group(1))

    # FAILED + compile command pairs
    for m in re.finditer(r'^FAILED:.*\n.*-c \.\./(\S+\.cpp)', build_output, re.MULTILINE):
        failing.add(m.group(1))

    # FAILED lines with .cpp in the object path
    for m in re.finditer(r'^FAILED:.*$', build_output, re.MULTILINE):
        line = m.group(0)
        src_match = re.search(r'-c \.\./(\S+\.cpp)', line)
        if src_match:
            failing.add(src_match.group(1))

    # Linker errors: undefined reference to 'func_name'
    # These show up when a .cpp file defines a function without extern "C"
    # and C code tries to call it. The linker mentions the .o file.
    # Map .o files back to our ported .cpp files.
    if ported_cpp_rels:
        # Build a map from .o basenames to .cpp paths
        # e.g., "hw/foo/bar.cpp" -> possible .o names include "bar.cpp.o"
        basename_to_cpp = {}
        for cpp_rel in ported_cpp_rels:
            base = Path(cpp_rel).stem  # e.g., "bar"
            basename_to_cpp[base] = cpp_rel

        # Look for undefined reference errors mentioning our files
        for m in re.finditer(r'(\S+\.cpp)\.o[:\s]', build_output):
            obj_stem = Path(m.group(1)).stem
            if obj_stem in basename_to_cpp:
                failing.add(basename_to_cpp[obj_stem])

        # Also look for linker errors referencing mangled C++ symbols
        # that indicate missing extern "C" on functions
        if re.search(r'undefined reference to', build_output):
            # Check each FAILED linker line for .o references
            for m in re.finditer(r'^.*undefined reference to.*$', build_output, re.MULTILINE):
                line = m.group(0)
                # Extract .o file path if present
                obj_match = re.search(r'(\S+)\.cpp\.o:', line)
                if obj_match:
                    obj_stem = Path(obj_match.group(1)).stem
                    if obj_stem in basename_to_cpp:
                        failing.add(basename_to_cpp[obj_stem])

    return failing


def add_extern_c_guards_to_header(header_path):
    """Add #ifdef __cplusplus extern "C" guards to a header if missing."""
    if not os.path.exists(header_path):
        return False
    content = open(header_path).read()
    if '__cplusplus' in content:
        return False  # Already has guards

    lines = content.split('\n')
    # Find #ifndef/#define guard pair
    insert_after = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('#define') and i > 0 and lines[i-1].strip().startswith('#ifndef'):
            insert_after = i
            break
    if insert_after < 0:
        return False

    # Find last #endif
    endif_line = -1
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == '#endif':
            endif_line = i
            break
    if endif_line < 0:
        return False

    lines.insert(insert_after + 1, '')
    lines.insert(insert_after + 2, '#ifdef __cplusplus')
    lines.insert(insert_after + 3, 'extern "C" {')
    lines.insert(insert_after + 4, '#endif')

    endif_line += 4  # adjust for insertions
    lines.insert(endif_line, '')
    lines.insert(endif_line + 1, '#ifdef __cplusplus')
    lines.insert(endif_line + 2, '}')
    lines.insert(endif_line + 3, '#endif')

    open(header_path, 'w').write('\n'.join(lines))
    return True


def try_fix_linker_errors(build_output, ported_cpp_rels):
    """Try to fix undefined reference errors by adding extern "C" to function defs
    or header guards. Returns number of fixes applied."""
    fixes = 0

    # Find undefined references in .cpp.o files
    for m in re.finditer(r'(\S+\.cpp)\.o:.*undefined reference to `(\w+)', build_output):
        obj_file = m.group(1)
        func_name = m.group(2)

        # Find the .cpp source file
        cpp_rel = None
        for p in ported_cpp_rels:
            if Path(p).stem == Path(obj_file).stem:
                cpp_rel = p
                break
        if not cpp_rel:
            continue

        cpp_path = QEMU_ROOT / cpp_rel
        if not cpp_path.exists():
            continue

        content = cpp_path.read_text()

        # Check if function is defined in this file (at file scope, not indented)
        func_pattern = re.compile(
            rf'^(\w[\w\s\*]+?)\b({re.escape(func_name)})\s*\(',
            re.MULTILINE
        )
        fm = func_pattern.search(content)
        if fm and 'extern "C"' not in content[max(0, fm.start()-20):fm.start()]:
            # Add extern "C" before the function definition
            content = content[:fm.start()] + 'extern "C"\n' + content[fm.start():]
            cpp_path.write_text(content)
            fixes += 1
            print(f"    Fixed: extern \"C\" on {func_name} in {cpp_rel}")
            continue

        # Try finding the header that declares this function and add guards
        result = subprocess.run(
            ['grep', '-rl', f'{func_name}(', 'include/'],
            capture_output=True, text=True, cwd=QEMU_ROOT
        )
        if result.returncode == 0:
            for h in result.stdout.strip().split('\n'):
                h = h.strip()
                if h.endswith('.h'):
                    header_path = str(QEMU_ROOT / h)
                    if add_extern_c_guards_to_header(header_path):
                        fixes += 1
                        print(f"    Fixed: extern C guards in {h}")
                        break

    return fixes


def build_and_get_failures(ported_cpp_rels=None):
    """Build the project and return (failing_set, success_bool, build_output).

    Args:
        ported_cpp_rels: set of relative paths of .cpp files we ported,
                        used to identify linker errors back to our files
    """
    result = subprocess.run(
        ['ninja', '-C', 'build', '-k0', '-j', str(os.cpu_count())],
        capture_output=True, text=True, timeout=600
    )
    output = result.stdout + result.stderr
    if result.returncode == 0:
        return set(), True, output

    failing = extract_failing_sources(output, ported_cpp_rels)

    return failing, False, output


def reconfigure_meson():
    """Reconfigure meson build system."""
    subprocess.run(
        ['build/pyvenv/bin/meson', 'setup', '--reconfigure', 'build'],
        cwd=QEMU_ROOT, capture_output=True, text=True, timeout=120
    )


def batch_port(files, difficulty, dry_run=False):
    """Port a batch of files."""
    print(f"\n{'='*60}")
    print(f"  Batch porting {len(files)} {difficulty} files")
    print(f"{'='*60}")

    ported = []
    failed = []

    for f, issues in files:
        rel = f.relative_to(QEMU_ROOT)
        cpp_file = f.with_suffix('.cpp')

        if dry_run:
            print(f"  [DRY RUN] Would port {rel}")
            continue

        try:
            # git mv
            subprocess.run(['git', 'mv', str(f), str(cpp_file)],
                         cwd=QEMU_ROOT, check=True, capture_output=True)

            # Update meson.build
            update_meson_build(f, cpp_file)

            # Apply fixes for EASY and MEDIUM files
            fixes = []
            if difficulty in ('EASY', 'MEDIUM'):
                fixes = apply_easy_fixes(cpp_file)

            ported.append((rel, cpp_file, fixes))
            detail = f" ({', '.join(fixes)})" if fixes else ""
            print(f"  OK: {rel}{detail}")

        except Exception as e:
            print(f"  FAIL: {rel}: {e}")
            failed.append((rel, str(e)))
            try:
                subprocess.run(['git', 'checkout', '--', str(f)],
                             cwd=QEMU_ROOT, capture_output=True)
            except:
                pass

    return ported, failed


def get_preexisting_failures():
    """Build before porting and record which files are already failing."""
    result = subprocess.run(
        ['ninja', '-C', 'build', '-k0', '-j', str(os.cpu_count())],
        capture_output=True, text=True, timeout=600
    )
    if result.returncode == 0:
        return set()
    output = result.stdout + result.stderr
    return extract_failing_sources(output)


def build_and_revert_loop(ported, max_iterations=5, preexisting=None):
    """Build, identify failures, revert them, repeat until clean."""
    if not ported:
        return [], []
    if preexisting is None:
        preexisting = set()

    print(f"\n  Reconfiguring meson...")
    reconfigure_meson()

    survived = list(ported)
    all_reverted = []

    for iteration in range(max_iterations):
        # Build set of our ported .cpp relative paths for linker error matching
        ported_cpp_rels = set()
        for rel, cpp_file, fixes in survived:
            ported_cpp_rels.add(str(cpp_file.relative_to(QEMU_ROOT)))

        print(f"\n  Build attempt {iteration + 1} ({len(survived)} files)...")
        failing, success, build_output = build_and_get_failures(ported_cpp_rels)

        # Subtract pre-existing failures
        failing -= preexisting

        if success or not failing:
            if success:
                print(f"  BUILD SUCCESS!")
            else:
                print(f"  BUILD SUCCESS (only pre-existing failures)!")
            break

        # Try to fix linker errors (missing extern "C") before reverting
        if build_output and 'undefined reference' in build_output:
            linker_fixes = try_fix_linker_errors(build_output, ported_cpp_rels)
            if linker_fixes > 0:
                print(f"  Applied {linker_fixes} linker error fixes, rebuilding...")
                failing2, success2, build_output = build_and_get_failures(ported_cpp_rels)
                if success2:
                    print(f"  BUILD SUCCESS after linker fixes!")
                    break
                # Update failing set with new results
                failing = failing2 - preexisting
                if not failing:
                    print(f"  BUILD SUCCESS after linker fixes (only pre-existing failures)!")
                    break

        # Identify which of our ported files are failing
        to_revert = []
        for rel, cpp_file, fixes in survived:
            cpp_rel = str(cpp_file.relative_to(QEMU_ROOT))
            if cpp_rel in failing:
                to_revert.append((rel, cpp_file, fixes))

        if not failing:
            print(f"  BUILD FAILED but could not identify specific failing files.")
            print(f"  Saving build output to /tmp/batch-port-build.log")
            with open('/tmp/batch-port-build.log', 'w') as f:
                f.write(build_output)
            print(f"  Run 'ninja -C build' manually to see errors.")
            break

        if not to_revert:
            print(f"  {len(failing)} files failing but none are from our batch:")
            for f in sorted(failing)[:15]:
                print(f"    {f}")
            break

        print(f"  Reverting {len(to_revert)} failing files...")
        for rel, cpp_file, fixes in to_revert:
            if revert_file(cpp_file):
                print(f"    REVERTED: {rel}")
                all_reverted.append(rel)
                survived = [(r, c, f) for r, c, f in survived
                           if str(c) != str(cpp_file)]

        # Reconfigure after reverting
        reconfigure_meson()

    return survived, all_reverted


def main():
    parser = argparse.ArgumentParser(description='Batch port C files to C++')
    parser.add_argument('--difficulty', choices=['TRIVIAL', 'EASY', 'MEDIUM', 'BOTH', 'ALL'],
                       default='BOTH', help='Difficulty level to port (BOTH=TRIVIAL+EASY, ALL=+MEDIUM)')
    parser.add_argument('--dirs', nargs='+', required=True,
                       help='Directories to scan (relative to QEMU root)')
    parser.add_argument('--list', action='store_true',
                       help='Just list files, do not port')
    parser.add_argument('--dry-run', action='store_true',
                       help='Show what would be done')
    parser.add_argument('--no-build', action='store_true',
                       help='Skip build verification')
    parser.add_argument('--auto-revert', action='store_true', default=True,
                       help='Auto-revert files that fail to compile (default)')
    parser.add_argument('--no-auto-revert', action='store_true',
                       help='Do not auto-revert failing files')
    args = parser.parse_args()

    os.chdir(QEMU_ROOT)

    # Find and classify files
    print("Scanning for .c files...")
    c_files = find_c_files(args.dirs)
    print(f"Found {len(c_files)} .c files")

    classified = classify_files(c_files)

    for diff in ['TRIVIAL', 'EASY', 'MEDIUM', 'HARD']:
        print(f"  {diff}: {len(classified[diff])} files")

    if args.list:
        for diff in ['TRIVIAL', 'EASY', 'MEDIUM', 'HARD']:
            if classified[diff]:
                print(f"\n{diff}:")
                for f, issues in classified[diff]:
                    if '_blocker' in issues:
                        print(f"  {f.relative_to(QEMU_ROOT)} (BLOCKED: {issues['_blocker']})")
                    else:
                        detail = ', '.join(f'{k}={len(v)}' for k, v in issues.items() if v)
                        print(f"  {f.relative_to(QEMU_ROOT)} ({detail or 'clean'})")
        return

    # Check for pre-existing build failures before porting
    preexisting_failures = set()
    if not args.no_build and not args.dry_run:
        print("\nChecking for pre-existing build failures...")
        preexisting_failures = get_preexisting_failures()
        if preexisting_failures:
            print(f"Found {len(preexisting_failures)} pre-existing failures (will ignore):")
            for f in sorted(preexisting_failures)[:10]:
                print(f"  {f}")
            if len(preexisting_failures) > 10:
                print(f"  ... and {len(preexisting_failures) - 10} more")
        else:
            print("No pre-existing failures.")

    # Port files
    total_ported = []
    total_failed = []

    if args.difficulty in ('TRIVIAL', 'BOTH', 'ALL') and classified['TRIVIAL']:
        ported, failed = batch_port(classified['TRIVIAL'], 'TRIVIAL', args.dry_run)
        total_ported.extend(ported)
        total_failed.extend(failed)

    if args.difficulty in ('EASY', 'BOTH', 'ALL') and classified['EASY']:
        ported, failed = batch_port(classified['EASY'], 'EASY', args.dry_run)
        total_ported.extend(ported)
        total_failed.extend(failed)

    if args.difficulty in ('MEDIUM', 'ALL') and classified['MEDIUM']:
        ported, failed = batch_port(classified['MEDIUM'], 'MEDIUM', args.dry_run)
        total_ported.extend(ported)
        total_failed.extend(failed)

    if args.dry_run:
        return

    # Build verification with auto-revert
    if not args.no_build and total_ported:
        if not args.no_auto_revert:
            survived, reverted = build_and_revert_loop(total_ported, preexisting=preexisting_failures)
            print(f"\n{'='*60}")
            print(f"  FINAL SUMMARY")
            print(f"{'='*60}")
            print(f"  Successfully ported: {len(survived)}")
            print(f"  Auto-reverted:       {len(reverted)}")
            print(f"  Failed to rename:    {len(total_failed)}")
            if reverted:
                print(f"\n  Auto-reverted files:")
                for r in reverted:
                    print(f"    {r}")
        else:
            print(f"\n  Building to verify...")
            result = subprocess.run(
                ['ninja', '-C', 'build', '-k0', '-j', str(os.cpu_count())],
                capture_output=True, text=True, timeout=600
            )
            if result.returncode == 0:
                print(f"  BUILD SUCCESS!")
            else:
                errors = re.findall(r'error:', result.stderr + result.stdout)
                print(f"  BUILD FAILED with {len(errors)} errors")
                err_lines = [l for l in (result.stderr + result.stdout).split('\n')
                            if 'error:' in l]
                for l in err_lines[:20]:
                    print(f"    {l.strip()}")
    else:
        print(f"\n  Ported: {len(total_ported)} files (build verification skipped)")


if __name__ == '__main__':
    main()
