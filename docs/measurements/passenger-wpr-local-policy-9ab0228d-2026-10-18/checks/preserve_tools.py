"""Keep the currently needed WPT tools outside the previous results scheduled for deletion."""
from pathlib import Path
import json
import subprocess

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
source=(ROOT/'build-rebaseline-88cb61e3/tools').resolve()
dest=(WORK/'tools').resolve()
assert source.is_relative_to(ROOT) and dest.is_relative_to(WORK) and not dest.exists()
tracked=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT,text=True).split('\0')
assert not any(p.startswith('build-rebaseline-88cb61e3/tools/') for p in tracked)
source.rename(dest)
(WORK/'tools-preserved.json').write_text(json.dumps(dict(source=str(source),destination=str(dest),
    purpose='WPAExporter and xperf required for current 9ab0228d exports; preserved before old-results deletion'),indent=2)+'\n')
print('Current WPT tools preserved at '+str(dest),flush=True)
