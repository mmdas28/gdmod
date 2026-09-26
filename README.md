# Rhythm Path

A [Geode](https://geode-sdk.org) mod for Geometry Dash 2.2081. It finds a way through a level by itself, then turns the inputs into a scrolling rhythm-game chart:

```
┌─┃────────────────────────────────┐
│ ┃     ▌━━━      ▌━━━━━━       P1 │
├─┃────────────────────────────────┤
│ ┃            ▌━━              P2 │
└─┃────────────────────────────────┘
  ↑ timing line (blue)   notes scroll ←
```

Each note is a hold. Press when its head reaches the blue line and release when its tail reaches it. Short taps are just very short holds.

## Install (one click)

**Windows:** open the [latest release](https://github.com/mmdas28/gdmod/releases/latest), download **`RhythmPath-Installer-Windows.bat`**, and double-click it.

The installer finds Geometry Dash (Steam, any library folder, or it asks you), installs [Geode](https://geode-sdk.org) if you don't have it yet, and copies the mod into Geode's mods folder. Then it offers to start the game. The mod itself is packed inside the installer. If Geode (or the Visual C++ runtime it needs) is missing, the installer downloads it from GitHub or Microsoft, so you need an internet connection then. If Geometry Dash is in a protected folder such as Program Files, the installer asks for administrator rights and gives your Windows account write access to the Geometry Dash folder, which Geode needs every time the game starts.

If Windows shows "Windows protected your PC", click **More info → Run anyway**. That warning appears for any script downloaded from the internet that isn't signed.

**macOS / Linux (Steam + Proton):** download **`RhythmPath-Installer-macOS-Linux.sh`** from the same release and run `bash RhythmPath-Installer-macOS-Linux.sh`. Geode needs to be installed already. If it isn't, the script tells you how.

**Android / manual:** download `mmdas28.rhythm-path.geode` from the release and put it in your Geode `mods` folder (Geode → Settings → Open mods folder).

The repository is private, so only GitHub accounts with access to it can open the release page and download the installers.

## Use

- Open any classic (non-platformer) level. If no chart is saved for it, the mod solves it right away. The level is hidden and simulated as fast as your CPU allows, many times faster than real time (the progress screen shows the speed). It restarts the level when it's done. Easy and medium levels usually take a few seconds; hard levels take longer.
- You can also use the pause menu:
  - **Solve / Re-solve** runs the solver again.
  - **Cancel solve** stops a running solve. The solving screen also has its own **Cancel** button.
  - **Chart: ON/OFF** shows or hides the overlay.
- Charts are saved in the mod's save folder (`charts/`), one per level and start position. Editing the level makes it solve again.
- While you play, each press is rated PERFECT / GREAT / GOOD / EARLY / LATE with the error in ms. Notes you don't press show MISS. 1 tick = 1/240 s ≈ 4.2 ms.

## How the solver works

- **Fast simulation.** While solving, the level isn't drawn, and per-frame visual work (object fading and colours, particles, shaders, progress bar) is skipped. Only the game's own physics step runs, many times per frame. At the start, a self-test plays the opening of the level both ways and compares the results tick by tick. Fast mode is only used if they match.
- **Search.** A complete depth-first search over the button state at every physics tick (240 per second). The player keeps its current input and only changes it when that input leads to death. On a death, it rolls back to a saved state and tries the other input at the latest tick first. States that always lead to death are hashed and never retried, which keeps the search small. If the search runs out of options, it doesn't give up straight away. It first tries again from the start with fast simulation off, then once more in exact replay mode, because a slightly inaccurate rollback can make a solvable level look impossible. (A greedy "change one input and see" shortcut was tried and dropped: in simulation tests it was slower than this search.)
- **Exact state.** It restores the game's own practice checkpoint plus about 170 extra player fields that practice checkpoints normally drop (velocities, slope and collision state, jump buffers, held buttons, orb and pad sets). At the start it runs a self-test comparing a restored run with a straight run. If they differ right away, it uses exact replay from the start instead.
- **Verify.** The finished path is replayed from a clean level restart. If it dies anywhere, the search resumes a little before that point, using states saved during the clean run. After repeated mismatches it switches to exact replay mode.
- **Center.** When "Center inputs in their timing window" is on, each note is shifted earlier and later, tick by tick (binary search), to find the range where the level still plays out the same. The note is then moved to the middle of that range. This step is skipped in exact replay mode, and stops early if it runs out of time.
- **Final check.** When fast simulation was used or notes were moved, the finished chart is replayed once more from a clean restart with every per-frame function running (nothing skipped), using the same physics steps per frame. If the centered chart fails, the uncentered one is checked. If fast simulation turns out to disagree with the full game, the level is solved again with fast simulation off. A chart that fails its final check is never saved as complete.
- **Sync.** The overlay places notes with `levelTime` read from the game's own state when it draws. It never keeps its own clock, so there is no drift and no frame of lag.

## Settings worth knowing

| Setting | Default | What it does |
|---|---|---|
| Solver time limit | 600 s | Time allowed for finding a path. After this it keeps the furthest path found and shows how far the chart reaches. Centering afterwards can take up to half this long again (at least 30 s), plus a final replay. |
| Physics steps per simulated frame | 4 | Physics steps per simulated game frame. 4 matches playing at 60 FPS, 1 matches 240 FPS. Lower is more faithful to high-FPS play, higher is faster. |
| Input resolution | 1 tick | Raise to 2–4 to search faster on long or easy levels. |
| Solver CPU budget per frame | 50 ms | How long each frame spends solving. The level is hidden while solving, so higher is faster; the progress screen just updates less often. |
| Center inputs in their timing window | on | Makes the chart as forgiving as possible to hit. |
| Scroll speed / lane height / width / position | — | Overlay look. |
| Visual offset | 0 ms | Keep at 0 for exact sync. Only change it to make up for your own input lag. |

## Limitations

- Platformer levels aren't supported. It only handles jump inputs in classic mode.
- Levels that depend on random triggers can't be solved reliably, because the game's randomness changes between attempts.
- Turn off noclip, speedhack, frame steppers and other bots or macro tools while solving.
- On 2-player levels, the top lane is the game's player-1 button and the bottom lane is the player-2 button. Charts are saved separately for each "Flip 2-player controls" setting, so changing that option makes the level solve again.

## Building

CI builds it for Windows, macOS, iOS and Android on every push (`.github/workflows/build.yml`). Pushes to the default branch, and manual runs of the workflow on the default branch, also replace the GitHub Release for the version in `mod.json` with fresh installers (`installer/`). To build locally:

```sh
geode sdk install        # once, needs the Geode CLI
geode build
```
