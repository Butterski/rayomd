param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [Parameter(Mandatory = $true)][string]$TinyAscii,
    [Parameter(Mandatory = $true)][string]$Tester,
    [Parameter(Mandatory = $true)][string]$Table,
    [Parameter(Mandatory = $true)][string]$LargeMixed,
    [ValidateRange(1, 100000)][int]$TinyIterations = 1000,
    [ValidateRange(1, 100000)][int]$TesterIterations = 100,
    [ValidateRange(1, 100000)][int]$TableIterations = 50,
    [ValidateRange(1, 100000)][int]$MixedIterations = 50
)

$ErrorActionPreference = "Stop"
$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Windows Performance Recorder CPU sampling requires an elevated PowerShell session."
}

$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$cases = @(
    @{ Name = "tiny-ascii"; Input = (Resolve-Path -LiteralPath $TinyAscii).Path; Iterations = $TinyIterations },
    @{ Name = "tester"; Input = (Resolve-Path -LiteralPath $Tester).Path; Iterations = $TesterIterations },
    @{ Name = "table"; Input = (Resolve-Path -LiteralPath $Table).Path; Iterations = $TableIterations },
    @{ Name = "large-mixed"; Input = (Resolve-Path -LiteralPath $LargeMixed).Path; Iterations = $MixedIterations }
)

$records = @()
foreach ($case in $cases) {
    $recorder = Start-Process -FilePath "wpr.exe" `
        -ArgumentList @("-start", "CPU", "-filemode") `
        -Wait -PassThru -WindowStyle Hidden
    if ($recorder.ExitCode -ne 0) {
        throw "WPR start failed for $($case.Name) with exit code $($recorder.ExitCode)."
    }

    try {
        $workDir = Join-Path $outputPath ($case.Name + "-work")
        $converter = Start-Process -FilePath $binaryPath `
            -ArgumentList @("--bench", $case.Input, $workDir, [string]$case.Iterations, "modern", "normal") `
            -Wait -PassThru -WindowStyle Hidden
        if ($converter.ExitCode -ne 0) {
            throw "RayoMD failed for $($case.Name) with exit code $($converter.ExitCode)."
        }

        $etlPath = Join-Path $outputPath ($case.Name + ".etl")
        $recorder = Start-Process -FilePath "wpr.exe" `
            -ArgumentList @("-stop", $etlPath) `
            -Wait -PassThru -WindowStyle Hidden
        if ($recorder.ExitCode -ne 0) {
            throw "WPR stop failed for $($case.Name) with exit code $($recorder.ExitCode)."
        }
        $records += "$($case.Name)`t$etlPath`t$((Get-Item -LiteralPath $etlPath).Length)"
    }
    catch {
        Start-Process -FilePath "wpr.exe" -ArgumentList @("-cancel") `
            -Wait -WindowStyle Hidden | Out-Null
        throw
    }
}

$records | Set-Content -LiteralPath (Join-Path $outputPath "captures.tsv") -Encoding UTF8
$records