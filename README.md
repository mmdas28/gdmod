# Rhythm Path

A [Geode](https://geode-sdk.org) mod for Geometry Dash 2.2081. It finds a way through a level by itself, then turns the inputs into a scrolling rhythm-game chart you play along to:

```
╭─┃──────────────────────────────────╮
│ ◉     ●━━━━━      ●     ●━━━━   P1 │
├─┃──────────────────────────────────┤
│ ◉            ●━━          ●     P2 │
╰─┃──────────────────────────────────╯
  ↑ timing line          notes scroll ←
```

Press when a note's head reaches the timing line. Long notes are holds: keep holding until the tail reaches the line. On 2-player levels the second lane is the P2 button. There is also a vertical, FNF-style layout.

## Install (one click)

**Windows:** open the [latest release](https://github.com/mmdas28/gdmod/releases/latest), download **`RhythmPath-Installer-Windows.bat`**, and double-click it.

The installer finds Geometry Dash (Steam, any library folder, or it asks you), installs [Geode](https://geode-sdk.org) if you don't have it yet, and copies the mod into Geode's mods folder. Then it offers to start the game. The mod itself is packed inside the installer. If Geode (or the Visual C++ runtime it needs) is missing, the installer downloads it from GitHub or Microsoft, so you need an internet connection then. If Geometry Dash is in a protected folder such as Program Files, the installer asks for administrator rights and gives your Windows account write access to the Geometry Dash folder, which Geode needs every time the game starts.

If Windows shows "Windows protected your PC", click **More info → Run anyway**. That warning appears for any script downloaded from the internet that isn't signed.

**macOS / Linux (Steam + Proton):** download **`RhythmPath-Installer-macOS-Linux.sh`** from the same release and run `bash RhythmPath-Installer-macOS-Linux.sh`. Geode needs to be installed already. If it isn't, the script tells you how.

**Android / manual:** download `mmdas28.rhythm-path.geode` from the release and put it in your Geode `mods` folder (Geode → Settings → Open mods folder).

The repository is private, so only GitHub accounts with access to it can open the release page and download the installers.

## The level menu

When you open a level, the Rhythm Path menu appears and the level waits until you pick something:

- **Saved chart**: the chart saved for this level, if there is one: whether it's complete, how many notes, how long it took, when it was saved. It also warns you if the level was updated since. **Play**, **Export** or **Delete** it.
- **Solve**: find a path now. If an earlier solve was stopped, **Resume** continues from its saved progress (or **Start over**). **Up to %** lets you solve only part of the level.
- **Import**: load a chart file or a bot macro (see below). It is replayed in the real game to check it, cleaned up, and saved as this level's chart.
- **Show chart from / to %**: only show the chart in part of the level (for practising a section). Turn on **This level only** to keep a range just for this level. Otherwise the defaults from the settings apply.
- **Customize** opens the mod's settings (Geode's own settings screen). **Play without chart** closes the menu.

The level and its music wait while the menu is open, and opening the menu doesn't count as an attempt. You can open the menu again from the pause menu (**Rhythm Path**). Turn off **Show on level open** if you'd rather use the pause menu only. Then **Auto-solve when the menu is off** decides whether levels without a chart are solved automatically (it resumes saved progress if there is some).

Charts are saved per level ID in the mod's save folder: `levels/<level id>/` for online levels, `levels/main-<id>/` for the official levels and `levels/local-…/` for your own levels (by name, so editing a level keeps its chart and shows the "level was updated" warning). Each start position gets its own chart, and so does each "Flip 2-player controls" setting on 2-player levels. If you switch start positions while in a level (for example with a start position switcher mod), the menu and chart switch with it.

## Playing

- Notes scroll toward the timing line. The markers on the line light up while you hold the button.
- Each press is rated PERFECT / GREAT / GOOD / EARLY / LATE with its error in ms, and a note you don't press shows MISS. Letting go of a hold well before its end shows DROP. The timing windows are in the settings.
- Combo and accuracy are shown next to the lanes.
- The chart fades out when nothing is coming up and fades back in before the next note.
- Notes are placed using the game's own level clock at the moment the frame is drawn, so there is no drift and no extra frame of lag.

## Solving

While solving, the level is hidden and simulated using only the game's own physics, many times faster than real time. The solving screen shows the steps, how far it has got, and the speed. **Cancel** stops it and keeps the progress, so you can **Resume** later.

1. **Test.** Checks that fast simulation and rolling back to saved states match the real game exactly on this level, and picks the fastest safe mode.
2. **Search.** A depth-first search over the button state at every physics tick (240 per second). States that always lead to death are remembered and never tried again.
   - **No spam clicking.** In cube, ball, UFO, robot, spider and swing the default is *not* pressing, so the solver only clicks when a click is needed. In ship and wave it keeps the current input and only changes it when it has to.
   - **Ships and waves.** Nearly identical ship, wave, UFO and swing positions are treated as the same state ("Ship/wave search precision: Fast"). This turns the search into pathfinding on a grid and stops it from getting stuck on long ship sections. If a section can't be solved like that, that section is searched again with exact positions.
   - **Human-playable timing.** Ship and wave holds and gaps have a minimum length (settings). If a section is impossible within those limits, they are relaxed for that section only.
   - **Progress is saved** every 45 seconds, when you pause, cancel or quit, and when the time limit runs out (not during the first few seconds of testing). Only time spent solving counts toward the time limit, not time paused. A solve that proves there is no path doesn't leave anything to resume.
3. **Check.** The path is replayed from a clean restart. If it dies anywhere, the search resumes a little before that point.
4. **Clean up.** Each click is tested: it is removed if it isn't needed, taps are shortened to what matters, and repeated clicks are turned into one hold when holding plays exactly the same ("Prefer holding over repeated clicks"). Ship and wave holds with gaps shorter than "Join ship/wave inputs closer than" are joined.
5. **Center.** Each note is moved to the middle of the range of ticks where the level still plays out the same, so the chart is as forgiving as possible.

   Clean up and Center are skipped in exact replay mode (used when rolling back to saved states isn't exact on a level), and both stop early when their share of the time runs out (together at most 3/4 of the time limit, at least 30 s).
6. **Final check.** The finished chart is replayed from a clean restart with nothing skipped. If the cleaned-up chart fails, the original verified chart is used instead. A chart that fails its final check is never saved as complete.

Only one level can be simulated at a time: Geometry Dash's engine runs a level on one thread, and running extra copies of the game in the background would put your save file and other mods at risk. The speed comes from the smarter search instead.

## Importing

- **Rhythm Path charts** (`.rpchart`, or the older `.txt` charts), for example one exported from another computer.
- **GDR macros** (`.gdr` and `.gdr.json`, the format used by xdBot and other 2.2 bots). Only jump inputs are used. On 2-player levels, xdBot macros are mapped to players the way xdBot plays them back; on 1-player levels every input goes to P1.

Imported inputs are replayed in the real game first, with nothing skipped. If they fail at some point, the working part is kept and the solver finds the rest ("Fix imported inputs that fail"). An import always covers the whole level (the "Up to %" field only applies to solving), and it doesn't touch saved solve progress.

## Settings

Everything is in the mod's settings (Geode → Rhythm Path → Settings, or **Customize** in the level menu). The main groups:

| Group | Examples |
|---|---|
| General | Show the menu on level open, auto-solve, show the chart |
| Solver | Time limit, solving screen frame rate (60 by default; the solver works for about 85% of each frame), ship/wave precision, clean-up, holds instead of repeated clicks, centering |
| Human-playable timing | Shortest ship/wave hold and gap, other modes |
| When the chart shows | Default percent range |
| Look | Theme (Clean, Neon, Pastel, Mono, Classic) or custom colors (their alpha sets how see-through the notes are), note shape, glow, key markers, lane labels |
| Layout | Horizontal or vertical, position (including custom), length, lane size, timing line position, scroll speed and direction, P2 lane, swap lanes |
| Translucency | Lane and note opacity, fade out when idle |
| Feedback | Judgements, combo, accuracy, hold judging, timing windows, visual offset |

## Limitations

- Platformer levels aren't supported. It only handles jump inputs in classic mode.
- Levels that depend on random triggers can't be solved reliably, because the game's randomness changes between attempts.
- Turn off noclip, speedhack, frame steppers and other bots or macro tools while solving.
- A level with no possible path usually runs until the time limit, because the solver retries more carefully before giving up.

## Building

CI builds it for Windows, macOS, iOS and Android on every push (`.github/workflows/build.yml`). Pushes to the default branch, and manual runs of the workflow on the default branch, also replace the GitHub Release for the version in `mod.json` with fresh installers (`installer/`). To build locally:

```sh
geode sdk install        # once, needs the Geode CLI
geode build
```

## Geode mod list

The mod is a normal Geode package (`mod.json`, `logo.png`, `about.md`, `changelog.md`, settings shown in Geode's own settings screen). To put it on Geode's in-game mod list, the developer account named in `mod.json` (`mmdas28`) submits the `.geode` file from a release to the Geode index, following Geode's [publishing guide](https://docs.geode-sdk.org/mods/publishing). Geode's team reviews submissions before they appear.
