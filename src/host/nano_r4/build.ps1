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
if ($Upload -and -not $Port) {
    $boardListText = (& $ArduinoCli board list --format json 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to detect the Nano R4 serial port.'
    }

    $boardList = $boardListText | ConvertFrom-Json
    $nanoPorts = @($boardList.detected_ports | Where-Object {
        $_.PSObject.Properties.Name -contains 'matching_boards' -and
        @($_.matching_boards | Where-Object {
            $_.fqbn -eq 'arduino:renesas_uno:nanor4'
        }).Count -gt 0
    })

    if ($nanoPorts.Count -eq 0) {
        throw 'Arduino Nano R4 was not found. Connect it or pass -Port COMxx.'
    }
    if ($nanoPorts.Count -gt 1) {
        $addresses = ($nanoPorts | ForEach-Object { $_.port.address }) -join ', '
        throw "Multiple Nano R4 boards were found ($addresses). Pass -Port COMxx."
    }

    $Port = $nanoPorts[0].port.address
    Write-Host "Detected Arduino Nano R4 on $Port."
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$buildRoot = Join-Path $repo 'build\nano_r4'
$stageRoot = Join-Path $repo 'build\nano_r4_stage'
$sketch = Join-Path $stageRoot 'nano_r4'

$stageFull = [IO.Path]::GetFullPath($stageRoot)
if (-not $stageFull.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean staging path outside the repository: $stageFull"
}
if (Test-Path -LiteralPath $stageFull) {
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}
New-Item -ItemType Directory -Path $sketch -Force | Out-Null
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

# Arduino CLI copies a sketch before compiling it. Stage the unmodified Yajir
# core beside the host so the normal Arduino builder can compile every C file.
Get-ChildItem (Join-Path $repo 'src\core') -File |
    Where-Object { $_.Extension -in '.c', '.h' } |
    Copy-Item -Destination $sketch -Force
Copy-Item -LiteralPath (Join-Path $repo 'src\host\common\host_diag.c'),
                           (Join-Path $repo 'src\host\common\host_diag.h') `
          -Destination $sketch -Force
Get-ChildItem $PSScriptRoot -File |
    Where-Object { $_.Extension -in '.ino', '.cpp', '.h' } |
    Copy-Item -Destination $sketch -Force

# The core remains platform-neutral in the repository. Generated staging C
# files receive the Nano R4 configuration before their original first line.
$utf8 = [Text.UTF8Encoding]::new($false)
Get-ChildItem $sketch -Filter '*.c' | ForEach-Object {
    $source = [IO.File]::ReadAllText($_.FullName)
    [IO.File]::WriteAllText(
        $_.FullName,
        "#include `"yajir_build_config.h`"`n" + $source,
        $utf8)
}

& $ArduinoCli compile `
    --fqbn arduino:renesas_uno:nanor4 `
    --build-path $buildRoot `
    $sketch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Upload) {
    & $ArduinoCli upload `
        --fqbn arduino:renesas_uno:nanor4 `
        --port $Port `
        --input-dir $buildRoot `
        $sketch
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
