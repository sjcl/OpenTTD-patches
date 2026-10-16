import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
build = root / 'build-integration'
assert (build / 'completed-table-load/comparison.json').is_file(), 'Finish isolated load profile before compiling'
subprocess.run([sys.executable, '-B', str(build / 'prepare_quality_probe.py')], check=True)
with (build / 'build-quality-probe.log').open('w', encoding='utf-8') as log:
    subprocess.run(['cmake','--build',str(build),'--config','Release','--target','openttd','--parallel','8'],stdout=log,stderr=subprocess.STDOUT,check=True)
(build / 'quality-probe').mkdir(exist_ok=True)
shutil.copy2(build / 'Release/openttd.exe',build / 'quality-probe/openttd.exe')
with (build / 'configure-product-probe-restored.log').open('w',encoding='utf-8') as log:
    subprocess.run(['cmake','-S',str(root),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64',
                    '-DBUILD_TESTING=ON','-DOPTION_USE_ASSERTS=ON','-DOPTION_NO_LTO=ON','-DVCPKG_TARGET_TRIPLET=x64-windows-static',
                    '-DCMAKE_PREFIX_PATH=C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static'],stdout=log,stderr=subprocess.STDOUT,check=True)
with (build / 'build-product-probe-restored.log').open('w',encoding='utf-8') as log:
    subprocess.run(['cmake','--build',str(build),'--config','Release','--target','openttd','--parallel','8'],stdout=log,stderr=subprocess.STDOUT,check=True)
print('Quality probe built; ordinary product restored',flush=True)
