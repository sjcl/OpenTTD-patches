# Station-local alternative candidates (A4)

This is the measured, **unadopted** candidate against `cf42a42660`.
The `.txt` inputs are never compiled into the game. Production keeps the
original occurrence-then-SectionRide traversal.

It preserves final Labels, alternatives, Plans, logical Work, all existing
counters at slice boundaries, and exact heap state/order. The station-local
index and resume/cache bookkeeping made the candidate phase slower in both
counter-free orders. See the additional-optimization report for the evidence.

Regenerate an isolated comparison with:

```powershell
python -B utils/passenger_table_build_prepare.py build-msvc/a4-reproduce --reference cf42a42660 --heap product --candidate-snapshot utils/passenger_table_build_experiments/station_alternatives
```

Use the generated benchmark.cmake with the locked build helper; add
`--timing-only` for separate counter-free normal/reverse trials.
