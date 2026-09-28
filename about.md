# Rhythm Path

Rhythm Path works out how to beat a level, then shows every input as a scrolling rhythm-game chart.

## When you open a level

A menu shows the chart saved for this level (per level ID and start position) and lets you:

- **Play** the saved chart, **Export** it or **Delete** it.
- **Solve** the level, or **Resume** a solve that was stopped.
- **Import** a chart or a GDR bot macro (.gdr / .gdr.json). Imports are checked in the real game and fixed by the solver where they fail.
- Choose the **percent range** where the chart is shown.

## How it solves

1. **Plan.** The solver reads every hitbox in the level and learns how each game mode moves. It then plans the inputs: cube jumps in the middle of their safe window, and ship, wave and UFO paths through their corridors. The game only has to confirm the plan, so levels like Stereo Madness solve in seconds.
2. **Search.** Wherever the plan needs help, the level is hidden and simulated using only the game's own physics, many times faster than real time. The solver only clicks when a click is needed, and progress is saved, so a stopped solve can be resumed.
3. **Check.** The path is replayed from a clean restart. If it dies, the search continues from just before that point.
4. **Clean up.** Clicks that aren't needed are removed, taps are shortened, and repeated clicks become holds when holding plays the same.
5. **Center.** Every input is moved to the middle of the range of ticks that still works.
6. **Final check.** The finished chart is replayed with nothing skipped before it is saved as complete.

## Playing

Press when a note reaches the timing line and hold long notes until their tail reaches it. You get PERFECT / GREAT / GOOD / EARLY / LATE / MISS / DROP feedback, a combo and an accuracy score. The chart reads the game's own level clock when it draws, so there is no extra delay.

## Customize

Themes (Clean, Neon, Pastel, Mono, Classic) or custom colors, horizontal or vertical (FNF-style) layout, position, size, scroll speed and direction, translucency and fade-out, timing windows, shortest ship/wave holds and gaps, and more: everything is in the mod settings.

## Notes

- On 2-player levels the second lane is the P2 button.
- Turn off noclip, speedhack and other bots while it solves.
- Platformer levels are not supported.
