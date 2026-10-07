# The execution policy, if needed, can be bypassed only for the calling process.
$wslExecutable = Join-Path $env:WINDIR 'System32\wsl.exe'
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $wslExecutable = Join-Path $env:WINDIR 'Sysnative\wsl.exe'
}
if (-not (Test-Path -LiteralPath $wslExecutable)) { throw 'WSL executable was not found.' }

& $wslExecutable -d Ubuntu --cd $PSScriptRoot -- cmake --preset debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $wslExecutable -d Ubuntu --cd $PSScriptRoot -- cmake --build --preset debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$programArguments = @($args)
if ($programArguments.Count -eq 0) {
    $programArguments = @('data/demo.txt')
}
& $wslExecutable -d Ubuntu --cd $PSScriptRoot -- ./build/cmake-debug/gardeners @programArguments
exit $LASTEXITCODE
