#!/usr/bin/env bash
# Multi-repo status for the bro ecosystem.
#
# Walks every repo in scripts/repos.txt (bro, bronze/brass, the libraries bro
# links, the desktop substrate libraries, the apps and tools), each a standalone
# checkout at ../<name>, printing the working-tree state of each and how far it
# sits from its upstream. Then, for the repos bro pins (cmake/bro_pins.cmake),
# reports which pins are stale: the working tree you actually build against
# (../<name>) at a different commit than the one a plain clone of bro fetches.
#
# Usage: scripts/repo-status.sh [-v] [-p] [-s] [-u]
#   -v, --verbose   also list changed files for dirty repos
#   -p, --pull      fast-forward every repo to its upstream first, so the
#                   status below reflects the remotes
#   -s, --sync      move bro's stale pins to the working trees' HEADs
#                   (scripts/bump-deps.sh --local) and make a single bro commit
#   -u, --push      push every repo that is ahead of its upstream, bro last so
#                   its pins never name a commit GitHub does not have yet
#
# Ahead/behind (up<n> / dn<n>) is against the upstream as last fetched; --pull
# fetches. Pull is --ff-only: a repo that has diverged, is detached, or has no
# upstream is reported and skipped, never merged.
# Sync only acts on pins where the working tree is ahead of (or diverged from)
# the pin; those whose working tree is *behind* the pin are left alone (pull
# it first). A repo that is not checked out is listed and skipped. See
# docs/ecosystem.md and docs/multi-repo-workflow.md.

set -uo pipefail

cd "$(dirname "$0")/.."
BRO_ROOT="$(pwd)"
PROJECTS_ROOT="$(cd .. && pwd)"
REPOS_FILE="$BRO_ROOT/scripts/repos.txt"

VERBOSE=0
PULL=0
SYNC=0
PUSH=0
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        -p|--pull)    PULL=1 ;;
        -s|--sync)    SYNC=1 ;;
        -u|--push)    PUSH=1 ;;
        -h|--help)
            sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
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

# Pinned siblings: the repos bro pins in cmake/bro_pins.cmake. bronze and brass
# are among them on the same terms as the libraries: bro builds ../bronze and
# ../brass when they are there and the pin otherwise, so a working tree ahead of
# its pin means CI and the nightly build an older one than you do.
PINS_FILE="$BRO_ROOT/cmake/bro_pins.cmake"
SIBLINGS=()
for i in "${!NAMES[@]}"; do
    [[ "${BROREL[$i]}" == "pinned" ]] && SIBLINGS+=("${NAMES[$i]}")
done

# The commit bro pins for <name>, from cmake/bro_pins.cmake ('' if none).
pinned_sha() {
    sed -nE "s/^bro_dependency\\($1 GITHUB [^ ]+ REF ([0-9a-f]{40}).*/\\1/p" "$PINS_FILE" | head -1
}

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

# Fast-forward one repo onto its upstream. Never merges, never rebases (bro's
# pins move via --sync, not via a pull).
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
echo "${BOLD}== Pins (working tree ../<name> vs the commit cmake/bro_pins.cmake pins) ==${N}"

out_of_sync=0
SYNC_NAMES=()   # siblings whose pin should move to the working tree's HEAD
for name in "${SIBLINGS[@]}"; do
    standalone="$PROJECTS_ROOT/$name"

    recorded="$(pinned_sha "$name")"
    if [[ -z "$recorded" ]]; then
        printf '  %-14s %snot pinned in cmake/bro_pins.cmake (scripts/repos.txt says it is)%s\n' "$name" "$Y" "$N"
        continue
    fi

    if ! is_repo "$standalone"; then
        printf '  %-14s %sno working tree - builds use the pin%s\n' "$name" "$DIM" "$N"
        continue
    fi

    head="$(git -C "$standalone" rev-parse HEAD 2>/dev/null || true)"
    if [[ "$head" == "$recorded" ]]; then
        printf '  %-14s %sin sync%s %s(%s)%s\n' "$name" "$G" "$N" "$DIM" "${recorded:0:9}" "$N"
        continue
    fi

    out_of_sync=$((out_of_sync + 1))

    # Describe the divergence if the pinned commit is reachable locally.
    # syncable=1 means moving the pin to the working tree's HEAD is the right fix.
    local_ahead='' local_behind='' rel='' syncable=0
    if git -C "$standalone" cat-file -e "$recorded^{commit}" 2>/dev/null; then
        local_ahead="$(git -C "$standalone" rev-list --count "$recorded..HEAD" 2>/dev/null || echo '?')"
        local_behind="$(git -C "$standalone" rev-list --count "HEAD..$recorded" 2>/dev/null || echo '?')"
        if [[ "$local_ahead" -gt 0 && "$local_behind" -gt 0 ]]; then
            rel="${R}diverged${N} (working tree ${local_ahead} ahead, ${local_behind} behind)"
            syncable=1
        elif [[ "$local_ahead" -gt 0 ]]; then
            rel="${Y}working tree ahead by ${local_ahead}${N} - bro's pin is stale"
            syncable=1
        else
            rel="${Y}working tree behind by ${local_behind}${N} - it needs a pull (--pull)"
        fi
    else
        # Can't compare, but the working tree is the source of truth, so a bump is valid.
        rel="${R}pinned commit not in the working tree${N} (fetch it to compare)"
        syncable=1
    fi

    printf '  %-14s %sSTALE PIN%s - %s\n' "$name" "$R" "$N" "$rel"
    printf '  %14s %spinned %s  working tree %s%s\n' '' "$DIM" "${recorded:0:9}" "${head:0:9}" "$N"

    [[ "$syncable" -eq 1 ]] && SYNC_NAMES+=("$name")
done

# A wlejon pin bro carries that scripts/repos.txt does not mark as one would be
# skipped above without a word; name it instead.
while read -r pin_name; do
    listed=0
    for name in "${SIBLINGS[@]}"; do [[ "$name" == "$pin_name" ]] && listed=1; done
    if [[ "$listed" -eq 0 ]]; then
        printf '  %-14s %sbro pins it but scripts/repos.txt does not mark it pinned - add it there%s\n' \
            "$pin_name" "$Y" "$N"
    fi
done < <(sed -nE 's/^bro_dependency\(([A-Za-z0-9_.-]+) GITHUB wlejon\/.*/\1/p' "$PINS_FILE")

echo
if [[ "$out_of_sync" -eq 0 ]]; then
    echo "${G}All pins match the working trees.${N}"
else
    echo "${Y}${out_of_sync} stale pin(s).${N}"
fi

if [[ "$SYNC" -eq 1 ]]; then
    if [[ "${#SYNC_NAMES[@]}" -eq 0 ]]; then
        echo "${Y}Nothing to sync: stale pins have working trees behind them (pull those first).${N}"
    else
        echo
        echo "${BOLD}== Moving ${#SYNC_NAMES[@]} pin(s) to the working trees' HEADs ==${N}"
        if "$BRO_ROOT/scripts/bump-deps.sh" --local "${SYNC_NAMES[@]}" | sed 's/^/  /'; then
            # One bro commit recording exactly the moved pins (the pathspec keeps
            # any unrelated staged changes out of it).
            names_list="$(printf '%s, ' "${SYNC_NAMES[@]}")"; names_list="${names_list%, }"
            msg="Pin ${names_list} to the working trees' HEADs"
            echo
            if git -C "$BRO_ROOT" commit --quiet -m "$msg" -- cmake/bro_pins.cmake; then
                echo "${G}Committed:${N} $msg"
                git -C "$BRO_ROOT" log -1 --oneline | sed 's/^/  /'
            else
                echo "${R}Commit failed.${N}"
            fi
        else
            echo "${R}bump-deps.sh failed; nothing committed.${N}"
        fi
    fi
elif [[ "$out_of_sync" -gt 0 ]]; then
    echo "${DIM}Re-run with --sync to move bro's pins to the working trees' HEADs and commit.${N}"
fi

if [[ "$PUSH" -eq 1 ]]; then
    echo
    echo "${BOLD}== Pushing (bro last) ==${N}"
    for name in "${NAMES[@]}"; do
        [[ "$name" == "bro" ]] && continue
        repo_push "$name" "$(repo_path "$name")"
    done
    repo_push bro "$BRO_ROOT"
fi

exit 0
