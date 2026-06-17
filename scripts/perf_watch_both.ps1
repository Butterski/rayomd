param(
    [string]$WindowsBinary = "build/windows/fast-markdown-imgui.exe",
    [string]$LinuxBinary = "build/linux/fast-markdown",
    [ValidateSet("quick", "watch", "full")]
    [string]$Suite = "watch",
    [int]$Seed = 1337,
    [string]$Label = "local"
)

$ErrorActionPreference = "Stop"

function Quote-Sh([string]$Value) {
    if ($Value.Contains("'")) {
        throw "WSL shell arguments with single quotes are not supported by this helper: $Value"
    }
    return "'" + $Value + "'"
}

python scripts/perf_watch.py `
    --binary $WindowsBinary `
    --platform windows `
    --suite $Suite `
    --seed $Seed `
    --label $Label

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Write-Warning "wsl.exe was not found; skipped Linux/WSL perf watch."
    exit 0
}

$repo = (Get-Location).Path
$wslRepo = (& wsl.exe wslpath -a $repo).Trim()
$linuxCommand = "cd $(Quote-Sh $wslRepo) && python3 scripts/perf_watch.py --binary $(Quote-Sh $LinuxBinary) --platform linux-wsl --suite $(Quote-Sh $Suite) --seed $Seed --label $(Quote-Sh $Label)"

& wsl.exe sh -lc $linuxCommand
