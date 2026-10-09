<#
.SYNOPSIS
    Multi-repo status for the bro ecosystem (PowerShell port of repo-status.sh).

.DESCRIPTION
    Walks every repo in scripts/repos.txt (bro, bronze/brass, the libraries bro
    links, the desktop substrate libraries, the apps and tools), each a standalone
    checkout at ..\<name>, printing the working-tree state of each and how far it
    sits from its upstream. Then, for the repos bro pins (cmake/bro_pins.cmake),
    reports which pins are stale: the working tree you actually build against
    (..\<name>) at a different commit than the one a plain clone of bro fetches.

    Ahead/behind (up<n> / dn<n>) is against the upstream as last fetched; -Pull
    fetches. A repo that is not checked out is listed and skipped.

    See docs/ecosystem.md and docs/multi-repo-workflow.md.

.PARAMETER ListFiles
    Also list changed files for dirty repos.

.PARAMETER Pull
    Fast-forward every repo to its upstream before reporting, so the status below
    reflects what's on the remotes. Uses --ff-only: a repo that has diverged, is
    detached, or has no upstream is reported and skipped, never merged.

.PARAMETER Sync
    Move bro's stale pins (cmake/bro_pins.cmake) to the working trees' HEADs and
    make a single bro commit recording it (what scripts/bump-deps.sh --local
    does). Only acts on pins whose working tree is ahead of (or diverged from)
    the pin; those whose working tree is behind are left alone (pull it first).

.PARAMETER Push
    Push every repo that is ahead of its upstream, bro last, so its pins never
    name a commit GitHub does not have yet. If run alongside -Sync, the pins are
    moved and committed first.

.EXAMPLE
    pwsh scripts/repo-status.ps1
    pwsh scripts/repo-status.ps1 -ListFiles
    pwsh scripts/repo-status.ps1 -Pull
    pwsh scripts/repo-status.ps1 -Pull -Sync
    pwsh scripts/repo-status.ps1 -Push
    pwsh scripts/repo-status.ps1 -Sync -Push
#>
[CmdletBinding()]
param([switch]$ListFiles, [switch]$Pull, [switch]$Sync, [switch]$Push)

$ErrorActionPreference = 'Continue'

$BroRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjectsRoot = (Resolve-Path (Join-Path $BroRoot '..')).Path
$ReposFile = Join-Path $BroRoot 'scripts/repos.txt'

if (-not (Test-Path $ReposFile)) {
    Write-Host "missing $ReposFile" -ForegroundColor Red
    exit 1
}

# The repo list: name, group and bro relation, from scripts/repos.txt.
$Repos = @()
foreach ($line in (Get-Content $ReposFile)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $f = $t -split '\s+'
    $Repos += [pscustomobject]@{ Name = $f[0]; Group = $f[1]; Bro = $f[2] }
}

# Pinned siblings: the repos bro pins in cmake/bro_pins.cmake. bronze and brass
# are among them on the same terms as the libraries: bro builds ..\bronze and
# ..\brass when they are there and the pin otherwise, so a working tree ahead of
# its pin means CI and the nightly build an older one than you do.
$PinsFile = Join-Path $BroRoot 'cmake/bro_pins.cmake'
$Siblings = @($Repos | Where-Object { $_.Bro -eq 'pinned' } | ForEach-Object { $_.Name })

# The commit bro pins for a name, from cmake/bro_pins.cmake ('' if none).
$PinRegex = '(?m)^bro_dependency\((?<name>[A-Za-z0-9_.-]+) GITHUB (?<repo>\S+) REF (?<sha>[0-9a-f]{40})'
function Pinned-Sha {
    param([string]$Name)
    foreach ($m in [regex]::Matches((Get-Content -Raw $PinsFile), $PinRegex)) {
        if ($m.Groups['name'].Value -eq $Name) { return $m.Groups['sha'].Value }
    }
    return ''
}

function Repo-Path {
    param([string]$Name)
    if ($Name -eq 'bro') { return $BroRoot }
    return (Join-Path $ProjectsRoot $Name)
}

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

    Write-Host ("  {0,-14} " -f $Label) -NoNewline
    if (-not (Is-GitRepo $Path)) {
        Write-Host "not checked out (github.com/wlejon/$Label)" -ForegroundColor DarkGray
        return
    }

    $branch = Git-In $Path rev-parse --abbrev-ref HEAD
    $detached = $false
    if ($branch -eq 'HEAD') {
        $short = Git-In $Path rev-parse --short HEAD
        $branch = "(detached @ $short)"
        $detached = $true
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

    Write-Host $branch -ForegroundColor Blue -NoNewline
    if ($ahead -gt 0) { Write-Host " up$ahead" -ForegroundColor Yellow -NoNewline }
    if ($behind -gt 0) { Write-Host " dn$behind" -ForegroundColor Yellow -NoNewline }
    # Name an upstream that is not on origin (a repo tracking another machine).
    if ($upstream -and -not $upstream.StartsWith('origin/')) {
        Write-Host " [$upstream]" -ForegroundColor DarkGray -NoNewline
    }
    if (-not $upstream -and -not $detached) { Write-Host ' no upstream' -ForegroundColor DarkGray -NoNewline }

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

# Fast-forward one repo onto its upstream. Never merges, never rebases (bro's
# pins move via -Sync, not via a pull).
function Repo-Pull {
    param([string]$Label, [string]$Path)

    Write-Host ("  {0,-14} " -f $Label) -NoNewline

    if (-not (Is-GitRepo $Path)) {
        Write-Host 'not checked out' -ForegroundColor DarkGray
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

# Push one repo to its upstream.
function Repo-Push {
    param([string]$Label, [string]$Path)

    Write-Host ("  {0,-14} " -f $Label) -NoNewline

    if (-not (Is-GitRepo $Path)) {
        Write-Host 'not checked out' -ForegroundColor DarkGray
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

    $ahead = 0
    $a = Git-In $Path rev-list --count '@{u}..HEAD'
    if ($a) { $ahead = [int]$a }

    if ($ahead -eq 0) {
        $head = Git-In $Path rev-parse HEAD
        Write-Host 'up to date ' -ForegroundColor Green -NoNewline
        Write-Host ("({0})" -f $head.Substring(0, 9)) -ForegroundColor DarkGray
        return
    }

    $out = & git -C $Path push --quiet 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'push failed' -ForegroundColor Red -NoNewline
        $why = $out | Where-Object { $_ -match '\S' } | Select-Object -First 1
        if ($why) { Write-Host " - $why" -ForegroundColor DarkGray } else { Write-Host '' }
        return
    }

    $head = Git-In $Path rev-parse HEAD
    Write-Host ("pushed +{0} " -f $ahead) -ForegroundColor Green -NoNewline
    Write-Host ("({0})" -f $head.Substring(0, 9)) -ForegroundColor DarkGray
}

if ($Pull) {
    Write-Host '== Pulling (fast-forward only) ==' -ForegroundColor White
    foreach ($r in $Repos) { Repo-Pull $r.Name (Repo-Path $r.Name) }
    Write-Host ''
}

Write-Host '== Repo state ==' -ForegroundColor White
$prevGroup = ''
foreach ($r in $Repos) {
    if ($r.Group -ne $prevGroup) {
        $prevGroup = $r.Group
        Write-Host " $prevGroup" -ForegroundColor DarkGray
    }
    Repo-State $r.Name (Repo-Path $r.Name)
}

Write-Host ''
Write-Host "== Pins (working tree ..\<name> vs the commit cmake/bro_pins.cmake pins) ==" -ForegroundColor White

$outOfSync = 0
$toSync = @()   # @{ Name; Sha } for siblings whose pin should move to the working tree's HEAD
foreach ($name in $Siblings) {
    $standalone = Join-Path $ProjectsRoot $name

    $recorded = Pinned-Sha $name
    if (-not $recorded) {
        Write-Host ("  {0,-14} " -f $name) -NoNewline
        Write-Host 'not pinned in cmake/bro_pins.cmake (scripts/repos.txt says it is)' -ForegroundColor Yellow
        continue
    }

    if (-not (Is-GitRepo $standalone)) {
        Write-Host ("  {0,-14} " -f $name) -NoNewline
        Write-Host 'no working tree - builds use the pin' -ForegroundColor DarkGray
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

    # Describe the divergence if the pinned commit is reachable locally.
    & git -C $standalone cat-file -e "$recorded^{commit}" 2>$null
    $reachable = ($LASTEXITCODE -eq 0)

    Write-Host ("  {0,-14} " -f $name) -NoNewline
    Write-Host 'STALE PIN' -ForegroundColor Red -NoNewline
    Write-Host ' - ' -NoNewline

    # syncable: moving the pin to the working tree's HEAD is the right fix.
    $syncable = $false
    if ($reachable) {
        $localAhead = [int](Git-In $standalone rev-list --count "$recorded..HEAD")
        $localBehind = [int](Git-In $standalone rev-list --count "HEAD..$recorded")
        if ($localAhead -gt 0 -and $localBehind -gt 0) {
            Write-Host "diverged (working tree $localAhead ahead, $localBehind behind)" -ForegroundColor Red
            $syncable = $true
        }
        elseif ($localAhead -gt 0) {
            Write-Host "working tree ahead by $localAhead - bro's pin is stale" -ForegroundColor Yellow
            $syncable = $true
        }
        else {
            Write-Host "working tree behind by $localBehind - it needs a pull (-Pull)" -ForegroundColor Yellow
        }
    }
    else {
        # Can't compare, but the working tree is the source of truth, so a bump is valid.
        Write-Host 'pinned commit not in the working tree (fetch it to compare)' -ForegroundColor Red
        $syncable = $true
    }

    Write-Host ("  {0,14} pinned {1}  working tree {2}" -f '', $recorded.Substring(0, 9), $head.Substring(0, 9)) -ForegroundColor DarkGray

    if ($syncable) {
        $toSync += [pscustomobject]@{ Name = $name; Sha = $head }
    }
}

# A wlejon pin bro carries that scripts/repos.txt does not mark as one would be
# skipped above without a word; name it instead.
foreach ($m in [regex]::Matches((Get-Content -Raw $PinsFile), $PinRegex)) {
    if ($m.Groups['repo'].Value -notmatch '^wlejon/') { continue }
    $pinName = $m.Groups['name'].Value
    if ($Siblings -notcontains $pinName) {
        Write-Host ("  {0,-14} " -f $pinName) -NoNewline
        Write-Host 'bro pins it but scripts/repos.txt does not mark it pinned - add it there' -ForegroundColor Yellow
    }
}

Write-Host ''
if ($outOfSync -eq 0) {
    Write-Host 'All pins match the working trees.' -ForegroundColor Green
} else {
    Write-Host "$outOfSync stale pin(s)." -ForegroundColor Yellow
}

if ($Sync) {
    if ($toSync.Count -eq 0) {
        Write-Host 'Nothing to sync: stale pins have working trees behind them (pull those first).' -ForegroundColor Yellow
    } else {
        Write-Host ''
        Write-Host ("== Moving {0} pin(s) to the working trees' HEADs ==" -f $toSync.Count) -ForegroundColor White

        # The same rewrite scripts/bump-deps.sh --local makes.
        $text = Get-Content -Raw $PinsFile
        $movedNames = @()
        foreach ($s in $toSync) {
            $pattern = '(?m)^(bro_dependency\(' + [regex]::Escape($s.Name) + ' GITHUB \S+ REF )[0-9a-f]{40}'
            $new = [regex]::Replace($text, $pattern, { param($m) $m.Groups[1].Value + $s.Sha })
            if ($new -ne $text) {
                $text = $new
                $movedNames += $s.Name
                Write-Host ("  {0,-14} " -f $s.Name) -NoNewline
                Write-Host ("pinned -> {0}" -f $s.Sha.Substring(0, 9)) -ForegroundColor Green
            }
        }

        if ($movedNames.Count -gt 0) {
            [IO.File]::WriteAllText($PinsFile, $text)
            # One bro commit recording exactly the moved pins (the pathspec keeps
            # any unrelated staged changes out of it).
            $namesList = $movedNames -join ', '
            $msg = "Pin $namesList to the working trees' HEADs"
            Write-Host ''
            & git -C $BroRoot commit --quiet -m $msg -- cmake/bro_pins.cmake 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "Committed: " -ForegroundColor Green -NoNewline
                Write-Host $msg
                & git -C $BroRoot log -1 --oneline | ForEach-Object { Write-Host "  $_" }
            }
            else {
                Write-Host 'Commit failed.' -ForegroundColor Red
            }
        } else {
            Write-Host 'No pins were moved.' -ForegroundColor Yellow
        }
    }
} elseif ($outOfSync -gt 0) {
    Write-Host "Re-run with -Sync to move bro's pins to the working trees' HEADs and commit." -ForegroundColor DarkGray
}

if ($Push) {
    Write-Host ''
    Write-Host '== Pushing (bro last) ==' -ForegroundColor White
    foreach ($r in $Repos) {
        if ($r.Name -eq 'bro') { continue }
        Repo-Push $r.Name (Repo-Path $r.Name)
    }
    Repo-Push 'bro' $BroRoot
}
