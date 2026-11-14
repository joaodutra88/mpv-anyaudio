# Installs anyaudio into Nuvio Desktop (Windows). Run again after every Nuvio update.
# Nuvio ignores mpv.conf and scripts, so a small libmpv-2.dll shim sits in front of the
# real one and loads them. Nothing inside the Nuvio install folder is changed.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$shim = Join-Path $PSScriptRoot 'libmpv-2.dll'
if (-not (Test-Path $shim)) { throw 'libmpv-2.dll is not next to this script: use the release zip or build it with make.cmd' }

$jar = Get-ChildItem "$env:ProgramFiles\Nuvio\app\composeApp-desktop-*.jar" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $jar) { throw "Nuvio Desktop not found in $env:ProgramFiles\Nuvio" }

# The real libmpv is copied from the user's own Nuvio: it is never redistributed.
$real = Join-Path $PSScriptRoot 'real\libmpv-2.dll'
New-Item -ItemType Directory -Force -Path (Split-Path $real) | Out-Null
$zip = [IO.Compression.ZipFile]::OpenRead($jar.FullName)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -eq 'native/windows/libmpv-2.dll' }
    if (-not $entry) { throw "no native/windows/libmpv-2.dll inside $($jar.Name)" }
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $real, $true)
} finally { $zip.Dispose() }

# Kept if it already exists: it is the user's place for their own mpv options.
$conf = Join-Path $PSScriptRoot 'nuvio-mpv.conf'
if (-not (Test-Path $conf)) {
    Set-Content -Path $conf -Value "scripts=$((Resolve-Path (Join-Path $PSScriptRoot '..\anyaudio.lua')).Path)"
}

[Environment]::SetEnvironmentVariable('NUVIO_LIBMPV_PATH', $shim, 'User')
"anyaudio installed for Nuvio ($($jar.Name)). Restart Nuvio, then use F6 / F7 / F8 in the player."
