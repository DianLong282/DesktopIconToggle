#requires -version 5.1
<#
    Build DesktopIconToggle.exe

    Does NOT rely on vcvars or cmd.exe: it locates MSVC + the Windows SDK,
    sets INCLUDE / LIB, then invokes cl.exe directly. That keeps non-ASCII
    source paths out of the command interpreter's hands.

    Usage:
        pwsh -ExecutionPolicy Bypass -File build.ps1
        pwsh -ExecutionPolicy Bypass -File build.ps1 -Config Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$srcDir = Join-Path $root 'src'
$outDir = Join-Path $root 'dist'
$objDir = Join-Path $root "build\$Config"

if ($Clean) {
    foreach ($d in @($outDir, (Join-Path $root 'build'))) {
        if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    }
}

New-Item -ItemType Directory -Force -Path $outDir, $objDir | Out-Null

# ------------------------------------------------------------- locate toolchain
function Find-Msvc {
    $roots = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*' -Directory -ErrorAction SilentlyContinue
    $hit = $roots |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') } |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $hit) { throw 'MSVC toolchain not found (need VS Build Tools with C++ desktop workload).' }
    return $hit.FullName
}

function Find-Sdk {
    $hit = Get-ChildItem 'C:\Program Files*\Windows Kits\10\Include\*' -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $hit) { throw 'Windows SDK headers not found.' }
    $ver = $hit.Name
    $sdk = (Get-ChildItem 'C:\Program Files*\Windows Kits\10' -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "Include\$ver\um\windows.h") } |
            Select-Object -First 1).FullName
    if (-not $sdk) { throw 'Windows SDK root not found.' }
    return [pscustomobject]@{ Version = $ver; Root = $sdk }
}

$msvc = Find-Msvc
$sdk  = Find-Sdk

$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$rc = (Get-ChildItem 'C:\Program Files*\Windows Kits\10\bin\*\x64\rc.exe' -ErrorAction SilentlyContinue |
       Sort-Object FullName -Descending | Select-Object -First 1).FullName
if (-not $rc) { throw 'rc.exe (resource compiler) not found.' }

$env:INCLUDE = @(
    (Join-Path $msvc 'include')
    (Join-Path $sdk.Root "Include\$($sdk.Version)\ucrt")
    (Join-Path $sdk.Root "Include\$($sdk.Version)\shared")
    (Join-Path $sdk.Root "Include\$($sdk.Version)\um")
    (Join-Path $sdk.Root "Include\$($sdk.Version)\winrt")
) -join ';'

$env:LIB = @(
    (Join-Path $msvc 'lib\x64')
    (Join-Path $sdk.Root "Lib\$($sdk.Version)\ucrt\x64")
    (Join-Path $sdk.Root "Lib\$($sdk.Version)\um\x64")
) -join ';'

Write-Host "MSVC      : $msvc"
Write-Host "SDK       : $($sdk.Version)"
Write-Host "Config    : $Config"

# ------------------------------------------------------------------- icon
# src/app.ico is a source file kept in version control, so this branch is
# normally never taken -- it only runs if the .ico was deleted.
$ico = Join-Path $srcDir 'app.ico'
if (-not (Test-Path $ico)) {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue }
    if (-not $py) {
        throw 'src/app.ico is missing and no Python was found. Restore it from the repository, or install Python + Pillow (pip install Pillow) and retry.'
    }
    Write-Host "Icon      : generating $ico"
    & $py.Source (Join-Path $root 'tools\make_icon.py') $ico
    if ($LASTEXITCODE -ne 0) { throw 'Icon generation failed (is Pillow installed? try: pip install Pillow).' }
}

# ----------------------------------------------------------------- resource
$res = Join-Path $objDir 'app.res'
Write-Host "Resource  : app.rc"
& $rc /nologo /fo $res (Join-Path $srcDir 'app.rc')
if ($LASTEXITCODE -ne 0) { throw "rc.exe failed ($LASTEXITCODE)" }

# ------------------------------------------------------------ compile + link
$cflags = @('/nologo', '/utf-8', '/W3', '/EHsc', '/DUNICODE', '/D_UNICODE')
if ($Config -eq 'Release') {
    $cflags += @('/O2', '/MT', '/GS', '/GL', '/DNDEBUG')
} else {
    $cflags += @('/Od', '/MTd', '/Zi', '/RTC1', '/D_DEBUG')
}

$linkFlags = @(
    '/SUBSYSTEM:WINDOWS', '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO',
    'user32.lib', 'shell32.lib', 'advapi32.lib', 'gdi32.lib', 'comctl32.lib'
)
if ($Config -eq 'Release') { $linkFlags += '/LTCG' }

$exe = Join-Path $outDir 'DesktopIconToggle.exe'

Write-Host "Compile   : main.cpp"
& $cl @cflags "/Fo$objDir\" (Join-Path $srcDir 'main.cpp') $res "/link" "/OUT:$exe" @linkFlags
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed ($LASTEXITCODE)" }

$item = Get-Item $exe
Write-Host ''
Write-Host ("OK: {0}  ({1:N0} bytes)" -f $item.FullName, $item.Length) -ForegroundColor Green
