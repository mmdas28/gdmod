# v2.1.0

- **The solver now plans instead of guessing.** Before it touches the level, it reads every hitbox: blocks, spikes, portals, pads, orbs and speed changes. Within the first few ticks it also learns how each game mode moves. It then plans the inputs:
  - cube jumps in the middle of the window where they are safe;
  - ship, wave and UFO paths through their corridors, with a look-ahead.
  The game then only has to confirm the plan. When a planned input still fails, the planner works out which earlier input caused it and retries from there, instead of trying every possibility.
- **Much faster.** On test levels built like Stereo Madness (about 85 seconds long, cube and ship), a solve took about 3 seconds of estimated game time instead of 8 to 10 minutes. Ship- and wave-heavy test levels took under 2 seconds. These numbers come from a test setup that simulates Geometry Dash–style levels, not from timing inside the game itself.
- **Cleaner charts.** Planned inputs need fewer notes, with no zig-zagging in ship and wave sections.
- **Moving blocks** from move triggers are followed while planning.
- With the planner on, cleaning up and centering the chart share a fixed budget, and the planner already puts presses in the middle of their windows.
- **New setting:** "Plan inputs from the level's hitboxes" (on by default). Turn it off to use the previous search, which behaves exactly as before.
- The solving screen shows how many inputs came from the plan. The log gets a "Solver stats" line with the time spent in each step.

# v2.0.1

- The mouse cursor is visible and free to move while the Rhythm Path menu or the solving screen is open. When you start playing, Geometry Dash's own "show cursor" and "lock cursor" options apply again.

# v2.0.0

- **Level menu** when you open a level: play, export or delete the saved chart, solve or resume, import a chart or a GDR macro, and pick the percent range where the chart shows. Charts are now saved per level ID (and per start position).
- **Solver**
  - No more spam clicking: outside ship and wave, the solver only presses when a press is needed.
  - Ships and waves no longer get it stuck: nearly identical positions are treated as the same state, with an automatic switch to exact positions for sections that need it.
  - Human-playable timing: minimum hold and gap lengths for ship, wave and other modes, relaxed per section only when needed.
  - New clean-up step: removes clicks that aren't needed, shortens taps, turns repeated clicks into holds and joins tiny ship/wave gaps, then re-checks the chart.
  - Progress is saved and can be resumed. Cancel keeps it.
  - Solve only up to a chosen percent.
  - The solving screen runs smoothly (60 FPS by default, was about 16).
- **Import**: Rhythm Path charts and GDR macros (.gdr, .gdr.json). Imports are checked in the real game and repaired by the solver if they fail.
- **New look**: themes (Clean, Neon, Pastel, Mono, Classic) or custom colors, rounded notes, glow, key markers on the timing line, lane labels, combo and accuracy, and a new solving screen.
- **Customization**: horizontal or vertical (FNF-style) layout, position presets or a custom position, scroll direction, lane swap, translucency and fade when idle, and adjustable timing windows.
- **Holds**: hold notes are judged; letting go early shows DROP.
- Settings use Geode's own settings screen and can be opened from the level menu.

# v1.1.0

- Much faster solving. While solving, the level is hidden and only the physics runs, after a self-test confirms this matches the full game. The progress screen shows the speed.
- Final full-game check (nothing skipped) before a chart is saved. A chart that fails it is never marked complete. If fast simulation disagrees with the full game, the level is solved again in full mode.
- Before giving up on a level, the solver retries with fast simulation off and then in exact replay mode (in fuzz tests of the search, this solved every solvable level even when rollbacks were slightly inaccurate).
- Cancel button on the solving screen, and a notification when a chart is ready.
- Fixes: switching to exact replay mode after repeated verify failures no longer reuses stale states from the inexact search, which could rarely make a solvable level report "no possible path"; progress no longer shows 100% after a failed check; verification repairs stay aligned with the found path; the self-test no longer assumes its checkpoint was created; the pause button keeps working while solving.
- One-click installers (Windows `.bat`, macOS/Linux `.sh`) are published on GitHub Releases. The Windows installer also installs Geode if it's missing.

# v1.0.0

- First release: level solver, clean-restart verification, timing centering and the P1/P2 rhythm overlay.
