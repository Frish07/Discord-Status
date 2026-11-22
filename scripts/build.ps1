param([string]$OutputDirectory = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    $compiler = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $compiler -and (Test-Path 'C:\msys64\ucrt64\bin\g++.exe')) {
        $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
        $compiler = Get-Command g++
    }
    if (-not $compiler) { throw 'Install MinGW-w64 (MSYS2 UCRT64), or build with CMake + Visual Studio.' }
    & $compiler.Source -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -Ithird_party tests/presence_tests.cpp -static -o "$OutputDirectory/presence_tests.exe"
    if ($LASTEXITCODE) { throw 'Test compilation failed' }
    & "./$OutputDirectory/presence_tests.exe"
    if ($LASTEXITCODE) { throw 'Tests failed' }
    & (Join-Path (Split-Path $compiler.Source) 'windres.exe') -Iassets assets/app.rc -O coff -o "$OutputDirectory/app-resources.o"
    if ($LASTEXITCODE) { throw 'Resource compilation failed' }
    $uiFlags = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-Isrc', '-Ithird_party')
    $uiLibs = @('-static', '-static-libgcc', '-static-libstdc++', '-lcomdlg32', '-lcomctl32', '-lshell32', '-lole32', '-lgdi32', '-luser32', '-ld2d1', '-ldwrite', '-ldwmapi')
    & $compiler.Source @uiFlags src/main.cpp "$OutputDirectory/app-resources.o" -municode -mwindows @uiLibs -o "$OutputDirectory/DiscordPresenceManager.exe"
    if ($LASTEXITCODE) { throw 'Application compilation failed' }
    & $compiler.Source -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -Ithird_party tests/ipc_tests.cpp -static -o "$OutputDirectory/ipc_tests.exe"
    if ($LASTEXITCODE) { throw 'IPC test compilation failed' }
    & "./$OutputDirectory/ipc_tests.exe"
    if ($LASTEXITCODE) { throw 'IPC tests failed' }
    & $compiler.Source @uiFlags tests/window_tests.cpp "$OutputDirectory/app-resources.o" @uiLibs -o "$OutputDirectory/window_tests.exe"
    if ($LASTEXITCODE) { throw 'Window test compilation failed' }
    & "./$OutputDirectory/window_tests.exe"
    if ($LASTEXITCODE) { throw 'Window tests failed' }
} finally { Pop-Location }
