# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'windows-import-exports.ps1')

# Layout follows retained MSVC dumpbin output, including the Summary section.
# Non-lance and decorated symbols must survive extraction so staging rejects them.
$fixture = @'
Microsoft (R) COFF/PE Dumper
Dump of file lance_c.dll.lib

File Type: LIBRARY

     Exports

       ordinal    name

                  lance_dataset_open
                  crc_fast_checksum
            7     lance_dataset_close
                  ?foreign@@YAHXZ

  Summary

          C0 .debug$S
          14 .idata$2
'@
$expected = @('lance_dataset_open', 'crc_fast_checksum', 'lance_dataset_close', '?foreign@@YAHXZ')
foreach ($text in @($fixture, ($fixture -replace '\r?\n', "`r`n"))) {
    $actual = @(Get-WindowsImportLibraryExports -Text $text)
    if (($actual -join ',') -cne ($expected -join ',')) {
        throw "Import-library export names differ: $($actual -join ', ')"
    }
}

foreach ($invalid in @('', 'File Type: LIBRARY', "ordinal name`ninvalid row with spaces")) {
    $rejected = $false
    try {
        $null = Get-WindowsImportLibraryExports -Text $invalid
    } catch {
        if ($_.Exception.Message -notmatch 'no export table|Unrecognized import-library export row') {
            throw
        }
        $rejected = $true
    }
    if (-not $rejected) {
        throw "Invalid import-library dump was accepted: $invalid"
    }
}
Write-Host 'Import-library export parsing: PASS'
