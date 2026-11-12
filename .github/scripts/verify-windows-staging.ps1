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
. (Join-Path $PSScriptRoot 'windows-import-exports.ps1')

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
$packageConfig = Join-Path $prefixPath 'lib\cmake\LanceC\LanceCConfig.cmake'

foreach ($required in @(
    $dll,
    $importLibrary,
    $staticLibrary,
    $header,
    $headerCpp,
    $readme,
    $license,
    $packageConfig
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "staged package is missing $required"
    }
    if ((Get-Item -LiteralPath $required).Length -eq 0) {
        throw "staged package file is empty: $required"
    }
}

$packageConfigText = Get-Content -LiteralPath $packageConfig -Raw
if ($packageConfigText -notmatch '(?m)^set\(LanceC_STATIC_CRT ON\)\s*$') {
    throw 'staged package does not declare LanceC_STATIC_CRT=ON'
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
    $exportText, '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(?:\s+=.*)?\s*$') |
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
$importExports = @(Get-WindowsImportLibraryExports -Text $importText | Sort-Object -Unique)
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
$dependencies = Get-PeDependencies -DependentsText $dependentsText
if ($dependencies.Direct.Count -eq 0) {
    throw "staged DLL reports no direct runtime dependencies:`n$dependentsText"
}
Assert-NoRedistributableRuntimeDependencies `
    -Dependencies $dependencies.Direct `
    -Description 'staged DLL direct imports'
Assert-NoRedistributableRuntimeDependencies `
    -Dependencies $dependencies.Delay `
    -Description 'staged DLL delay-load imports'

$system32 = Join-Path $env:SystemRoot 'System32'
foreach ($dependency in $dependencies.Direct) {
    $dependencySource = Resolve-RuntimeDependency `
        -Dependency $dependency `
        -PrefixPath $prefixPath `
        -System32 $system32
    switch ($dependencySource) {
        'api-set' {
            Write-Host "    direct $dependency -> API-set contract; native loader check follows"
        }
        'system32' {
            Write-Host "    direct $dependency -> System32"
        }
        'staged' {
            Write-Host "    direct $dependency -> staged lib"
        }
        default {
            throw "unknown dependency classification for ${dependency}: $dependencySource"
        }
    }
}
foreach ($dependency in $dependencies.Delay) {
    $dependencySource = Resolve-RuntimeDependency `
        -Dependency $dependency `
        -PrefixPath $prefixPath `
        -System32 $system32
    switch ($dependencySource) {
        'api-set' {
            Write-Host "    delay $dependency -> API-set contract; native loader check follows"
        }
        'system32' {
            Write-Host "    delay $dependency -> System32"
        }
        'staged' {
            Write-Host "    delay $dependency -> staged lib"
        }
        default {
            throw "unknown dependency classification for ${dependency}: $dependencySource"
        }
    }
}
Write-Host ("==> Inspected $($dependencies.Direct.Count) direct and " +
    "$($dependencies.Delay.Count) delay-load DLL dependencies")

$staticDirectives = @(
    & $script:Dumpbin /directives $staticLibrary 2>&1 |
        Select-String -Pattern '/DEFAULTLIB:'
)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /directives failed for the staged static library"
}
$staticDirectiveText = $staticDirectives -join "`n"
if ($staticDirectiveText -notmatch '(?i)/DEFAULTLIB:(LIBCMT|libcmt)') {
    throw "staged static library does not carry a static CRT default library:`n$staticDirectiveText"
}
if ($staticDirectiveText -match
    '(?i)/DEFAULTLIB:(MSVCRT|MSVCRTD|VCRUNTIME|MSVCP)') {
    throw "staged static library carries an incompatible CRT default library:`n$staticDirectiveText"
}
Write-Host '==> Verified static archive CRT directives'

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
    $buildOutput = & cmake @buildArguments 2>&1
    $buildOutput | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed for $Linkage consumer with exit code $LASTEXITCODE"
    }
    $crtLinkDiagnostics = @(
        $buildOutput | Select-String -Pattern 'LNK4098|LNK2038'
    )
    if ($crtLinkDiagnostics.Count -ne 0) {
        throw ("$Linkage consumer produced an incompatible CRT link diagnostic:`n" +
            "$($crtLinkDiagnostics -join "`n")")
    }

    $executable = Join-Path $buildDir 'Release\acceptance.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "acceptance executable was not built: $executable"
    }
    Assert-PeImage -Path $executable -Description "$Linkage acceptance executable"

    $consumerObject = Join-Path $buildDir 'acceptance.dir\Release\acceptance.obj'
    if (-not (Test-Path -LiteralPath $consumerObject -PathType Leaf)) {
        throw "$Linkage consumer object was not built: $consumerObject"
    }
    $objectDirectives = @(
        & $script:Dumpbin /directives $consumerObject 2>&1 |
            Select-String -Pattern '/DEFAULTLIB:'
    )
    if ($LASTEXITCODE -ne 0) {
        throw "$Linkage consumer object directive inspection failed"
    }
    $objectDirectiveText = $objectDirectives -join "`n"
    if ($objectDirectiveText -notmatch '(?i)/DEFAULTLIB:(LIBCMT|libcmt)') {
        throw "$Linkage consumer object does not use the static CRT:`n$objectDirectiveText"
    }
    if ($objectDirectiveText -match
        '(?i)/DEFAULTLIB:(MSVCRT|MSVCRTD|VCRUNTIME|MSVCP)') {
        throw "$Linkage consumer object uses an incompatible CRT:`n$objectDirectiveText"
    }
    Write-Host "==> Verified $Linkage consumer object CRT directives"

    $vcxproj = Join-Path $buildDir 'acceptance.vcxproj'
    $runtimeLibraries = @(Get-MsvcRuntimeLibraries -ProjectPath $vcxproj)
    if ($runtimeLibraries.Count -eq 0) {
        throw "$Linkage consumer does not declare MSVC_RUNTIME_LIBRARY"
    }
    $unexpectedRuntimeLibraries = @(
        $runtimeLibraries | Where-Object { $_ -ne 'MultiThreaded' }
    )
    if ($unexpectedRuntimeLibraries.Count -ne 0) {
        throw ("$Linkage consumer does not use the /MT runtime: " +
            "$($unexpectedRuntimeLibraries -join ', ')")
    }
    Write-Host "==> Verified $Linkage consumer runtime library: MultiThreaded"

    $executableDependents = (Invoke-Dumpbin @('/dependents', $executable)) -join "`n"
    $executableDependencies = Get-PeDependencies `
        -DependentsText $executableDependents
    Assert-NoRedistributableRuntimeDependencies `
        -Dependencies $executableDependencies.Direct `
        -Description "$Linkage acceptance direct imports"
    Assert-NoRedistributableRuntimeDependencies `
        -Dependencies $executableDependencies.Delay `
        -Description "$Linkage acceptance delay-load imports"
    if ($Linkage -eq 'shared') {
        Copy-Item -LiteralPath $dll -Destination (Split-Path -Parent $executable) -Force
        if ($executableDependencies.Direct -notcontains 'lance_c.dll') {
            throw "shared acceptance executable does not import lance_c.dll"
        }
    } elseif ($executableDependencies.Direct -contains 'lance_c.dll') {
        throw "static acceptance executable unexpectedly imports lance_c.dll"
    }

    $dataRoot = Join-Path $workPath "dataset-$Linkage"
    New-Item -ItemType Directory -Path $dataRoot -Force | Out-Null
    $dataPath = (Resolve-Path -LiteralPath $dataRoot).Path -replace '\\', '/'
    $uri = "file:///$dataPath/acceptance.lance"

    Write-Host "==> Running installed $Linkage consumer with system-only PATH"
    $previousPath = $env:Path
    $previousModuleReport = $env:LANCE_C_MODULE_REPORT
    $moduleReport = Join-Path $workPath "loaded-modules-$Linkage.txt"
    Remove-Item -LiteralPath $moduleReport -Force -ErrorAction SilentlyContinue
    try {
        $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:LANCE_C_MODULE_REPORT = $moduleReport
        & $executable $uri
        if ($LASTEXITCODE -ne 0) {
            throw "$Linkage acceptance consumer failed with exit code $LASTEXITCODE"
        }
    } finally {
        $env:Path = $previousPath
        if ($null -eq $previousModuleReport) {
            Remove-Item Env:\LANCE_C_MODULE_REPORT `
                -ErrorAction SilentlyContinue
        } else {
            $env:LANCE_C_MODULE_REPORT = $previousModuleReport
        }
    }

    if (-not (Test-Path -LiteralPath $moduleReport -PathType Leaf)) {
        throw "$Linkage acceptance consumer did not write its loaded-module report"
    }
    $loadedModules = @(
        Get-Content -LiteralPath $moduleReport |
            Where-Object { $_ -match '\.dll$' } |
            Sort-Object -Unique
    )
    if ($loadedModules.Count -eq 0) {
        throw "$Linkage loaded-module report is empty"
    }
    Assert-NoRedistributableRuntimeDependencies `
        -Dependencies $loadedModules `
        -Description "$Linkage loaded modules"
    $hasLanceDll = $loadedModules -contains 'lance_c.dll'
    if ($Linkage -eq 'shared' -and -not $hasLanceDll) {
        throw "shared acceptance executable did not load lance_c.dll"
    }
    if ($Linkage -eq 'static' -and $hasLanceDll) {
        throw "static acceptance executable unexpectedly loaded lance_c.dll"
    }
    Write-Host "==> Verified $($loadedModules.Count) loaded modules for $Linkage consumer"
    $loadedModules | ForEach-Object { Write-Host "    $_" }
}

Test-InstalledConsumer -Linkage static
Test-InstalledConsumer -Linkage shared

Write-Host "==> Windows staging verification passed"
