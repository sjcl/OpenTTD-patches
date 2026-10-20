# Shared Windows PGO profile

`windows.profdata` is a prepared LLVM IR instrumentation profile for the
Clang/LLVM version in `../WindowsLLVMVersion.cmake`. Windows Clang builds use
it by default in Release. The same indexed profile is used locally and in
the x86, x64 and ARM64 Actions builds. No training is performed in CI.
The default build uses Full LTO. The existing IR profile is reused without
retraining; instrumentation counts are applied before link-time optimization.

`windows.json` records the compiler, source revision, input hash, tick interval
and profile checksum. The training savegame, its assets and raw profiles are
not distributed. The workload is the previously specified populated real map,
not a newly generated empty map. The profile is trained on Windows x64; target
specific or subsequently changed functions may have no matching counts on
other targets. They do not receive measured branch counts. Performance on other
maps and architectures is not implied by the x64 real-map measurements.

Game simulation translation units are compiled directly from `src/` during
training. The optional training overlay replaces only the null video driver
to stop after 4,096 actual world ticks; `OPTION_PASSENGER_BENCHMARK` enables
the existing error-pause resume helper in the training console. These two
driver/console functions are excluded from the merged profile, as are startup
initializer records whose compiler-generated names can collide between game
and test files with the same basename. The Kernel32 lookup and its wrappers
changed by the shutdown deadlock fix are excluded too. Game algorithms,
world tick code, routing and boarding code are unchanged.

`PGO_PORTABLE_NAMES=ON` explicitly keys local functions by translation-unit
basename in both generation and use. The preparation tool also normalizes
legacy full-path names and re-indexes indirect-call targets. Thus the profile
is independent of the source checkout's absolute directory.
The compiler also maps the checkout prefix to `/openttd` with
`-fmacro-prefix-map`. This is required for MSVC ABI anonymous-namespace hashes;
shortening IR function names alone does not make those hashes portable.
The checked-in data's original nine namespace hashes were canonicalized using
the same xxh3 algorithm as LLVM, including references in indirect-call targets.
New training builds emit canonical hashes directly and need no conversion tool.
Keep source basenames stable when moving files. Compiler upgrades require
refreshing the profile and version/checksum settings together. Ordinary CI
runs keep using the checked-in profile; function changes can make individual
records stale, so refresh it periodically outside CI.

## Manual refresh (maintainer only)

Run from the repository root with Python 3.11+, the pinned LLVM and all assets
required by the training save. PACKAGE is an isolated directory containing
`openttd.cfg` (exit autosave disabled), basesets and NewGRFs.

```powershell
python os/windows/PreparePGO.py prepare build-windows-training
./os/windows/Build.ps1 -BuildDirectory build-windows-training -CMakeArguments @(
  '-DOPTION_PGO=GENERATE',
  '-DOPTION_PASSENGER_BENCHMARK=ON',
  "-DCMAKE_PROJECT_INCLUDE=$PWD/build-windows-training/pgo-driver.cmake"
)
python os/windows/PreparePGO.py train build-windows-training C:/PACKAGE C:/map.sav
```

This writes the profile and metadata here. Use a fresh training directory for
each refresh; the tool refuses to mix earlier raw profiles. It runs a copied
save and config and leaves the original files untouched. Review the metadata,
build the final default Release, run the tests, and commit both files together.

Use `OPTION_PGO=OFF` to opt out, or `PGO_PROFILE_FILE` for a custom profile.
The bundled profile requires its matching LLVM version; a mismatched compiler
fails configuration with an actionable error instead of silently using it.
