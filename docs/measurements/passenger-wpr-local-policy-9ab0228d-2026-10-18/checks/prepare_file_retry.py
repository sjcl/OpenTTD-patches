"""Retake with unbounded file recording after three memory-mode traces lost begin markers."""
from pathlib import Path
import hashlib
import json
import shutil
import xml.etree.ElementTree as ET

WORK=Path(__file__).resolve().parent
source=WORK/'hyperpath';dest=WORK/'rejected-missing-begin-marker-3'
assert source.resolve().parent==WORK and dest.resolve().parent==WORK and not dest.exists()
assert not source.is_symlink() and not source.is_junction()
marks=(source/'window-markers.txt').read_text(encoding='utf-8-sig')
assert 'hyperpath-end' in marks and 'hyperpath-begin' not in marks
source.rename(dest)
for before,after in [('export-runner.log','export-rejected-marker-3-runner.log'),
    ('wpr-retry-recorder.log','wpr-buffered-memory-retry-recorder.log'),
    ('wpr-retry-recorder-complete.txt','wpr-buffered-memory-retry-recorder-complete.txt'),
    ('hyperpath-retry-complete.txt','hyperpath-buffered-memory-retry-complete.txt'),
    ('retry-runner.log','retry-buffered-memory-runner.log'),
    ('hyperpath-retry-runner.log','hyperpath-buffered-memory-retry-runner.log')]:
    a=WORK/before;b=WORK/after;assert a.is_file() and not b.exists();a.rename(b)
shutil.copy2(WORK/'record_retry.ps1',WORK/'record_buffered_memory_retry.ps1')
memory=ET.parse(WORK/'cpu-memory.wprp').getroot();file=ET.parse(WORK/'cpu-file.wprp').getroot()
for path in ['./Profiles/SystemProvider','./Profiles/EventProvider','./Profiles/Profile/Collectors']:
    assert [ET.tostring(e) for e in memory.findall(path)]==[ET.tostring(e) for e in file.findall(path)],path
settings=dict(profile='CPU',baseline_logging_mode='Memory',current_logging_mode='File',
    same_system_provider_keywords_and_stacks=True,same_event_providers=True,same_collector_provider_references=True,
    baseline_system_buffer_count=int(memory.find('./Profiles/SystemCollector/Buffers').get('Value')),
    current_system_buffer_count=int(file.find('./Profiles/SystemCollector/Buffers').get('Value')),
    baseline_event_buffer_count=int(memory.find('./Profiles/EventCollector/Buffers').get('Value')),
    current_event_buffer_count=int(file.find('./Profiles/EventCollector/Buffers').get('Value')),
    reason='Three memory traces missed begin markers; only a complete file-mode trace will be used for Hyperpath timing',
    timing_comparison_has_recording_mode_difference=True,
    documentation='https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options')
(WORK/'recorder-settings.json').write_text(json.dumps(settings,indent=2)+'\n')
(WORK/'wpr-filemode-temp').mkdir(exist_ok=True)
print(json.dumps(settings,indent=2),flush=True)
