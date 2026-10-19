"""Reuse the prior capture procedure and incremental compiler cache; preserve baseline binaries."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-97cdc0ad'
OLD_NAME='passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
NAME='passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
REV='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684'
BASE_REV='97cdc0ad4c9493daea0db6079e90a03924d96b56'
for name in ['freeze_input.py','measure.py','verify_profiles.py','post.py','package.py','check_staged_evidence.py','finalize.py']:
    source=(OLD/name).read_text(encoding='utf-8')
    source=source.replace(OLD_NAME,NAME)
    source=source.replace('passenger-wpr-local-policy-9ab0228d-2026-10-18',OLD_NAME)
    source=source.replace("OLD=ROOT/'build-wpr-9ab0228d'", "OLD=ROOT/'build-wpr-97cdc0ad'")
    source=source.replace('same_as_9ab0228d','same_as_97cdc0ad').replace('against 9ab0228d','against 97cdc0ad')
    source=source.replace("identity['product_commit']=='"+BASE_REV+"'", "identity['product_commit']=='"+REV+"'")
    source=source.replace("v['product_commit']=='"+BASE_REV+"'", "v['product_commit']=='"+REV+"'")
    source=source.replace("baseline_commit='9ab0228db17178ef931de961305ebec544162f6a'", "baseline_commit='"+BASE_REV+"'")
    source=source.replace('passenger-wpr-recordings-97cdc0ad-2026-10-19','passenger-wpr-recordings-0b8a21c0-2026-10-19')
    source=source.replace("baseline='9ab0228d'", "baseline='97cdc0ad'")
    source=source.replace('Legacy and Hyperpath','Legacy and Hyperpath')
    if name=='package.py':
        source=source.replace("WORK/'compile'/name", "ROOT/'build-wpr-97cdc0ad/compile'/name")
        source=source.replace("for mode in ['legacy','hyperpath']:", "for mode in ['legacy','hyperpath']:",1)
    if name=='finalize.py':
        a=source.index("(OUT/'README.md').write_text(");b=source.index('\nreport=REPORT.read_bytes()',a)
        source=source[:a]+"(OUT/'README.md').write_text(f'# Coach-memo WPR: 0b8a21c0 vs 97cdc0ad\\n\\nSee the [report](../../{NAME}.md).\\n\\nBoth modes replay the same fixed input for 2048 ticks. The 24 canonical fields are compared independently by mode. Raw WPA CSV gzip files, function tables, sampled counts and machine-adjusted estimates are retained. Recorder settings match the baseline per mode: Legacy Memory, Hyperpath File. The separate validate run checks memo reuse and is excluded from performance measurements. Recording and input backup manifests locate the raw artifacts. Scripts in checks document the original workspace paths.\\n',encoding='utf-8',newline='\\n')"+source[b:]
    (WORK/name).write_text(source,encoding='utf-8')
source=(OLD/'prepare.py').read_text(encoding='utf-8')
source=source.replace("BUILD=WORK/'compile'", "BUILD=ROOT/'build-wpr-97cdc0ad/compile'")
source=source.replace("OLD=ROOT/'build-wpr-9ab0228d'", "OLD=ROOT/'build-wpr-97cdc0ad'")
source=source.replace('passenger-wpr-local-policy-9ab0228d-2026-10-18',OLD_NAME)
source=source.replace("REVISION='"+BASE_REV+"'", "REVISION='"+REV+"'")
source=source.replace('exactly 97cdc0ad','exactly 0b8a21c0').replace('97cdc0ad executable/PDB','0b8a21c0 executable/PDB').replace('helpers as 9ab0228d','helpers as 97cdc0ad')
(WORK/'prepare.py').write_text(source,encoding='utf-8')
(WORK/'record.ps1').write_text((OLD/'record.ps1').read_text(encoding='utf-8'),encoding='utf-8')
print('Capture helpers adapted; baseline executable/PDB remain unchanged')
