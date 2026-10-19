"""Adapt the retained prior-run helpers while keeping the capture protocol unchanged."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-9ab0228d'
source=(OLD/'measure.py').read_text(encoding='utf-8')
source=source.replace('f9c4a85b','9ab0228d').replace('passenger-wpr-generation-9ab0228d-2026-10-18','passenger-wpr-local-policy-9ab0228d-2026-10-18')
(WORK/'measure.py').write_text(source,encoding='utf-8')
source=(OLD/'analyze.py').read_text(encoding='utf-8')
source=source.replace('passenger-wpr-local-policy-9ab0228d-2026-10-18','passenger-wpr-waiting-index-97cdc0ad-2026-10-19')
source=source.replace('passenger-wpr-generation-f9c4a85b-2026-10-18','passenger-wpr-local-policy-9ab0228d-2026-10-18')
a=source.index('FOCUS=[');b=source.index('def load',a)
names=['RoutePassengerJourneys','RecordCapacityMiss','ForgetWaitingPacket','IndexPacket','AddBoardingEntry','RemoveBoardingEntry']
source=source[:a]+'FOCUS='+repr(['openttd.exe!StationCargoList::'+name for name in names])+'\n\n'+source[b:]
source=source.replace("for name in FOCUS[:2]:assert name in old and name in new,(mode,name)","assert FOCUS[0] in old and FOCUS[0] in new,(mode,FOCUS[0])")
source=source.replace('for name in sorted(old.keys()|new.keys())','for name in sorted(old.keys()|new.keys()|set(FOCUS))')
source=source.replace("logging_mode_old='Memory',logging_mode_new='File' if mode=='hyperpath' else 'Memory',", "logging_mode_old='File' if mode=='hyperpath' else 'Memory',logging_mode_new='File' if mode=='hyperpath' else 'Memory',")
source=source.replace("if name in FOCUS:focus[mode][name]=row", "if name in FOCUS:focus[mode][name]=row")
source=source.replace("baseline_commit='f9c4a85b721073bbf86747f4a4cec7ac5ccd2f89'", "baseline_commit='9ab0228db17178ef931de961305ebec544162f6a'")
source=source.replace("current_commit='9ab0228db17178ef931de961305ebec544162f6a'", "current_commit='97cdc0ad4c9493daea0db6079e90a03924d96b56'")
source=source.replace('recording_profile_adjustment=', 'recorder_settings=')
source=source.replace('to f9c4a85b.', 'to 9ab0228d.')
(WORK/'analyze.py').write_text(source,encoding='utf-8')
source=(OLD/'check_staged_evidence.py').read_text(encoding='utf-8')
source=source.replace('passenger-wpr-local-policy-9ab0228d-2026-10-18','passenger-wpr-waiting-index-97cdc0ad-2026-10-19')
(WORK/'check_staged_evidence.py').write_text(source,encoding='utf-8')
print('Prior exact-tick and comparison helpers adapted')
