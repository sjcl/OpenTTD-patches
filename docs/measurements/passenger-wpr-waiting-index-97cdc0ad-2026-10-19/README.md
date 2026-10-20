# Waiting-index WPR: 97cdc0ad vs 9ab0228d

See the [report](../../passenger-wpr-waiting-index-97cdc0ad-2026-10-19.md).

Both modes replay the same unchanged input for 2048 ticks. All 24 canonical fields are compared independently by mode. `focus.json` covers the six requested inclusive functions; `function-before-after.csv` also contains exclusive values and machine-adjusted estimates. `legacy/` and `hyperpath/` retain raw WPA CSV gzip files and exact-window metadata. Recording modes match the baseline by mode: Legacy Memory, Hyperpath File. `recording-backup.json` locates the ETL and matching executable/PDB; `input-backup.json` references the identical retained frozen input. `checks/` contains build/capture/analysis scripts and copied measurement TUs. The helpers document the original workspace paths; restore those inputs when reproducing.
