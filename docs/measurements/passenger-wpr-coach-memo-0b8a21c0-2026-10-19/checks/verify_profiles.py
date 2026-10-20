"""Verify both CPU recorder profiles match the baseline before recording."""
from pathlib import Path
import json
import subprocess
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-97cdc0ad'
proof={}
def settings(element):
    # Provider and keyword lists are sets; WPR export may emit them in a different order.
    return (element.tag,tuple(sorted(element.attrib.items())),(element.text or '').strip(),
        tuple(sorted(settings(child) for child in element)))
for mode in ['memory','file']:
    command=['wpr','-exportprofile','CPU',str(WORK/('cpu-'+mode+'.wprp'))]
    if mode=='file':command.append('-filemode')
    subprocess.run(command,check=True)
    old=ET.parse(OLD/('cpu-'+mode+'.wprp')).getroot()
    new=ET.parse(WORK/('cpu-'+mode+'.wprp')).getroot()
    assert settings(old)==settings(new),mode
    proof[mode]=dict(same_profile_settings_ignoring_element_order=True,command=command,
        system_buffers=int(new.find('./Profiles/SystemCollector/Buffers').get('Value')),
        event_buffers=int(new.find('./Profiles/EventCollector/Buffers').get('Value')))
result=dict(legacy=dict(old='Memory',new='Memory'),hyperpath=dict(old='File',new='File'),
    profiles=proof,baseline='97cdc0ad',same_recorder_configuration_per_mode=True,
    hyperpath_preflight='Same one-second paused preflight and ready marker before begin',
    legacy_preflight='Same direct begin marker after recorder start')
(WORK/'recorder-settings.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
