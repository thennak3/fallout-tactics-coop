# Fallout Tactics co-op

Play the Fallout Tactics: Brotherhood of Steel single-player campaign with friends. 2 to 4 players
share one squad: everyone can order every squad member, and the host's campaign is the one being played.

**Status: work in progress.** It works in automated tests with several copies of the game on one PC. It has not yet
been tested across two PCs. See [What works](#what-works) and [Known issues](#known-issues).

## 1. Check your game version

Every player needs their own copy of **Fallout Tactics from GOG**: https://www.gog.com/en/game/fallout_tactics

The mod changes the game in memory at fixed places, so it only works with the exact game build it was made for:
**GOG, version 1.27**. That is the version GOG sells, already patched. Other copies (the Steam version, an old CD
with or without patches) are untested; if their `BOS.exe` differs, the mod won't work with them.

To check, open PowerShell in your game folder (Shift + right-click in the folder, "Open PowerShell window here") and run:

```
Get-FileHash BOS.exe
```

The hash must be exactly:

```
C5B91FE56EF8DE118CE22CCD5E6512F217CA16C8B8758EA14C626229EB5A4E40
```

If it's different, the mod won't work with your copy. Please open an issue with the hash and where you bought the game.

Nothing else in the game folder needs changing: the mod was tested on a plain GOG install with only FTSE and the mod
added. It doesn't modify `BOS.exe` or any game file on disk.

## 2. Install FTSE

The mod is loaded by [FTSE](https://github.com/melindil/FTSE) (Fallout Tactics Scripting Engine, MIT).

1. Download **FTSE_0.60a.zip** from https://github.com/melindil/FTSE/releases/tag/0.60a
2. Back up `binkw32.dll` from your game folder, so you can undo it later.
3. Extract the zip into your game folder, next to `BOS.exe`, and let it replace `binkw32.dll`. You should now have
   `binkw32.dll`, `realBinkw32.dll` and a `core\FTSE` folder.

## 3. Install the mod

1. Download `fallout-tactics-coop-<version>.zip` from the
   [Releases page](https://github.com/thennak3/fallout-tactics-coop/releases).
2. Extract it into your game folder, next to `BOS.exe`. It adds two files:
   - `netlog.dll` (the mod)
   - `core\FTSE\ftse.lua` (two lines that tell FTSE to load the mod). If you already have an `ftse.lua` from another
     FTSE mod, add its two lines to yours instead of replacing it.

To uninstall, delete those two files. To remove FTSE as well, delete `realBinkw32.dll` and `core\FTSE`, and put back
your original `binkw32.dll`.

## 4. Play

- **Host:** Multiplayer, then **Start Co-op**, then load a save or start a new campaign. You play straight away.
- **Friends:** Multiplayer, select **[Co-op] <host name>** in the session list, then **Join**. A message shows
  while you wait. Your game then copies the host's save and loads it.
- Friends can join a game already in progress. The host's game saves and reloads once to bring them in.
- Everyone controls the whole squad. Agree among yourselves who moves whom.
- On the world map, only the host chooses where to go; everyone else follows.

### Networking

All players must be able to reach the host's PC. On the same home network this should just work (not yet tested
across two PCs). For friends elsewhere, the simplest way is a virtual LAN such as Tailscale or ZeroTier, then join
as if on one network. The mod uses port **47701** (TCP for the save copy, UDP to find the host) on top of the
game's own multiplayer ports. Windows Firewall may ask to allow the game the first time: allow it.

## What works

Checked by the automated tests in `test/`:

- Moving and fighting with the shared squad, with every copy in step.
- Joining from the Multiplayer menu, and joining a game already in progress.
- Saving and reloading mid-mission (the others follow), and mission → world map → next mission.
- In the bunker: buying and selling, and recruiting or dismissing squad members, without any reload.
  Only one player at a time can talk to a given merchant or recruiter.
- The world map: only the host picks where to go; the others follow.

## Known issues

- While one player is in a conversation, trade or recruit screen, the other players' games pause until it closes.
- A conversation that changes quest state still saves and reloads everyone.
- Not tested yet: 3 or 4 players with the bunker sync, two separate PCs, buying an item that starts a new stack,
  recruiting someone who is not already in the world.
- Turn-based modes are untested; the tests use Continuous Turn-Based.

## Build from source

Needs the Visual Studio 2022 Build Tools (x86 C++). Run `logger\build.bat`; it writes `logger\netlog.dll`.

## Tests

The tests start 2 to 4 copies of the game on one PC, with an in-game bot driving each one, and compare every copy's
state frame by frame. They need your own test copies of the game (never your real install), each with FTSE and the
mod, in `test-install`, `test-install-2`, ... under one folder. Set `FTCOOP_TEST_ROOT` to that folder. The test
copies also use [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) so several windows fit on one screen; it isn't
needed to play.

```
python -I test/run_test.py trade_host     # one test
python -I test/run_suite.py               # every test (slow)
```

The tests load saves from the host copy's `core/user/save`: `New Save Game.sav` and `QuickSave.sav` (two different
mission saves), and `bunker.sav` (made in the bunker, with the merchant and the recruiter nearby) for the bunker tests.
The list of tests is in `test/run_test.py`.

## How it works

The game already has a lockstep multiplayer layer: every player's orders go to the host and come back to everyone
on the same frame. The mod runs the campaign inside a multiplayer session, shares the squad, copies the host's save to
joiners, and syncs the single-player screens that the multiplayer code never sent. A write-up of the reverse
engineering will go in the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

## License

MIT, see [LICENSE](LICENSE). Fallout Tactics belongs to its rights holders; this project contains none of its files.
