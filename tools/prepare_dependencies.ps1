[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ImGuiTag = "v1.92.9b",
    [string]$NlohmannJsonTag = "v3.12.0"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# Normalize the argument in case a caller accidentally leaves a literal quote
# at the end of a quoted Windows path. MSBuild uses
# $(MSBuildProjectDirectory), which does not include a trailing backslash.
$normalizedProjectRoot = $ProjectRoot.Trim().Trim([char]34)
if ([string]::IsNullOrWhiteSpace($normalizedProjectRoot)) {
    throw "ProjectRoot cannot be empty."
}

$resolvedProjectRoot = (Resolve-Path -LiteralPath $normalizedProjectRoot).Path
$thirdPartyRoot = Join-Path $resolvedProjectRoot "third_party"
$imguiVersion = $ImGuiTag -replace '^[vV]', ''
$jsonVersion = $NlohmannJsonTag -replace '^[vV]', ''
$imguiTarget = Join-Path $thirdPartyRoot "imgui-$imguiVersion"
$jsonTarget = Join-Path $thirdPartyRoot "nlohmann-json-$jsonVersion"
$tempParent = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [System.IO.Path]::GetTempPath()
}
else {
    $env:RUNNER_TEMP
}
$tempRoot = Join-Path $tempParent ("AIQuotaChecker-deps-" + [Guid]::NewGuid().ToString("N"))

# Windows PowerShell 5.1 can otherwise negotiate an older TLS version on some systems.
if ($PSVersionTable.PSEdition -eq "Desktop") {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
}

New-Item -ItemType Directory -Path $thirdPartyRoot -Force | Out-Null
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

function Test-RequiredFiles {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$RelativePaths
    )

    foreach ($relativePath in $RelativePaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $relativePath) -PathType Leaf)) {
            return $false
        }
    }

    return $true
}

function Install-GitHubArchive {
    param(
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$ExtractedDirectoryName,
        [Parameter(Mandatory = $true)][string]$TargetDirectory,
        [Parameter(Mandatory = $true)][string[]]$RequiredRelativePaths
    )

    if (Test-RequiredFiles -Root $TargetDirectory -RelativePaths $RequiredRelativePaths) {
        Write-Host "[OK] $DisplayName already exists: $TargetDirectory"
        return
    }

    if (Test-Path -LiteralPath $TargetDirectory) {
        Remove-Item -LiteralPath $TargetDirectory -Recurse -Force
    }

    $safeName = $DisplayName -replace '[^A-Za-z0-9._-]', '_'
    $archivePath = Join-Path $tempRoot "$safeName.zip"
    $extractRoot = Join-Path $tempRoot "$safeName-extracted"

    Write-Host "[INFO] Downloading $DisplayName"
    Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $archivePath

    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot -Force

    $sourceDirectory = Join-Path $extractRoot $ExtractedDirectoryName
    if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) {
        throw "$DisplayName archive did not contain '$ExtractedDirectoryName'."
    }

    Move-Item -LiteralPath $sourceDirectory -Destination $TargetDirectory

    if (-not (Test-RequiredFiles -Root $TargetDirectory -RelativePaths $RequiredRelativePaths)) {
        throw "$DisplayName was extracted, but required files are missing."
    }

    Write-Host "[OK] $DisplayName prepared: $TargetDirectory"
}

try {
    Install-GitHubArchive `
        -DisplayName "Dear ImGui $ImGuiTag" `
        -Uri "https://github.com/ocornut/imgui/archive/refs/tags/$ImGuiTag.zip" `
        -ExtractedDirectoryName "imgui-$imguiVersion" `
        -TargetDirectory $imguiTarget `
        -RequiredRelativePaths @(
            "imgui.h",
            "imgui.cpp",
            "imgui_draw.cpp",
            "imgui_tables.cpp",
            "imgui_widgets.cpp",
            "backends\imgui_impl_dx11.h",
            "backends\imgui_impl_dx11.cpp",
            "backends\imgui_impl_win32.h",
            "backends\imgui_impl_win32.cpp"
        )

    Install-GitHubArchive `
        -DisplayName "nlohmann/json $NlohmannJsonTag" `
        -Uri "https://github.com/nlohmann/json/archive/refs/tags/$NlohmannJsonTag.zip" `
        -ExtractedDirectoryName "json-$jsonVersion" `
        -TargetDirectory $jsonTarget `
        -RequiredRelativePaths @("single_include\nlohmann\json.hpp")
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}

Write-Host "[OK] All dependencies are ready."
