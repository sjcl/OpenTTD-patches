"""Archive validated diagnostics without committing saves, assets or executables.

The summary records save verification performed at collection. Reuse it with
Summarize-Profile.py --existing-summary; this archive does not rehash full saves.
"""
import argparse
import hashlib
import json
import pathlib


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_bytes((json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))


def archive(source, output):
    data = json.loads(source.read_text(encoding='utf-8'))
    if not data['runs'] or any(not run['valid'] for run in data['runs']):
        raise ValueError('Archive only a nonempty validated dataset; retain failed attempts separately')
    if output.exists():
        raise ValueError('Archive destination already exists')
    output.mkdir(parents=True)
    # Canonical LF survives Git checkout on Windows. Keep the raw source hash
    # separately; source.write_text() may have emitted CRLF on that platform.
    (output / 'summary.json').write_bytes(source.read_text(encoding='utf-8').encode('utf-8'))
    entries = []
    for run in data['runs']:
        candidates = [manifest for manifest in data['manifests']
                      if (manifest['label'], manifest['kind'], manifest['phase'], manifest['revision']) ==
                         (run['label'], run['kind'], run['phase'], run['revision']) and
                      run['id'] in {record['id'] for record in manifest['runs']}]
        if len(candidates) != 1:
            raise ValueError('Run lacks unambiguous manifest provenance')
        directory = pathlib.Path(candidates[0]['path']).parent / run['directory']
        target = output / (run['label'] + '-' + run['kind'] + '-' + run['id'])
        target.mkdir()
        console = directory / (run['id'] + '-console.txt')
        original = console.read_bytes()
        if digest(original) != run['evidence']['console_sha256']:
            raise ValueError('Console evidence changed after validation')
        # LF text is portable through Git; retain both raw and archived hashes.
        text = original.decode('utf-8-sig', errors='replace').replace('\r\n', '\n')
        (target / 'console.txt').write_bytes(text.encode('utf-8'))
        write_json(target / 'process.json', run['process'])
        write_json(target / 'diagnostics.json', run['diagnostics'])
        write_json(target / 'stderr-evidence.json', {key: run['evidence'][key] for key in
                   ['stderr_sha256', 'stderr_bytes', 'unique_stderr_lines']})
        if 'detail' in run:
            write_json(target / 'detail-scopes.json', run['detail'])
            write_json(target / 'detail-windows.json', run['detail_windows'])
        entries.append({'id': run['id'], 'label': run['label'], 'kind': run['kind'],
                        'source_directory': str(directory), 'archive_directory': target.name,
                        'raw_console_sha256': digest(original),
                        'archived_console_sha256': digest(text.encode('utf-8')),
                        'final_save_sha256_verified_at_collection': run['final_save_sha256']})
    write_json(output / 'collection-index.json', {'schema_version': 1, 'runs': entries,
        'source_summary_sha256': digest(source.read_bytes()),
        'archived_summary_sha256': digest((output / 'summary.json').read_bytes()),
        'note': 'Save/asset/exe files remain outside Git. Text/JSON is LF normalized; raw source and archived summary/console hashes are distinct. --existing-summary reuses recorded validation.'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('summary', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    archive(args.summary, args.output)
