"""Read existing rolling component timings; never replace raw tick statistics."""
import hashlib, json, pathlib, re
root=pathlib.Path(__file__).resolve().parent
result={}
for mode in ['integrated-a','legacy-a','legacy-b','integrated-b']:
    p=root/('runtime-same-image-'+mode)/'on-1-console.txt'
    if not p.exists():continue
    rows={};counts=None
    for line in p.read_text(encoding='utf-8',errors='replace').splitlines():
        line=re.sub(r'^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\] ','',line)
        match=re.search(r'Based on num\. data points: (\d+) (\d+) (\d+)',line)
        if match:counts=list(map(int,match.groups()))
        match=re.match(r'(.+?) times: ([\d.]+)ms\s+([\d.]+)ms\s+([\d.]+)ms',line)
        if match:rows[match[1].strip()]=list(map(float,match.groups()[1:]))
    if rows:result[mode]=dict(console_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),rolling_points=counts,mean_ms=rows)
output=dict(runs=result,note='Existing ConPrintFramerate rolling averages after the timed ticks. Component histories may contain fewer valid points; categories overlap with journey diagnostics. These are auxiliary, not the full 2048-tick statistics.')
(root/'runtime-same-image-components.json').write_text(json.dumps(output,indent=2)+'\n')
print(json.dumps(output,indent=2))
