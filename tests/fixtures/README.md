# Fixtures

`synthyy/` holds fixture project JSON and golden WAVs copied from synthyy (read-only) by
`scripts/sync_synthyy_fixtures.sh`, plus `baseline.txt` (the scores synthyy's own Rust port reached, which the
C++ port is gated against). They are committed: 75 files, none over 1 MB, 34 MB in total (synthyy is AGPL, like
DDAW). If they grow, move them to Git LFS with `git lfs migrate`. Never write to the synthyy repo.

`ddaw/` holds DDAW's own small fixtures and stub goldens (`make_stub_golden.py`).

The parity test `parity_synthyy` (tests/CMakeLists.txt) runs `ddaw_parity` over `synthyy/`.
