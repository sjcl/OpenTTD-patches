# Ordinary f9c4a85b captures from gc3833d807.sav

See the [report](../../passenger-wpr-generation-f9c4a85b-2026-10-18.md).

Both modes directly use the same unconverted input save. `metrics.json` records generation counts, diagnostic wall time, waiting-event inclusive/exclusive CPU time, endpoint overdue values and separate zero-tick initial events. The previous 8ff4834d values use different inputs and are reference only. Endpoint canonical equality is deliberately not checked. `legacy/` and `hyperpath/` retain WPA raw CSV gzip files and exact-window metadata; the top-level function CSVs contain inclusive/exclusive CPU ms/tick. `checks/` retains build and reproduction helpers in their original working-directory layout.
