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

## Install

1. Install [Geode](https://geode-sdk.org) for Geometry Dash 2.2081.
2. Download `mmdas28.rhythm-path.geode` from this repo's GitHub Actions: open the latest **Build Geode Mod** run and download the **Build Output** artifact.
3. Put the `.geode` file in your Geode mods folder. In game you can find it under Geode → Settings (gear) → Open mods folder.
4. Restart the game.

## Use

- Open any classic (non-platformer) level. If no chart is saved for it, the mod solves it right away and shows a progress screen. It restarts the level when it's done.
- You can also use the pause menu:
  - **Solve / Re-solve** runs the solver again.
  - **Cancel solve** stops a running solve.
  - **Chart: ON/OFF** shows or hides the overlay.
- Charts are saved in the mod's save folder (`charts/`), one per level and start position. Editing the level makes it solve again.
- While you play, each press is rated PERFECT / GREAT / GOOD / EARLY / LATE with the error in ms. Notes you don't press show MISS. 1 tick = 1/240 s ≈ 4.2 ms.

## How the solver works

- **Search.** A depth-first search over the button state at every physics tick (240 per second). It uses the game's own step function, so the physics are exactly the game's physics. By default it keeps the current input and only changes it when the player would die. When the player dies it restores a checkpoint and tries the other input. States that always lead to death are hashed and skipped from then on.
- **Exact state.** It restores the game's own practice checkpoint plus about 170 extra player fields that practice checkpoints normally drop (velocities, slope and collision state, jump buffers, held buttons, orb and pad sets). At the start it runs a self-test comparing a restored run with a straight run. If they differ right away, it uses exact replay from the start instead.
- **Verify.** The finished path is replayed from a clean level restart. If it dies anywhere, the search resumes a little before that point, using states saved during the clean run. After repeated mismatches it switches to exact replay mode.
- **Center.** Each note is shifted earlier and later, tick by tick (binary search), to find the range where the level still plays out the same. The note is then moved to the middle of that range. A final clean replay confirms the result. If that check fails, the unshifted, already-verified chart is kept.
- **Sync.** The overlay places notes with `levelTime` read from the game's own state when it draws. It never keeps its own clock, so there is no drift and no frame of lag.

## Settings worth knowing

| Setting | Default | What it does |
|---|---|---|
| Solver time limit | 600 s | After this it keeps the furthest path found and shows how far the chart reaches. |
| Physics steps per simulated frame | 2 | 1 is the most faithful, 4 is faster. |
| Input resolution | 1 tick | Raise to 2–4 to search faster on long or easy levels. |
| Solver CPU budget per frame | 14 ms | Higher values solve faster; the screen will stutter while solving. |
| Center inputs in their timing window | on | Makes the chart as forgiving as possible to hit. |
| Scroll speed / lane height / width / position | — | Overlay look. |
| Visual offset | 0 ms | Keep at 0 for exact sync. Only change it to make up for your own input lag. |

## Limitations

- Platformer levels aren't supported. It only handles jump inputs in classic mode.
- Levels that depend on random triggers can't be solved reliably, because the game's randomness changes between attempts.
- Turn off noclip, speedhack, frame steppers and other bots or macro tools while solving.
- On 2-player levels, the top lane is player 1's input and the bottom lane is player 2's input. If you use the "flip 2-player controls" option, the lanes are swapped relative to your keys.

## Building

CI builds it for Windows, macOS and Android on every push (`.github/workflows/build.yml`). To build locally:

```sh
geode sdk install        # once, needs the Geode CLI
geode build
```
