# LocalPolicy 9ab0228d vs f9c4a85b

See the [report](../../passenger-wpr-local-policy-9ab0228d-2026-10-18.md).

Both modes replay the same unchanged input for 2048 ticks. `canonical-comparison.json` compares all 24 endpoint canonical fields against the Git-tracked f9c4a85b results. `focus.json` and `function-before-after.csv` include raw and machine-adjusted inclusive/exclusive CPU ms/tick. `legacy/` and `hyperpath/` retain WPA raw CSV gzip files and exact-window metadata. `previous-results-cleanup.json` records deletion of previous untracked measurement/build folders and old external backups at the user request; old ETLs are no longer available. Only current artifacts remain outside Git. `checks/` retains the original preparation and verification helpers, including steps that ran before old directories were removed.
