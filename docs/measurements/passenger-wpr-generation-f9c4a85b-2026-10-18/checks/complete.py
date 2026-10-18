"""After both captures and WPA exports, validate backups and render the report."""
from pathlib import Path
import subprocess
import sys

WORK=Path(__file__).resolve().parent
for name in ['package.py','render_report.py','finalize_evidence.py']:
    subprocess.run([sys.executable,'-B',str(WORK/name)],check=True)
