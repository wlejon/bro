#!/usr/bin/env bash
# Keep the ecosystem's dependency declarations in one shape.
#
#   scripts/sync-deps.sh [--check] [<name>...]
#
# For every repo in scripts/repos.txt checked out at ../<name> that carries
# cmake/bro_deps.cmake (or just the named ones):
#
#   - copies bro's cmake/bro_deps.cmake over it when they differ, so the file
#     stays byte-identical everywhere;
#   - drops the `REF <sha>` from every bro_dependency() of a wlejon/* repo in
#     the CMake files git sees, so ecosystem dependencies track their branch.
#     Third-party REFs are left alone. A release pins wlejon repos with
#     cmake/bro_lock.cmake (scripts/lock-deps.sh), never with a REF.
#
#   --check   change nothing: list what differs, exit 1 if anything does.
#
# Nothing is committed; review with git diff in each repo.
set -euo pipefail

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

check=0
names=()
while (($#)); do
    case "$1" in
        --check) check=1 ;;
        -h|--help) usage 0 ;;
        -*) echo "unknown option: $1" >&2; usage 2 ;;
        *) names+=("$1") ;;
    esac
    shift
done

bro="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$bro/.." && pwd)"
master="$bro/cmake/bro_deps.cmake"

if ((${#names[@]} == 0)); then
    while read -r name _; do
        [[ -z "$name" || "$name" == \#* || "$name" == bro ]] && continue
        names+=("$name")
    done < "$bro/scripts/repos.txt"
fi

ref_re='(bro_dependency\([A-Za-z0-9_.-]+ GITHUB wlejon/[A-Za-z0-9_.-]+) REF [0-9a-f]{40}'
issues=0
for name in bro "${names[@]}"; do
    dir="$root/$name"
    [[ "$name" == bro ]] && dir="$bro"
    [[ -f "$dir/cmake/bro_deps.cmake" ]] || continue
    if [[ "$name" != bro ]] && ! cmp -s "$master" "$dir/cmake/bro_deps.cmake"; then
        issues=$((issues + 1))
        if ((check)); then
            echo "$name: cmake/bro_deps.cmake differs from bro's"
        else
            cp "$master" "$dir/cmake/bro_deps.cmake"
            echo "$name: cmake/bro_deps.cmake updated"
        fi
    fi
    while IFS= read -r file; do
        grep -Eq "$ref_re" "$dir/$file" || continue
        while IFS= read -r hit; do
            issues=$((issues + 1))
            if ((check)); then
                echo "$name: $file: ${hit%% REF *} pins a REF"
            else
                echo "$name: $file: ${hit%% REF *}: REF dropped"
            fi
        done < <(grep -Eo "$ref_re" "$dir/$file")
        ((check)) || sed -i -E "s#$ref_re#\\1#g" "$dir/$file"
    done < <(git -C "$dir" ls-files --cached --others --exclude-standard -- \
                 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' | sort -u)
done

if ((check)); then
    ((issues == 0)) && echo "all repos in sync with bro's cmake/bro_deps.cmake" || exit 1
else
    echo "$issues change(s); review with git diff in each repo"
fi
