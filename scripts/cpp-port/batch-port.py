#!/usr/bin/env python3
"""
batch-port.py - Batch port TRIVIAL and EASY .c files to .cpp

This script:
1. Scans directories for TRIVIAL/EASY files using auto-port analysis
2. Renames all files in a batch (git mv + meson.build update)
3. Applies mechanical fixes for EASY files
4. Builds once to verify

Usage:
    ./scripts/cpp-port/batch-port.py --difficulty TRIVIAL --dirs hw/virtio hw/arm ...
    ./scripts/cpp-port/batch-port.py --difficulty EASY --dirs hw/misc hw/char ...
    ./scripts/cpp-port/batch-port.py --list --dirs hw/  # just list files
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# Import from auto-port
sys.path.insert(0, str(Path(__file__).parent))
from importlib import import_module

QEMU_ROOT = Path(__file__).resolve().parent.parent.parent

# Inline the analysis functions to avoid import issues
def analyze_file(c_file):
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
        if re.search(r'\(\w+\)\s*\{', line):
            issues['compound_literals'].append((i, line.strip()))
        if re.search(r'\.\w+\s*=', line) and not re.search(r'^\s*\.', line):
            if re.search(r'\.\w+\.\w+\s*=', line):
                issues['designator_inits'].append((i, line.strip()))
        if re.search(r'=\s*(opaque|pv|user_data|userdata|data|arg)\s*;', line):
            if 'static_cast' not in line and 'void' not in line.split('=')[0]:
                issues['void_star_casts'].append((i, line.strip()))
        if re.search(r'"PRI[diouxX]|PRI[diouxX]\d+"', line):
            issues['pri_macros'].append((i, line.strip()))
        for kw in ['new', 'class', 'template', 'typename', 'namespace', 'this', 'delete', 'export']:
            if re.search(rf'^\s*\w+\s+\*?{kw}\s*[=,;)]', line):
                issues['cpp_keywords'].append((i, f'{kw}: {line.strip()}'))
        if re.search(r'\btypeof\b', line) and 'typeof_strip_qual' not in line:
            issues['typeof_usage'].append((i, line.strip()))
        if re.search(r'\[\d+\s*\.\.\.\s*\d+\]', line):
            issues['gnu_extensions'].append((i, line.strip()))
    return issues


def classify_difficulty(issues):
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


# Files to skip (target-specific, windows, etc.)
SKIP_FILES = {
    'os-win32.c', 'os-wasm.c',
}

# Directories that use specific_ss (poisoned macros)
SKIP_DIRS = {
    'target',
    'accel/tcg',
    'accel/kvm',
}


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
            # Skip if already has a .cpp sibling
            if f.with_suffix('.cpp').exists():
                continue
            files.append(f)
    return files


def classify_files(files):
    """Classify files by difficulty."""
    result = {'TRIVIAL': [], 'EASY': [], 'MEDIUM': [], 'HARD': []}
    for f in files:
        try:
            issues = analyze_file(f)
            diff = classify_difficulty(issues)
            result[diff].append((f, issues))
        except Exception as e:
            print(f"WARNING: Could not analyze {f}: {e}")
    return result


def update_meson_build(c_file, cpp_file):
    """Update meson.build to reference .cpp instead of .c."""
    meson_dir = c_file.parent
    meson_file = meson_dir / 'meson.build'
    if not meson_file.exists():
        # Try parent
        meson_file = meson_dir.parent / 'meson.build'
    if not meson_file.exists():
        meson_file = QEMU_ROOT / 'meson.build'

    if meson_file.exists():
        content = meson_file.read_text()
        old_name = c_file.name
        new_name = cpp_file.name
        new_content = content.replace(f"'{old_name}'", f"'{new_name}'")
        if new_content != content:
            meson_file.write_text(new_content)
            return True
    return False


def apply_easy_fixes(filepath):
    """Apply mechanical fixes for EASY files."""
    content = filepath.read_text()
    fixes = []

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
    void_params = r'opaque|pv|user_data|userdata|data|arg|cb_arg|cb_opaque|timer_opaque'
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

            # Apply fixes for EASY files
            fixes = []
            if difficulty == 'EASY':
                fixes = apply_easy_fixes(cpp_file)

            ported.append((rel, fixes))
            detail = f" ({', '.join(fixes)})" if fixes else ""
            print(f"  OK: {rel}{detail}")

        except Exception as e:
            print(f"  FAIL: {rel}: {e}")
            failed.append((rel, str(e)))
            # Try to revert
            try:
                subprocess.run(['git', 'checkout', '--', str(f)],
                             cwd=QEMU_ROOT, capture_output=True)
            except:
                pass

    return ported, failed


def main():
    parser = argparse.ArgumentParser(description='Batch port C files to C++')
    parser.add_argument('--difficulty', choices=['TRIVIAL', 'EASY', 'BOTH'],
                       default='BOTH', help='Difficulty level to port')
    parser.add_argument('--dirs', nargs='+', required=True,
                       help='Directories to scan (relative to QEMU root)')
    parser.add_argument('--list', action='store_true',
                       help='Just list files, do not port')
    parser.add_argument('--dry-run', action='store_true',
                       help='Show what would be done')
    parser.add_argument('--no-build', action='store_true',
                       help='Skip build verification')
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
                    detail = ', '.join(f'{k}={len(v)}' for k, v in issues.items() if v)
                    print(f"  {f.relative_to(QEMU_ROOT)} ({detail or 'clean'})")
        return

    # Port files
    total_ported = []
    total_failed = []

    if args.difficulty in ('TRIVIAL', 'BOTH') and classified['TRIVIAL']:
        ported, failed = batch_port(classified['TRIVIAL'], 'TRIVIAL', args.dry_run)
        total_ported.extend(ported)
        total_failed.extend(failed)

    if args.difficulty in ('EASY', 'BOTH') and classified['EASY']:
        ported, failed = batch_port(classified['EASY'], 'EASY', args.dry_run)
        total_ported.extend(ported)
        total_failed.extend(failed)

    if args.dry_run:
        return

    # Summary
    print(f"\n{'='*60}")
    print(f"  SUMMARY")
    print(f"{'='*60}")
    print(f"  Ported: {len(total_ported)}")
    print(f"  Failed: {len(total_failed)}")

    if total_failed:
        print(f"\n  FAILED:")
        for f, err in total_failed:
            print(f"    {f}: {err}")

    # Build verification
    if not args.no_build and total_ported:
        print(f"\n  Building to verify...")
        result = subprocess.run(
            ['ninja', '-C', 'build', '-j', str(os.cpu_count())],
            capture_output=True, text=True, timeout=600
        )
        if result.returncode == 0:
            print(f"  BUILD SUCCESS!")
        else:
            # Count errors
            errors = re.findall(r'error:', result.stderr + result.stdout)
            print(f"  BUILD FAILED with {len(errors)} errors")
            # Show first few
            err_lines = [l for l in (result.stderr + result.stdout).split('\n')
                        if 'error:' in l]
            for l in err_lines[:20]:
                print(f"    {l.strip()}")

            if len(err_lines) > 20:
                print(f"    ... and {len(err_lines) - 20} more errors")


if __name__ == '__main__':
    main()
