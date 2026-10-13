"""Reuse the serial real-map harness, with the storage variants from this change."""
import pathlib

root = pathlib.Path(__file__).resolve().parents[3]
previous = root / "docs/measurements/passenger-timed-pruning-2026-10-13/Run-Comparison.py"
text = previous.read_text(encoding="utf-8")
start, end = text.index("sequence = "), text.index("try:\n")
text = text[:start] + '''sequence = [("legacy-1", "legacy", False, a.ticks), ("arena-1", "arena", False, a.ticks),
            ("arena-2", "arena", False, a.ticks), ("legacy-2", "legacy", False, a.ticks),
            ("disabled", "disabled", True, a.ticks),
            ("legacy-detail", "legacy-detail", False, a.detail_ticks), ("arena-detail", "arena-detail", False, a.detail_ticks)]
''' + text[end:]
exec(compile(text, str(previous), "exec"), globals())
