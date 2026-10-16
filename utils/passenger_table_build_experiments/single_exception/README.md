# Single-exception range relaxation (A1)

This is the measured, **unadopted** candidate against `4734ea1d1e`. These `.txt`
files are measurement inputs and are never compiled into the game. The normal
integrated engine retains the scalar range scan.

The candidate passed the complete Label/alternative/binding/Plan comparison,
logical Work and heap-order checks. Assertion-enabled builds became slower;
assertion-disabled timing varied substantially by order. See
`docs/passenger-precomputed-additional-2026-10-16.md` for the measurements and
decision. Full descriptor-boundary invariant checks remain in this snapshot.

To regenerate its isolated, counted comparison:

```powershell
python -B utils/passenger_table_build_prepare.py build-msvc/a1-reproduce --reference 4734ea1d1e --heap product --candidate-snapshot utils/passenger_table_build_experiments/single_exception
```

Use the generated `table-build/benchmark.cmake` with the existing locked build
helper, and run the opt-in range, slice, synthetic and real-map tests. Add
`--timing-only` for a separate counter-free build. Keep the assertion setting
explicit and compare both orders with the same completed warm reference resident.
The four regenerated reference/candidate h/cpp files were byte-identical to the
original counted trial after moving the candidate here.
