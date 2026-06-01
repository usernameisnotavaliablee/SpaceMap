param(
    [string]$ExePath = ".\map.exe"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $ExePath)) {
    throw "Executable not found: $ExePath. Run .\build.bat first."
}

$fixture = Join-Path $PSScriptRoot ".smoke-fixture"
$outFile = Join-Path $PSScriptRoot ".smoke-output.json"

try {
    if (Test-Path -LiteralPath $fixture) {
        Remove-Item -LiteralPath $fixture -Recurse -Force
    }
    if (Test-Path -LiteralPath $outFile) {
        Remove-Item -LiteralPath $outFile -Force
    }

    New-Item -ItemType Directory -Force -Path (Join-Path $fixture "b_dir") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture "a_dir") | Out-Null
    Set-Content -LiteralPath (Join-Path $fixture "root.txt") -Value "root-data" -NoNewline
    Set-Content -LiteralPath (Join-Path $fixture "b_dir\big.bin") -Value ("x" * 2000) -NoNewline
    Set-Content -LiteralPath (Join-Path $fixture "a_dir\small.bin") -Value ("x" * 10) -NoNewline

    $json = & $ExePath -j -t 3 $fixture | ConvertFrom-Json
    if ($json.total.files -ne 3) {
        throw "Expected 3 files including root file, got $($json.total.files)"
    }
    if ($json.total.dirs -ne 3) {
        throw "Expected root plus 2 subdirectories, got $($json.total.dirs)"
    }
    if ($json.top_files[0].path -notlike "*b_dir*big.bin") {
        throw "Expected b_dir\big.bin to be largest file"
    }

    & $ExePath -j -o $outFile $fixture | Out-Null
    $fileJson = Get-Content -LiteralPath $outFile -Raw | ConvertFrom-Json
    if ($fileJson.total.files -ne 3) {
        throw "-o JSON file did not contain expected total"
    }

    $nameSorted = & $ExePath --sort=name --no-color $fixture
    if (($nameSorted -join "`n") -notmatch "a_dir[\s\S]*b_dir") {
        throw "Expected --sort=name output to list a_dir before b_dir"
    }

    Write-Host "Smoke tests passed"
} finally {
    if (Test-Path -LiteralPath $fixture) {
        Remove-Item -LiteralPath $fixture -Recurse -Force
    }
    if (Test-Path -LiteralPath $outFile) {
        Remove-Item -LiteralPath $outFile -Force
    }
}
