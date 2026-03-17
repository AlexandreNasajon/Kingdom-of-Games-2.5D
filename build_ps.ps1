$vsEnvScript = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$buildDir = 'C:\Users\User\Desktop\KOG-2.5D-Editor-de-mapa\build_cmake'

# Capture VS environment variables via cmd
$envOutput = cmd /c "`"$vsEnvScript`" x64 && set" 2>&1
$envOutput | Where-Object { $_ -match '^[A-Za-z_][A-Za-z0-9_]*=' } | ForEach-Object {
    $parts = $_ -split '=', 2
    [System.Environment]::SetEnvironmentVariable($parts[0], $parts[1], 'Process')
}

Write-Host "VS environment loaded. Running cmake build..."
& $cmake --build $buildDir --config Release -j4
$exit = $LASTEXITCODE
Write-Host "BUILD EXIT: $exit"
exit $exit
