# OpenDaw per-user installer (no admin rights, no extra software).
#
#   .\installer\install-user.ps1               install / update from build-release
#   .\installer\install-user.ps1 -Uninstall    remove it again
#
# Installs to %LOCALAPPDATA%\Programs\OpenDaw and adds:
#   - Start Menu and desktop shortcuts
#   - the .tracktionedit file association (current user)
#   - an "OpenDaw" entry in Settings > Apps > Installed apps (uninstall)
# Your projects, settings and plugin cache live elsewhere and are kept.
#
# Build first with:  .\build.ps1 -Release

param(
    [string]$Source = (Join-Path $PSScriptRoot "..\build-release\OpenDaw_artefacts\Release"),
    [string]$Dest = (Join-Path $env:LOCALAPPDATA "Programs\OpenDaw"),
    [switch]$NoDesktopShortcut,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$appName      = "OpenDaw"
$exeName      = "OpenDaw.exe"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenDaw"
$classesKey   = "HKCU:\Software\Classes"
$startMenuLnk = Join-Path ([Environment]::GetFolderPath("Programs")) "$appName.lnk"
$desktopLnk   = Join-Path ([Environment]::GetFolderPath("Desktop")) "$appName.lnk"

function Stop-InstalledCopies([string]$dir) {
    # Only instances running from the install folder; a dev build keeps running.
    if (-not (Test-Path $dir)) { return }
    $full = (Resolve-Path $dir).Path.TrimEnd('\') + '\'
    Get-Process -Name OpenDaw, opendaw-mcp, OpenDawPluginScanner -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($full, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object {
            Write-Host "Closing $($_.Name) (pid $($_.Id))..." -ForegroundColor Yellow
            $_.CloseMainWindow() | Out-Null
            if (-not $_.WaitForExit(5000)) { Stop-Process -Id $_.Id -Force }
        }
}

function Notify-Shell {
    # Tell Explorer the file associations changed (SHChangeNotify).
    $sig = '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);'
    $t = Add-Type -MemberDefinition $sig -Name Shell -Namespace OpenDawInstall -PassThru -ErrorAction SilentlyContinue
    if ($t) { $t::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero) }
}

function New-Shortcut([string]$path, [string]$target, [string]$workDir) {
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath = $target
    $lnk.WorkingDirectory = $workDir
    $lnk.IconLocation = "$target,0"
    $lnk.Description = "OpenDaw digital audio workstation"
    $lnk.Save()
}

if ($Uninstall) {
    Stop-InstalledCopies $Dest
    foreach ($l in @($startMenuLnk, $desktopLnk)) { if (Test-Path $l) { Remove-Item $l -Force } }
    if ((Get-ItemProperty "$classesKey\.tracktionedit" -ErrorAction SilentlyContinue).'(default)' -eq "OpenDaw.Project") {
        Remove-Item "$classesKey\.tracktionedit" -Recurse -Force
    }
    if (Test-Path "$classesKey\OpenDaw.Project") { Remove-Item "$classesKey\OpenDaw.Project" -Recurse -Force }
    if (Test-Path $uninstallKey) { Remove-Item $uninstallKey -Recurse -Force }
    Notify-Shell
    if (Test-Path $Dest) {
        # This script may be running from inside $Dest: remove the folder once it exits.
        $cmd = "ping -n 3 127.0.0.1 >nul & rmdir /s /q `"$Dest`""
        Start-Process -FilePath cmd.exe -ArgumentList "/c $cmd" -WindowStyle Hidden
    }
    Write-Host "OpenDaw was uninstalled. Your projects and settings were kept." -ForegroundColor Green
    exit 0
}

# ── Install / update ─────────────────────────────────────────────────────────
$Source = (Resolve-Path $Source -ErrorAction SilentlyContinue).Path
if (-not $Source -or -not (Test-Path (Join-Path $Source $exeName))) {
    throw "No Release build found. Build it first with: .\build.ps1 -Release"
}
foreach ($need in @("Qt6Core.dll", "platforms\qwindows.dll", "OpenDawPluginScanner.exe", "opendaw-mcp.exe", "soundfonts")) {
    if (-not (Test-Path (Join-Path $Source $need))) {
        throw "$need is missing from $Source. Run .\build.ps1 -Release (it deploys the runtime) and try again."
    }
}

Stop-InstalledCopies $Dest
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

Write-Host "Copying OpenDaw to $Dest ..." -ForegroundColor Cyan
# /MIR keeps the install an exact copy of the build (old files are removed);
# logs, crash dumps and debug files from test runs are not installed.
robocopy $Source $Dest /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:1 `
    /XF *.log *.dmp *.pdb *.ilk *.exp *.lib .opendaw-write-test `
    /XD __pycache__ | Out-Host
if ($LASTEXITCODE -ge 8) { throw "Copying failed (robocopy exit code $LASTEXITCODE)." }
Copy-Item -Force $PSCommandPath (Join-Path $Dest "uninstall.ps1")

$exe = Join-Path $Dest $exeName
$version = (Get-Item $exe).VersionInfo.ProductVersion
if (-not $version) { $version = "dev" }

Write-Host "Creating shortcuts..." -ForegroundColor Cyan
New-Shortcut $startMenuLnk $exe $Dest
if (-not $NoDesktopShortcut) { New-Shortcut $desktopLnk $exe $Dest }

Write-Host "Associating .tracktionedit files..." -ForegroundColor Cyan
New-Item -Force -Path "$classesKey\.tracktionedit" -Value "OpenDaw.Project" | Out-Null
New-Item -Force -Path "$classesKey\OpenDaw.Project" -Value "OpenDaw Project" | Out-Null
New-Item -Force -Path "$classesKey\OpenDaw.Project\DefaultIcon" -Value "`"$exe`",0" | Out-Null
New-Item -Force -Path "$classesKey\OpenDaw.Project\shell\open\command" -Value "`"$exe`" `"%1`"" | Out-Null
Notify-Shell

Write-Host "Registering in Installed apps..." -ForegroundColor Cyan
$sizeKb = [int]((Get-ChildItem $Dest -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
New-Item -Force -Path $uninstallKey | Out-Null
$props = @{
    DisplayName     = $appName
    DisplayVersion  = $version
    Publisher       = "OpenDaw contributors"
    DisplayIcon     = "`"$exe`",0"
    InstallLocation = $Dest
    UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$Dest\uninstall.ps1`" -Uninstall -Dest `"$Dest`""
    URLInfoAbout    = "https://github.com/glenwrhodes/OpenDaw"
}
foreach ($k in $props.Keys) { Set-ItemProperty -Path $uninstallKey -Name $k -Value $props[$k] }
Set-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name EstimatedSize -Value $sizeKb -Type DWord

Write-Host ""
Write-Host "OpenDaw $version is installed." -ForegroundColor Green
Write-Host "  Start it from the Start Menu or the desktop shortcut."
Write-Host "  For Claude Code: claude mcp add opendaw -- `"$Dest\opendaw-mcp.exe`" --launch"
Write-Host "  Uninstall from Settings > Apps > Installed apps > OpenDaw."
