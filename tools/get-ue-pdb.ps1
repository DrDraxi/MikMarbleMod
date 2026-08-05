<#
.SYNOPSIS
    Builds Unreal Engine from source and produces the Shipping PDB that UE4SS's
    UVTD tool needs to generate versioned-container offsets for a new UE version.

.DESCRIPTION
    UE4SS does not hand-write engine offsets. It runs UVTD (Unreal VTable Dumper)
    against a PDB of a real engine build and generates the vtable / member-variable
    layout tables from the debug symbols. UE4SS ships tables for 4.10 - 5.7 only
    (see assets/Default_UVTD_Configs/Config/pdbs_to_dump.json), so a game on UE 5.8
    makes it abort with "Fatal Error: Engine version is not supported."

    UE 5.8 is not offered as a prebuilt Epic Games Launcher install -- only as
    GitHub source -- so this script builds it from source. That is also what
    UVTD's own README prefers ("PDBs from source engine builds are preferred").

    It builds the engine's built-in UnrealGame target as Win64 SHIPPING. That is
    a MONOLITHIC binary statically linking the whole engine, so its PDB carries
    the complete engine type information UVTD needs -- and it requires no
    project to be created at all.

    WHY SHIPPING, SPECIFICALLY:
    UE class layouts differ between build configurations, because members are
    guarded by #if !UE_BUILD_SHIPPING. The target game is a Shipping build, so
    the PDB must be Shipping too. A Development PDB yields offsets that are
    subtly wrong -- UE4SS would load and then SILENTLY misread game data, which
    is far worse than an obvious failure.

    The script only compiles; it never cooks or packages. We need the .pdb, not
    a runnable game.

.PARAMETER Ref
    Git tag/branch to build. Default 5.8.1-release, whose recorded
    CompatibleChangelist (55116800) matches the shipped game build
    "++UE5+Release-5.8-CL-55116800". Do not change this casually.

.PARAMETER ExpectedChangelist
    Cross-checked against Engine/Build/Build.version after cloning. Set to 0 to
    skip the check.

.NOTES
    Disk: ~150 GB total (source + dependencies + build intermediates).
    Time: dependency download 20-60 min, compile 1-3 h depending on CPU.
    Access: EpicGames/UnrealEngine is private; you must be in the Epic Games
    GitHub organisation and have git authenticated.

.EXAMPLE
    .\get-ue-pdb.ps1
    .\get-ue-pdb.ps1 -SourceDir D:\UE58 -OutDir D:\out
    .\get-ue-pdb.ps1 -SkipClone -SkipSetup      # resume after a failed compile
#>

[CmdletBinding()]
param(
    [string]$Ref = "5.8.1-release",
    [string]$EngineVersion = "5.8",
    [int]$ExpectedChangelist = 55116800,
    [string]$SourceDir = "$env:USERPROFILE\UnrealEngine-5.8",
    [string]$OutDir    = "$env:USERPROFILE\Desktop\ue-pdb-out",
    [switch]$SkipClone,
    [switch]$SkipSetup,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Info($m) { Write-Host "[*] $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "[X] $m" -ForegroundColor Red; exit 1 }
function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Magenta }

$overall = [Diagnostics.Stopwatch]::StartNew()

# ============================================================ preflight
Step "Preflight"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die "git is not on PATH." }
Ok "git: $((git --version))"

# --- disk space -------------------------------------------------------------
$driveLetter = (Split-Path -Qualifier $SourceDir).TrimEnd(':')
try {
    $free = (Get-PSDrive -Name $driveLetter -ErrorAction Stop).Free
    $freeGB = [math]::Round($free / 1GB)
    if ($freeGB -lt 150) {
        Warn "Only ${freeGB} GB free on ${driveLetter}: -- a full source build needs ~150 GB."
        Warn "Pass -SourceDir on a bigger drive if this runs out."
    } else {
        Ok "Free space on ${driveLetter}: ${freeGB} GB"
    }
} catch { Warn "Could not determine free space on ${driveLetter}:" }

# --- MSVC toolchain ---------------------------------------------------------
# Unreal does not support the v145 toolset shipped with Visual Studio 2026, and
# UBT additionally expects a VS2022 x64 install. Just as important: the game is
# a v143-built binary, so matching the toolset removes a variable from the
# layout extraction. Wrong offsets fail SILENTLY, so check before compiling.
Info "Checking MSVC toolchain..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$haveV143 = $false
$haveV145 = $false
if (Test-Path $vswhere) {
    foreach ($i in (& $vswhere -products * -format json 2>$null | ConvertFrom-Json)) {
        Write-Host "    $($i.displayName) [$($i.installationVersion)]"
        $msvcDir = Join-Path $i.installationPath "VC\Tools\MSVC"
        if (Test-Path $msvcDir) {
            foreach ($t in (Get-ChildItem $msvcDir -Directory -ErrorAction SilentlyContinue)) {
                # v143 (VS2022) == MSVC 14.30 .. 14.4x   <- supported by Unreal
                # v145 (VS2026) == MSVC 14.50+           <- NOT supported
                if     ($t.Name -match '^14\.[34]\d')  { $haveV143 = $true; Write-Host "      -> v143 toolset $($t.Name)" -ForegroundColor Green }
                elseif ($t.Name -match '^14\.[5-9]\d') { $haveV145 = $true; Write-Host "      -> v145 toolset $($t.Name) (unsupported by Unreal)" -ForegroundColor Yellow }
            }
        }
    }
    if ($haveV143) {
        Ok "v143 toolset present"
    } else {
        Warn "No MSVC v143 (14.3x/14.4x) toolset found."
        Warn "In the Visual Studio Installer add the individual component:"
        Warn "    'MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)'"
        Warn "plus a Windows 10/11 SDK."
        Die  "Aborting: building with an unsupported toolset risks silently-wrong offsets."
    }
} else {
    Warn "vswhere.exe not found; cannot verify the toolchain. Continuing anyway."
}

# Only pin the compiler when there is something to disambiguate. On a
# v143-only machine UBT already picks correctly, and rewriting the user's
# global UBT config would be a pointless side effect.
if ($haveV145 -and $haveV143) {
    $ubtCfgDir = Join-Path $env:APPDATA "Unreal Engine\UnrealBuildTool"
    $ubtCfg = Join-Path $ubtCfgDir "BuildConfiguration.xml"
    New-Item -ItemType Directory -Force -Path $ubtCfgDir | Out-Null
    if (Test-Path $ubtCfg) {
        $bak = "$ubtCfg.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item $ubtCfg $bak -Force
        Warn "Existing BuildConfiguration.xml backed up to: $bak"
    }
@"
<?xml version="1.0" encoding="utf-8" ?>
<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">
  <!-- Written by get-ue-pdb.ps1: force v143. Unreal does not support VS2026's
       v145, and the target game is a v143 build. -->
  <WindowsPlatform>
    <Compiler>VisualStudio2022</Compiler>
  </WindowsPlatform>
</Configuration>
"@ | Set-Content $ubtCfg -Encoding UTF8
    Ok "Pinned UBT to the VisualStudio2022 toolchain"
} elseif ($haveV143) {
    Ok "Only v143 present - leaving your global BuildConfiguration.xml untouched"
}

# ============================================================ clone
Step "Engine source"

if ($SkipClone) {
    Info "-SkipClone given; using existing $SourceDir"
    if (-not (Test-Path "$SourceDir\Setup.bat")) { Die "$SourceDir does not contain an Unreal Engine source tree." }
} elseif (Test-Path "$SourceDir\Setup.bat") {
    Ok "Source already present at $SourceDir (skipping clone)"
} else {
    Info "Shallow-cloning EpicGames/UnrealEngine at '$Ref'"
    Info "This is a large private repo. If it fails with 'Repository not found',"
    Info "you are either not in the Epic Games GitHub org, or git is not authenticated"
    Info "(fix with: gh auth login  &&  gh auth setup-git)."
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SourceDir) | Out-Null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    git clone --depth 1 --branch $Ref https://github.com/EpicGames/UnrealEngine.git $SourceDir
    if ($LASTEXITCODE -ne 0) { Die "git clone failed (exit $LASTEXITCODE)." }
    Ok "Cloned in $([int]$sw.Elapsed.TotalMinutes) min"
}

# --- verify we cloned the RIGHT engine build --------------------------------
# This is the check that stops us silently generating offsets for the wrong
# engine revision. The game reports ++UE5+Release-5.8-CL-55116800.
$bvPath = Join-Path $SourceDir "Engine\Build\Build.version"
if (Test-Path $bvPath) {
    $bv = Get-Content $bvPath -Raw | ConvertFrom-Json
    Ok "Engine source reports $($bv.MajorVersion).$($bv.MinorVersion).$($bv.PatchVersion) (CompatibleChangelist $($bv.CompatibleChangelist))"
    if ("$($bv.MajorVersion).$($bv.MinorVersion)" -ne $EngineVersion) {
        Die "Source is $($bv.MajorVersion).$($bv.MinorVersion) but $EngineVersion was requested."
    }
    if ($ExpectedChangelist -ne 0 -and [int]$bv.CompatibleChangelist -ne $ExpectedChangelist) {
        Warn "CompatibleChangelist is $($bv.CompatibleChangelist), expected $ExpectedChangelist."
        Warn "The game binary is CL-$ExpectedChangelist. A different revision may have"
        Warn "different class layouts, which would produce silently-wrong offsets."
        Die  "Aborting. Re-run with -Ref pointing at the matching release, or -ExpectedChangelist 0 to override."
    }
    Ok "Changelist matches the shipped game build"
} else {
    Warn "No Engine/Build/Build.version found; cannot verify the engine revision."
}

# ============================================================ setup
Step "Dependencies"

$depMarker = Join-Path $SourceDir "Engine\Binaries\ThirdParty"
if ($SkipSetup) {
    Info "-SkipSetup given"
} elseif (Test-Path $depMarker) {
    Ok "Dependencies appear present (skipping Setup.bat). Use -SkipSetup:`$false to force."
} else {
    Info "Running Setup.bat (downloads ~20-25 GB of binary dependencies)."
    Info "It may launch prerequisite installers that request elevation."
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Push-Location $SourceDir
    try   { & "$SourceDir\Setup.bat" }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { Die "Setup.bat failed (exit $LASTEXITCODE)." }
    Ok "Dependencies fetched in $([int]$sw.Elapsed.TotalMinutes) min"
}

# ============================================================ build
Step "Build UnrealGame Win64 Shipping (monolithic)"

$buildBat = Join-Path $SourceDir "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path $buildBat)) { Die "Missing $buildBat" }

if ($SkipBuild) {
    Info "-SkipBuild given"
} else {
    Info "Compiling the engine statically. Expect 1-3 hours."
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $buildBat UnrealGame Win64 Shipping -waitmutex
    $code = $LASTEXITCODE
    $sw.Stop()
    if ($code -ne 0) {
        Die "Build failed (exit $code) after $([int]$sw.Elapsed.TotalMinutes) min. Re-run with -SkipClone -SkipSetup to retry just the compile."
    }
    Ok "Built in $([int]$sw.Elapsed.TotalMinutes) min"
}

# ============================================================ collect
Step "Collect PDB"

$binDir = Join-Path $SourceDir "Engine\Binaries\Win64"
$pdb = Get-ChildItem $binDir -Filter "UnrealGame*Shipping*.pdb" -ErrorAction SilentlyContinue |
       Sort-Object Length -Descending | Select-Object -First 1
if (-not $pdb) {
    $pdb = Get-ChildItem $binDir -Filter "*.pdb" -ErrorAction SilentlyContinue |
           Sort-Object Length -Descending | Select-Object -First 1
}
if (-not $pdb) { Die "No PDB found in $binDir" }

# UVTD expects Major_Minor.pdb, zero-padded minor -> 5_08.pdb
$parts = $EngineVersion.Split('.')
$target = "{0}_{1:D2}.pdb" -f $parts[0], [int]$parts[1]

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$dest = Join-Path $OutDir $target
Info "Copying $($pdb.Name) -> $target ($([math]::Round($pdb.Length/1MB)) MB)"
Copy-Item $pdb.FullName $dest -Force

# ============================================================ validate
Step "Validate"

# UVTD needs these engine types in the PDB's type stream. Their names appear as
# raw ASCII inside the PDB, so a byte scan is a reliable presence check without
# needing the DIA SDK on this machine.
$need = @("FUObjectArray","FUObjectItem","UObjectBase","UStruct","UClass","FProperty","FField")
$found = @{}; foreach ($n in $need) { $found[$n] = $false }
$fs = [IO.File]::OpenRead($dest)
try {
    $bufSize = 8MB
    $buf = New-Object byte[] ($bufSize + 64)
    $carry = 0
    while (($read = $fs.Read($buf, $carry, $bufSize)) -gt 0) {
        $total = $carry + $read
        $slice = [Text.Encoding]::ASCII.GetString($buf, 0, $total)
        foreach ($n in $need) { if (-not $found[$n] -and $slice.Contains($n)) { $found[$n] = $true } }
        if ($found.Values -notcontains $false) { break }
        [Array]::Copy($buf, $total - 64, $buf, 0, 64)
        $carry = 64
    }
} finally { $fs.Close() }

$missing = $need | Where-Object { -not $found[$_] }
if ($missing) {
    Warn "Types NOT found in the PDB: $($missing -join ', ')"
    Warn "The PDB may be stripped or from the wrong configuration. UVTD will likely fail."
} else {
    Ok "All required engine types present"
}

$overall.Stop()
Write-Host ""
Ok "DONE in $([int]$overall.Elapsed.TotalMinutes) min"
Write-Host "  PDB:  $dest"
Write-Host "  Size: $([math]::Round((Get-Item $dest).Length/1MB)) MB"
Write-Host ""
Write-Host "Compress it (7-Zip; PDBs shrink 3-5x) and copy it to the modding machine." -ForegroundColor White
Write-Host "'$target' is the only artifact needed - the source tree can be deleted." -ForegroundColor White
