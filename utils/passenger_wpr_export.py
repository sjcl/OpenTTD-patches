"""Export marked CPU Usage (Sampled) tables with the matching PDB and Microsoft symbols."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('toolkit', type=Path)
    parser.add_argument('--mode', choices=['legacy', 'hyperpath'], nargs='+', default=['legacy', 'hyperpath'])
    parser.add_argument('--output-name', default='wpa')
    symbols = parser.add_mutually_exclusive_group()
    symbols.add_argument('--cached-symbols-only', action='store_true')
    symbols.add_argument('--local-symbols-only', action='store_true', help='Build missing game symcache from the local PDB; reuse existing Windows symcaches without network lookups')
    args = parser.parse_args()
    build, toolkit = args.build.resolve(), args.toolkit.resolve()
    env = dict(os.environ)
    for directory in ['symbols', 'symcache']:
        (build / directory).mkdir(exist_ok=True)
    env['_NT_SYMBOL_PATH'] = str(build / 'Release') + ';srv*' + str(build / 'symbols') + '*https://msdl.microsoft.com/download/symbols'
    if args.local_symbols_only:
        env['_NT_SYMBOL_PATH'] = str(build / 'Release')
    env['_NT_SYMCACHE_PATH'] = str(build / 'symcache')
    env['_NT_EXECUTABLE_IMAGE_PATH'] = str(build / 'Release')
    for mode in args.mode:
        directory = build / mode
        metadata = json.loads((directory / 'process.json').read_text(encoding='utf-8'))
        clock = metadata['actual-ticks.json']
        assert metadata['exit_code'] == 0 and metadata['simulation_timing_valid']
        assert clock['end_tick'] - clock['start_tick'] == len(clock['samples']) == 2048
        assert metadata['hyperpath'] == (mode == 'hyperpath') and metadata['hyperpath_filter'] is True
        assert (directory / 'wpr-saved.txt').is_file()
        trace = directory / 'cpu.etl'
        stats = directory / 'trace-stats.txt'
        subprocess.run([str(toolkit / 'xperf.exe'), '-i', str(trace), '-o', str(stats), '-a', 'tracestats', '-timespan'], check=True)
        text = stats.read_text(encoding='utf-8-sig')
        assert 'Total # Lost Buffers : 0' in text and 'Total # Lost Events  : 0' in text
        if Path(args.output_name).name != args.output_name:
            raise ValueError('--output-name must be a directory name')
        output = directory / args.output_name
        output.mkdir(exist_ok=False)
        command = [str(toolkit / 'wpaexporter.exe'), '-i', str(trace), '-symbols', '-marks', mode + '-begin', mode + '-end',
                   '-profile', str(build / 'cpu-sampled.wpaProfile'), '-outputfolder', str(output), '-outputformat', 'CSV']
        if args.cached_symbols_only:
            command.insert(4, '-symcacheonly')
        identity = dict(command=command, environment={k: env[k] for k in ['_NT_SYMBOL_PATH', '_NT_SYMCACHE_PATH', '_NT_EXECUTABLE_IMAGE_PATH']},
                        trace_bytes=trace.stat().st_size, trace_sha256=sha(trace), pdb_sha256=sha(build / 'Release/openttd.pdb'))
        (directory / 'export-command.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')
        print('WPA export:', mode, flush=True)
        with (directory / 'wpa-export.log').open('wb') as log:
            result = subprocess.run(command, env=env, cwd=build, stdout=log, stderr=subprocess.STDOUT)
        identity['exit_code'] = result.returncode
        identity['exports'] = {p.name: dict(bytes=p.stat().st_size, sha256=sha(p)) for p in output.glob('*.csv')}
        (directory / 'export-command.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')
        result.check_returncode()
        assert identity['exports'], 'WPAExporter produced no CSV tables'
        print('Exported:', ', '.join(identity['exports']), flush=True)


if __name__ == '__main__':
    main()
