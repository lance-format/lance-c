[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'ARM64')][string]$Architecture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'windows-import-exports.ps1')

$dll = Join-Path $Prefix 'lib\lance_c.dll'
$importLibrary = Join-Path $Prefix 'lib\lance_c.dll.lib'
$staticLibrary = Join-Path $Prefix 'lib\lance_c.lib'
foreach ($artifact in @($dll, $importLibrary, $staticLibrary)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "installed probe artifact is missing: $artifact"
    }
}

$msvcComponent = if ($Architecture -eq 'ARM64') {
    'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
} else {
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * `
    -requires $msvcComponent -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
    throw "no Visual Studio installation with $msvcComponent"
}
$visualStudio = $visualStudio | Select-Object -First 1
$relativePaths = if ($Architecture -eq 'ARM64') {
    @('bin\Hostarm64\arm64\dumpbin.exe', 'bin\Hostx64\arm64\dumpbin.exe')
} else {
    @('bin\Hostx64\x64\dumpbin.exe')
}
$dumpbin = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') `
    -Directory | Sort-Object Name -Descending | ForEach-Object {
        foreach ($relativePath in $relativePaths) {
            Join-Path $_.FullName $relativePath
        }
    } | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1
if (-not $dumpbin) {
    throw "dumpbin.exe not found for $Architecture"
}

$dllText = (& $dumpbin /nologo /exports $dll 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $dll`n$dllText"
}
$dllExports = @(
    [regex]::Matches(
        $dllText, '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(?:\s+=.*)?\s*$') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
)
if ($dllExports.Count -ne 1 -or $dllExports[0] -ne 'lance_api') {
    throw "installed DLL exports differ: $($dllExports -join ', ')"
}

$importText = (& $dumpbin /nologo /exports $importLibrary 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $importLibrary`n$importText"
}
$importExports = @(Get-WindowsImportLibraryExports -Text $importText | Sort-Object -Unique)
if ($importExports.Count -ne 1 -or $importExports[0] -ne 'lance_api') {
    throw "installed import-library exports differ: $($importExports -join ', ')"
}

Write-Host "Corrosion install export probe: PASS"
