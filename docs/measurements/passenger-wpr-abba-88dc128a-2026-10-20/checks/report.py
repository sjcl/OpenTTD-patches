"""Write the measured report, then seal hashes after documentation is finalized."""
from pathlib import Path
import hashlib,json
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-abba-88dc128a-2026-10-20'
OUT=ROOT/'docs/measurements'/NAME
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
f=load(OUT/'focus.json');c=load(OUT/'comparison.json');canonical=load(OUT/'canonical-comparison.json');v=load(OUT/'validation.json')
def fmt(n,digits=6):return '出力なし' if n is None else f'{n:,.{digits}f}'
def table(mode):
 lines=['| 項目 | A 平均 [最小, 最大] | B 平均 [最小, 最大] | B−A | 変化 |','| --- | ---: | ---: | ---: | ---: |']
 for metric,title,digits in [('generation_packets','generation packets / 2048 tick',0),('generation_us','generation microseconds / 2048 tick',1),('generation_ms_per_tick','generation timer ms/tick',6),('generation_us_per_packet','generation timer us/packet',6),('sweeps_full','sweeps full',0),('sweeps_row_limited','sweeps row_limited',0),('sweeps_skipped_packets','sweeps skipped_packets',0),('RoutePassengerJourneys','RoutePassengerJourneys inclusive ms/tick',6),('RunPassengerWaitingEvents','RunPassengerWaitingEvents inclusive ms/tick',6),('StateGameLoop','StateGameLoop inclusive ms/tick',6),('simulation_wall_ms_per_tick','tick wall ms/tick',6)]:
  r=f['comparison'][mode][metric]
  cols=[fmt(r[x]['mean'],digits)+' ['+fmt(r[x]['minimum'],digits)+', '+fmt(r[x]['maximum'],digits)+']' if r[x]['mean'] is not None else '出力なし' for x in ['A','B']]
  pct='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
  lines.append(f"| {title} | {cols[0]} | {cols[1]} | {fmt(r['delta'],digits)} | {pct} |")
 return '\n'.join(lines)
def change(mode,metric):
 r=f['comparison'][mode][metric]
 return f"{r['A']['mean']:,.0f} → {r['B']['mean']:,.0f}（{r['raw_percent']:+.2f}%）" if metric=='generation_packets' else f"{r['A']['mean']:,.6f} → {r['B']['mean']:,.6f}（{r['raw_percent']:+.2f}%）"
report=f'''# cdebb1a0 / 88dc128a Clang ThinLTO ABBA・validate計測

A=`{c['A_commit']}`、B=`{c['B_commit']}`。2026-10-20 JST、凍結済み`gc3833d807.sav`からLegacy／Hyperpathを各A→B→B→A、計8本、各2,048 tick採取した。Aの全treeは9439af0eと同一（`b8d78638dafb629eb0b8c38ab4ff3756243f3a5c`）。

**同じ版・同じモードの2本どうしでcanonical 24項目は4組すべて完全一致。A/B間の差は今回の仕様変更で予期されたもので、採用条件にしていない。両版の先行Hyperpath validateもmemo・lookupのviolations=0、lookup_duplicates=0だった。**

- Hyperpath `RoutePassengerJourneys` inclusive: {change('hyperpath','RoutePassengerJourneys')} ms/tick。
- Legacy `RoutePassengerJourneys` inclusive: {change('legacy','RoutePassengerJourneys')} ms/tick。
- Hyperpath generation packets: {change('hyperpath','generation_packets')} / 2,048 tick。
- Legacy generation packets: {change('legacy','generation_packets')} / 2,048 tick。

時間はメインスレッドの`StateGameLoop`を含むCPUサンプリングのinclusive。原因別のmicrosecondsだけは製品のpacket処理タイマーの合計で、WPRのinclusiveやスキップを含む走査全体の時間とは範囲が異なる。A/Bで処理対象が変わるため、同一処理量の命令単位の高速化とは呼ばない。

Hyperpathではgeneration処理の件数・原因別タイマーは減ったが、`RoutePassengerJourneys`のinclusiveはA 0.702091～0.742149、B 0.794897～0.976647 ms/tickと両方のB実行で高かった。今回の採取では、この関数の時間短縮は確認できない。

ただし、Hyperpathでは`RunTileLoop` inclusiveも8.374311→10.376622、`CallVehicleTicks`も9.198551→11.510443 ms/tickと広く増加している。LegacyのRouteはA/Bの範囲が重なる。全体の生CPU値も表に残すが、この各2本だけで差をrow-limited sweep単独の回帰・高速化やCPU周波数の変化と断定しない。機械状態とコード／処理経路の寄与はこの記録だけでは特定していない。

## Hyperpath

{table('hyperpath')}

## Legacy

{table('legacy')}

Aには`Passenger generation sweeps`の出力がない。A欄の「出力なし」は0を意味しない。Bの`full`・`row_limited`は全走査／公開行を限定した走査、`skipped_packets`は限定走査で処理を省略したpacketの観測件数。

## 各本の生値

| 順序 | 版 | mode | generation packets | generation us | sweeps full | row_limited | skipped_packets | Route inclusive ms/tick | Route samples | StateGameLoop ms/tick |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
'''
for key,r in f['runs'].items():
 x=r['values'];report+=f"| {key} | {r['label']} | {r['mode']} | {x['generation_packets']:,} | {x['generation_us']:,} | {fmt(x['sweeps_full'],0)} | {fmt(x['sweeps_row_limited'],0)} | {fmt(x['sweeps_skipped_packets'],0)} | {x['RoutePassengerJourneys']:.6f} | {r['focus']['RoutePassengerJourneys']['samples']:,} | {x['StateGameLoop']:.6f} |\n"
report+='''
各ラウンドはLegacy→Hyperpath、新しいプロセスで同じ入力をロードした。平均と実測範囲は各版2本の値で、信頼区間ではない。ABBAは直線的な時間ドリフトを釣り合わせるが、全ノイズを除去するものではない。untouched補正は行っていない。inclusiveは同じスタック内の同じ関数を1回だけ数え、親子を重複加算しない。全関数のinclusive/exclusiveとsamplesはCSVに保存した。

## 正しさとvalidate

'''
for label in ['A','B']:
 valid=v['real_map_validate'][label];m=valid['diagnostics']['memo'];l=valid['diagnostics']['lookup'];check=canonical['validate_vs_index'][label]
 report+=f"- {label}: memo validated={m['validated']:,}, conservative={m['conservative']:,}, violations={m['violations']}, lookup_duplicates={m['lookup_duplicates']}。lookup validated_groups={l['validated_groups']:,}, violations={l['violations']}。validateと同じ版の通常indexのcanonical一致={check['validate_vs_normal_index_equal']}。\n"
report+='\n'
for mode in ['legacy','hyperpath']:
 cross=canonical['cross_version'][mode]
 report+=f"- A/B間の{mode} canonical差: {len(cross['differences'])}/24項目。差分を保存し、同じ版の2本で決定性を判定した。\n"
report+='''
validateは両版ともWPR前に同じ入力から`--hyperpath-filter-mode validate --validation-only`で各1本。各2,048 tick・準備完了・準備中tick不変を確認し、性能平均には含めていない。`simulation_timing_valid=false`はvalidation-onlyの意図した値。Aはmerge後の7b3f2c4bを含む版として検証した。この1マップの成功は全入力の決定性を証明しない。

## 条件・採用検査・保管

- 入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットを既存manifestに照合。StateTicks **3,197,276,448 → 3,197,278,496**。
- ClangCL/LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、静的CRT、`/O2 /Ob2 /MT /Z7`、compiler-rt builtins、mt.exeでmanifestを再埋め込み。両版の正確なcheckoutからビルドし、exe/PDBを各々保存してから元ブランチへ戻した。計測フックはコピーTUだけ。
- merge後CMakeの既定は`-flto=full`だった。最初のconfigureの確認で検出し、採取前に`OPTION_NO_LTO=ON`で自動Full LTOを抑止、C/C++のRelease flagsへ`-flto=thin`を明示した。実clang-clコマンド・bitcode・builtins・対応PDB・抽出manifestを検証。製品CMakeの変更はしていない。
- 通常条件は`--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。追加warm-up tick・cold・再構築指定なし。
'''
for label,t in v['native_tests'].items():report+=f"- {label}: 全{t['tests']} nativeテスト、{t['assertions']:,} assertions成功。\n"
report+=f'''- 同じkernel CPU専用File profile、1ms、1,024×1,024KiB。短いpilot合格後、8本すべてevents/buffers欠落0・両マーカーあり。kernel process lifetimeの区間監査で競合ビルド／テスト／ゲームなし。関連チャットへ採取枠を調整した。
- WPA製品関数の未解決0、CSVのgzip展開ハッシュ、exclusive時間・samples保存則、支払い整合性を確認。InvalidEvent警告は測定マーカー外。
- ETL8本・終了セーブ10本・両版exe/PDBを作業ツリー外へ独立コピー。固定入力ZIPは既存の独立保管をハッシュ照合して再利用。[保管manifest](measurements/{NAME}/recording-backup.json)、[入力ZIP](measurements/{NAME}/input-backup.json)。

[各本CSV](measurements/{NAME}/focus-runs.csv)、[全関数A/B差CSV](measurements/{NAME}/function-before-after.csv)、[canonical差分](measurements/{NAME}/canonical-comparison.json)、[採用検査・validate](measurements/{NAME}/validation.json)、[比較JSON](measurements/{NAME}/comparison.json)、[再現レシピ](measurements/{NAME}/checks/)。各ラウンドに関数別inclusive/exclusive CSV、各モード配下に生WPA CSVのgzipを保存。checksは当日の絶対パスを含む再現資料で、その場所から実行する汎用ツールではない。
'''
p=ROOT/'docs'/(NAME+'.md');p.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=p.relative_to(ROOT).as_posix(),sha256=sha(p)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
print(p,flush=True)
