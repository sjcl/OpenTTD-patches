"""Build merged product sources with LLVM 23 ThinLTO, assert/PGO off and /Z7."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BUILD=ROOT/'build-wpr-clang-thin-2026-10-19/compile'
PREVIOUS=ROOT/'build-wpr-8f829b8c'
BASE=ROOT/'docs/measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19'
REVISION='6c11c67fd08115ce370fbfb182cdc952f37d75fd'
LLVM=Path('C:/Program Files/LLVM/bin')
MT=Path('C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe')
PREFIX='C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static'
VS='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools'

def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def dump(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def link_asset(source,target):
    if Path(source).name in {'input.sav','openttd.cfg','package.json'}:return shutil.copy2(source,target)
    try:os.link(source,target)
    except OSError:shutil.copy2(source,target)
    return str(target)

def freeze():
    identity=load(BASE/'input-identity.json');manifest=load(BASE/'input/package.json')
    assert sha(PREVIOUS/'input/input.sav')==identity['input_sha256']
    assert sha(PREVIOUS/'input/package.json')==sha(BASE/'input/package.json')
    assert sha(PREVIOUS/'input/openttd.cfg')==manifest['config_sha256']
    assert all(sha(PREVIOUS/'input'/e['path'])==e['sha256'] for e in manifest['assets'])
    shutil.copytree(PREVIOUS/'input',WORK/'input',copy_function=link_asset)
    assert all(sha(WORK/'input'/e['path'])==e['sha256'] for e in manifest['assets'])
    (WORK/'input/input.sav').chmod(stat.S_IREAD)
    identity.update(same_as_8f829b8c=True,source_package=str(PREVIOUS/'input'))
    dump(WORK/'input-identity.json',identity)
    print('Frozen input save/cfg/245 assets verified',flush=True)

def build():
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==REVISION
    assert not subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()
    env={k:v for k,v in os.environ.items() if not k.startswith(('OPENTTD_','LLVM_PROFILE_'))}
    env['PATH']=str(LLVM)+os.pathsep+env['PATH']
    versions={name:subprocess.check_output([str(LLVM/name),'--version'],text=True).strip() for name in ['clang-cl.exe','lld-link.exe']}
    assert all('23.1.3' in value for value in versions.values())
    flags='/O2 /Ob2 /DNDEBUG /Z7'
    link='/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /lldltocache:'+str(BUILD/'thinlto-cache')
    command=['cmake','-S',str(ROOT),'-B',str(BUILD),'-G','Visual Studio 17 2022','-A','x64','-T','ClangCL,host=x64',
        '-DCMAKE_GENERATOR_INSTANCE='+VS,'-DCMAKE_VS_GLOBALS=LLVMInstallDir=C:/Program Files/LLVM/;LLVMToolsVersion=23.1.3',
        '-DCMAKE_PREFIX_PATH='+PREFIX,'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DCMAKE_CXX_FLAGS_RELEASE='+flags,'-DCMAKE_C_FLAGS_RELEASE='+flags,'-DCMAKE_EXE_LINKER_FLAGS_RELEASE='+link,
        '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded','-DCMAKE_POLICY_DEFAULT_CMP0141=NEW',
        '-DEDITBIN_DIRECTORY='+VS+'/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64',
        '-DOPTION_USE_ASSERTS=OFF','-DOPTION_PGO=OFF','-DPGO_PROFILE_FILE=','-DPGO_PORTABLE_NAMES=OFF',
        '-DOPTION_NO_LTO=OFF','-DOPTION_DEDICATED=OFF']
    config=dict(configure_command=command,checkout_commit=REVISION,measurement_target=REVISION,compiler_versions=versions,
                native_test_command=[str(BUILD/'Release/openttd_test.exe'),'--reporter','compact'],asserts=False,pgo=False,lto='thin')
    dump(WORK/'build-command.json',config)
    with (WORK/'configure.log').open('wb') as output:subprocess.run(command,cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT,check=True)
    original=ROOT/'utils/passenger_estimated_prepare.py';source=original.read_text(encoding='utf-8')
    before="    anchor = '        const auto started = std::chrono::steady_clock::now();'"
    after="    anchor = '        bool ready = false;\\n        const auto started = std::chrono::steady_clock::now();'"
    assert source.count(before)==1
    compatibility=WORK/'estimated_prepare_compat.py';compatibility.write_text(source.replace(before,after),encoding='utf-8',newline='\n')
    dump(WORK/'prepare-compat.json',dict(original_sha256=sha(original),copy_sha256=sha(compatibility),before=before,after=after,product_sources_unchanged=True))
    subprocess.run([sys.executable,'-B',str(compatibility),str(ROOT),str(BUILD),REVISION,'--cpu'],cwd=ROOT,env=env,check=True)
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_prepare.py'),str(BUILD)],cwd=ROOT,env=env,check=True)
    projects={}
    ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
    for target in ['openttd_lib','openttd','openttd_test']:
        path=BUILD/(target+'.vcxproj');xml=ET.parse(path)
        releases=[g for g in xml.findall('m:ItemDefinitionGroup',ns) if 'Release|x64' in g.attrib.get('Condition','')]
        assert len(releases)==1
        text=ET.tostring(releases[0],encoding='unicode')
        assert '-flto=thin' in text and '-fprofile' not in text
        compile_node=releases[0].find('m:ClCompile',ns)
        options={node.tag.split('}')[-1]:node.text for node in compile_node}
        assert options['RuntimeLibrary']=='MultiThreaded' and options['Optimization']=='MaxSpeed'
        assert options['InlineFunctionExpansion']=='AnySuitable' and options['DebugInformationFormat']=='OldStyle'
        assert 'NDEBUG' in options['PreprocessorDefinitions'] and 'UNDEBUG' not in text
        projects[target]=dict(release_compile_options=options,release_xml=text,project_sha256=sha(path))
        if target!='openttd_lib':assert 'clang_rt.builtins' in text
    dump(WORK/'project-flags.json',projects)
    with (WORK/'build.log').open('wb') as output:
        subprocess.run(['cmake','--build',str(BUILD),'--config','Release','--target','openttd_test','openttd','--parallel','12',
                        '--','/nodeReuse:false','/p:MultiProcCL=true','/p:CL_MPCount=12'],cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT,check=True)
    manifests={}
    with (WORK/'manifest.log').open('wb') as output:
        for name in ['openttd.exe','openttd_test.exe']:
            exe=BUILD/'Release'/name;extracted=WORK/(name+'.manifest.xml')
            subprocess.run([str(MT),'-manifest',str(ROOT/'os/windows/openttd.manifest'),'-outputresource:'+str(exe)+';#1'],stdout=output,stderr=subprocess.STDOUT,check=True)
            subprocess.run([str(MT),'-inputresource:'+str(exe)+';#1','-out:'+str(extracted)],stdout=output,stderr=subprocess.STDOUT,check=True)
            def signature(node):return node.tag,sorted(node.attrib.items()),(node.text or '').strip(),[signature(c) for c in node]
            assert signature(ET.parse(extracted).getroot())==signature(ET.parse(ROOT/'os/windows/openttd.manifest').getroot())
            manifests[name]=dict(source_sha256=sha(ROOT/'os/windows/openttd.manifest'),extracted_sha256=sha(extracted),verified=True)
    dump(WORK/'manifest-verification.json',manifests)
    objects={}
    for target in ['openttd_lib','openttd','openttd_test']:
        paths=list((BUILD/(target+'.dir')/'Release').rglob('*.obj'));assert paths,target
        bitcode=[]
        for path in paths:
            with path.open('rb') as stream:magic=stream.read(4).hex()
            if magic=='4243c0de':bitcode.append(path.relative_to(BUILD).as_posix())
        assert bitcode,target
        objects[target]=dict(total_objects=len(paths),bitcode_objects=len(bitcode),examples=bitcode[:12])
    dump(WORK/'lto-objects.json',objects)
    with (WORK/'native-all.log').open('wb') as output:
        subprocess.run(config['native_test_command'],cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT,check=True)
    print((WORK/'native-all.log').read_text(errors='replace')[-500:],flush=True)
    identity=dict(product_commit=REVISION,src_tree=subprocess.check_output(['git','rev-parse','HEAD:src'],cwd=ROOT,text=True).strip(),
                  compiler_versions=versions,asserts=False,pgo=False,lto='thin',debug_information='/Z7',files={},instrumentation='Same copied-TU exact-tick/canonical/WPR hooks as 8f829b8c; no product changes')
    (WORK/'Release').mkdir()
    for name in ['openttd.exe','openttd.pdb','openttd_test.exe','openttd_test.pdb']:
        source=BUILD/'Release'/name;target=WORK/'Release'/name;shutil.copy2(source,target)
        assert sha(source)==sha(target);identity['files']['Release/'+name]=dict(bytes=target.stat().st_size,sha256=sha(target))
    for folder in ['lang','baseset','save-integrity','real-map-driver']:shutil.copytree(BUILD/folder,WORK/folder)
    dump(WORK/'build-identity.json',identity)
    (WORK/'build-ready.txt').write_text('Merged LLVM ThinLTO build, flags, objects, manifests and tests passed\n',encoding='utf-8',newline='\n')
    print('LLVM 23 ThinLTO executable/PDB prepared',flush=True)

if __name__=='__main__':{'freeze':freeze,'build':build}[sys.argv[1]]()
