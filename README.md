# Fallout Tactics co-op

Play the Fallout Tactics: Brotherhood of Steel single-player campaign with friends. 2 to 4 players share one squad:
everyone can order every squad member, and the host's campaign is the one being played.

**Status: beta.** The campaign is playable in co-op and has been played by 3 players on one home network; expect
bugs. Every feature below is covered by automated tests. See [Known issues](#known-issues), and the
[Releases page](https://github.com/thennak3/fallout-tactics-coop/releases) for what changed in each version.

## Co-op features

**Playing together**
- **Start Co-op** in the Multiplayer menu: the host loads a save or starts a new campaign; friends pick
  **[Co-op] <host name>** from the session list and join.
- **Join at any time:** friends can join a game already in progress, and leave again.
- **One shared squad:** every player can order every squad member, in real time and in turn-based combat (on the
  squad's turn everyone can act, and anyone can end the turn).
- **Selection dots:** coloured dots above each name tab show who has which character selected, and each player's
  selection is kept across reloads and map changes.
- **Pings:** right-click the world map or middle-click in a mission to drop a flag in your colour that everyone sees.
  A mission flag stays on its spot of the ground as you scroll.
- **On-screen notices** when something needs another player: who's trading, who got to a character first, who left.

**The campaign, shared**
- **Shops, traders and gambling pause nobody:** the others keep playing while one player trades, and what was bought,
  sold or won reaches everyone.
- **Conversations, quests, recruiting and level-ups** carry over to every player's game.
- **The world map:** the host moves the party and picks where to go; everyone travels along, sees the same uncovered
  ground, and follows into random encounters and the next mission.
- **Saving** (quick-save or the game menu's Save) is written on every player's game in step; the game menu itself
  pauses nobody.
- **The host's settings** (difficulty, turn-based mode, combat speed, friendly fire, violence) count for everyone.

**Staying in step**
- The games run in lockstep, and the host checks every second that they still match. Small differences are put
  right on the fly; only a real split ends in a short save-and-reload for everyone.
- A player who leaves or loses their connection in the middle of a shop, conversation, save or level-up doesn't leave
  the others stuck: the game goes on once the host notices.

## 1. Check your game version

Every player needs their own copy of **Fallout Tactics from GOG**: https://www.gog.com/en/game/fallout_tactics

The mod changes the game in memory at fixed places, so it only works with the exact build it was made for: **GOG,
version 1.27** (the version GOG sells, already patched). Other copies (Steam, an old CD with or without patches) are
untested and almost certainly won't work.

To check, open PowerShell in your game folder (Shift + right-click in the folder, "Open PowerShell window here") and
run `Get-FileHash BOS.exe`. The hash must be exactly:

```
C5B91FE56EF8DE118CE22CCD5E6512F217CA16C8B8758EA14C626229EB5A4E40
```

If it's different, please open an issue with the hash and where you bought the game. The mod doesn't change
`BOS.exe` or any game file on disk.

## 2. Install FTSE

The mod is loaded by [FTSE](https://github.com/melindil/FTSE) (Fallout Tactics Scripting Engine, MIT).

1. Download **FTSE_0.60a.zip** from https://github.com/melindil/FTSE/releases/tag/0.60a
2. Back up `binkw32.dll` from your game folder, so you can undo it later.
3. Extract the zip into your game folder, next to `BOS.exe`, and let it replace `binkw32.dll`. You should now have
   `binkw32.dll`, `realBinkw32.dll` and a `core\FTSE` folder.

## 3. Install the mod

1. Download the latest `fallout-tactics-coop-<version>.zip` from the
   [Releases page](https://github.com/thennak3/fallout-tactics-coop/releases).
2. Extract it into your game folder, next to `BOS.exe`. It adds `netlog.dll` (the mod) and `core\FTSE\ftse.lua`
   (loads the mod and makes FTSE's burst rolls the same on every player's game). If you already have an `ftse.lua`
   from another FTSE mod, copy the whole of ours into yours instead of replacing it.

**Every player must run the same version.** Different versions won't play together.

To uninstall, delete those two files. To remove FTSE as well, delete `realBinkw32.dll` and `core\FTSE`, and put back
your original `binkw32.dll`.

## 4. Play

- **Host:** Multiplayer, then **Start Co-op**, then load a save or start a new campaign. You play straight away.
- **Friends:** Multiplayer, select **[Co-op] <host name>** in the session list, then **Join**. Your game copies the
  host's save and loads it.
- Friends can join a game already in progress: the host's game saves and reloads once to bring them in (about 9
  seconds).
- Everyone controls the whole squad; agree among yourselves who moves whom. Coloured dots above each name tab show
  which players have that character selected (the host is red, then blue, yellow and green in the order players
  joined). Your selection is kept across reloads and map changes.
- On the world map only the host moves the party and chooses where to go. Right-click the map to ping a spot in your
  colour; in a mission, middle-click. Everyone sees the flag for 8 seconds.

### Networking

All players must be able to reach the host's PC. On a home network this just works. For friends elsewhere, the
simplest way is a virtual LAN such as Tailscale or ZeroTier. The mod uses port **47701** (TCP for the save copy, UDP
to find the host) on top of the game's own multiplayer ports. Windows Firewall may ask to allow the game the first
time: allow it.

### Options

Optional settings go in a file `ftcoop.ini` next to `BOS.exe`, under a `[coop]` line:

- `keep_selection=0`: don't restore each player's selection after a reload or map change.
- `shop_no_pause=0`: the old way, a shop pauses every player while it is open (set it on every player's PC).

## Known issues

- While one player talks to a story character or uses the recruiter, the other players' games pause until that
  conversation closes. Shops and traders don't pause anyone.
- A character who joins the squad through a story conversation (not the recruiter), or hiring at the recruiter,
  means a save-and-reload for everyone.
- Joining a game in progress takes about 9 seconds, most of it the game's own network set-up.
- When a player leaves, the host notices only after about 15 seconds; until then anything that player had open
  (a shop, a conversation everyone waits for) stays open.
- A save screen opened by a keyboard shortcut saves on that player's game alone; any difference it causes is put
  right afterwards.
- Tested on one home network; not yet over the internet.

## Build from source

Needs the Visual Studio 2022 Build Tools (x86 C++). Run `src\build.bat`; it writes `src\netlog.dll`. The source is in
`src/`; `netlog.cpp` lists which file holds what.

## Tests

- **Game tests** (`test/run_test.py`): start 2 to 4 copies of the game on one PC, with an in-game bot driving each,
  and compare every copy's state. They need your own test copies of the game (never your real install), each with
  FTSE and the mod, in `test-install`, `test-install-2`, ... under one folder (`FTCOOP_TEST_ROOT`), and use saves
  in the host copy's `core/user/save` (listed in `test/run_test.py`). The test copies use
  [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) so several windows fit on one screen.
  `python -I test/run_test.py trade_host` runs one test; `python -I test/run_suite.py` runs them all (slow).
- **Fuzzer** (`python -I test/fuzz/run_fuzz.py`): plays thousands of random 2 to 4 player sessions on a fake world,
  without the game, with desyncs and bad timings forced on purpose, and checks that every past bug, switched back
  on, is caught. It uses the fuzzer from [coop-anything](https://github.com/cranebeaker/coop-anything)
  (`tools/fuzz`).
- **Log checker** (`python -I test/netlog_tool.py summary <netlog.txt> ...`): turns one or several players'
  `netlog.txt` into a timeline and flags known problems.

## How it works

The game already has a lockstep multiplayer layer: every player's orders go to the host and come back to everyone
on the same frame. The mod runs the campaign inside a multiplayer session, shares the squad, copies the host's save to
joiners, and syncs the single-player screens (shops, conversations, level-ups) that the multiplayer code never sent.

## License

MIT, see [LICENSE](LICENSE). Fallout Tactics belongs to its rights holders; this project contains none of its files.
