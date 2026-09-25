# Rhythm Path

Rhythm Path works out how to beat a level, then shows every input as a scrolling rhythm-game chart.

## How it works

1. **Solve.** When you open a level, the mod plays it in the background using the game's own physics at 240 ticks per second. When the player dies, it rewinds to a saved state and tries a different input. It remembers states that always lead to death so it doesn't retry them.
2. **Verify.** The path it finds is replayed from a clean restart. If the replay differs anywhere, that section is searched again.
3. **Center.** Every press is moved to the middle of the range of ticks that still works, so the chart gives you the largest margin for error.
4. **Play.** The level restarts and the chart scrolls toward the blue timing line. Press when a note's head reaches the line and hold until its tail reaches it.

The chart reads the game's own level clock at render time, so it has no extra delay compared to what the game shows.

## Controls

- **Pause menu → Solve / Re-solve / Cancel solve**
- **Pause menu → Chart: ON/OFF**
- Everything else (scroll speed, lane size, position, colours, solver limits) is in the mod settings.

## Notes

- The top lane is P1 and the bottom lane is P2. P2 notes only appear on 2-player levels.
- Charts are saved per level (and per start position), so each level is only solved once. Editing a level makes it solve again.
- Turn off noclip, speedhack and other bots while it solves.
- Platformer levels are not supported.
