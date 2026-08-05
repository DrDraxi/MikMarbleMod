<#
.SYNOPSIS
    Restores the UE 5.8 support into the RE-UE4SS/deps/first/Unreal submodule.

.DESCRIPTION
    The UE 5.8 versioned container (UnrealVirtual508 + 62 generated offset
    tables + the Version::IsEqual(5, 8) branch in Container.cpp) lives inside a
    NESTED submodule:

        RE-UE4SS/deps/first/Unreal      (Re-UE4SS/UEPseudo)

    That is outside this repository's git, so `setup.bat`, a
    `git submodule update`, or any submodule reset will detach the submodule
    back to the pinned upstream commit and the 5.8 files disappear from the
    working tree -- after which UE4SS builds fine but aborts at runtime with
    "Fatal Error: Engine version is not supported."

    This script puts them back from our fork. It is safe to run repeatedly, and
    setup.bat calls it automatically after initialising submodules.

    The 5.8 work is deliberately kept on a fork rather than upstreamed:
    Re-UE4SS/UEPseudo is private and its 5.7 support landed as a direct
    maintainer push, not a PR. If upstream ever ships 5.8, delete the fork
    remote and this script.

    NOTE: the fork is private (forks of private repos always are), so this
    requires a git credential with access -- `gh auth setup-git` is enough.

.PARAMETER Fork
    Git remote URL holding the 5.8 branch.

.PARAMETER Branch
    Branch name on that remote.

.PARAMETER Commit
    Expected commit SHA, for reporting only.

.EXAMPLE
    .\restore-ue58-support.ps1
    .\restore-ue58-support.ps1 -Check     # report status, change nothing
#>

[CmdletBinding()]
param(
    [string]$Fork = "https://github.com/DrDraxi/UEPseudo.git",
    [string]$Branch = "ue5.8-support",
    [string]$Commit = "852378ab1125d547dff1ce82c9c47cc356e89bc6",
    [switch]$Check
)

$ErrorActionPreference = "Stop"

function Info($m) { Write-Host "[*] $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "[X] $m" -ForegroundColor Red; exit 1 }

$repoRoot = Split-Path -Parent $PSScriptRoot
$unreal = Join-Path $repoRoot "RE-UE4SS\deps\first\Unreal"

if (-not (Test-Path (Join-Path $unreal ".git"))) {
    Die "Submodule not initialised at '$unreal'. Run setup.bat first."
}

# --- what does the tree look like right now? --------------------------------
$fnBodies = Join-Path $unreal "generated_include\FunctionBodies"
$count = (Get-ChildItem $fnBodies -Filter "5_08_*" -ErrorAction SilentlyContinue).Count
$virt = Test-Path (Join-Path $unreal "src\VersionedContainer\UnrealVirtualImpl\UnrealVirtual508.cpp")
$containerHas58 = Select-String -Path (Join-Path $unreal "src\VersionedContainer\Container.cpp") `
                                -Pattern 'IsEqual\(5,\s*8\)' -Quiet -ErrorAction SilentlyContinue

Info "Current state of the Unreal submodule:"
Write-Host "      5_08_* offset tables : $count (expected 62)"
Write-Host "      UnrealVirtual508.cpp : $virt"
Write-Host "      Container.cpp 5.8 arm: $containerHas58"

$healthy = ($count -eq 62) -and $virt -and $containerHas58
if ($healthy) {
    Ok "UE 5.8 support is present - nothing to do."
    if (-not $Check) { exit 0 }
}

if ($Check) {
    # NB: no ternary here - this script must also run under Windows PowerShell 5.1.
    if ($healthy) {
        exit 0
    } else {
        Warn "UE 5.8 support is MISSING or incomplete. Re-run without -Check to restore."
        exit 1
    }
}

# --- restore ----------------------------------------------------------------
Push-Location $unreal
# git writes ordinary progress ("From https://...", "Switched to branch ...") to
# STDERR. With $ErrorActionPreference='Stop' PowerShell promotes that to a
# terminating NativeCommandError even when git exited 0, so drop to 'Continue'
# around the native calls and judge success by $LASTEXITCODE instead.
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $remotes = & git remote
    if ($remotes -notcontains "fork") {
        Info "Adding 'fork' remote -> $Fork"
        & git remote add fork $Fork 2>&1 | Out-Null
    }

    Info "Fetching $Branch ..."
    $out = & git fetch fork $Branch 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host "      $_" }
        $ErrorActionPreference = $prevEAP
        Die "Could not fetch '$Branch' from $Fork (is the fork private and git unauthenticated? try: gh auth setup-git)"
    }

    # Refuse to clobber uncommitted local work.
    $dirty = & git status --porcelain
    if ($dirty) {
        Warn "The submodule has uncommitted changes:"
        $dirty | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" }
        $ErrorActionPreference = $prevEAP
        Die "Refusing to overwrite them. Commit or stash inside '$unreal' first."
    }

    Info "Checking out fork/$Branch ..."
    $out = & git checkout -B $Branch "fork/$Branch" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host "      $_" }
        $ErrorActionPreference = $prevEAP
        Die "checkout failed"
    }
}
finally {
    $ErrorActionPreference = $prevEAP
    Pop-Location
}

# --- verify -----------------------------------------------------------------
$count = (Get-ChildItem $fnBodies -Filter "5_08_*" -ErrorAction SilentlyContinue).Count
$virt = Test-Path (Join-Path $unreal "src\VersionedContainer\UnrealVirtualImpl\UnrealVirtual508.cpp")
$containerHas58 = Select-String -Path (Join-Path $unreal "src\VersionedContainer\Container.cpp") `
                                -Pattern 'IsEqual\(5,\s*8\)' -Quiet -ErrorAction SilentlyContinue

if (($count -eq 62) -and $virt -and $containerHas58) {
    Push-Location $unreal
    $head = git rev-parse HEAD
    Pop-Location
    Ok "UE 5.8 support restored (HEAD $head)"
    if ($head -ne $Commit) { Warn "HEAD differs from the recorded commit $Commit (branch has moved - probably fine)." }
    Write-Host ""
    Write-Host "Next: re-run CMake configure so the new sources are globbed, then build:" -ForegroundColor White
    Write-Host "  cmake -B RE-UE4SS\build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64" -ForegroundColor White
    Write-Host "  build.bat --deploy" -ForegroundColor White
} else {
    Die "Restore did not produce the expected files (tables=$count virt=$virt container=$containerHas58)"
}
