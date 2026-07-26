[CmdletBinding()]
param(
    [string]$ArduinoCli = $env:ARDUINO_CLI,
    [switch]$Upload,
    [string]$Port
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $ArduinoCli) {
    $command = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($command) {
        $ArduinoCli = $command.Source
    }
}
if (-not $ArduinoCli -or -not (Test-Path -LiteralPath $ArduinoCli)) {
    throw 'arduino-cli.exe was not found. Pass -ArduinoCli or set ARDUINO_CLI.'
}

$fqbn = 'm5stack:esp32:m5stack_cardputer'
if ($Upload -and -not $Port) {
    $boardListText = (& $ArduinoCli board list --format json 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to detect the Cardputer serial port.'
    }

    $boardList = $boardListText | ConvertFrom-Json
    $cardputerPorts = @($boardList.detected_ports | Where-Object {
        $_.PSObject.Properties.Name -contains 'matching_boards' -and
        @($_.matching_boards | Where-Object { $_.fqbn -eq $fqbn }).Count -gt 0
    })

    if ($cardputerPorts.Count -eq 0) {
        throw 'M5Stack Cardputer was not found. Connect it or pass -Port COMxx.'
    }
    if ($cardputerPorts.Count -gt 1) {
        $addresses = ($cardputerPorts | ForEach-Object { $_.port.address }) -join ', '
        throw "Multiple Cardputer boards were found ($addresses). Pass -Port COMxx."
    }

    $Port = $cardputerPorts[0].port.address
    Write-Host "Detected M5Stack Cardputer on $Port."
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$outputRoot = Join-Path $repo 'build\cardputer'
$buildRoot = Join-Path ([IO.Path]::GetTempPath()) 'yajir_cardputer_build'
$stageRoot = Join-Path $repo 'build\cardputer_stage'
$sketch = Join-Path $stageRoot 'cardputer'

$stageFull = [IO.Path]::GetFullPath($stageRoot)
if (-not $stageFull.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean staging path outside the repository: $stageFull"
}
Write-Host 'Preparing Cardputer build files...'
if (Test-Path -LiteralPath $stageFull) {
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}
New-Item -ItemType Directory -Path $sketch -Force | Out-Null
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$buildFull = [IO.Path]::GetFullPath($buildRoot)
$tempFull = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
if (-not $buildFull.StartsWith($tempFull, [StringComparison]::OrdinalIgnoreCase) -or
    $buildFull -eq $tempFull) {
    throw "Refusing to clean build path outside the temporary directory: $buildFull"
}
if (Test-Path -LiteralPath $buildFull) {
    Remove-Item -LiteralPath $buildFull -Recurse -Force
}
New-Item -ItemType Directory -Path $buildFull -Force | Out-Null

Get-ChildItem (Join-Path $repo 'src\core') -File |
    Where-Object { $_.Extension -in '.c', '.h' } |
    Copy-Item -Destination $sketch -Force
Copy-Item -LiteralPath (Join-Path $repo 'src\host\common\host_diag.c'),
                           (Join-Path $repo 'src\host\common\host_diag.h') `
          -Destination $sketch -Force
Get-ChildItem $PSScriptRoot -File |
    Where-Object { $_.Extension -in '.ino', '.cpp', '.h' } |
    Copy-Item -Destination $sketch -Force

$utf8 = [Text.UTF8Encoding]::new($false)
Get-ChildItem $sketch -Filter '*.c' | ForEach-Object {
    $source = [IO.File]::ReadAllText($_.FullName)
    [IO.File]::WriteAllText(
        $_.FullName,
        "#include `"yajir_build_config.h`"`n" + $source,
        $utf8)
}

Write-Host 'Compiling Yajir for Cardputer (this can take about 1-2 minutes)...'
& $ArduinoCli compile `
    --fqbn $fqbn `
    --build-path $buildFull `
    --output-dir $outputRoot `
    $sketch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Cardputer build completed: $outputRoot"

if ($Upload) {
    Write-Host "Uploading Cardputer firmware to $Port..."
    & $ArduinoCli upload `
        --fqbn $fqbn `
        --port $Port `
        --input-dir $outputRoot `
        $sketch
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "Cardputer firmware uploaded to $Port."
}
