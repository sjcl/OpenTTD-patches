# Ordinary 8ff4834d captures

See the [report](../../passenger-wpr-generation-8ff4834d-2026-10-18.md).

`comparison.json` records generation counts, diagnostic wall time, waiting-event inclusive CPU time and endpoint overdue values. Endpoint canonical equality is deliberately not checked. `function-before-after.csv` compares raw inclusive/exclusive CPU ms/tick and includes machine-adjusted values for reference. `legacy/` and `hyperpath/` retain WPA raw CSV gzip files and exact-window metadata. `checks/` contains build, conversion, validation and reproduction helpers.
