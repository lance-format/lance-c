# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Verify a staged windows-msvc package and run the installed CMake consumer
# against it. The shared-library run copies lance_c.dll beside the executable
# and restricts PATH to the Windows system directories, so build-tool paths
# cannot supply missing runtime dependencies.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [ValidateSet('x64', 'ARM64')][string]$Architecture = 'x64',
    [string]$CMakeGenerator = 'Visual Studio 17 2022',
    [string]$CMakeToolset = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'windows-runtime-dependencies.ps1')

function Invoke-Dumpbin {
    param([string[]]$Arguments)

    $output = & $script:Dumpbin @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin $($Arguments -join ' ') failed with exit code $LASTEXITCODE`n$output"
    }
    return $output
}

$prefixPath = (Resolve-Path -LiteralPath $Prefix).Path
$sourcePath = (Resolve-Path -LiteralPath $SourceRoot).Path
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
$workPath = (Resolve-Path -LiteralPath $WorkDir).Path
$expectedMachine = "machine ($Architecture)"

$dll = Join-Path $prefixPath 'lib\lance_c.dll'
$importLibrary = Join-Path $prefixPath 'lib\lance_c.dll.lib'
$staticLibrary = Join-Path $prefixPath 'lib\lance_c.lib'
$header = Join-Path $prefixPath 'include\lance\lance.h'
$headerCpp = Join-Path $prefixPath 'include\lance\lance.hpp'
$readme = Join-Path $prefixPath 'share\doc\lance-c\README.md'
$license = Join-Path $prefixPath 'share\licenses\lance-c\LICENSE'

foreach ($required in @(
    $dll,
    $importLibrary,
    $staticLibrary,
    $header,
    $headerCpp,
    $readme,
    $license
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "staged package is missing $required"
    }
    if ((Get-Item -LiteralPath $required).Length -eq 0) {
        throw "staged package file is empty: $required"
    }
}

$msvcComponent = if ($Architecture -eq 'ARM64') {
    'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
} else {
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe not found at $vswhere"
}
$visualStudio = & $vswhere -latest -products * `
    -requires $msvcComponent `
    -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
    throw "no Visual Studio installation with MSVC component $msvcComponent was found"
}
$dumpbinRelativePaths = if ($Architecture -eq 'ARM64') {
    @('bin\Hostarm64\arm64\dumpbin.exe', 'bin\Hostx64\arm64\dumpbin.exe')
} else {
    @('bin\Hostx64\x64\dumpbin.exe')
}
$dumpbinCandidates = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') `
    -Directory | Sort-Object Name -Descending | ForEach-Object {
        foreach ($relativePath in $dumpbinRelativePaths) {
            Join-Path $_.FullName $relativePath
        }
    }
$script:Dumpbin = $dumpbinCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $script:Dumpbin) {
    throw "dumpbin.exe not found under $visualStudio"
}

function Assert-PeImage {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $headers = (Invoke-Dumpbin @('/headers', $Path)) -join "`n"
    if ($headers -notmatch [regex]::Escape($expectedMachine)) {
        throw "$Description is not $Architecture`n$headers"
    }
    if ($headers -notmatch 'magic # \(PE32\+\)') {
        throw "$Description is not PE32+:`n$headers"
    }
}

Write-Host "==> Verifying $Architecture package with $CMakeGenerator"
Write-Host "==> Inspecting $dll with $script:Dumpbin"
Assert-PeImage -Path $dll -Description 'staged DLL'

$headerSource = Get-Content -LiteralPath $header -Raw
$headerText = [regex]::Replace($headerSource, '(?s)/\*.*?\*/', ' ')
$headerText = [regex]::Replace($headerText, '(?m)//.*$', '')
$expectedExports = [regex]::Matches(
    $headerText, 'lance_[A-Za-z0-9_]+(?=\s*\()') |
    ForEach-Object { $_.Value } |
    Sort-Object -Unique
if ($expectedExports.Count -eq 0) {
    throw "no public lance_* declarations found in $header"
}

$exportText = (Invoke-Dumpbin @('/exports', $dll)) -join "`n"
$actualExports = [regex]::Matches(
    $exportText, '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$') |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique
if ($actualExports.Count -eq 0) {
    throw "staged DLL has no exports:`n$exportText"
}

$missingExports = @($expectedExports | Where-Object { $actualExports -notcontains $_ })
$extraExports = @($actualExports | Where-Object { $expectedExports -notcontains $_ })
if ($missingExports.Count -ne 0) {
    throw "staged DLL is missing public exports:`n$($missingExports -join "`n")"
}
if ($extraExports.Count -ne 0) {
    throw "staged DLL has non-header exports:`n$($extraExports -join "`n")"
}
if ($actualExports.Count -ne $expectedExports.Count) {
    throw "staged DLL has $($actualExports.Count) exports, expected $($expectedExports.Count)"
}
Write-Host "==> Verified $($expectedExports.Count) public lance_* exports ($($actualExports.Count) total exports)"

$importText = (Invoke-Dumpbin @('/exports', $importLibrary)) -join "`n"
$importExports = [regex]::Matches(
    $importText, '(?m)^\s*(?:\d+\s+)?(lance_[A-Za-z0-9_]+)\s*$') |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique
if ($importExports.Count -eq 0) {
    throw "staged import library reports no public exports:`n$importText"
}

$missingImportExports = @($expectedExports | Where-Object { $importExports -notcontains $_ })
$extraImportExports = @($importExports | Where-Object { $expectedExports -notcontains $_ })
if ($missingImportExports.Count -ne 0) {
    throw "staged import library is missing public exports:`n$($missingImportExports -join "`n")"
}
if ($extraImportExports.Count -ne 0) {
    throw "staged import library has non-header exports:`n$($extraImportExports -join "`n")"
}
if ($importExports.Count -ne $expectedExports.Count) {
    throw "staged import library has $($importExports.Count) exports, expected $($expectedExports.Count)"
}
Write-Host "==> Verified $($importExports.Count) public lance_* imports"

$dependentsText = (Invoke-Dumpbin @('/dependents', $dll)) -join "`n"
$dependencySection = ($dependentsText -split 'Image has the following dependencies:')[-1]
$dependencies = [regex]::Matches(
    $dependencySection, '(?m)^\s+([A-Za-z0-9_.-]+\.dll)\s*$') |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique
if ($dependencies.Count -eq 0) {
    throw "staged DLL reports no runtime dependencies:`n$dependentsText"
}

$system32 = Join-Path $env:SystemRoot 'System32'
foreach ($dependency in $dependencies) {
    $dependencySource = Resolve-RuntimeDependency `
        -Dependency $dependency `
        -PrefixPath $prefixPath `
        -System32 $system32
    switch ($dependencySource) {
        'api-set' {
            Write-Host "    $dependency -> API-set contract; native loader check follows"
        }
        'system32' {
            Write-Host "    $dependency -> System32"
        }
        'staged' {
            Write-Host "    $dependency -> staged lib"
        }
        default {
            throw "unknown dependency classification for ${dependency}: $dependencySource"
        }
    }
}
Write-Host "==> Inspected $($dependencies.Count) runtime DLL dependencies"

function Test-InstalledConsumer {
    param(
        [Parameter(Mandatory = $true)][string]$Linkage
    )

    $buildDir = Join-Path $workPath "consumer-$Linkage"
    $consumerSource = Join-Path $sourcePath 'examples\cmake-consumer'
    $configureArguments = @(
        '-S', $consumerSource,
        '-B', $buildDir,
        '-G', $CMakeGenerator,
        '-A', $Architecture,
        "-DCMAKE_PREFIX_PATH=$prefixPath",
        "-DLanceC_LINK=$Linkage",
        '-DCMAKE_BUILD_TYPE=Release'
    )
    if (-not [string]::IsNullOrWhiteSpace($CMakeToolset)) {
        $configureArguments += @('-T', $CMakeToolset)
    }
    & cmake @configureArguments
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed for $Linkage consumer with exit code $LASTEXITCODE"
    }
    $buildArguments = @(
        '--build', $buildDir, '--config', 'Release', '--parallel', '4'
    )
    & cmake @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed for $Linkage consumer with exit code $LASTEXITCODE"
    }

    $executable = Join-Path $buildDir 'Release\acceptance.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "acceptance executable was not built: $executable"
    }
    Assert-PeImage -Path $executable -Description "$Linkage acceptance executable"

    $executableDependents = (Invoke-Dumpbin @('/dependents', $executable)) -join "`n"
    if ($Linkage -eq 'shared') {
        Copy-Item -LiteralPath $dll -Destination (Split-Path -Parent $executable) -Force
        if ($executableDependents -notmatch '(?im)^\s*lance_c\.dll\s*$') {
            throw "shared acceptance executable does not import lance_c.dll"
        }
    } elseif ($executableDependents -match '(?im)^\s*lance_c\.dll\s*$') {
        throw "static acceptance executable unexpectedly imports lance_c.dll"
    }

    $dataRoot = Join-Path $workPath "dataset-$Linkage"
    New-Item -ItemType Directory -Path $dataRoot -Force | Out-Null
    $dataPath = (Resolve-Path -LiteralPath $dataRoot).Path -replace '\\', '/'
    $uri = "file:///$dataPath/acceptance.lance"

    Write-Host "==> Running installed $Linkage consumer with system-only PATH"
    $previousPath = $env:Path
    try {
        $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
        & $executable $uri
        if ($LASTEXITCODE -ne 0) {
            throw "$Linkage acceptance consumer failed with exit code $LASTEXITCODE"
        }
    } finally {
        $env:Path = $previousPath
    }
}

Test-InstalledConsumer -Linkage static
Test-InstalledConsumer -Linkage shared

Write-Host "==> Windows staging verification passed"
