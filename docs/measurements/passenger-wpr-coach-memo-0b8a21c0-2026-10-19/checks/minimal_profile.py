"""Keep only the kernel CPU sampling/loader/process session; remove noisy user providers."""
from pathlib import Path
import hashlib
import json
import xml.etree.ElementTree as ET
WORK=Path(__file__).resolve().parent
root=ET.parse(WORK/'cpu-file-buffered.wprp').getroot();profiles=root.find('Profiles')
removed=[]
for child in list(profiles):
    if child.tag in ['EventCollector','EventProvider']:
        removed.append(dict(tag=child.tag,attributes=child.attrib));profiles.remove(child)
for collectors in profiles.findall('./Profile/Collectors'):
    for child in list(collectors):
        if child.tag=='EventCollectorId':collectors.remove(child)
path=WORK/'cpu-minimal.wprp';ET.ElementTree(root).write(path,encoding='utf-8',xml_declaration=True)
proof=dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),removed=removed,
    system_keywords=[n.get('Value') for n in profiles.findall('./SystemProvider/Keywords/Keyword')],
    system_stacks=[n.get('Value') for n in profiles.findall('./SystemProvider/Stacks/Stack')],
    sampling_interval_ms=1,system_buffer_size_kib=1024,system_buffers=1024,
    note='A short pilot must prove zero lost events and both WPR markers before real-map runs')
(WORK/'minimal-profile.json').write_text(json.dumps(proof,indent=2)+'\n')
(WORK/'sampling-jobs/pilot').mkdir(parents=True,exist_ok=True)
(WORK/'sampling-jobs/pilot/job.json').write_text(json.dumps(dict(kind='pilot',directory='pilot-minimal',mode='pilot'),indent=2)+'\n')
