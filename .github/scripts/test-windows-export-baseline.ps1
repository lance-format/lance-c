# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'windows-export-baseline.ps1')

# An additive baseline passes and reports the added export.
$additiveOutput = (Assert-WindowsExportBaseline `
        -BaselineExports @('lance_alpha', 'lance_beta') `
        -ActualExports @('lance_alpha', 'lance_beta', 'lance_gamma') `
        -BaselineDescription 'fixture' 6>&1) | Out-String
if ($additiveOutput -notmatch 'lance_gamma') {
    throw "the additive pass did not report lance_gamma:`n$additiveOutput"
}

# An exact baseline passes.
Assert-WindowsExportBaseline -BaselineExports @('lance_alpha') `
    -ActualExports @('lance_alpha') -BaselineDescription 'fixture'

# A removed published export fails and names the symbol.
$removedRejected = $false
try {
    Assert-WindowsExportBaseline -BaselineExports @('lance_alpha', 'lance_gamma') `
        -ActualExports @('lance_alpha') -BaselineDescription 'fixture'
} catch {
    if ($_.Exception.Message -notmatch 'lance_gamma') {
        throw
    }
    $removedRejected = $true
}
if (-not $removedRejected) {
    throw 'removing a published symbol was accepted'
}

# Export names are case-sensitive, so a case-different symbol is a removal.
$caseRejected = $false
try {
    Assert-WindowsExportBaseline -BaselineExports @('lance_alpha') `
        -ActualExports @('Lance_alpha') -BaselineDescription 'fixture'
} catch {
    $caseRejected = $true
}
if (-not $caseRejected) {
    throw 'a case-different export was accepted as present'
}

# Empty and invalid baselines are rejected.
foreach ($baseline in @(@(), @(''), @('lance_alpha', 'other_symbol'), @('Lance_alpha'))) {
    $rejected = $false
    try {
        Assert-WindowsExportBaseline -BaselineExports $baseline `
            -ActualExports @('lance_alpha') -BaselineDescription 'fixture'
    } catch {
        if ($_.Exception.Message -notmatch 'empty|invalid') {
            throw
        }
        $rejected = $true
    }
    if (-not $rejected) {
        throw "baseline was accepted: $($baseline -join ', ')"
    }
}

Write-Host 'Windows export baseline checks: PASS'
