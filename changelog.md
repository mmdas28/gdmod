# v1.1.0

- Much faster solving. While solving, the level is hidden and only the physics runs, after a self-test confirms this matches the full game. The progress screen shows the speed.
- Final full-game check (nothing skipped) before a chart is saved. A chart that fails it is never marked complete. If fast simulation disagrees with the full game, the level is solved again in full mode.
- Before giving up on a level, the solver retries with fast simulation off and then in exact replay mode (in fuzz tests of the search, this solved every solvable level even when rollbacks were slightly inaccurate).
- Cancel button on the solving screen, and a notification when a chart is ready.
- Fixes: progress no longer shows 100% after a failed check; verification repairs stay aligned with the found path; the self-test no longer assumes its checkpoint was created; the pause button keeps working while solving.
- One-click installers (Windows `.bat`, macOS/Linux `.sh`) are published on GitHub Releases. The Windows installer also installs Geode if it's missing.

# v1.0.0

- First release: level solver, clean-restart verification, timing centering and the P1/P2 rhythm overlay.
