[CmdletBinding()]
param(
    [ValidateSet('x86','x64','arm64')][string]$Architecture = 'x64',
    [string]$BuildDirectory = 'build',
    [string[]]$CMakeArguments = @(),
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$llvm = Join-Path $env:ProgramFiles 'LLVM/bin'
$env:PATH = "$llvm;$env:PATH"
if (!(Test-Path -LiteralPath (Join-Path $llvm 'clang-cl.exe'))) { throw 'Install LLVM with os/windows/InstallLLVM.ps1 first' }
$versionSettings = Get-Content -LiteralPath (Join-Path $root 'cmake/WindowsLLVMVersion.cmake') -Raw
$llvmVersion = [regex]::Match($versionSettings, 'OPENTTD_WINDOWS_LLVM_VERSION "([^"]+)"').Groups[1].Value
if ((& (Join-Path $llvm 'clang-cl.exe') --version | Out-String) -notmatch "clang version $([regex]::Escape($llvmVersion))\b") {
    throw "This default build requires LLVM $llvmVersion. Run os/windows/InstallLLVM.ps1 first."
}
if (!$env:VSCMD_VER -or $env:VSCMD_ARG_TGT_ARCH -ne $Architecture) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -property installationPath
    if (!$visualStudio) { throw 'Visual Studio C++ tools and Windows SDK are required' }
    $devcmd = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
    # Capture the developer environment without printing credentials or other
    # inherited environment values into the build log.
    $environmentScript = Join-Path ([IO.Path]::GetTempPath()) ("openttd-env-"+[guid]::NewGuid().ToString('N')+'.cmd')
    try {
        $hostArchitecture = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'x64' }
        [IO.File]::WriteAllText($environmentScript, "@call `"$devcmd`" -no_logo -host_arch=$hostArchitecture -arch=$Architecture >nul`r`n@if errorlevel 1 exit /b 1`r`n@set`r`n")
        $environment = & $env:ComSpec /d /c $environmentScript
        if ($LASTEXITCODE -ne 0) { throw 'Developer environment setup failed' }
    } finally { Remove-Item -LiteralPath $environmentScript -ErrorAction SilentlyContinue }
    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
    $env:PATH = "$llvm;$env:PATH"
}
$triple = @{x86='i686-pc-windows-msvc';x64='x86_64-pc-windows-msvc';arm64='aarch64-pc-windows-msvc'}[$Architecture]
$arguments = @('-S',$root,'-B',$BuildDirectory,'-G','Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl',
    '-DCMAKE_LINKER=lld-link', "-DCMAKE_C_COMPILER_TARGET=$triple", "-DCMAKE_CXX_COMPILER_TARGET=$triple",
    "-DVCPKG_TARGET_TRIPLET=$Architecture-windows-static", '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7','-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7',
    '-DCMAKE_EXE_LINKER_FLAGS=/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO',
    '-DOPTION_USE_ASSERTS=OFF','-DOPTION_NO_LTO=OFF')
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $env:VCPKG_INSTALLATION_ROOT }
if ($vcpkg) { $arguments += "-DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake" }
& cmake @arguments @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
$targets = @('openttd')
if ($Test) { $targets = @('all','openttd_test') } # Includes the regression fixtures.
& cmake --build $BuildDirectory --parallel --target @targets
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed' }
if ($Test) {
    & ctest --test-dir $BuildDirectory --output-on-failure --timeout 120
    if ($LASTEXITCODE -ne 0) { throw 'Windows tests failed' }
}
