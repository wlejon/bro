#!/usr/bin/env bash
# Multi-repo status for the bro stack.
#
# Walks bro + its sibling libraries (standalone repos at ../<name>) + broworkshop,
# printing the working-tree state of each, then reports which siblings are out of
# submodule sync: i.e. the standalone repo you actually build against (../<name>)
# sits at a different commit than the pointer bro records in third_party/<name>.
#
# Usage: scripts/repo-status.sh [-v] [-p] [-s]
#   -v, --verbose   also list changed files for dirty repos
#   -p, --pull      fast-forward every repo (bro, broworkshop, each sibling) to
#                   its upstream first, so the status below reflects the remotes
#   -s, --sync      bump bro's stale submodule pointers up to the standalone
#                   repos' HEADs and make a single bro commit recording it
#
# Pull is --ff-only and never recurses into submodules: a repo that has diverged,
# is detached, or has no upstream is reported and skipped, never merged.
# Sync only acts on siblings where the standalone repo is ahead of (or diverged
# from) bro's recorded pointer. Siblings whose standalone is *behind* bro are
# left alone (pull the standalone first); the apps tree has no submodule.
#
# See docs/multi-repo-workflow.md for the layout this reflects.

set -uo pipefail

cd "$(dirname "$0")/.."
BRO_ROOT="$(pwd)"
PROJECTS_ROOT="$(cd .. && pwd)"

VERBOSE=0
PULL=0
SYNC=0
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        -p|--pull)    PULL=1 ;;
        -s|--sync)    SYNC=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

# Sibling libraries: <name> => standalone at ../<name>, submodule at third_party/<name>.
# bronze is in this list on the same terms as the rest even though it is a
# compiler rather than a library bro links: bro resolves ../bronze first and
# third_party/bronze second (src/bronze_host/CMakeLists.txt), so the standalone
# tree being ahead of the recorded pointer means exactly what it means for the
# others — CI and the nightly package are building an older bronze than you are.
SIBLINGS=(
    bromath qjsbind brokit htmlayout broaudio bromesh broflora
    brotensor brogameagent brolm brodiffusion broimage brosoundml brovisionml
    bronze
)

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
    if [[ ! -d "$path/.git" && ! -f "$path/.git" ]]; then
        printf '  %-14s %sno git repo (%s)%s\n' "$label" "$DIM" "$path" "$N"
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

# Fast-forward one repo onto its upstream. Never merges, never rebases, and never
# recurses into submodules (bro's pointers move via --sync, not via a pull).
# Args: <label> <path>
repo_pull() {
    local label="$1" path="$2" branch upstream before after n out
    if [[ ! -d "$path/.git" && ! -f "$path/.git" ]]; then
        printf '  %-14s %sno git repo (%s)%s\n' "$label" "$DIM" "$path" "$N"
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

if [[ "$PULL" -eq 1 ]]; then
    echo "${BOLD}== Pulling (fast-forward only) ==${N}"
    repo_pull "bro" "$BRO_ROOT"
    repo_pull "broworkshop" "$PROJECTS_ROOT/broworkshop"
    repo_pull "brosurface" "$PROJECTS_ROOT/brosurface"
    for name in "${SIBLINGS[@]}"; do
        repo_pull "$name" "$PROJECTS_ROOT/$name"
    done
    echo
fi

echo "${BOLD}== Repo state ==${N}"
repo_state "bro" "$BRO_ROOT"
repo_state "broworkshop" "$PROJECTS_ROOT/broworkshop"
repo_state "brosurface" "$PROJECTS_ROOT/brosurface"
for name in "${SIBLINGS[@]}"; do
    repo_state "$name" "$PROJECTS_ROOT/$name"
done

echo
echo "${BOLD}== Submodule sync (standalone ../<name> vs bro's recorded pointer) ==${N}"

out_of_sync=0
SYNC_NAMES=()   # siblings whose pointer should be bumped to standalone HEAD
SYNC_SHAS=()    # matching standalone HEAD sha for each
for name in "${SIBLINGS[@]}"; do
    standalone="$PROJECTS_ROOT/$name"
    sub_path="third_party/$name"

    # Commit bro records for this submodule in its HEAD tree. --verify, because
    # a bare rev-parse echoes an argument it could not resolve straight back at
    # you: a path that is not a recorded submodule would come out as the literal
    # "HEAD:third_party/<name>" and be compared as though it were a sha.
    recorded="$(git -C "$BRO_ROOT" rev-parse --verify --quiet "HEAD:$sub_path" 2>/dev/null || true)"
    if [[ -z "$recorded" ]]; then
        printf '  %-14s %snot a recorded submodule%s\n' "$name" "$DIM" "$N"
        continue
    fi

    if [[ ! -d "$standalone/.git" && ! -f "$standalone/.git" ]]; then
        printf '  %-14s %sstandalone repo missing - using submodule only%s\n' "$name" "$DIM" "$N"
        continue
    fi

    head="$(git -C "$standalone" rev-parse HEAD 2>/dev/null || true)"
    if [[ "$head" == "$recorded" ]]; then
        printf '  %-14s %sin sync%s %s(%s)%s\n' "$name" "$G" "$N" "$DIM" "${recorded:0:9}" "$N"
        continue
    fi

    out_of_sync=$((out_of_sync + 1))

    # Try to describe the divergence if the recorded commit is reachable locally.
    # syncable=1 means bumping bro's pointer to standalone HEAD is the right fix.
    local_ahead='' local_behind='' rel='' syncable=0
    if git -C "$standalone" cat-file -e "$recorded^{commit}" 2>/dev/null; then
        local_ahead="$(git -C "$standalone" rev-list --count "$recorded..HEAD" 2>/dev/null || echo '?')"
        local_behind="$(git -C "$standalone" rev-list --count "HEAD..$recorded" 2>/dev/null || echo '?')"
        if [[ "$local_ahead" -gt 0 && "$local_behind" -gt 0 ]]; then
            rel="${R}diverged${N} (standalone ${local_ahead} ahead, ${local_behind} behind)"
            syncable=1
        elif [[ "$local_ahead" -gt 0 ]]; then
            rel="${Y}standalone ahead by ${local_ahead}${N} - bro pointer is stale"
            syncable=1
        else
            rel="${Y}standalone behind by ${local_behind}${N} - standalone needs a pull (--pull)"
        fi
    else
        # Can't compare, but standalone is the source of truth, so a bump is valid.
        rel="${R}recorded commit not in standalone${N} (will fetch on sync)"
        syncable=1
    fi

    printf '  %-14s %sOUT OF SYNC%s - %s\n' "$name" "$R" "$N" "$rel"
    printf '  %14s %srecorded %s  standalone %s%s\n' '' "$DIM" "${recorded:0:9}" "${head:0:9}" "$N"

    if [[ "$syncable" -eq 1 ]]; then
        SYNC_NAMES+=("$name")
        SYNC_SHAS+=("$head")
    fi
done

echo
if [[ "$out_of_sync" -eq 0 ]]; then
    echo "${G}All siblings in submodule sync.${N}"
    exit 0
fi

echo "${Y}${out_of_sync} sibling(s) out of submodule sync.${N}"

if [[ "$SYNC" -eq 0 ]]; then
    echo "${DIM}Re-run with --sync to bump bro's pointers to the standalone HEADs and commit.${N}"
    exit 0
fi

if [[ "${#SYNC_NAMES[@]}" -eq 0 ]]; then
    echo "${Y}Nothing to sync: out-of-sync siblings have standalone behind bro (pull them first).${N}"
    exit 0
fi

echo
echo "${BOLD}== Syncing ${#SYNC_NAMES[@]} pointer(s) to standalone HEAD ==${N}"

staged_paths=()
staged_names=()
for i in "${!SYNC_NAMES[@]}"; do
    name="${SYNC_NAMES[$i]}"
    sha="${SYNC_SHAS[$i]}"
    standalone="$PROJECTS_ROOT/$name"
    sub_path="third_party/$name"

    if [[ ! -e "$sub_path/.git" ]]; then
        printf '  %-14s %sskip: submodule not initialized (git submodule update --init %s)%s\n' \
            "$name" "$Y" "$sub_path" "$N"
        continue
    fi

    # Bring the standalone HEAD commit into the submodule, then point at it.
    if ! git -C "$sub_path" fetch --quiet "$standalone" HEAD 2>/dev/null; then
        printf '  %-14s %sskip: fetch from standalone failed%s\n' "$name" "$R" "$N"
        continue
    fi
    if ! git -C "$sub_path" checkout --quiet "$sha" 2>/dev/null; then
        printf '  %-14s %sskip: checkout %s failed%s\n' "$name" "$R" "${sha:0:9}" "$N"
        continue
    fi
    git -C "$BRO_ROOT" add "$sub_path"
    printf '  %-14s %sbumped -> %s%s\n' "$name" "$G" "${sha:0:9}" "$N"
    staged_paths+=("$sub_path")
    staged_names+=("$name")
done

if [[ "${#staged_paths[@]}" -eq 0 ]]; then
    echo "${Y}No pointers were updated.${N}"
    exit 1
fi

# Single bro commit recording exactly the bumped pointers (pathspec keeps any
# unrelated staged changes out of this commit).
names_list="$(printf '%s, ' "${staged_names[@]}")"; names_list="${names_list%, }"
msg="Update submodules: ${names_list} (sync to standalone HEAD)"
echo
if git -C "$BRO_ROOT" commit --quiet -m "$msg" -- "${staged_paths[@]}"; then
    echo "${G}Committed:${N} $msg"
    git -C "$BRO_ROOT" log -1 --oneline | sed 's/^/  /'
else
    echo "${R}Commit failed.${N}"
    exit 1
fi

exit 0
