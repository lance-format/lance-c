# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = $PSScriptRoot
$build = Join-Path (Split-Path -Parent $PSScriptRoot) 'build-crt-static-diagnostic'

& cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) {
    throw 'configure failed'
}

$buildOutput = & cmake --build $build --config Release --parallel 4 2>&1
$buildOutput | Write-Host
if ($LASTEXITCODE -ne 0) {
    throw 'build failed'
}
$crtWarnings = $buildOutput | Select-String -Pattern 'LNK4098|LNK2038'
if ($crtWarnings) {
    throw "incompatible CRT link diagnostic: $($crtWarnings -join "`n")"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * -property installationPath
$dumpbin = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') `
    -Directory |
    Sort-Object Name -Descending |
    ForEach-Object {
        Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe'
    } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $dumpbin) {
    throw 'dumpbin not found'
}

Write-Host '==> Consumer runtime-library property'
Select-String -LiteralPath (Join-Path $build 'probe.vcxproj') `
    -Pattern '<RuntimeLibrary>MultiThreaded</RuntimeLibrary>'

$exe = Join-Path $build 'Release\probe.exe'
$staticLibrary = Join-Path $build 'crt_probe.lib'
$consumerObject = Join-Path $build 'probe.dir\Release\static.obj'
$dll = Join-Path $build 'crt_probe.dll'

Write-Host '==> Consumer object CRT directives'
& $dumpbin /directives $consumerObject
if ($LASTEXITCODE -ne 0) {
    throw 'consumer object directive inspection failed'
}

Write-Host '==> Static archive CRT directives'
& $dumpbin /directives $staticLibrary
if ($LASTEXITCODE -ne 0) {
    throw 'static archive directive inspection failed'
}

Write-Host '==> Executable dependencies'
$dependents = (& $dumpbin /dependents $exe) -join "`n"
Write-Host $dependents
if ($dependents -match '(?i)\b(VCRUNTIME\d*|MSVCP\d*|MSVCR\d*|CONCRT\d*|VCOMP\d*)\.dll\b') {
    throw "static consumer imports a redistributable VC Runtime DLL:`n$dependents"
}

Write-Host '==> DLL dependencies'
$dllDependents = (& $dumpbin /dependents $dll) -join "`n"
Write-Host $dllDependents
if ($dllDependents -match '(?i)\b(VCRUNTIME\d*|MSVCP\d*|MSVCR\d*|CONCRT\d*|VCOMP\d*)\.dll\b') {
    throw "DLL imports a redistributable VC Runtime DLL:`n$dllDependents"
}

$oldPath = $env:Path
try {
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    & $exe
    if ($LASTEXITCODE -ne 0) {
        throw 'static consumer runtime probe failed'
    }
} finally {
    $env:Path = $oldPath
}
