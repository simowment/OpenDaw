# OpenDaw Build Script
# Usage:
#   .\build.ps1           - incremental build
#   .\build.ps1 -Clean    - full reconfigure + build
#   .\build.ps1 -Test     - build, then run the test suite (ctest)
#   .\build.ps1 -Release  - optimised build in build-release (no tests), made
#                           runnable from any shell: the Qt and FFmpeg DLLs
#                           are copied beside the exe. Debug Qt + Debug JUCE
#                           are many times slower: use this for making music.
#                           -> build-release\OpenDaw_artefacts\Release\OpenDaw.exe
#
# Finds Visual Studio (any edition, incl. Build Tools) with vswhere, Qt under
# $env:QT_DIR or C:\Qt\6.10.2\msvc2022_64, CMake/Ninja on PATH or in Qt\Tools,
# and FFmpeg through vcpkg ($env:VCPKG_ROOT or C:\vcpkg).

param(
    [switch]$Clean,
    [switch]$Test,
    [switch]$Release
)

$ErrorActionPreference = "Stop"

# Native tools (vcvarsall, cmake, windeployqt) write progress to stderr; in
# Windows PowerShell 5.1 that would abort the script under "Stop". Run them
# with "Continue" and judge them by their exit code only.
function Invoke-Native([scriptblock]$cmd) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $cmd 2>&1 | ForEach-Object { "$_" } | Out-Host } finally { $ErrorActionPreference = $prev }
}

function Find-First([string[]]$candidates) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# vcvarsall itself calls vswhere; make sure it can find it.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;" + $env:PATH
$vsRoot = $null
if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$vcvars = Find-First @(
    $(if ($vsRoot) { Join-Path $vsRoot "VC\Auxiliary\Build\vcvarsall.bat" }),
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")
if (-not $vcvars) { throw "vcvarsall.bat not found: install Visual Studio 2022 (or Build Tools) with the C++ workload." }

$qtPath = Find-First @($env:QT_DIR, "C:\Qt\6.10.2\msvc2022_64")
if (-not $qtPath) { throw "Qt 6.10.2 msvc2022_64 not found. Set QT_DIR." }
$qtPath = $qtPath -replace '\\', '/'

$cmake = Find-First @((Get-Command cmake -ErrorAction SilentlyContinue).Source, "C:\Qt\Tools\CMake_64\bin\cmake.exe")
$ninja = Find-First @((Get-Command ninja -ErrorAction SilentlyContinue).Source, "C:\Qt\Tools\Ninja\ninja.exe")
if (-not $cmake) { throw "cmake not found" }
if (-not $ninja) { throw "ninja not found" }
$ninja = $ninja -replace '\\', '/'
$ctest = Join-Path (Split-Path $cmake) "ctest.exe"

$vcpkgRoot = Find-First @($env:VCPKG_ROOT, "C:\vcpkg")
$toolchain = ""
if ($vcpkgRoot) { $toolchain = "-DCMAKE_TOOLCHAIN_FILE=" + (($vcpkgRoot -replace '\\', '/') + "/scripts/buildsystems/vcpkg.cmake") }

# Kill running instances (a running helper locks its exe and the link fails)
foreach ($name in @("OpenDaw", "opendaw-mcp", "OpenDawPluginScanner", "OpenDawTests")) {
    if (Get-Process -Name $name -ErrorAction SilentlyContinue) {
        Write-Host "Stopping running $name..." -ForegroundColor Yellow
        taskkill /IM "$name.exe" /F 2>$null | Out-Null
    }
}
Start-Sleep -Milliseconds 300

# Debug and Release live in separate build trees, so switching between them
# never rebuilds the other one.
if ($Release) {
    $buildDir = "build-release"
    $config = "Release"
    $extraArgs = "-DCMAKE_BUILD_TYPE=Release -DOPENDAW_BUILD_TESTS=OFF"
    if ($Test) {
        Write-Host "-Test is ignored with -Release (the tests are not built there)." -ForegroundColor Yellow
        $Test = $false
    }
} else {
    $buildDir = "build"
    $config = "Debug"
    $extraArgs = ""
}
$outDir = Join-Path $buildDir "OpenDaw_artefacts\$config"

if ($Clean -or !(Test-Path "$buildDir\build.ninja")) {
    Write-Host "`n=== Configuring ($config) ===" -ForegroundColor Cyan
    $configCmd = "`"$vcvars`" x64 && `"$cmake`" -B $buildDir -G Ninja $extraArgs -DCMAKE_PREFIX_PATH=$qtPath $toolchain -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_MAKE_PROGRAM=$ninja 2>&1"
    Invoke-Native { cmd /c $configCmd }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nConfigure FAILED" -ForegroundColor Red
        exit 1
    }
}

Write-Host "`n=== Building ($config) ===" -ForegroundColor Cyan
$buildCmd = "`"$vcvars`" x64 && `"$cmake`" --build $buildDir 2>&1"
Invoke-Native { cmd /c $buildCmd }
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBuild FAILED" -ForegroundColor Red
    exit 1
}

if ($Release) {
    # The release workflow's steps, so the exe runs without Qt on PATH: the
    # release Qt DLLs and plugins (platforms\qwindows.dll, ...) beside the exe
    # and its MCP helper, and the FFmpeg DLLs (vcpkg's applocal step usually
    # copies those already; this makes sure).
    Write-Host "`n=== Deploying runtime ===" -ForegroundColor Cyan
    $windeployqt = Find-First @((Join-Path $qtPath "bin\windeployqt.exe"), (Join-Path $qtPath "bin\windeployqt6.exe"))
    if (-not $windeployqt) { throw "windeployqt not found in $qtPath/bin" }
    foreach ($exe in @("OpenDaw.exe", "opendaw-mcp.exe")) {
        $path = Join-Path $outDir $exe
        if (-not (Test-Path $path)) { throw "$path was not built" }
        Invoke-Native { & $windeployqt --release --no-translations --no-opengl-sw --verbose 0 $path }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "`nwindeployqt FAILED for $exe" -ForegroundColor Red
            exit 1
        }
    }
    if (-not (Test-Path (Join-Path $outDir "OpenDawPluginScanner.exe"))) {
        Write-Host "Warning: OpenDawPluginScanner.exe is missing; plugins would be scanned in-process." -ForegroundColor Yellow
    }
    if ($vcpkgRoot) {
        $vcpkgBin = Join-Path $vcpkgRoot "installed\x64-windows\bin"
        if (Test-Path $vcpkgBin) {
            foreach ($pattern in @("avcodec-*.dll", "avformat-*.dll", "avutil-*.dll", "swscale-*.dll", "swresample-*.dll", "libmp3lame*.dll")) {
                Get-ChildItem $vcpkgBin -Filter $pattern | ForEach-Object { Copy-Item $_.FullName $outDir -Force }
            }
        }
    }
}

Write-Host "`nBuild OK  ->  $outDir\OpenDaw.exe" -ForegroundColor Green

if ($Test) {
    Write-Host "`n=== Testing ===" -ForegroundColor Cyan
    Invoke-Native { & $ctest --test-dir $buildDir --output-on-failure }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nTests FAILED" -ForegroundColor Red
        exit 1
    }
    Write-Host "`nTests OK" -ForegroundColor Green
}
