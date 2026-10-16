# Empty retained table experiment

This experiment changes a copied measurement TU only. It does not change the fixed product source.

`retained_precomputed_revision == UINT64_MAX` implies empty retained table objects:

- Construction creates an empty retained array and the invalid revision.
- The only non-empty array assignment is the move in BeginPrecomputed; it immediately stores the valid current revision before returning.
- ClearRetainedPrecomputed clears the array before marking the revision invalid.
- Deserialize uses a fresh database, rejects non-empty retained services with an invalid revision, and starts retained replay only for a valid revision.
- The database is accessed by the simulation thread; no worker observes the intermediate move.

The experiment guards only `retained_precomputed = {}` with the valid-revision test. Services, revision, transfer, sections, estimates, optimized and integrated flags are still reset on every call. An early return would fail to preserve old-save flag normalization and is deliberately avoided.

Measure 2,048 actual StateTicks from the same common input, regular static-CRT binary, assert ON, LTO OFF, main thread CPU 0, worker mask unrestricted. Compare full canonical save-integrity fields with the unmodified candidate, and retain raw timing even when the hypothesis fails.

Hypothesis: repeated construction/destruction of the larger empty table array can pollute caches before ordinary vehicle work. This is unproven until measured.
