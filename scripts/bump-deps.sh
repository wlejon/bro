#!/usr/bin/env bash
# Move bro_dependency() pins (see cmake/bro_deps.cmake) to newer commits.
#
#   scripts/bump-deps.sh [--local] [--repo <dir>] [<name>...]
#
# Rewrites `bro_dependency(<name> GITHUB <owner/repo> REF <sha>` in the
# CMakeLists.txt / *.cmake files git sees in a repo (tracked, or untracked and
# not ignored; default repo: this bro checkout).
#
#   (default)     each pin moves to the HEAD of its GitHub repo (origin/main
#                 for the wlejon/* siblings), read with `git ls-remote`.
#   --local       each pin moves to the HEAD of the working tree ../<name>
#                 beside the repo, which must be pushed before the pin is.
#   --repo <dir>  rewrite the pins of another repo (a sibling) instead.
#   <name>...     only these dependencies. Without names, every wlejon/* pin;
#                 third-party pins move only when named.
#
# The file is edited in place; review with `git diff` and commit as usual.
set -euo pipefail

usage() { sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

local_mode=0
repo="$(cd "$(dirname "$0")/.." && pwd)"
names=()
while (($#)); do
    case "$1" in
        --local) local_mode=1 ;;
        --repo) shift; [[ $# -gt 0 ]] || usage 2; repo="$(cd "$1" && pwd)" ;;
        -h|--help) usage 0 ;;
        -*) echo "unknown option: $1" >&2; usage 2 ;;
        *) names+=("$1") ;;
    esac
    shift
done

wanted() {  # wanted <name> <owner/repo>
    if ((${#names[@]})); then
        local n
        for n in "${names[@]}"; do [[ "$n" == "$1" ]] && return 0; done
        return 1
    fi
    [[ "$2" == wlejon/* ]]
}

new_sha() {  # new_sha <name> <owner/repo>
    if ((local_mode)); then
        local dir="$repo/../$1"
        [[ -d "$dir/.git" || -f "$dir/.git" ]] || { echo "no working tree at $dir" >&2; return 1; }
        local sha
        sha="$(git -C "$dir" rev-parse HEAD)"
        if ! git -C "$dir" branch -r --contains "$sha" 2>/dev/null | grep -q .; then
            echo "warning: $1 $sha is not on any remote branch yet; push it before this pin" >&2
        fi
        if [[ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]]; then
            echo "warning: $dir has uncommitted changes the pin does not include" >&2
        fi
        echo "$sha"
    else
        git ls-remote "https://github.com/$2.git" HEAD | awk '{print $1; exit}'
    fi
}

pin_re='bro_dependency\(([A-Za-z0-9_.-]+) GITHUB ([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+) REF ([0-9a-f]{40})'
changed=0
seen=()
while IFS= read -r file; do
    path="$repo/$file"
    grep -Eo "$pin_re" "$path" >/dev/null 2>&1 || continue
    while IFS= read -r match; do
        [[ "$match" =~ $pin_re ]] || continue
        name="${BASH_REMATCH[1]}" gh="${BASH_REMATCH[2]}" old="${BASH_REMATCH[3]}"
        wanted "$name" "$gh" || continue
        new="$(new_sha "$name" "$gh")"
        [[ "$new" =~ ^[0-9a-f]{40}$ ]] || { echo "error: no commit for $name ($gh)" >&2; exit 1; }
        seen+=("$name")
        if [[ "$new" == "$old" ]]; then
            echo "$file: $name already at ${old:0:12}"
            continue
        fi
        sed -i -E "s#(bro_dependency\\($name GITHUB $gh REF )$old#\\1$new#" "$path"
        echo "$file: $name ${old:0:12} -> ${new:0:12}"
        changed=$((changed + 1))
    done < <(grep -Eo "$pin_re" "$path")
done < <(git -C "$repo" ls-files --cached --others --exclude-standard -- \
              'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' | sort -u)

for n in "${names[@]}"; do
    printf '%s\n' "${seen[@]}" | grep -qx "$n" || echo "warning: no pin named $n in $repo" >&2
done
echo "$changed pin(s) changed in $repo"
