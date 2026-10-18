"""Keep the CPU memory profile and enlarge only the event collector's marker buffers."""
from pathlib import Path
import copy
import hashlib
import json
import shutil
import xml.etree.ElementTree as ET

WORK=Path(__file__).resolve().parent
source=WORK/'hyperpath';dest=WORK/'rejected-missing-begin-marker-2'
assert source.resolve().parent==WORK and dest.resolve().parent==WORK and not dest.exists()
assert not source.is_symlink() and not source.is_junction()
marks=(source/'window-markers.txt').read_text(encoding='utf-8-sig')
assert 'hyperpath-end' in marks and 'hyperpath-begin' not in marks
source.rename(dest)
for before,after in [('export-runner.log','export-rejected-marker-2-runner.log'),
    ('wpr-retry-recorder.log','wpr-memory-retry-recorder.log'),
    ('wpr-retry-recorder-complete.txt','wpr-memory-retry-recorder-complete.txt'),
    ('hyperpath-retry-complete.txt','hyperpath-memory-retry-complete.txt'),
    ('retry-runner.log','retry-memory-runner.log'),
    ('hyperpath-retry-runner.log','hyperpath-memory-retry-runner.log')]:
    a=WORK/before;b=WORK/after;assert a.is_file() and not b.exists();a.rename(b)
shutil.copy2(WORK/'record_retry.ps1',WORK/'record_memory_retry.ps1')
root=ET.parse(WORK/'cpu-memory.wprp').getroot();baseline=ET.tostring(root)
buffers=root.findall('./Profiles/EventCollector/Buffers');assert len(buffers)==1 and buffers[0].get('Value')=='20'
buffers[0].set('Value','256')
ET.ElementTree(root).write(WORK/'cpu-memory-large-events.wprp',encoding='utf-8',xml_declaration=True)
restored=copy.deepcopy(root);restored.find('./Profiles/EventCollector/Buffers').set('Value','20')
assert ET.tostring(restored)==baseline
profiles=root.findall('./Profiles/Profile');assert profiles and all(p.get('LoggingMode')=='Memory' for p in profiles)
proof=dict(original_event_buffers=20,new_event_buffers=256,buffer_size_kb=1024,logging_mode='Memory',
    all_other_profile_settings_identical=True,system_provider_sha256=hashlib.sha256(ET.tostring(root.find('./Profiles/SystemProvider'))).hexdigest(),
    reason='Both standard and one-second-preflight traces lost begin-side markers; retain CPU sampling settings and enlarge the event collector only',
    documentation='https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options')
(WORK/'buffer-change.json').write_text(json.dumps(proof,indent=2)+'\n')
print(json.dumps(proof,indent=2),flush=True)
