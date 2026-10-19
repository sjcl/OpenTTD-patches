"""Increase only File collector buffers; keep every provider and stack setting unchanged."""
from pathlib import Path
import copy
import json
import xml.etree.ElementTree as ET
WORK=Path(__file__).resolve().parent
root=ET.parse(WORK/'cpu-file.wprp').getroot();before=copy.deepcopy(root)
changes={}
for path,count in [('./Profiles/SystemCollector/Buffers',1024),('./Profiles/EventCollector/Buffers',128)]:
    node=root.find(path);changes[path]=dict(old=int(node.get('Value')),new=count);node.set('Value',str(count))
restored=copy.deepcopy(root)
for path,change in changes.items():restored.find(path).set('Value',str(change['old']))
assert ET.tostring(restored)==ET.tostring(before)
ET.ElementTree(root).write(WORK/'cpu-file-buffered.wprp',encoding='utf-8',xml_declaration=True)
(WORK/'buffer-adjustment.json').write_text(json.dumps(dict(changes=changes,all_other_profile_settings_identical=True,
    reason='Default Hyperpath File trace reported 2024287 dropped events. Use the previously proven 1024/128 buffer counts.'),indent=2)+'\n')
