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

The installer finds Geometry Dash (Steam, any library folder, or it asks you), installs [Geode](https://geode-sdk.org) if you don't have it yet, and copies the mod into Geode's mods folder. Then it offers to start the game. The mod is packed inside the installer, so nothing else needs to be downloaded.

If Windows shows "Windows protected your PC", click **More info → Run anyway**. That warning appears for any script downloaded from the internet that isn't signed.

**macOS / Linux (Steam + Proton):** download **`RhythmPath-Installer-macOS-Linux.sh`** from the same release and run `bash RhythmPath-Installer-macOS-Linux.sh`. Geode needs to be installed already. If it isn't, the script tells you how.

**Android / manual:** download `mmdas28.rhythm-path.geode` from the release and put it in your Geode `mods` folder (Geode → Settings → Open mods folder).

The repository is private, so the release downloads work while you're signed in to GitHub.

## Use

- Open any classic (non-platformer) level. If no chart is saved for it, the mod solves it right away. The level is hidden and simulated as fast as your CPU allows, many times faster than real time (the progress screen shows the speed). It restarts the level when it's done. Easy and medium levels usually take a few seconds; hard levels take longer.
- You can also use the pause menu:
  - **Solve / Re-solve** runs the solver again.
  - **Cancel solve** stops a running solve.
  - **Chart: ON/OFF** shows or hides the overlay.
- Charts are saved in the mod's save folder (`charts/`), one per level and start position. Editing the level makes it solve again.
- While you play, each press is rated PERFECT / GREAT / GOOD / EARLY / LATE with the error in ms. Notes you don't press show MISS. 1 tick = 1/240 s ≈ 4.2 ms.

## How the solver works

- **Fast simulation.** While solving, the level isn't drawn, and per-frame visual work (object fading and colours, particles, shaders, progress bar) is skipped. Only the game's own physics step runs, many times per frame. At the start, a self-test plays the opening of the level both ways and compares the results tick by tick. Fast mode is only used if they match.
- **Search.** The player starts by keeping its current input and only changes it when it's about to die. On a death, it first tries fast "probes": change the input once at each tick in the last half second, then keep going. The first change that gets past the obstacle is used. Probes cost only a few hundred ticks per obstacle. If no single change works, it falls back to a complete depth-first search over the button state at every tick (240 per second). That search remembers states that always lead to death and never retries them.
- **Exact state.** It restores the game's own practice checkpoint plus about 170 extra player fields that practice checkpoints normally drop (velocities, slope and collision state, jump buffers, held buttons, orb and pad sets). At the start it runs a self-test comparing a restored run with a straight run. If they differ right away, it uses exact replay from the start instead.
- **Verify.** The finished path is replayed from a clean level restart. If it dies anywhere, the search resumes a little before that point, using states saved during the clean run. After repeated mismatches it switches to exact replay mode.
- **Center.** Each note is shifted earlier and later, tick by tick (binary search), to find the range where the level still plays out the same. The note is then moved to the middle of that range.
- **Final check.** The finished chart is replayed once more from a clean restart through the normal, unmodified game loop, with nothing skipped. If the centered chart fails, the uncentered one is checked. If fast simulation turns out to disagree with the real game, that part is solved again in full mode.
- **Sync.** The overlay places notes with `levelTime` read from the game's own state when it draws. It never keeps its own clock, so there is no drift and no frame of lag.

## Settings worth knowing

| Setting | Default | What it does |
|---|---|---|
| Solver time limit | 600 s | After this it keeps the furthest path found and shows how far the chart reaches. |
| Physics steps per simulated frame | 4 | Higher is faster. The final check always uses the normal game loop. |
| Input resolution | 1 tick | Raise to 2–4 to search faster on long or easy levels. |
| Solver CPU budget per frame | 50 ms | How long each frame spends solving. The level is hidden while solving, so higher is faster; the progress screen just updates less often. |
| Center inputs in their timing window | on | Makes the chart as forgiving as possible to hit. |
| Scroll speed / lane height / width / position | — | Overlay look. |
| Visual offset | 0 ms | Keep at 0 for exact sync. Only change it to make up for your own input lag. |

## Limitations

- Platformer levels aren't supported. It only handles jump inputs in classic mode.
- Levels that depend on random triggers can't be solved reliably, because the game's randomness changes between attempts.
- Turn off noclip, speedhack, frame steppers and other bots or macro tools while solving.
- On 2-player levels, the top lane is player 1's input and the bottom lane is player 2's input. If you use the "flip 2-player controls" option, the lanes are swapped relative to your keys.

## Building

CI builds it for Windows, macOS and Android on every push (`.github/workflows/build.yml`). Pushes to the default branch also publish a GitHub Release with the installers (`installer/`). To build locally:

```sh
geode sdk install        # once, needs the Geode CLI
geode build
```
