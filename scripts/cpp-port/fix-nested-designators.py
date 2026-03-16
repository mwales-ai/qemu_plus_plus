#!/usr/bin/env python3
"""
fix-nested-designators.py - Fix nested designator initializers for C++ compatibility.

Transforms patterns like:
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,

Into:
    .valid = { .min_access_size = 1, .max_access_size = 4, },
    .impl = { .min_access_size = 1, .max_access_size = 4, },

This is required because C++ does not support nested designated initializers.
"""

import re
import sys
from pathlib import Path


def fix_nested_designators(content):
    """Fix nested designator initializers in C source code."""
    lines = content.split('\n')
    result = []
    i = 0
    changes = 0

    while i < len(lines):
        line = lines[i]

        # Check if this line has a nested designator like .foo.bar =
        # Only match simple single-line values (not block openers like "= {")
        m = re.match(r'^(\s*)\.(\w+)\.(\w+)\s*=\s*(.+?),?\s*$', line)
        if m:
            indent = m.group(1)
            parent = m.group(2)
            child = m.group(3)
            value = m.group(4).rstrip(',').strip()

            # Skip if value opens a multi-line block (ends with '{' or is just '{')
            if value.endswith('{') or value == '{':
                result.append(line)
                i += 1
                continue

            # Collect all consecutive lines with the same parent (simple values only)
            members = [(child, value)]
            j = i + 1
            while j < len(lines):
                m2 = re.match(rf'^(\s*)\.{re.escape(parent)}\.(\w+)\s*=\s*(.+?),?\s*$', lines[j])
                if m2:
                    val = m2.group(3).rstrip(',').strip()
                    if val.endswith('{') or val == '{':
                        break
                    members.append((m2.group(2), val))
                    j += 1
                else:
                    break

            # Generate flattened initialization
            inner = ', '.join(f'.{name} = {val}' for name, val in members)
            result.append(f'{indent}.{parent} = {{ {inner}, }},')
            changes += len(members)
            i = j
        else:
            result.append(line)
            i += 1

    return '\n'.join(result), changes


def process_file(filepath, dry_run=False):
    """Process a single file."""
    content = filepath.read_text()
    new_content, changes = fix_nested_designators(content)

    if changes > 0:
        if not dry_run:
            filepath.write_text(new_content)
        return changes
    return 0


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Fix nested designator initializers')
    parser.add_argument('files', nargs='*', help='Files to process')
    parser.add_argument('--scan-dir', help='Scan directory for files with nested designators')
    parser.add_argument('--dry-run', action='store_true', help='Show what would be done')
    args = parser.parse_args()

    if args.scan_dir:
        scan_dir = Path(args.scan_dir)
        files = []
        for ext in ('*.c', '*.cpp'):
            for f in scan_dir.rglob(ext):
                content = f.read_text()
                if re.search(r'\.\w+\.\w+\s*=', content):
                    files.append(f)
        print(f"Found {len(files)} files with nested designators in {args.scan_dir}")
        for f in sorted(files):
            changes = process_file(f, dry_run=True)
            if changes:
                print(f"  {f}: {changes} nested designators")
        return

    for filepath in args.files:
        p = Path(filepath)
        changes = process_file(p, args.dry_run)
        if changes:
            action = "Would fix" if args.dry_run else "Fixed"
            print(f"{action} {changes} nested designators in {filepath}")
        else:
            print(f"No nested designators found in {filepath}")


if __name__ == '__main__':
    main()
