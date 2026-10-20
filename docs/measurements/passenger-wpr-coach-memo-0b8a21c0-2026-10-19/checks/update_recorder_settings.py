"""Disclose the necessary Legacy Memory-to-File retake; Hyperpath stays unchanged."""
from pathlib import Path
import json
WORK=Path(__file__).resolve().parent
p=WORK/'recorder-settings.json';data=json.loads(p.read_text())
data['legacy'].update(new='File',reason='The completed Memory trace lacked its legacy-begin marker; only File retake is accepted')
data['same_recorder_configuration_per_mode']=False
data['same_recorder_configuration_hyperpath']=False
data['legacy_recording_difference']=dict(old_system_buffers=data['profiles']['memory']['system_buffers'],new_system_buffers=data['profiles']['file']['system_buffers'],
    old_event_buffers=data['profiles']['memory']['event_buffers'],new_event_buffers=data['profiles']['file']['event_buffers'],
    recording_mode_difference=True,cpu_profile_provider_settings_verified=True)
data['legacy_preflight']='File retake: same one-second paused preflight and ready marker as Hyperpath'
data['buffer_adjustment']=json.loads((WORK/'buffer-adjustment.json').read_text())
data['current_system_buffers']=1024;data['current_event_buffers']=128
data['same_cpu_sampling_provider_keywords_and_stacks']=True
data['hyperpath_recording_difference']=dict(logging_mode_unchanged='File',old_system_buffers=20,new_system_buffers=1024,
    old_event_buffers=20,new_event_buffers=128,reason='Default File trace dropped 2024287 events')
data['legacy_recording_difference']['new_system_buffers']=1024
data['legacy_recording_difference']['new_event_buffers']=128
p.write_text(json.dumps(data,indent=2)+'\n')
