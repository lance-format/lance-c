# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Establish that the Windows ARM64 job is running a native ARM64 process and
# Rust host toolchain. The staged PE checks separately establish the target
# architecture of the DLL and both installed consumers.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '1.94.0'
$expectedHost = 'aarch64-pc-windows-msvc'

if ($env:RUNNER_ARCH -ne 'ARM64') {
    throw "RUNNER_ARCH is '$env:RUNNER_ARCH', expected 'ARM64'"
}
if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') {
    throw "PROCESSOR_ARCHITECTURE is '$env:PROCESSOR_ARCHITECTURE', expected 'ARM64'"
}

$osArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
if ($osArchitecture -ne 'Arm64') {
    throw "OS architecture is '$osArchitecture', expected 'Arm64'"
}
$processArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
if ($processArchitecture -ne 'Arm64') {
    throw "PowerShell process architecture is '$processArchitecture', expected 'Arm64'"
}

$rustcOutput = & rustc --version --verbose
if ($LASTEXITCODE -ne 0) {
    throw "rustc --version --verbose failed with exit code $LASTEXITCODE"
}
$rustcVersion = $rustcOutput |
    Where-Object { $_ -match '^rustc ' } |
    Select-Object -First 1
$rustcHost = $rustcOutput |
    Where-Object { $_ -match '^host: ' } |
    Select-Object -First 1
if ((-not $rustcVersion) -or
        $rustcVersion -notmatch "^rustc $([regex]::Escape($expectedVersion)) ") {
    throw "rustc version is '$rustcVersion', expected $expectedVersion"
}
if ($rustcHost -ne "host: $expectedHost") {
    throw "rustc host is '$rustcHost', expected 'host: $expectedHost'"
}

$cargoVersion = (& cargo --version).Trim()
if ($LASTEXITCODE -ne 0 -or
        $cargoVersion -notmatch "^cargo $([regex]::Escape($expectedVersion)) ") {
    throw "cargo version is '$cargoVersion', expected $expectedVersion"
}

Write-Host "==> Native Windows ARM64 host verified"
Write-Host "    runner arch: $env:RUNNER_ARCH"
Write-Host "    OS architecture: $osArchitecture"
Write-Host "    PowerShell process architecture: $processArchitecture"
Write-Host "    $rustcVersion"
Write-Host "    $rustcHost"
Write-Host "    $cargoVersion"
