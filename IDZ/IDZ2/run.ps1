param([switch]$Cond, [string]$InputFile = 'data/demo.txt')

$wslExecutable = Join-Path $env:WINDIR 'System32\wsl.exe'
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $wslExecutable = Join-Path $env:WINDIR 'Sysnative\wsl.exe'
}
if (-not (Test-Path -LiteralPath $wslExecutable)) { throw 'WSL executable was not found.' }
& $wslExecutable -d Ubuntu --cd $PSScriptRoot -- cmake --preset debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $wslExecutable -d Ubuntu --cd $PSScriptRoot -- cmake --build --preset debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$program = if ($Cond) { './build/cmake-debug/gardeners_cond' } else { './build/cmake-debug/gardeners' }
& $wslExecutable -d Ubuntu --cd $PSScriptRoot -- $program $InputFile
exit $LASTEXITCODE
