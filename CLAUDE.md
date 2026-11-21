# mrun

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

Build and test inside mwc's `mwc-dev` distrobox; the host lacks grim.
`distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/mrun && ...'`.

| Target | |
|---|---|
| `make` | mrun |
| `make test` | `tests/ui.sh` under a headless mwc from `MWC_DIR` (default `../mwc`), log in `build/ui.log`, mrun's own in `build/ui-mrun.log` |

The test uses mwc's helpers from `tests/lib.sh` (`check`, `wait_for`, `pixel_is`,
`reload_mwc`) and its `build/vinput` and `build/pngtool`. When those can't do what a
test needs, extend them in mwc.


## Rules

- `mtk/` is mtk, vendored with git subtree: changed in mtk's repository and taken
  with `git subtree pull --prefix=mtk`, never edited here.
- Colours and look follow mwc through mtk: its colours, Motif's shadows and text
  colour.
