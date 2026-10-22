# Fallout Tactics co-op

Play the Fallout Tactics: Brotherhood of Steel single-player campaign with friends. 2 to 4 players
share one squad: everyone can order every squad member, and the host's campaign is the one being played.

**Status: work in progress.** It works in automated tests with 2 copies of the game on one PC. It has not yet
been tested across two PCs. See [What works](#what-works) and [Known issues](#known-issues).

## Requirements

- Fallout Tactics, **GOG version 1.27**. `BOS.exe` SHA-256:
  `C5B91FE56EF8DE118CE22CCD5E6512F217CA16C8B8758EA14C626229EB5A4E40`. Other versions are not supported: the mod
  patches the game at fixed addresses.
- [FTSE 0.60a](https://github.com/melindil/FTSE/releases) (Fallout Tactics Scripting Engine, MIT). It loads the mod.
- Every player needs their own copy of the game. This repo contains no game files.

## Install

1. Install FTSE 0.60a in the game folder, following its readme.
2. Copy `netlog.dll` into the game folder, next to `BOS.exe`.
3. Copy `install/core/FTSE/ftse.lua` to `<game dir>/core/FTSE/ftse.lua`. If you already have an `ftse.lua`, add its
   two lines to yours instead.

There is no prebuilt release yet; build `netlog.dll` as described below.

## Play

- **Host:** Multiplayer, then **Start Co-op**, then load a save or start the campaign. You play at once.
- **Friends:** Multiplayer, select **[Co-op] <host name>** in the session list, then **Join**. The game waits for
  the host, copies the host's save and loads it.
- Friends can join a game already in progress: the host's game saves and reloads once to bring them in.

The host's game needs to be reachable on the game's own multiplayer (DirectPlay) ports and on port **47701**
(TCP for the save copy, UDP to find the host). Playing over the internet without a LAN or VPN is untested.

## What works

Checked by the automated two-copy tests in `test/`:

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

## Build

Needs the Visual Studio 2022 Build Tools (x86 C++). Run `logger\build.bat`; it writes `logger\netlog.dll`.

## Tests

The tests start 2 to 4 copies of the game on one PC, with an in-game bot driving each one, and compare every copy's
state frame by frame. They need your own test copies of the game (never your real install), each with FTSE and the
mod, in `test-install`, `test-install-2`, ... under one folder. Set `FTCOOP_TEST_ROOT` to that folder.

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
joiners, and syncs the single-player screens that the multiplayer code never sent. A write-up of the reverse engineering
will go in the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

## License

MIT, see [LICENSE](LICENSE). Fallout Tactics belongs to its rights holders; this project contains none of its files.
