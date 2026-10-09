<#
.SYNOPSIS
    Multi-repo status for the bro ecosystem (PowerShell port of repo-status.sh).

.DESCRIPTION
    Walks every repo in scripts/repos.txt (bro, bronze/brass, the libraries bro
    links, the desktop substrate libraries, the apps and tools), each a standalone
    checkout at ..\<name>, printing the working-tree state of each and how far it
    sits from its upstream. Then checks the dependency declarations: every repo's
    cmake/bro_deps.cmake identical to bro's, no wlejon/* dependency pinned with a
    REF (they track main), no release lock (cmake/bro_lock.cmake) left on a
    checkout, and bro's declared dependency list matching scripts/repos.txt.

    Ahead/behind (up<n> / dn<n>) is against the upstream as last fetched; -Pull
    fetches. Dependencies track main, so CI and a fresh clone build what is
    pushed: an up<n> is invisible to them. A repo that is not checked out is
    listed and skipped.

    See docs/ecosystem.md and docs/multi-repo-workflow.md.

.PARAMETER ListFiles
    Also list changed files for dirty repos.

.PARAMETER Pull
    Fast-forward every repo to its upstream before reporting, so the status below
    reflects what's on the remotes. Uses --ff-only: a repo that has diverged, is
    detached, or has no upstream is reported and skipped, never merged.

.PARAMETER Push
    Push every repo that is ahead of its upstream: the libraries first, then bro,
    then the apps and tools that build on it, so a consumer's main never needs a
    dependency commit GitHub does not have yet.

.EXAMPLE
    pwsh scripts/repo-status.ps1
    pwsh scripts/repo-status.ps1 -ListFiles
    pwsh scripts/repo-status.ps1 -Pull
    pwsh scripts/repo-status.ps1 -Push
#>
[CmdletBinding()]
param([switch]$ListFiles, [switch]$Pull, [switch]$Push)

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

# Fast-forward one repo onto its upstream. Never merges, never rebases.
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
Write-Host '== Dependencies (cmake/bro_deps.cmake) ==' -ForegroundColor White

# Per repo: a bro_deps.cmake that drifted from bro's, a wlejon dependency pinned
# with a REF, or a release lock.
$depIssues = 0
$BroDeps = Join-Path $BroRoot 'cmake/bro_deps.cmake'
$BroDepsHash = (Get-FileHash $BroDeps).Hash
$RefRegex = 'bro_dependency\((?<name>[A-Za-z0-9_.-]+) GITHUB wlejon/[A-Za-z0-9_.-]+ REF [0-9a-f]{40}'
foreach ($r in $Repos) {
    $path = Repo-Path $r.Name
    if (-not (Is-GitRepo $path)) { continue }
    $notes = @()
    $deps = Join-Path $path 'cmake/bro_deps.cmake'
    if ($r.Name -ne 'bro' -and (Test-Path $deps) -and (Get-FileHash $deps).Hash -ne $BroDepsHash) {
        $notes += @{ Text = "cmake/bro_deps.cmake differs from bro's"; Color = 'Yellow' }
    }
    $files = @(& git -C $path ls-files -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' 2>$null)
    $refs = @()
    foreach ($f in $files) {
        $full = Join-Path $path $f
        if (-not (Test-Path $full)) { continue }
        foreach ($m in [regex]::Matches((Get-Content -Raw $full), $RefRegex)) { $refs += $m.Groups['name'].Value }
    }
    if ($refs.Count -gt 0) {
        $notes += @{ Text = ('pinned with a REF: ' + (($refs | Sort-Object -Unique) -join ' ')); Color = 'Yellow' }
    }
    $lock = Join-Path $path 'cmake/bro_lock.cmake'
    if (Test-Path $lock) {
        $locked = @(Get-Content $lock | Where-Object { $_ -match '^bro_lock\(' }).Count
        $notes += @{ Text = "LOCKED ($locked dependencies in cmake/bro_lock.cmake)"; Color = 'Red' }
    }
    if ($notes.Count -eq 0) { continue }
    $depIssues++
    Write-Host ("  {0,-14} " -f $r.Name) -NoNewline
    for ($i = 0; $i -lt $notes.Count; $i++) {
        if ($i -gt 0) { Write-Host '; ' -NoNewline }
        Write-Host $notes[$i].Text -ForegroundColor $notes[$i].Color -NoNewline
    }
    Write-Host ''
}

# bro's declared dependencies (bro_dependencies() in cmake/bro_pins.cmake)
# against the repos scripts/repos.txt marks `dep`.
$pinsText = Get-Content -Raw (Join-Path $BroRoot 'cmake/bro_pins.cmake')
$declared = @()
$block = [regex]::Match($pinsText, '(?ms)^bro_dependencies\((?<body>.*?)^\)')
if ($block.Success) {
    foreach ($line in ($block.Groups['body'].Value -split "`n")) {
        $line = ($line -replace '#.*$', '').Trim()
        if ($line) { $declared += ($line -split '\s+') }
    }
}
$listed = @($Repos | Where-Object { $_.Bro -eq 'dep' } | ForEach-Object { $_.Name })
foreach ($x in ($declared | Where-Object { $listed -notcontains $_ })) {
    Write-Host ("  {0,-14} " -f $x) -NoNewline
    Write-Host 'declared in cmake/bro_pins.cmake, not marked dep in scripts/repos.txt' -ForegroundColor Yellow
    $depIssues++
}
foreach ($x in ($listed | Where-Object { $declared -notcontains $_ })) {
    Write-Host ("  {0,-14} " -f $x) -NoNewline
    Write-Host 'marked dep in scripts/repos.txt, not declared in cmake/bro_pins.cmake' -ForegroundColor Yellow
    $depIssues++
}

if ($depIssues -eq 0) {
    Write-Host "  Every repo carries bro's cmake/bro_deps.cmake; no REF pins, no locks." -ForegroundColor Green
} else {
    Write-Host '  scripts/sync-deps.sh fixes drift and REF pins; a lock belongs on a release tag (scripts/lock-deps.sh --unlock).' -ForegroundColor DarkGray
}

if ($Push) {
    Write-Host ''
    Write-Host '== Pushing (libraries, then bro, then apps and tools) ==' -ForegroundColor White
    $isApp = { param($r) $r.Group -eq 'app' -or $r.Group -eq 'tool' }
    foreach ($r in $Repos) {
        if ($r.Name -eq 'bro' -or (& $isApp $r)) { continue }
        Repo-Push $r.Name (Repo-Path $r.Name)
    }
    Repo-Push 'bro' $BroRoot
    foreach ($r in $Repos) {
        if (& $isApp $r) { Repo-Push $r.Name (Repo-Path $r.Name) }
    }
}
