# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InstallPath
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallPath)) {
    throw "path argument is empty"
}
if ($InstallPath.Contains(";")) {
    throw "path argument must not contain ';'"
}

$ResolvedPath = [System.IO.Path]::GetFullPath($InstallPath).TrimEnd("\")

function Split-VdsPathList {
    param(
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return @()
    }

    return $Value -split ";" |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_.Length -gt 0 }
}

function Test-VdsPathListContains {
    param(
        [string[]]$Entries,
        [string]$Path
    )

    $NormalizedPath = $Path.TrimEnd("\")
    foreach ($Entry in $Entries) {
        if ($Entry.TrimEnd("\").Equals(
                $NormalizedPath,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Join-VdsPathList {
    param(
        [string[]]$Entries
    )

    return ($Entries | Where-Object { $_.Length -gt 0 }) -join ";"
}

$UserPath = [System.Environment]::GetEnvironmentVariable(
    "Path",
    [System.EnvironmentVariableTarget]::User)
$UserEntries = @(Split-VdsPathList -Value $UserPath)
if (!(Test-VdsPathListContains -Entries $UserEntries -Path $ResolvedPath)) {
    $UserEntries += $ResolvedPath
    [System.Environment]::SetEnvironmentVariable(
        "Path",
        (Join-VdsPathList -Entries $UserEntries),
        [System.EnvironmentVariableTarget]::User)
}

$ProcessEntries = @(Split-VdsPathList -Value $env:Path)
if (!(Test-VdsPathListContains -Entries $ProcessEntries -Path $ResolvedPath)) {
    $ProcessEntries += $ResolvedPath
    $env:Path = Join-VdsPathList -Entries $ProcessEntries
}

Write-Output "Added to PATH: $ResolvedPath"
