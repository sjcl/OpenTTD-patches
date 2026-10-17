"""Summarize the two complete fixed-image orders without removing any run."""
import hashlib,json,pathlib,statistics
root=pathlib.Path(__file__).resolve().parent
source=root/'split-range-validation'
names=['before-scalar-real','before-simd-real','split-scalar-real','split-simd-real']
cases={}
for order in ['forward','reverse']:
 p=source/order/'result.json';data=json.loads(p.read_text())
 assert data['exit_code']==0 and data.get('destination_limit') is None
 rows={r['name']:r for r in data['rows']}
 for name in names:
  r=rows[name];assert r['operations']==5665642229 and r['array_bytes']==4635480774
  assert data['details'][name]['table_window']==[0,4917]
 cases[order]=dict(binary_sha256=data['binary_sha256'],result_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),
  variants={name:dict(graph_s=rows[name]['graph_ns']/1e9,rows_s=rows[name]['rows_ns']/1e9,
   search_s=rows[name]['search_ns']/1e9,total_s=(rows[name]['graph_ns']+rows[name]['rows_ns'])/1e9) for name in names})
assert cases['forward']['binary_sha256']==cases['reverse']['binary_sha256']
means={name:{field:statistics.mean(c['variants'][name][field] for c in cases.values())
 for field in ['graph_s','rows_s','search_s','total_s']} for name in names}
changes={name:{field:(v/means['before-scalar-real'][field]-1)*100 for field,v in values.items()}
 for name,values in means.items()}
out=dict(cases=cases,means=means,percent_vs_before_scalar=changes,
 note='Both complete orders are included. Reference-warm rows are ground truth and excluded from variant means by design. No timing normalization or outcome-based sample exclusion. Other applications remain active; small timing differences are not universal speedup guarantees. Prefix probes and rejected prototypes are separate evidence.')
(source/'summary.json').write_text(json.dumps(out,indent=2)+'\n')
for name in names:print(name,means[name],changes[name])
