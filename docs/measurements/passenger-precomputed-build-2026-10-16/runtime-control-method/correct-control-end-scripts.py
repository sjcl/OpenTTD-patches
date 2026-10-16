"""Fix the auxiliary command in future scripts from an already loaded runner."""
import datetime, hashlib, json, pathlib, tempfile, time
root=pathlib.Path(__file__).resolve().parent
record=root/'same-image-control/end-script-corrections.json'
lock=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
deadline=time.monotonic()+1800
pending={'legacy-b','integrated-b'}
while pending:
    if not lock.exists() or json.loads(lock.read_text())['pid']!=122388:
        raise RuntimeError('Original control lease ended before all scripts were created')
    for mode in sorted(pending):
        directory=root/('runtime-same-image-'+mode)
        path=directory/'scripts/passenger_benchmark_end.scr'
        if not path.exists():continue
        assert not (directory/'progress.json').exists(),'Never change the script during timed ticks'
        original=path.read_bytes();lines=original.splitlines(keepends=True)
        assert sum(line.strip()==b'framerate' for line in lines)==1
        corrected=b''.join(line.replace(b'framerate',b'fps') if line.strip()==b'framerate' else line for line in lines)
        path.write_bytes(corrected)
        records=json.loads(record.read_text())
        records[mode]=dict(corrected_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
            original_sha256=hashlib.sha256(original).hexdigest(),corrected_sha256=hashlib.sha256(corrected).hexdigest(),
            timed_ticks_started=False,reason='Correct post-window auxiliary console command name; no measured tick or product change.')
        record.write_text(json.dumps(records,indent=2)+'\n')
        pending.remove(mode)
        print(mode+' end script corrected before timed ticks',flush=True)
    if time.monotonic()>deadline:raise TimeoutError('Future control scripts were not created')
    if pending:time.sleep(2)
