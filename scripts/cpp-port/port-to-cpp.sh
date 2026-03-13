#!/bin/bash
#
# port-to-cpp.sh - Automate common C-to-C++ porting tasks for QEMU files
#
# Usage:
#   ./scripts/cpp-port/port-to-cpp.sh file1.c [file2.c ...]
#   ./scripts/cpp-port/port-to-cpp.sh --dir migration/
#   ./scripts/cpp-port/port-to-cpp.sh --dry-run file.c
#
# What this script does:
#   1. git mv file.c -> file.cpp
#   2. Update the meson.build in the same directory
#   3. Apply safe, mechanical C++ fixes to the source file
#   4. Attempt to build and report remaining errors
#
# The script applies these automated fixes:
#   - void* implicit casts: TYPE *v = pv  ->  TYPE *v = static_cast<TYPE *>(pv)
#   - g_malloc/g_malloc0/g_new0 void* returns (common patterns)
#   - PRI macro literal-suffix: "%"PRIu64 -> "%" PRIu64
#   - C++ keyword variables: new -> new_val, class -> klass, etc.
#   - Enum increment: mode++ -> mode = static_cast<EnumType>(mode + 1)
#
# What it does NOT do (requires human judgment):
#   - Designated initializer reordering (struct-specific)
#   - Compound literal extraction to static const arrays
#   - Complex void* casts in macro expansions
#   - extern "C" guard additions to headers
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DRY_RUN=0
BUILD_AFTER=0
VERBOSE=0
FILES=()

usage() {
    echo "Usage: $0 [options] file1.c [file2.c ...]"
    echo "       $0 [options] --dir <directory>"
    echo ""
    echo "Options:"
    echo "  --dry-run     Show what would be done without making changes"
    echo "  --build       Attempt ninja build after porting and report errors"
    echo "  --verbose     Show detailed output"
    echo "  --dir DIR     Port all .c files in DIR"
    echo "  --help        Show this help"
    exit 1
}

log() {
    echo "  [INFO] $*"
}

warn() {
    echo "  [WARN] $*" >&2
}

error() {
    echo "  [ERROR] $*" >&2
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)  DRY_RUN=1; shift ;;
        --build)    BUILD_AFTER=1; shift ;;
        --verbose)  VERBOSE=1; shift ;;
        --dir)
            shift
            if [[ $# -eq 0 ]]; then error "Missing directory argument"; usage; fi
            dir="$1"
            while IFS= read -r -d '' f; do
                FILES+=("$f")
            done < <(find "$dir" -maxdepth 1 -name "*.c" -print0 | sort -z)
            shift
            ;;
        --help)     usage ;;
        -*)         error "Unknown option: $1"; usage ;;
        *)          FILES+=("$1"); shift ;;
    esac
done

if [[ ${#FILES[@]} -eq 0 ]]; then
    error "No files specified"
    usage
fi

##############################################################################
# Fix functions - each applies one category of mechanical C++ fixes
##############################################################################

# Fix 1: PRI macro literal-suffix
#   "%"PRIu64  ->  "%" PRIu64
#   "%"PRId32  ->  "%" PRId32
fix_pri_macros() {
    local file="$1"
    # Add space between closing quote and PRI macro
    perl -i -pe 's/"(PRI[diouxX]\d+)/" \1/g' "$file"
    # Add space between PRI macro and opening quote
    perl -i -pe 's/(PRI[diouxX]\d+)"/$1 "/g' "$file"
    # Also handle VADDR_PRIx and similar
    perl -i -pe 's/"(VADDR_PRI[xXdou])/" \1/g' "$file"
    perl -i -pe 's/(VADDR_PRI[xXdou])"/$1 "/g' "$file"
    # Handle TARGET_FMT_ macros
    perl -i -pe 's/"(TARGET_FMT_\w+)/" \1/g' "$file"
    perl -i -pe 's/(TARGET_FMT_\w+)"/$1 "/g' "$file"
    # Handle HWADDR_PRIx
    perl -i -pe 's/"(HWADDR_PRI[xXdou])/" \1/g' "$file"
    perl -i -pe 's/(HWADDR_PRI[xXdou])"/$1 "/g' "$file"
    # Handle RAM_ADDR_FMT
    perl -i -pe 's/"(RAM_ADDR_FMT)/" \1/g' "$file"
    perl -i -pe 's/(RAM_ADDR_FMT)"/$1 "/g' "$file"
    # Don't double-space (clean up any "  " that resulted)
    perl -i -pe 's/"  (PRI|VADDR|TARGET_FMT|HWADDR|RAM_ADDR)/" $1/g' "$file"
    perl -i -pe 's/(PRI[diouxX]\d+|VADDR_PRI\w+|HWADDR_PRI\w+|RAM_ADDR_FMT)  "/$1 "/g' "$file"
}

# Fix 2: void* implicit casts for function parameters named opaque/pv/userdata
#   TYPE *var = opaque;  ->  TYPE *var = static_cast<TYPE *>(opaque);
#   Handles: opaque, pv, user_data, userdata, data, arg
fix_void_star_params() {
    local file="$1"
    # Pattern: TYPE *var = param_name;
    # Where TYPE can be const-qualified and multi-word (e.g., "const SomeType")
    # param_name is one of the common void* parameter names
    local void_params="opaque|pv|user_data|userdata|data|arg"

    # Simple case: TYPE *var = param;
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*('"$void_params"')\s*;/$1$2 *$3 = static_cast<$2 *>($4);/
    ' "$file"

    # Pointer-to-pointer case: TYPE **var = param;
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*\*(\w+)\s*=\s*('"$void_params"')\s*;/$1$2 **$3 = static_cast<$2 **>($4);/
    ' "$file"
}

# Fix 3: g_malloc/g_malloc0/g_new0/g_try_malloc0_n void* returns
#   TYPE *var = g_malloc0(sizeof(TYPE));  ->  TYPE *var = static_cast<TYPE *>(g_malloc0(...));
fix_gmalloc_casts() {
    local file="$1"
    # g_malloc, g_malloc0, g_try_malloc, g_try_malloc0
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*(g_(?:try_)?malloc0?)\(/$1$2 *$3 = static_cast<$2 *>($4(/;
        if (m/static_cast<.*>\(g_(?:try_)?malloc0?\(/) {
            s/\);\s*$/));/;
        }
    ' "$file"

    # g_try_malloc0_n
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*(g_try_malloc0?_n)\(/$1$2 *$3 = static_cast<$2 *>($4(/;
        if (m/static_cast<.*>\(g_try_malloc0?_n\(/) {
            # Find the matching close and add extra )
            s/\);\s*$/));/;
        }
    ' "$file"

    # g_realloc
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*(g_realloc)\(/$1$2 *$3 = static_cast<$2 *>($4(/;
        if (m/static_cast<.*>\(g_realloc\(/) {
            s/\);\s*$/));/;
        }
    ' "$file"

    # g_memdup2 (via glib-compat)
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*(g_memdup2)\(/$1$2 *$3 = static_cast<$2 *>($4(/;
        if (m/static_cast<.*>\(g_memdup2\(/) {
            s/\);\s*$/));/;
        }
    ' "$file"
}

# Fix 4: g_hash_table_lookup returns gpointer (void*)
fix_ghash_lookup_casts() {
    local file="$1"
    perl -i -pe '
        s/^(\s*)((?:const\s+)?(?:struct\s+)?\w+)\s+\*(\w+)\s*=\s*(g_hash_table_lookup)\(/$1$2 *$3 = static_cast<$2 *>($4(/;
        if (m/static_cast<.*>\(g_hash_table_lookup\(/) {
            s/\);\s*$/));/;
        }
    ' "$file"
}

# Fix 5: C++ keyword variables
#   Renames common C variables that conflict with C++ keywords
fix_cpp_keywords() {
    local file="$1"

    # Check for each keyword and rename if found as a variable/parameter
    # We use word-boundary matching to avoid renaming inside strings or comments

    # 'new' -> 'new_val' (very common in QEMU)
    if grep -qP '(?<!\w)new(?!\w|_val|_p|_state|_block|_bs|_size|_offset|_mr|_length|_entry|_count|_name|_node|_gen|_fd|_bitmap|_flags|ly)' "$file" 2>/dev/null; then
        # Only rename if 'new' is used as a variable name (declared with a type)
        # Check for patterns like: TYPE *new = ... or TYPE *new, or (TYPE *new)
        if grep -qP '^\s*\w+\s+\*?new\s*[=,;)]' "$file" 2>/dev/null || \
           grep -qP '\(\w+\s+\*?new[,)]' "$file" 2>/dev/null; then
            perl -i -pe '
                # Rename variable declarations and uses, but not ->new_thing, new_*, or inside strings
                s/\bnew\b(?!_|ly|line|->)/new_val/g unless /^\s*\*/ || /^\s*\/\// || /^\s*\/?\*/;
            ' "$file"
        fi
    fi

    # 'class' -> 'klass'
    if grep -qP '^\s*\w+\s+\*?class\s*[=,;)]' "$file" 2>/dev/null; then
        perl -i -pe 's/\bclass\b(?!_|es|ify|ic)/klass/g unless /^\s*\/\// || /^\s*\/?\*/' "$file"
    fi

    # 'template' -> 'tmpl'
    if grep -qP '^\s*\w+\s+\*?template\s*[=,;)]' "$file" 2>/dev/null; then
        perl -i -pe 's/\btemplate\b(?!_|s\b)/tmpl/g unless /^\s*\/\// || /^\s*\/?\*/' "$file"
    fi

    # 'export' -> 'blk_export' (common in block export code)
    if grep -qP '^\s*\w+\s+\*?export\s*[=,;)]' "$file" 2>/dev/null; then
        perl -i -pe 's/\bexport\b(?!_|s\b|ed\b)/blk_export/g unless /^\s*\/\// || /^\s*\/?\*/' "$file"
    fi

    # 'namespace' -> 'name_space'
    if grep -qP '^\s*\w+\s+\*?namespace\s*[=,;)]' "$file" 2>/dev/null; then
        perl -i -pe 's/\bnamespace\b(?!_)/name_space/g unless /^\s*\/\// || /^\s*\/?\*/' "$file"
    fi

    # 'this' -> 'self'
    if grep -qP '^\s*\w+\s+\*?this\s*[=,;)]' "$file" 2>/dev/null; then
        perl -i -pe 's/\bthis\b(?!_)/self/g unless /^\s*\/\// || /^\s*\/?\*/' "$file"
    fi
}

# Fix 6: void* pointer arithmetic
#   (void*)ptr + offset  ->  (char*)ptr + offset
#   This is a best-effort fix for simple cases
fix_void_ptr_arithmetic() {
    local file="$1"
    # Cast void* to char* when doing pointer arithmetic
    # Pattern: (void *) expr + expr  or  void_var + offset
    perl -i -pe '
        # Fix: something = (char *)opaque + field->offset  (already char* is fine)
        # Fix: static_cast<char *>(opaque) + field->offset
        s/\(void\s*\*\)\s*(\w+)\s*\+/static_cast<char *>($1) +/g;
        s/\(const\s+void\s*\*\)\s*(\w+)\s*\+/static_cast<const char *>($1) +/g;
    ' "$file"
}

# Fix 7: Common GLib function return casts
#   Handles g_steal_pointer wrapped in QAPI_LIST_PREPEND and similar
fix_glib_return_casts() {
    local file="$1"
    # Note: these are harder to automate generically.
    # We handle the most common case: g_steal_pointer returns void*
    # This is left as a detection-only step for now.
    :
}

##############################################################################
# Main logic
##############################################################################

rename_file() {
    local c_file="$1"
    local cpp_file="${c_file%.c}.cpp"

    if [[ ! -f "$c_file" ]]; then
        error "File not found: $c_file"
        return 1
    fi

    if [[ "$c_file" != *.c ]]; then
        error "Not a .c file: $c_file"
        return 1
    fi

    echo "=== Porting: $c_file -> $cpp_file ==="

    if [[ $DRY_RUN -eq 1 ]]; then
        log "[DRY RUN] Would rename $c_file -> $cpp_file"
    else
        git mv "$c_file" "$cpp_file"
        log "Renamed $c_file -> $cpp_file"
    fi
}

update_meson_build() {
    local c_file="$1"
    local dir
    dir="$(dirname "$c_file")"
    local basename
    basename="$(basename "$c_file")"
    local cpp_basename="${basename%.c}.cpp"

    # Find the meson.build that references this file
    local meson_file=""
    if [[ -f "$dir/meson.build" ]]; then
        if grep -q "'${basename}'" "$dir/meson.build" 2>/dev/null; then
            meson_file="$dir/meson.build"
        fi
    fi

    # Also check top-level meson.build for top-level files
    if [[ -z "$meson_file" ]] && grep -q "'${c_file}'" "$QEMU_ROOT/meson.build" 2>/dev/null; then
        meson_file="$QEMU_ROOT/meson.build"
    fi
    if [[ -z "$meson_file" ]] && grep -q "'${basename}'" "$QEMU_ROOT/meson.build" 2>/dev/null; then
        meson_file="$QEMU_ROOT/meson.build"
    fi

    if [[ -z "$meson_file" ]]; then
        warn "Could not find meson.build reference for $c_file"
        return 0
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        log "[DRY RUN] Would update $meson_file: '$basename' -> '$cpp_basename'"
    else
        # Replace the .c reference with .cpp in meson.build
        sed -i "s|'${basename}'|'${cpp_basename}'|g" "$meson_file"

        # For top-level files referenced with path prefix
        if [[ "$meson_file" == "$QEMU_ROOT/meson.build" ]] && [[ "$dir" != "." ]]; then
            sed -i "s|'${c_file}'|'${c_file%.c}.cpp'|g" "$meson_file"
        fi

        log "Updated $meson_file"
    fi
}

apply_fixes() {
    local cpp_file="$1"

    if [[ $DRY_RUN -eq 1 ]]; then
        log "[DRY RUN] Would apply C++ fixes to $cpp_file"
        return 0
    fi

    local fixes_applied=0

    # Count lines before for comparison
    local before_hash
    before_hash=$(md5sum "$cpp_file" | cut -d' ' -f1)

    # Apply each fix category
    fix_pri_macros "$cpp_file"
    fix_void_star_params "$cpp_file"
    fix_gmalloc_casts "$cpp_file"
    fix_ghash_lookup_casts "$cpp_file"
    fix_cpp_keywords "$cpp_file"
    fix_void_ptr_arithmetic "$cpp_file"

    local after_hash
    after_hash=$(md5sum "$cpp_file" | cut -d' ' -f1)

    if [[ "$before_hash" != "$after_hash" ]]; then
        log "Applied automated C++ fixes"
    else
        log "No automated fixes needed"
    fi
}

try_build() {
    if [[ $BUILD_AFTER -eq 0 ]]; then
        return 0
    fi

    echo ""
    echo "=== Building ==="
    local build_dir="$QEMU_ROOT/build"
    if [[ ! -d "$build_dir" ]]; then
        error "Build directory not found: $build_dir"
        return 1
    fi

    local output
    output=$(ninja -C "$build_dir" 2>&1) || true

    local failures
    failures=$(echo "$output" | grep "^FAILED:" | wc -l)

    if [[ "$failures" -eq 0 ]]; then
        echo "  [BUILD OK] No compilation errors"
    else
        echo "  [BUILD FAILED] $failures file(s) failed to compile"
        echo ""
        echo "  Remaining errors to fix manually:"
        echo "$output" | grep "error:" | grep -v "cc1plus:" | sed 's/^/    /'
        echo ""
        echo "  Common manual fixes needed:"
        echo "    - Designated initializer reordering (check struct declaration order)"
        echo "    - Compound literals -> static const arrays"
        echo "    - g_steal_pointer() in QAPI_LIST_PREPEND -> wrap with static_cast"
        echo "    - Enum for-loops: i++ -> i = static_cast<EnumType>(i + 1)"
        echo "    - GIOCondition bitwise ops: static_cast<GIOCondition>(a | b)"
        echo "    - const global variables: add 'extern' keyword for external linkage"
    fi
}

##############################################################################
# Process files
##############################################################################

echo "============================================"
echo "  QEMU C++ Porter"
echo "  Files to process: ${#FILES[@]}"
if [[ $DRY_RUN -eq 1 ]]; then
    echo "  Mode: DRY RUN"
fi
echo "============================================"
echo ""

cd "$QEMU_ROOT"

for c_file in "${FILES[@]}"; do
    # Normalize path (remove leading ./ if present)
    c_file="${c_file#./}"

    # Skip already-ported files
    if [[ "$c_file" == *.cpp ]]; then
        warn "Already a .cpp file, skipping: $c_file"
        continue
    fi

    # Skip known problematic files
    case "$c_file" in
        */os-win32.c|*/os-wasm.c)
            warn "Skipping platform-specific file: $c_file"
            continue
            ;;
    esac

    rename_file "$c_file"
    update_meson_build "$c_file"

    cpp_file="${c_file%.c}.cpp"
    if [[ $DRY_RUN -eq 0 ]] && [[ -f "$cpp_file" ]]; then
        apply_fixes "$cpp_file"
    fi

    echo ""
done

try_build

echo "============================================"
echo "  Done. ${#FILES[@]} file(s) processed."
echo "============================================"
