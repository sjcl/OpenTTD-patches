# Previous-release save fixture

`jgrpp-ya-0.73.3+2.sav` was written on 2026-10-11 by the Windows x64
`jgrpp-ya-0.73.3+2` release build, commit
`9a3484bc78750e6ae1df5d58bba36761fe23d948`, during its station-list regression
smoke test. It is the retained `smoke/release-roundtrip.sav` from that build,
not a save synthesized by the current loader.

SHA-256: `102196929c429c595987afaa58a1b7cfa880985a974c5952544991f64fc8b6fe`.

`passenger_previous_release_load` loads this fixture through the production
loader, verifies that the new OD and journey settings default to disabled,
then enables each feature, saves in the current format and reloads it. Existing
pre-release passenger formats are deliberately outside the compatibility contract.
