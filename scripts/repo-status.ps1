<#
.SYNOPSIS
    Multi-repo status for the bro stack (PowerShell port of repo-status.sh).

.DESCRIPTION
    Walks bro + its sibling libraries (standalone repos at ..\<name>) + broworkshop,
    printing the working-tree state of each, then reports which siblings are out of
    submodule sync: i.e. the standalone repo you actually build against (..\<name>)
    sits at a different commit than the pointer bro records in third_party\<name>.

    See docs/multi-repo-workflow.md for the layout this reflects.

.PARAMETER ListFiles
    Also list changed files for dirty repos.

.PARAMETER Pull
    Fast-forward every repo (bro, broworkshop, and each sibling) to its upstream
    before reporting, so the status below reflects what's on the remotes. Uses
    --ff-only and never recurses into submodules: a repo that has diverged, is
    detached, or has no upstream is reported and skipped, never merged.

.PARAMETER Sync
    Bump bro's stale submodule pointers up to the standalone repos' HEADs and
    make a single bro commit recording it. Only acts on siblings where the
    standalone is ahead of (or diverged from) bro's recorded pointer; siblings
    whose standalone is behind bro are left alone (pull the standalone first).

.EXAMPLE
    pwsh scripts/repo-status.ps1
    pwsh scripts/repo-status.ps1 -ListFiles
    pwsh scripts/repo-status.ps1 -Pull
    pwsh scripts/repo-status.ps1 -Pull -Sync
#>
[CmdletBinding()]
param([switch]$ListFiles, [switch]$Pull, [switch]$Sync)

$ErrorActionPreference = 'Continue'

$BroRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjectsRoot = (Resolve-Path (Join-Path $BroRoot '..')).Path

# Sibling libraries: <name> => standalone at ..\<name>, submodule at third_party\<name>.
# bronze is in this list on the same terms as the rest even though it is a
# compiler rather than a library bro links: bro resolves ..\bronze first and
# third_party\bronze second (src/bronze_host/CMakeLists.txt), so the standalone
# tree being ahead of the recorded pointer means exactly what it means for the
# others - CI and the nightly package are building an older bronze than you are.
$Siblings = @(
    'bromath', 'qjsbind', 'brokit', 'htmlayout', 'broaudio', 'bromesh', 'broflora',
    'brotensor', 'brogameagent', 'brolm', 'brodiffusion', 'broimage', 'brosoundml', 'brovisionml',
    'bronze'
)

# Run a git command in a repo, returning trimmed stdout (errors swallowed).
function Git-In {
    param([string]$Path, [Parameter(ValueFromRemainingArguments = $true)][string[]]$GitArgs)
    $out = & git -C $Path @GitArgs 2>$null
    if ($null -eq $out) { return '' }
    return ($out -join "`n")
}

function Is-GitRepo {
    param([string]$Path)
    return (Test-Path (Join-Path $Path '.git'))
}

# Print the working-tree state of a single git repo.
function Repo-State {
    param([string]$Label, [string]$Path)

    if (-not (Is-GitRepo $Path)) {
        Write-Host ("  {0,-14} " -f $Label) -NoNewline
        Write-Host "no git repo ($Path)" -ForegroundColor DarkGray
        return
    }

    $branch = Git-In $Path rev-parse --abbrev-ref HEAD
    if ($branch -eq 'HEAD') {
        $short = Git-In $Path rev-parse --short HEAD
        $branch = "(detached @ $short)"
    }

    $diffStat = Git-In $Path diff --shortstat
    $dirty = 0
    if ($diffStat -match '(\d+) file') { $dirty = [int]$Matches[1] }

    $stagedOut = Git-In $Path diff --cached --name-only
    $staged = if ($stagedOut) { ($stagedOut -split "`n").Count } else { 0 }

    $untrackedOut = Git-In $Path ls-files --others --exclude-standard
    $untracked = if ($untrackedOut) { ($untrackedOut -split "`n").Count } else { 0 }

    # Ahead/behind vs upstream, if one is configured.
    $ahead = 0; $behind = 0
    $upstream = Git-In $Path rev-parse --abbrev-ref --symbolic-full-name '@{u}'
    if ($upstream) {
        $a = Git-In $Path rev-list --count '@{u}..HEAD'
        $b = Git-In $Path rev-list --count 'HEAD..@{u}'
        if ($a) { $ahead = [int]$a }
        if ($b) { $behind = [int]$b }
    }

    Write-Host ("  {0,-14} " -f $Label) -NoNewline
    Write-Host $branch -ForegroundColor Blue -NoNewline
    if ($ahead -gt 0) { Write-Host " up$ahead" -ForegroundColor Yellow -NoNewline }
    if ($behind -gt 0) { Write-Host " dn$behind" -ForegroundColor Yellow -NoNewline }

    $hasChanges = $false
    if ($dirty -gt 0) { Write-Host " ~$dirty" -ForegroundColor Red -NoNewline; $hasChanges = $true }
    if ($staged -gt 0) { Write-Host " +$staged staged" -ForegroundColor Yellow -NoNewline; $hasChanges = $true }
    if ($untracked -gt 0) { Write-Host " ?$untracked" -ForegroundColor DarkGray -NoNewline; $hasChanges = $true }
    if (-not $hasChanges) { Write-Host " clean" -ForegroundColor Green -NoNewline }
    Write-Host ''

    if ($ListFiles -and $hasChanges) {
        $porcelain = Git-In $Path status --porcelain
        if ($porcelain) {
            foreach ($line in ($porcelain -split "`n")) { Write-Host "      $line" -ForegroundColor DarkGray }
        }
    }
}

# Fast-forward one repo onto its upstream. Never merges, never rebases, and never
# recurses into submodules (bro's pointers move via -Sync, not via a pull).
function Repo-Pull {
    param([string]$Label, [string]$Path)

    Write-Host ("  {0,-14} " -f $Label) -NoNewline

    if (-not (Is-GitRepo $Path)) {
        Write-Host "no git repo ($Path)" -ForegroundColor DarkGray
        return
    }

    $branch = Git-In $Path rev-parse --abbrev-ref HEAD
    if ($branch -eq 'HEAD') {
        Write-Host 'skip: detached HEAD' -ForegroundColor Yellow
        return
    }

    $upstream = Git-In $Path rev-parse --abbrev-ref --symbolic-full-name '@{u}'
    if (-not $upstream) {
        Write-Host 'skip: no upstream configured' -ForegroundColor DarkGray
        return
    }

    $before = Git-In $Path rev-parse HEAD
    # -c pull.rebase=false: a repo configured to rebase on pull refuses outright
    # when the tree is dirty, even for a fast-forward. --ff-only never merges, so
    # forcing the merge backend here only removes that false failure.
    $out = & git -C $Path -c pull.rebase=false pull --ff-only --no-recurse-submodules --quiet 2>&1 |
        ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'pull failed' -ForegroundColor Red -NoNewline
        $why = $out | Where-Object { $_ -match '\S' } | Select-Object -First 1
        if ($why) { Write-Host " - $why" -ForegroundColor DarkGray } else { Write-Host '' }
        return
    }

    $after = Git-In $Path rev-parse HEAD
    if ($after -eq $before) {
        Write-Host 'up to date ' -ForegroundColor Green -NoNewline
        Write-Host ("({0})" -f $before.Substring(0, 9)) -ForegroundColor DarkGray
        return
    }

    $n = Git-In $Path rev-list --count "$before..$after"
    Write-Host ("fast-forwarded +{0} " -f $n) -ForegroundColor Yellow -NoNewline
    Write-Host ("{0} -> {1}" -f $before.Substring(0, 9), $after.Substring(0, 9)) -ForegroundColor DarkGray
}

if ($Pull) {
    Write-Host '== Pulling (fast-forward only) ==' -ForegroundColor White
    Repo-Pull 'bro' $BroRoot
    Repo-Pull 'broworkshop' (Join-Path $ProjectsRoot 'broworkshop')
    foreach ($name in $Siblings) {
        Repo-Pull $name (Join-Path $ProjectsRoot $name)
    }
    Write-Host ''
}

Write-Host '== Repo state ==' -ForegroundColor White
Repo-State 'bro' $BroRoot
Repo-State 'broworkshop' (Join-Path $ProjectsRoot 'broworkshop')
foreach ($name in $Siblings) {
    Repo-State $name (Join-Path $ProjectsRoot $name)
}

Write-Host ''
Write-Host "== Submodule sync (standalone ..\<name> vs bro's recorded pointer) ==" -ForegroundColor White

$outOfSync = 0
$toSync = @()   # @{ Name; Sha } for siblings whose pointer should bump to standalone HEAD
foreach ($name in $Siblings) {
    $standalone = Join-Path $ProjectsRoot $name
    $subPath = "third_party/$name"

    # Commit bro records for this submodule in its HEAD tree.
    # --verify: a bare rev-parse echoes an unresolvable argument back on
    # stdout, so a path that is not a recorded submodule would arrive here as
    # the literal "HEAD:third_party/<name>" and be compared as if it were a sha.
    $recorded = Git-In $BroRoot rev-parse --verify --quiet "HEAD:$subPath"
    if (-not $recorded) {
        Write-Host ("  {0,-14} " -f $name) -NoNewline
        Write-Host 'not a recorded submodule' -ForegroundColor DarkGray
        continue
    }

    if (-not (Is-GitRepo $standalone)) {
        Write-Host ("  {0,-14} " -f $name) -NoNewline
        Write-Host 'standalone repo missing - using submodule only' -ForegroundColor DarkGray
        continue
    }

    $head = Git-In $standalone rev-parse HEAD
    if ($head -eq $recorded) {
        Write-Host ("  {0,-14} " -f $name) -NoNewline
        Write-Host 'in sync ' -ForegroundColor Green -NoNewline
        Write-Host ("({0})" -f $recorded.Substring(0, 9)) -ForegroundColor DarkGray
        continue
    }

    $outOfSync++

    # Try to describe the divergence if the recorded commit is reachable locally.
    & git -C $standalone cat-file -e "$recorded^{commit}" 2>$null
    $reachable = ($LASTEXITCODE -eq 0)

    Write-Host ("  {0,-14} " -f $name) -NoNewline
    Write-Host 'OUT OF SYNC' -ForegroundColor Red -NoNewline
    Write-Host ' - ' -NoNewline

    # syncable: bumping bro's pointer to standalone HEAD is the right fix.
    $syncable = $false
    if ($reachable) {
        $localAhead = [int](Git-In $standalone rev-list --count "$recorded..HEAD")
        $localBehind = [int](Git-In $standalone rev-list --count "HEAD..$recorded")
        if ($localAhead -gt 0 -and $localBehind -gt 0) {
            Write-Host "diverged (standalone $localAhead ahead, $localBehind behind)" -ForegroundColor Red
            $syncable = $true
        }
        elseif ($localAhead -gt 0) {
            Write-Host "standalone ahead by $localAhead - bro pointer is stale" -ForegroundColor Yellow
            $syncable = $true
        }
        else {
            Write-Host "standalone behind by $localBehind - standalone needs a pull (-Pull)" -ForegroundColor Yellow
        }
    }
    else {
        # Can't compare, but standalone is the source of truth, so a bump is valid.
        Write-Host 'recorded commit not in standalone (will fetch on sync)' -ForegroundColor Red
        $syncable = $true
    }

    Write-Host ("  {0,14} recorded {1}  standalone {2}" -f '', $recorded.Substring(0, 9), $head.Substring(0, 9)) -ForegroundColor DarkGray

    if ($syncable) {
        $toSync += [pscustomobject]@{
            Name       = $name
            Sha        = $head
            SubPath    = $subPath                       # relative, for git add/commit under -C $BroRoot
            SubFull    = (Join-Path $BroRoot $subPath)   # absolute, for git -C / Test-Path
            Standalone = $standalone
        }
    }
}

Write-Host ''
if ($outOfSync -eq 0) {
    Write-Host 'All siblings in submodule sync.' -ForegroundColor Green
    exit 0
}

Write-Host "$outOfSync sibling(s) out of submodule sync." -ForegroundColor Yellow

if (-not $Sync) {
    Write-Host "Re-run with -Sync to bump bro's pointers to the standalone HEADs and commit." -ForegroundColor DarkGray
    exit 0
}

if ($toSync.Count -eq 0) {
    Write-Host 'Nothing to sync: out-of-sync siblings have standalone behind bro (pull them first).' -ForegroundColor Yellow
    exit 0
}

Write-Host ''
Write-Host ("== Syncing {0} pointer(s) to standalone HEAD ==" -f $toSync.Count) -ForegroundColor White

$stagedPaths = @()
$stagedNames = @()
foreach ($s in $toSync) {
    if (-not (Test-Path (Join-Path $s.SubFull '.git'))) {
        Write-Host ("  {0,-14} " -f $s.Name) -NoNewline
        Write-Host ("skip: submodule not initialized (git submodule update --init {0})" -f $s.SubPath) -ForegroundColor Yellow
        continue
    }

    # Bring the standalone HEAD commit into the submodule, then point at it.
    & git -C $s.SubFull fetch --quiet $s.Standalone HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  {0,-14} " -f $s.Name) -NoNewline
        Write-Host 'skip: fetch from standalone failed' -ForegroundColor Red
        continue
    }
    & git -C $s.SubFull checkout --quiet $s.Sha 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  {0,-14} " -f $s.Name) -NoNewline
        Write-Host ("skip: checkout {0} failed" -f $s.Sha.Substring(0, 9)) -ForegroundColor Red
        continue
    }
    & git -C $BroRoot add $s.SubPath 2>$null
    Write-Host ("  {0,-14} " -f $s.Name) -NoNewline
    Write-Host ("bumped -> {0}" -f $s.Sha.Substring(0, 9)) -ForegroundColor Green
    $stagedPaths += $s.SubPath
    $stagedNames += $s.Name
}

if ($stagedPaths.Count -eq 0) {
    Write-Host 'No pointers were updated.' -ForegroundColor Yellow
    exit 1
}

# Single bro commit recording exactly the bumped pointers (pathspec keeps any
# unrelated staged changes out of this commit).
$namesList = $stagedNames -join ', '
$msg = "Update submodules: $namesList (sync to standalone HEAD)"
Write-Host ''
& git -C $BroRoot commit --quiet -m $msg -- @stagedPaths 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Committed: " -ForegroundColor Green -NoNewline
    Write-Host $msg
    & git -C $BroRoot log -1 --oneline | ForEach-Object { Write-Host "  $_" }
}
else {
    Write-Host 'Commit failed.' -ForegroundColor Red
    exit 1
}
