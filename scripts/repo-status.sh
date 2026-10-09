#!/usr/bin/env bash
# Multi-repo status for the bro ecosystem.
#
# Walks every repo in scripts/repos.txt (bro, bronze/brass, the libraries bro
# links, the desktop substrate libraries, the apps and tools), each a standalone
# checkout at ../<name>, printing the working-tree state of each and how far it
# sits from its upstream. Then checks the dependency declarations: every repo's
# cmake/bro_deps.cmake identical to bro's, no wlejon/* dependency pinned with a
# REF (they track main), no release lock (cmake/bro_lock.cmake) left on a
# checkout, and bro's declared dependency list matching scripts/repos.txt.
#
# Usage: scripts/repo-status.sh [-v] [-p] [-u]
#   -v, --verbose   also list changed files for dirty repos
#   -p, --pull      fast-forward every repo to its upstream first, so the
#                   status below reflects the remotes
#   -u, --push      push every repo that is ahead of its upstream: the
#                   libraries first, then bro, then the apps that build on it,
#                   so a consumer's main never needs a dependency commit GitHub
#                   does not have yet
#
# Ahead/behind (up<n> / dn<n>) is against the upstream as last fetched; --pull
# fetches. Pull is --ff-only: a repo that has diverged, is detached, or has no
# upstream is reported and skipped, never merged. Dependencies track main, so
# CI and a fresh clone build what is pushed: an up<n> is invisible to them. A
# repo that is not checked out is listed and skipped. See docs/ecosystem.md and
# docs/multi-repo-workflow.md.

set -uo pipefail

cd "$(dirname "$0")/.."
BRO_ROOT="$(pwd)"
PROJECTS_ROOT="$(cd .. && pwd)"
REPOS_FILE="$BRO_ROOT/scripts/repos.txt"

VERBOSE=0
PULL=0
PUSH=0
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        -p|--pull)    PULL=1 ;;
        -u|--push)    PUSH=1 ;;
        -h|--help)
            sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

if [[ ! -f "$REPOS_FILE" ]]; then
    echo "missing $REPOS_FILE" >&2
    exit 1
fi

# The repo list: name, group and bro relation, from scripts/repos.txt.
NAMES=(); GROUPS_OF=(); BROREL=()
while read -r name group brorel _rest; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    NAMES+=("$name"); GROUPS_OF+=("$group"); BROREL+=("$brorel")
done < "$REPOS_FILE"

repo_path() {
    if [[ "$1" == "bro" ]]; then printf '%s' "$BRO_ROOT"; else printf '%s' "$PROJECTS_ROOT/$1"; fi
}

is_repo() { [[ -d "$1/.git" || -f "$1/.git" ]]; }

# ANSI colors (disabled when not a tty).
if [[ -t 1 ]]; then
    R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[34m'; DIM=$'\033[2m'; BOLD=$'\033[1m'; N=$'\033[0m'
else
    R=''; G=''; Y=''; B=''; DIM=''; BOLD=''; N=''
fi

# Print the working-tree state of a single git repo.
# Args: <label> <path>
repo_state() {
    local label="$1" path="$2"
    if ! is_repo "$path"; then
        printf '  %-14s %snot checked out (github.com/wlejon/%s)%s\n' "$label" "$DIM" "$label" "$N"
        return
    fi

    local branch ahead behind dirty staged untracked upstream tracking=''
    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    [[ "$branch" == "HEAD" ]] && branch="(detached @ $(git -C "$path" rev-parse --short HEAD))"

    dirty="$(git -C "$path" diff --shortstat 2>/dev/null | grep -oE '[0-9]+ file' | grep -oE '[0-9]+' || true)"
    staged="$(git -C "$path" diff --cached --name-only 2>/dev/null | wc -l | tr -d ' ')"
    untracked="$(git -C "$path" ls-files --others --exclude-standard 2>/dev/null | wc -l | tr -d ' ')"

    # Ahead/behind vs upstream, if one is configured.
    upstream="$(git -C "$path" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    if [[ -n "$upstream" ]]; then
        ahead="$(git -C "$path" rev-list --count '@{u}..HEAD' 2>/dev/null || echo 0)"
        behind="$(git -C "$path" rev-list --count 'HEAD..@{u}' 2>/dev/null || echo 0)"
        [[ "$ahead" -gt 0 ]] && tracking+=" ${Y}up${ahead}${N}"
        [[ "$behind" -gt 0 ]] && tracking+=" ${Y}dn${behind}${N}"
        # Name an upstream that is not on origin (a repo tracking another machine).
        [[ "$upstream" != origin/* ]] && tracking+=" ${DIM}[${upstream}]${N}"
    elif [[ "$branch" != \(detached* ]]; then
        tracking+=" ${DIM}no upstream${N}"
    fi

    local flags=''
    [[ -n "$dirty" && "$dirty" -gt 0 ]] && flags+=" ${R}~${dirty}${N}"
    [[ "$staged" -gt 0 ]] && flags+=" ${Y}+${staged} staged${N}"
    [[ "$untracked" -gt 0 ]] && flags+=" ${DIM}?${untracked}${N}"

    local clean=""
    [[ -z "$flags" ]] && clean="${G}clean${N}"

    printf '  %-14s %s%s%s%s%s %s\n' "$label" "$B" "$branch" "$N" "$tracking" "$flags" "$clean"

    if [[ "$VERBOSE" -eq 1 && -n "$flags" ]]; then
        git -C "$path" status --porcelain 2>/dev/null | sed 's/^/      /'
    fi
}

# Fast-forward one repo onto its upstream. Never merges, never rebases.
# Args: <label> <path>
repo_pull() {
    local label="$1" path="$2" branch upstream before after n out
    if ! is_repo "$path"; then
        printf '  %-14s %snot checked out%s\n' "$label" "$DIM" "$N"
        return
    fi

    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    if [[ "$branch" == "HEAD" ]]; then
        printf '  %-14s %sskip: detached HEAD%s\n' "$label" "$Y" "$N"
        return
    fi

    upstream="$(git -C "$path" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    if [[ -z "$upstream" ]]; then
        printf '  %-14s %sskip: no upstream configured%s\n' "$label" "$DIM" "$N"
        return
    fi

    before="$(git -C "$path" rev-parse HEAD)"
    # -c pull.rebase=false: a repo configured to rebase on pull refuses outright
    # when the tree is dirty, even for a fast-forward. --ff-only never merges, so
    # forcing the merge backend here only removes that false failure.
    if ! out="$(git -C "$path" -c pull.rebase=false pull --ff-only --no-recurse-submodules --quiet 2>&1)"; then
        printf '  %-14s %spull failed%s %s%s%s\n' "$label" "$R" "$N" "$DIM" \
            "$(printf '%s' "$out" | grep -m1 . || true)" "$N"
        return
    fi

    after="$(git -C "$path" rev-parse HEAD)"
    if [[ "$after" == "$before" ]]; then
        printf '  %-14s %sup to date%s %s(%s)%s\n' "$label" "$G" "$N" "$DIM" "${before:0:9}" "$N"
        return
    fi

    n="$(git -C "$path" rev-list --count "$before..$after" 2>/dev/null || echo '?')"
    printf '  %-14s %sfast-forwarded +%s%s %s%s -> %s%s\n' \
        "$label" "$Y" "$n" "$N" "$DIM" "${before:0:9}" "${after:0:9}" "$N"
}

# Push one repo to its upstream.
# Args: <label> <path>
repo_push() {
    local label="$1" path="$2" branch upstream ahead out head
    if ! is_repo "$path"; then
        printf '  %-14s %snot checked out%s\n' "$label" "$DIM" "$N"
        return
    fi

    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    if [[ "$branch" == "HEAD" ]]; then
        printf '  %-14s %sskip: detached HEAD%s\n' "$label" "$Y" "$N"
        return
    fi

    upstream="$(git -C "$path" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    if [[ -z "$upstream" ]]; then
        printf '  %-14s %sskip: no upstream configured%s\n' "$label" "$DIM" "$N"
        return
    fi

    ahead="$(git -C "$path" rev-list --count '@{u}..HEAD' 2>/dev/null || echo 0)"
    if [[ "$ahead" -eq 0 ]]; then
        head="$(git -C "$path" rev-parse HEAD 2>/dev/null)"
        printf '  %-14s %sup to date%s %s(%s)%s\n' "$label" "$G" "$N" "$DIM" "${head:0:9}" "$N"
        return
    fi

    if ! out="$(git -C "$path" push --quiet 2>&1)"; then
        printf '  %-14s %spush failed%s %s%s%s\n' "$label" "$R" "$N" "$DIM" \
            "$(printf '%s' "$out" | grep -m1 . || true)" "$N"
        return
    fi

    head="$(git -C "$path" rev-parse HEAD 2>/dev/null)"
    printf '  %-14s %spushed +%s%s %s(%s)%s\n' \
        "$label" "$G" "$ahead" "$N" "$DIM" "${head:0:9}" "$N"
}

if [[ "$PULL" -eq 1 ]]; then
    echo "${BOLD}== Pulling (fast-forward only) ==${N}"
    for name in "${NAMES[@]}"; do
        repo_pull "$name" "$(repo_path "$name")"
    done
    echo
fi

echo "${BOLD}== Repo state ==${N}"
prev_group=''
for i in "${!NAMES[@]}"; do
    if [[ "${GROUPS_OF[$i]}" != "$prev_group" ]]; then
        prev_group="${GROUPS_OF[$i]}"
        echo " ${DIM}${prev_group}${N}"
    fi
    repo_state "${NAMES[$i]}" "$(repo_path "${NAMES[$i]}")"
done

echo
echo "${BOLD}== Dependencies (cmake/bro_deps.cmake) ==${N}"

# Per repo: a bro_deps.cmake that drifted from bro's, a wlejon dependency pinned
# with a REF, or a release lock.
dep_issues=0
for name in "${NAMES[@]}"; do
    path="$(repo_path "$name")"
    is_repo "$path" || continue
    notes=''
    if [[ "$name" != bro && -f "$path/cmake/bro_deps.cmake" ]] &&
       ! cmp -s "$BRO_ROOT/cmake/bro_deps.cmake" "$path/cmake/bro_deps.cmake"; then
        notes+="; ${Y}cmake/bro_deps.cmake differs from bro's${N}"
    fi
    refs="$(git -C "$path" grep -hoE 'bro_dependency\([A-Za-z0-9_.-]+ GITHUB wlejon/[A-Za-z0-9_.-]+ REF [0-9a-f]{40}' \
                -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' 2>/dev/null |
            sed -E 's/bro_dependency\(([^ ]+).*/\1/' | sort -u | tr '\n' ' ')"
    [[ -n "$refs" ]] && notes+="; ${Y}pinned with a REF: ${refs% }${N}"
    if [[ -f "$path/cmake/bro_lock.cmake" ]]; then
        locked="$(grep -c '^bro_lock(' "$path/cmake/bro_lock.cmake")"
        notes+="; ${R}LOCKED${N} (${locked} dependencies in cmake/bro_lock.cmake)"
    fi
    [[ -z "$notes" ]] && continue
    dep_issues=$((dep_issues + 1))
    printf '  %-14s %s\n' "$name" "${notes#; }"
done

# bro's declared dependencies (bro_dependencies() in cmake/bro_pins.cmake)
# against the repos scripts/repos.txt marks `dep`.
declared="$(awk '/^bro_dependencies\(/ { f = 1; next } f && /^\)/ { f = 0 }
                 f { sub(/#.*/, ""); print }' "$BRO_ROOT/cmake/bro_pins.cmake" |
            tr -s ' \t' '\n' | grep -v '^$' | sort)"
listed="$(for i in "${!NAMES[@]}"; do [[ "${BROREL[$i]}" == dep ]] && echo "${NAMES[$i]}"; done | sort)"
while read -r x; do
    [[ -z "$x" ]] && continue
    printf '  %-14s %sdeclared in cmake/bro_pins.cmake, not marked dep in scripts/repos.txt%s\n' "$x" "$Y" "$N"
    dep_issues=$((dep_issues + 1))
done < <(comm -23 <(echo "$declared") <(echo "$listed"))
while read -r x; do
    [[ -z "$x" ]] && continue
    printf '  %-14s %smarked dep in scripts/repos.txt, not declared in cmake/bro_pins.cmake%s\n' "$x" "$Y" "$N"
    dep_issues=$((dep_issues + 1))
done < <(comm -13 <(echo "$declared") <(echo "$listed"))

if [[ "$dep_issues" -eq 0 ]]; then
    echo "  ${G}Every repo carries bro's cmake/bro_deps.cmake; no REF pins, no locks.${N}"
else
    echo "  ${DIM}scripts/sync-deps.sh fixes drift and REF pins; a lock belongs on a release tag (scripts/lock-deps.sh --unlock).${N}"
fi

if [[ "$PUSH" -eq 1 ]]; then
    echo
    echo "${BOLD}== Pushing (libraries, then bro, then apps and tools) ==${N}"
    for i in "${!NAMES[@]}"; do
        [[ "${NAMES[$i]}" == bro || "${GROUPS_OF[$i]}" == app || "${GROUPS_OF[$i]}" == tool ]] && continue
        repo_push "${NAMES[$i]}" "$(repo_path "${NAMES[$i]}")"
    done
    repo_push bro "$BRO_ROOT"
    for i in "${!NAMES[@]}"; do
        [[ "${GROUPS_OF[$i]}" == app || "${GROUPS_OF[$i]}" == tool ]] || continue
        repo_push "${NAMES[$i]}" "$(repo_path "${NAMES[$i]}")"
    done
fi

exit 0
