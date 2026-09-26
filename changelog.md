# v1.1.0

- Much faster solving. While solving, the level is hidden and only the physics runs, after a self-test confirms this matches the full game. The progress screen shows the speed.
- New greedy "probe" search: one input change per obstacle is tried before falling back to the full search.
- Final full-game check before a chart is saved.
- One-click installers (Windows `.bat`, macOS/Linux `.sh`) are published on GitHub Releases. The Windows installer also installs Geode if it's missing.

# v1.0.0

- First release: level solver, clean-restart verification, timing centering and the P1/P2 rhythm overlay.
