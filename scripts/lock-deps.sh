#!/usr/bin/env bash
# Lock (or unlock) a repo's ecosystem dependencies for a release.
#
#   scripts/lock-deps.sh [--local] [--repo <dir>] [--dry-run]
#   scripts/lock-deps.sh --unlock [--repo <dir>]
#
# On main, wlejon/* dependencies track their branch (cmake/bro_deps.cmake). A
# release writes cmake/bro_lock.cmake, one `bro_lock(<name> <sha>)` per
# ecosystem dependency, which cmake/bro_deps.cmake prefers over the branch when
# that repo is the top-level project, so the tag builds exactly those commits.
#
#   (default)     each dependency at the head of its GitHub default branch
#                 (`git ls-remote`, all at once).
#   --local       each at the HEAD of the working tree ../<name>, which must be
#                 pushed before the tag is (warns if not, or if it is dirty).
#   --repo <dir>  lock that repo (default: this bro checkout).
#   --dry-run     print the lock instead of writing it.
#   --unlock      remove cmake/bro_lock.cmake again (on main, after the tag).
#
# The dependency set is every non-third-party bro_dependency() the repo
# declares, closed over the declarations of the ../<name> working trees and the
# deps column of scripts/repos.txt, so transitive dependencies (bronze's brass)
# are locked too. Release flow: lock, commit, tag, push the tag; then --unlock
# and commit on main. Nothing here commits.
set -euo pipefail

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

local_mode=0 unlock=0 dry=0
self="$(cd "$(dirname "$0")/.." && pwd)"
repo="$self"
while (($#)); do
    case "$1" in
        --local) local_mode=1 ;;
        --unlock) unlock=1 ;;
        --dry-run) dry=1 ;;
        --repo) shift; [[ $# -gt 0 ]] || usage 2; repo="$(cd "$1" && pwd)" ;;
        -h|--help) usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 2 ;;
    esac
    shift
done
lock="$repo/cmake/bro_lock.cmake"
root="$(cd "$repo/.." && pwd)"
repo_name="$(basename "$repo")"

if ((unlock)); then
    if [[ -f "$lock" ]]; then
        rm "$lock"
        echo "removed $lock; commit the removal on main"
    else
        echo "no lock at $lock"
    fi
    exit 0
fi

# Every bro_dependency()/bro_dependencies() declaration in a tree's CMake files,
# one "name owner/repo-or-dash third_party(0|1)" line each.
declarations() {
    local dir="$1"
    git -C "$dir" ls-files --cached --others --exclude-standard -- \
        'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' 2>/dev/null |
    grep -v '^cmake/bro_deps.cmake$' | while IFS= read -r f; do printf '%s\n' "$dir/$f"; done |
    tr '\n' '\0' | xargs -0 -r awk '
        { sub(/#.*/, "") }
        inlist {
            line = $0; done = sub(/\).*/, "", line)
            n = split(line, w, /[ \t]+/)
            for (i = 1; i <= n; i++) if (w[i] != "") print w[i], "-", 0
            if (done) inlist = 0
            next
        }
        /^[ \t]*bro_dependencies\(/ {
            line = $0; sub(/^[ \t]*bro_dependencies\(/, "", line)
            done = sub(/\).*/, "", line)
            n = split(line, w, /[ \t]+/)
            for (i = 1; i <= n; i++) if (w[i] != "") print w[i], "-", 0
            if (!done) inlist = 1
            next
        }
        /^[ \t]*bro_dependency\(/ {
            line = $0; sub(/^[ \t]*bro_dependency\(/, "", line)
            n = split(line, w, /[ \t)]+/)
            gh = "-"; tp = 0
            for (i = 2; i <= n; i++) {
                if (w[i] == "GITHUB") gh = w[i + 1]
                if (w[i] == "THIRD_PARTY") tp = 1
            }
            print w[1], gh, tp
        }'
}

declare -A owner third seen_tree
queue=("$repo")
seen_tree["$repo"]=1
names=()
add_name() {  # add_name <name> <owner/repo|-> <tp>
    local n="$1" gh="$2" tp="$3"
    [[ "$n" == "$repo_name" ]] && return
    if [[ -z "${owner[$n]+x}" ]]; then
        owner[$n]="-"; third[$n]=0; names+=("$n")
    fi
    [[ "$gh" != "-" && "${owner[$n]}" == "-" ]] && owner[$n]="$gh"
    [[ "$gh" != "-" && "$gh" != wlejon/* ]] && third[$n]=1
    [[ "$tp" == 1 ]] && third[$n]=1
    if [[ "${third[$n]}" == 0 && -d "$root/$n" && -z "${seen_tree[$root/$n]+x}" ]]; then
        seen_tree["$root/$n"]=1
        queue+=("$root/$n")
    fi
}

# The deps column of scripts/repos.txt, closed transitively.
declare -A listed_deps
if [[ -f "$self/scripts/repos.txt" ]]; then
    while read -r n _ _ _ _ deps; do
        [[ -z "$n" || "$n" == \#* ]] && continue
        listed_deps[$n]="$deps"
    done < "$self/scripts/repos.txt"
fi

i=0
pending=("$repo_name")
while ((i < ${#queue[@]})) || ((${#pending[@]})); do
    while ((i < ${#queue[@]})); do
        while read -r n gh tp; do add_name "$n" "$gh" "$tp"; done < <(declarations "${queue[$i]}")
        i=$((i + 1))
    done
    next=()
    for p in "${pending[@]}"; do
        deps="${listed_deps[$p]:--}"
        [[ "$deps" == "-" ]] && continue
        for d in ${deps//,/ }; do
            [[ -n "${owner[$d]+x}" || "$d" == "$repo_name" ]] && continue
            add_name "$d" - 0
            next+=("$d")
        done
    done
    pending=("${next[@]+"${next[@]}"}")
done

ecosystem=()
for n in "${names[@]}"; do
    ((third[$n])) && continue
    [[ "${owner[$n]}" == "-" ]] && owner[$n]="wlejon/$n"
    ecosystem+=("$n")
done
if ((${#ecosystem[@]} == 0)); then
    echo "$repo declares no ecosystem dependencies; nothing to lock" >&2
    exit 1
fi
mapfile -t ecosystem < <(printf '%s\n' "${ecosystem[@]}" | sort)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for n in "${ecosystem[@]}"; do
    if ((local_mode)) && [[ -d "$root/$n/.git" || -f "$root/$n/.git" ]]; then
        sha="$(git -C "$root/$n" rev-parse HEAD)"
        if ! git -C "$root/$n" branch -r --contains "$sha" 2>/dev/null | grep -q .; then
            echo "warning: $n $sha is not on any remote branch; push it before the tag" >&2
        fi
        if [[ -n "$(git -C "$root/$n" status --porcelain --untracked-files=no)" ]]; then
            echo "warning: $root/$n has uncommitted changes the lock does not include" >&2
        fi
        printf '%s' "$sha" > "$tmp/$n"
    else
        ((local_mode)) && echo "warning: no working tree at $root/$n; locking its GitHub head" >&2
        ( GIT_TERMINAL_PROMPT=0 git ls-remote "https://github.com/${owner[$n]}.git" HEAD |
              awk '{print $1; exit}' > "$tmp/$n" ) &
    fi
done
wait

source_desc="the GitHub default-branch heads"
((local_mode)) && source_desc="the ../<name> working-tree HEADs"
{
    echo "# The release lock (cmake/bro_deps.cmake): every ecosystem dependency at one"
    echo "# commit, preferred over tracking main while this file exists. Written by"
    echo "# bro's scripts/lock-deps.sh from $source_desc."
    echo "# On main, remove it after tagging: scripts/lock-deps.sh --unlock."
    for n in "${ecosystem[@]}"; do
        sha="$(cat "$tmp/$n" 2>/dev/null || true)"
        if [[ ! "$sha" =~ ^[0-9a-f]{40}$ ]]; then
            echo "error: could not resolve $n (${owner[$n]})" >&2
            exit 1
        fi
        echo "bro_lock($n $sha)"
    done
} > "$tmp/lock"

if ((dry)); then
    cat "$tmp/lock"
else
    mkdir -p "$repo/cmake"
    cp "$tmp/lock" "$lock"
    echo "wrote $lock (${#ecosystem[@]} dependencies); review, commit, tag"
fi
