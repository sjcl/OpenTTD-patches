# Repository flat-map Section candidate (A5)

Unadopted alternative against `881cec768f`, using the existing robin_hood
unordered_flat_map for packed prefixes and minimum-service owners.
The text snapshots are measurement inputs, never part of the game.

Counter-free small-input tests compared full Labels, Plans, Section arrays,
heap states and logical Work. The real-map screening probe fully built and
compared all five Section arrays and a short published row prefix; it did
not compare a complete real-map table or all real-map Plans. Both orders
improved Section time by about 0.11 seconds, similar to repeat variation.
The report does not treat this probe as a full-table adoption measurement.

Regenerate with `passenger_table_build_prepare.py --reference 881cec768f
--heap product --candidate-snapshot utils/passenger_table_build_experiments/section_flat_robin
--timing-only`; run the locked helper and paired real-map prefix probe
(`--destinations 1 --skip-plan`). The limit is checked between Work slices,
so the actual published prefix can contain more than one row.
