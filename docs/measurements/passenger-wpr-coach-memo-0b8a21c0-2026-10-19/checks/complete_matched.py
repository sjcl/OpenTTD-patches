"""After exports and functional validation, package and render the final evidence."""
from pathlib import Path
import subprocess
import sys
import time
WORK=Path(__file__).resolve().parent
deadline=time.monotonic()+7200
while not ((WORK/'analysis-complete.txt').is_file() and (WORK/'validate-complete.txt').is_file()):
    if time.monotonic()>deadline:raise TimeoutError('Analysis/validate did not finish')
    time.sleep(2)
for name in ['package_matched.py','render_matched.py','finalize_matched.py']:
    subprocess.run([sys.executable,'-B',str(WORK/name)],check=True)
(WORK/'evidence-complete.txt').write_text('Final report and evidence verified\n')
