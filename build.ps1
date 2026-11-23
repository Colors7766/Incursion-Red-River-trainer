$ErrorActionPreference='Stop'
$vsRoot='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$vs=Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$cmake=Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$desktop=[Environment]::GetFolderPath([Environment+SpecialFolder]::DesktopDirectory)
$output=Join-Path $desktop 'build'
$buildDirectory=Join-Path $output '.cmake'
Push-Location -LiteralPath $PSScriptRoot
try {
    cmd /c ('call "{0}" && "{1}" -S . -B "{2}" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={3}" && "{1}" --build "{2}" --config Release' -f $vs,$cmake,$buildDirectory,$output)
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
