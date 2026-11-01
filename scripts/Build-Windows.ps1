param([ValidateRange(1,64)][int]$Jobs=8)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(-not (Get-Command cl.exe -ErrorAction SilentlyContinue)){throw 'Run from an x64 Native Tools/Developer PowerShell for Visual Studio with MSVC installed.'}
foreach($tool in @('cmake.exe','ninja.exe')){if(-not (Get-Command $tool -ErrorAction SilentlyContinue)){throw "Missing build tool: $tool"}}
$build=Join-Path $root ('rebuild/'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
if(Test-Path -LiteralPath $build){throw 'Build directory exists.'}
$args=@('-S',(Join-Path $root 'source/yosys'),'-B',$build,'-G','Ninja',
 '-DCMAKE_BUILD_TYPE=Release','-DBUILD_SHARED_LIBS=OFF','-DYOSYS_WITHOUT_READLINE=ON',
 '-DYOSYS_WITHOUT_TCL=ON','-DYOSYS_WITHOUT_LIBFFI=ON','-DYOSYS_WITHOUT_SLANG=ON',
 '-DYOSYS_WITHOUT_ZLIB=ON','-DYOSYS_ENABLE_UNIT_TESTS=OFF','-DYOSYS_USE_BUNDLED_LIBS=ON',
 ('-DFLEX_EXECUTABLE='+(Join-Path $root 'tools/winflexbison/win_flex.exe')),
 ('-DBISON_EXECUTABLE='+(Join-Path $root 'tools/winflexbison/win_bison.exe')),
 '-DYOSYS_ABC_EXECUTABLE=', '-DYOSYS_SKIP_ABC_SUBMODULE_CHECK=ON',
 '-DYOSYS_CHECKOUT_INFO=9f75ca1f9-pmux-sharing-lut-decomposition',('-DCMAKE_INSTALL_PREFIX='+(Join-Path $build 'install')))
& cmake.exe @args
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed.'}
& cmake.exe --build $build --parallel $Jobs
if($LASTEXITCODE -ne 0){throw 'Build failed.'}
& cmake.exe --install $build
if($LASTEXITCODE -ne 0){throw 'Install failed.'}
Write-Host "Rebuilt files: $build. Frozen runtime was not overwritten. Revalidate a rebuilt executable before using it for results."
