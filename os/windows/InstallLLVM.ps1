# Install the toolchain paired with cmake/pgo/windows.profdata (no CI training).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$settings = Get-Content -LiteralPath (Join-Path $root 'cmake/WindowsLLVMVersion.cmake') -Raw
$version = [regex]::Match($settings, 'OPENTTD_WINDOWS_LLVM_VERSION "([^"]+)"').Groups[1].Value
$checksum = [regex]::Match($settings, 'OPENTTD_WINDOWS_LLVM_MSI_SHA256 "([^"]+)"').Groups[1].Value
$platform = 'win64'
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') {
    $platform = 'woa64'
    $checksum = [regex]::Match($settings, 'OPENTTD_WINDOWS_LLVM_ARM64_MSI_SHA256 "([^"]+)"').Groups[1].Value
}
$bin = Join-Path $env:ProgramFiles 'LLVM/bin'
$compiler = Join-Path $bin 'clang-cl.exe'
if (!(Test-Path -LiteralPath $compiler) -or (& $compiler --version | Out-String) -notmatch "clang version $([regex]::Escape($version))\b") {
    $installer = Join-Path ([IO.Path]::GetTempPath()) "OpenTTD-LLVM-$version-$platform.msi"
    Invoke-WebRequest -Uri "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/LLVM-$version-$platform.msi" -OutFile $installer
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $checksum) { throw 'LLVM installer checksum mismatch' }
    $process = Start-Process msiexec.exe -ArgumentList @('/i', ('"'+$installer+'"'), '/qn', '/norestart') -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010)) { throw "LLVM installation failed: $($process.ExitCode)" }
}
if ((& $compiler --version | Out-String) -notmatch "clang version $([regex]::Escape($version))\b") { throw 'Wrong LLVM version after installation' }
& $compiler --version
if ($env:GITHUB_PATH) { $bin | Out-File -LiteralPath $env:GITHUB_PATH -Append -Encoding utf8 }
$env:PATH = "$bin;$env:PATH"
