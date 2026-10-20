# Coach memo WPR: 0b8a21c0 versus matched 97cdc0ad controls

See the [report](../../passenger-wpr-coach-memo-0b8a21c0-2026-10-19.md).

Memo validation passed: False. The memo line reports 13769660 validations and 13655 violations despite matching endpoint canonical states.

Four accepted traces use the same kernel CPU-only File profile after noisy full profiles lost events. `control-97cdc0ad/` contains new controls using the exact historical binary/PDB. Every run checks the original 24-field canonical state. `historical-focus.json` retains previous full-profile reference values; these are separate from the matched primary comparison. `focus.json`, raw WPA CSV gzip files, vector reallocation stacks, and the independent memo validate result are included. Raw ETL/binary backups are located by `recording-backup.json`; identical input archive by `input-backup.json`. Scripts preserve the original workspace paths and rejected attempts for audit.
