# Fallout Tactics co-op

Play the Fallout Tactics: Brotherhood of Steel single-player campaign with friends. 2 to 4 players
share one squad: everyone can order every squad member, and the host's campaign is the one being played.

**Status: work in progress.** It works in automated tests with several copies of the game on one PC, and has been
played on two PCs on one home network. See [What works](#what-works) and [Known issues](#known-issues).

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
   - `core\FTSE\ftse.lua` (loads the mod, and makes FTSE's burst rolls the same on every copy). If you already have
     an `ftse.lua` from another FTSE mod, copy the whole of ours into yours instead of replacing it: without the
     `OnBurstAttack` part, NPC bursts land differently on each copy and the game drifts apart.

To uninstall, delete those two files. To remove FTSE as well, delete `realBinkw32.dll` and `core\FTSE`, and put back
your original `binkw32.dll`.

## 4. Play

- **Host:** Multiplayer, then **Start Co-op**, then load a save or start a new campaign. You play straight away.
- **Friends:** Multiplayer, select **[Co-op] <host name>** in the session list, then **Join**. A message shows
  while you wait. Your game then copies the host's save and loads it.
- Friends can join a game already in progress. The host's game saves and reloads once to bring them in.
- Everyone controls the whole squad. Agree among yourselves who moves whom. Coloured dots above each name tab
  show which players have that character selected: the host is red, then blue, yellow and green in the order
  players joined.
- On the world map, only the host moves the party and chooses where to go; everyone else watches it travel.
  Right-click the map to ping a spot in your colour and show where you'd like to go.

### Networking

All players must be able to reach the host's PC. On the same home network this just works. For friends elsewhere, the simplest way is a virtual LAN such as Tailscale or ZeroTier, then join
as if on one network. The mod uses port **47701** (TCP for the save copy, UDP to find the host) on top of the
game's own multiplayer ports. Windows Firewall may ask to allow the game the first time: allow it.

## What works

Checked by the automated tests in `test/`:

- Moving and fighting with the shared squad, with every copy in step, for 2 to 4 players.
- Joining from the Multiplayer menu, and joining a game already in progress.
- Saving and reloading mid-mission (the others follow), and mission → world map → next mission.
- In the bunker, without any reload: buying and selling (new items included) and quest variables a conversation
  changes. Tested with 2, 3 and 4 players.
- Shopping pauses nobody: the other players keep playing while one shops. While a shop is open, the shopper's
  character can't be ordered by the others, and the recruiter waits until the shop closes. Item moves by the others
  meanwhile (giving, equipping, taking, dropping) take effect when the shop closes, so the games stay the same.
- The selection dots, with 2 and 4 players.
- Recruiting and dismissing squad members, without a reload (the game pauses for everyone while the recruit screen
  is up, since it changes the squad everyone plays).
- One player at a time per merchant or recruiter: if several click the same one at once, exactly one gets it and the
  others are told someone got there first.
- The world map: only the host moves the party and picks where to go. The others see the host's party travel on
  their own map.
- Map pings: right-click the world map (a joiner can also left-click) to mark a spot with a flag in your colour.
  Everyone sees it for 10 seconds; it stays on that spot as the map scrolls.
- Turn-based combat, Individual and Squad, with 2 and 3 players. On the squad's turn every player can order the squad
  (in Individual mode, the character whose turn it is), and any player can press End Turn or switch turn-based off.
- The host's game settings count for everyone: difficulty, turn-based mode, CTB speed, friendly fire, violence and
  fast turn-based come from the host's options. Changing them on another player's PC has no effect while you play
  together.
- Random encounters on the world map: the host's game rolls and enters them, and the others follow into the
  encounter.
- Long fights with every player acting at once (moving, swapping characters, giving items, opening containers): the
  games stay identical, with no corrections or reloads, for 2 and 3 players.
- Joining by the host's network address (tested on one PC through its LAN address).
- Equipping and swapping weapons and armour between squad members. If hands or armour ever differ between the
  games, the host's drift check puts them right without a reload.
- Leaving a co-op session to the main menu (it crashed in 0.9.2 and earlier).

## Known issues

- While one player talks to anyone other than a merchant, or uses the recruiter, the other players' games pause until
  the conversation closes. Shops don't pause anyone (for the old behaviour, put the lines `[coop]` and
  `shop_no_pause=0` in a file `ftcoop.ini` next to `BOS.exe`, on every player's PC).
- Joining from the Multiplayer menu waits about 3 s for DirectPlay to close the menu's session search; the waiting
  message shows meanwhile.
- A character who joins the squad through a story conversation (not the recruiter) still means a save and reload
  for everyone.
- Fog of war on the world map isn't synced: ground the host explored while you weren't connected stays dark for you.
- Played on two PCs on one home network; not tested over the internet yet.
- Map pings use the right mouse button; that a real right click reaches the map that way is not yet tested.

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
mission saves), `bunker.sav` (made in the bunker, with the merchant and the recruiter nearby) for the bunker tests,
and `QuestBarter.sav` (in the bunker, squad member 1 next to General Barnaky, member 2 next to the quartermaster, a
level-up pending) for the quest, level-up and shop-and-talk tests.
The list of tests is in `test/run_test.py`.

## How it works

The game already has a lockstep multiplayer layer: every player's orders go to the host and come back to everyone
on the same frame. The mod runs the campaign inside a multiplayer session, shares the squad, copies the host's save to
joiners, and syncs the single-player screens that the multiplayer code never sent. A write-up of the reverse
engineering will go in the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

## License

MIT, see [LICENSE](LICENSE). Fallout Tactics belongs to its rights holders; this project contains none of its files.
