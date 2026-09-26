# Rhythm Path

Rhythm Path works out how to beat a level, then shows every input as a scrolling rhythm-game chart.

## How it works

1. **Solve.** When you open a level, the mod hides it and plays it in the background using only the game's own physics, many times faster than real time. When the player dies, it rolls back and tries a different input, working back through every combination if it has to. It remembers states that always lead to death so it never retries them.
2. **Verify.** The path it finds is replayed from a clean restart. If that replay dies, the search continues from a little before that point. The final check runs every per-frame function (nothing skipped). If it shows fast simulation was wrong, the level is solved again from the start with fast simulation off.
3. **Center.** If "Center inputs in their timing window" is on (the default), every press is moved to the middle of the range of ticks that still works, so the chart gives you the largest margin for error. This is skipped in exact replay mode.
4. **Play.** The level restarts and the chart scrolls toward the blue timing line. Press when a note's head reaches the line and hold until its tail reaches it.

The chart reads the game's own level clock at render time, so it has no extra delay compared to what the game shows.

## Controls

- **Pause menu → Solve / Re-solve / Cancel solve**
- **Pause menu → Chart: ON/OFF**
- **Cancel** on the solving screen stops a solve (it works on touch devices too).
- Everything else (scroll speed, lane size, position, colours, solver limits) is in the mod settings.

## Notes

- The top lane is P1 and the bottom lane is P2 (the game's player-1 and player-2 buttons). P2 notes only appear on 2-player levels. Changing "Flip 2-player controls" makes a 2-player level solve again.
- Charts are saved per level (and per start position), so each level is only solved once. Editing a level makes it solve again.
- Turn off noclip, speedhack and other bots while it solves.
- Platformer levels are not supported.
