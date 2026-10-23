"""Two-copy co-op test for Fallout Tactics (ftcoop netlog bot).

Starts a host and a joiner test copy on this PC, both with ftcoop.ini [test] enabled, waits for the
in-game bot to finish, then checks both logs and prints one line: RESULT: PASS or RESULT: FAIL.

Tests:
  host_move   host orders a squad member to walk ~10-30 units
  join_move   the joiner orders it (exercises the shared-squad fix)
  simul       host and joiner order different units on the same frame
  attack      host orders an attack on the nearest character outside the squad
  soak        both sides take turns ordering random walks for --soak-frames frames
  fetch_save  the joiner starts from a different save; the host's save is transferred
  transition  both end the mission to the world map; the joiner must rejoin the next session
Every test compares every Actor's position and HP at every shared snapshot frame (every 50 frames).

Usage: python -I run_test.py <test> [--mutate-share-squad] [--timeout 180] [--soak-frames 4000]
Only processes started by this script are ever stopped.
"""
import argparse
import hashlib
import math
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# The folder holding the test copies (test-install, test-install-2, ...): FTCOOP_TEST_ROOT, else three levels up.
ROOT = os.path.abspath(os.environ.get("FTCOOP_TEST_ROOT") or os.path.join(HERE, "..", "..", ".."))
HOST_DIR = os.path.join(ROOT, "test-install", "Fallout Tactics")
JOIN_DIR = os.path.join(ROOT, "test-install-2", "Fallout Tactics")
# --joiners 2 or 3: more joiner copies (4 players at most), windows in a 2x2 grid (ddraw.ini posX/posY).
EXTRA_JOIN_DIRS = [os.path.join(ROOT, f"test-install-{n}", "Fallout Tactics") for n in (3, 4)]
DLL = os.path.join(HERE, "..", "logger", "netlog.dll")
RUNS = os.path.join(ROOT, "re", "test_runs")
SAVE = "user/save/New Save Game.sav"
OTHER_SAVE = "user/save/QuickSave.sav"  # differs from SAVE; the fetch_save joiner loads it
RECEIVED = "user/save/ftcoop_received.sav"
MIN_MOVE = 8.0

# test name -> (bot scenario, which side acts for single-actor scenarios, minimum shared snapshot frames)
TESTS = {
    "host_move": ("move", "host", 7),
    "join_move": ("move", "join", 7),
    "simul": ("simul", "host", 7),
    "attack": ("attack", "host", 9),
    "soak": ("soak", "host", 0),  # minimum computed from --soak-frames
    "fetch_save": ("move", "host", 7),  # joiner loads a different save; only the transfer can sync it
    "transition": ("transition", "host", 5),  # mission -> world map -> mission; host re-syncs, joiner follows
    "resync": ("resync", "host", 7),  # 1 snapshot in session 1 + 6 in session 2; mid-mission quick-save + quick-load; joiner follows into session 2
    "drift_host": ("drift", "host", 4),
    "joinload": ("joinload", "join", 5),
    "play": ("play", "host", 4),
    "invscreen": ("invscreen", "host", 10),  # exploration: the host sits in its inventory (--hold-frames: seconds; 0 = control)  # a person plays both copies (--save, --hold-frames); the bots only log and compare  # the joiner presses quick-load mid-mission: refused (only the host loads)  # the joiner moves an NPC on its copy only; the host notices and re-syncs
    "survey": ("survey", "host", 0),  # exploration: the NPCs next to the first two squad members (use with --save)
    "quest_host": ("quest", "host", 4),  # QuestBarter.sav: the host talks to the quest giver; the joiner gets the changes
    "quest_join": ("quest", "join", 4),  # the same from the joiner
    "shoptalk": ("shoptalk", "host", 4),  # QuestBarter.sav: the host shops; the joiner's talk waits until the shop is done
    "levelup_host": ("levelup", "host", 4),  # QuestBarter.sav: the host levels up (a perk); the joiner gets the stats
    "levelup_join": ("levelup", "join", 4),  # the same from the joiner
    "levelrace": ("levelup", "all", 4),  # QuestBarter.sav: both press Level on the same frame; one screen wins
    "menu_join": ("move", "join", 7),  # both pick their role in the Multiplayer menu (Host / Join), not ftcoop.ini
    "dropin": ("dropin", "host", 4),  # host plays alone; a joiner arrives later through the menu; host re-syncs
    "talk_host": ("talk", "host", 4),  # bunker save: the host talks to an NPC, changes its game, ends: joiner gets it
    "talk_join": ("talk", "join", 4),  # the same from the joiner: its save goes to the host, everyone reloads it
    "trade_host": ("trade", "host", 4),  # bunker: both move; the host walks to the merchant, barters, trades; joiner gets it
    "trade_join": ("trade", "join", 4),  # the same with the joiner trading
    "recruit_host": ("recruit", "host", 4),  # bunker: the host talks to the recruiter and recruits; the joiner gets the squad
    "recruit_join": ("recruit", "join", 4),  # the same with the joiner recruiting
    "recruitnew_host": ("recruitnew", "host", 4),  # bunker: the host recruits someone never recruited; no reload
    "recruitnew_join": ("recruitnew", "join", 4),  # the same from the joiner: its save goes to the host
    "trade2_host": ("trade2", "host", 4),  # bunker: the host trades, then the joiner trades with the same merchant and sells
    "trade_race": ("trade", "all", 4),  # bunker, 3+ players: every joiner talks to the merchant at once; one wins and trades
    "browse_host": ("browse", "host", 6),  # bunker: the host opens barter, buys nothing: no reload; joiner is refused
    "browse_join": ("browse", "join", 6),  # the same with the joiner browsing; the host is refused
    "exit_host": ("exit", "host", 4),  # bunker: the host walks the squad onto the exit grid; world map, host picks
    "exit_join": ("exit", "join", 4),  # the same with the joiner walking the squad out; the host still picks
}
MENU_TESTS = {"menu_join", "dropin"}
HOST_IP = "127.0.0.1"  # --host-ip: the address joiners connect to (this PC's LAN address tests the real network path)
FORCE_FILE_SERVER = False  # set in main: trades with more than one joiner use the file-server path
SHOP_NO_PAUSE = True  # a shop pauses nobody (the default); --shop-pause: the old held conversation
DROPIN_DELAY_S = 10  # dropin: start the joiner this long after the host, so the host is already playing


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def write_ini(folder, mode, scenario, actor, timeout, share_squad, soak_frames, save=SAVE, fetch=True, follow=True,
              menu=False, joiners=1, index=1, hold=0):
    # menu: the copy starts with co-op off and the bot presses Multiplayer > Host or Join (mode names which).
    with open(os.path.join(folder, "ftcoop.ini"), "w", newline="\r\n") as f:
        f.write(f"[coop]\nmode={'off' if menu else mode}\nhost={HOST_IP}\nshare_squad={1 if share_squad else 0}\n")
        f.write(f"fetch_save={1 if fetch else 0}\nfollow={1 if follow else 0}\n")
        f.write(f"shop_no_pause={1 if SHOP_NO_PAUSE else 0}\n")
        if FORCE_FILE_SERVER:  # trade with 3-4 players: the delta then goes through the host's file server
            f.write("lockstep_chunks=10\n")
        f.write(f"[test]\nenabled=1\nsave={save}\nscenario={scenario}\nactor={actor}\njoiners={joiners}\n")
        f.write(f"timeout={timeout}\nsoak_frames={soak_frames}\n")
        if menu:
            f.write(f"menu={mode}\n")
        f.write(f"index={index}\nhold={hold}\n")


def read(path):
    try:
        with open(path, encoding="latin1") as f:
            return f.read()
    except OSError:
        return ""


def snapshots(log):
    """(session, frame) -> {actor id: (x, y, z, hp)}"""
    out = {}
    for m in re.finditer(r"SNAP s=(\d+) f=(\d+) id=(\d+) pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\) hp=(-?\d+)", log):
        out.setdefault((int(m.group(1)), int(m.group(2))), {})[int(m.group(3))] = (
            float(m.group(4)), float(m.group(5)), float(m.group(6)), int(m.group(7)))
    return out


def inventories(log):
    """(session, frame) -> {actor id: "item:count,..."}"""
    out = {}
    for m in re.finditer(r"INV s=(\d+) f=(\d+) id=(\d+) items=(\S*)", log):
        out.setdefault((int(m.group(1)), int(m.group(2))), {})[int(m.group(3))] = m.group(4)
    return out


def tables(log, tag):
    """(session, frame) -> hash, for the ETAB and VARS lines"""
    return {(int(m.group(1)), int(m.group(2))): m.group(3)
            for m in re.finditer(tag + r" s=(\d+) f=(\d+) (?:n=\d+ )?hash=(\S+)", log)}


def squads(log):
    """(session, frame) -> "id,id,..." (the campaign squad, in order)"""
    return {(int(m.group(1)), int(m.group(2))): m.group(3)
            for m in re.finditer(r"SQUAD s=(\d+) f=(\d+) n=\d+ ids=(\S*)", log)}


def travelled(snaps, uid):
    frames = sorted(f for f in snaps if uid in snaps[f])
    if len(frames) < 2:
        return 0.0
    return math.dist(snaps[frames[0]][uid][:3], snaps[frames[-1]][uid][:3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("test", choices=sorted(TESTS))
    ap.add_argument("--mutate-share-squad", action="store_true", help="turn the shared-squad fix off; must FAIL")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--soak-frames", type=int, default=4000)
    ap.add_argument("--mutate-fetch", action="store_true", help="turn the save transfer off; fetch_save must FAIL")
    ap.add_argument("--mutate-follow", action="store_true", help="joiner never follows a new save; resync must FAIL")
    ap.add_argument("--joiners", type=int, default=1, choices=(1, 2, 3), help="joiner copies (players - 1)")
    ap.add_argument("--host-ip", default="127.0.0.1", help="address the joiners connect to (default loopback)")
    ap.add_argument("--inline-delta", action="store_true", help="3-4 player trades: send the deal through lockstep, not the file server")
    ap.add_argument("--shop-pause", action="store_true", help="the old way: a shop holds every copy while it is open")
    ap.add_argument("--hold-frames", type=int, default=0, help="dropin: stay this many frames in the joined session")
    ap.add_argument("--save", default=None, help="save both copies start from (default New Save Game.sav)")
    args = ap.parse_args()
    global HOST_IP, FORCE_FILE_SERVER, SHOP_NO_PAUSE
    HOST_IP = args.host_ip
    SHOP_NO_PAUSE = not args.shop_pause
    extras = EXTRA_JOIN_DIRS[:args.joiners - 1]
    scenario, actor, min_frames = TESTS[args.test]
    FORCE_FILE_SERVER = scenario == "trade" and args.joiners > 1 and not args.inline_delta
    global SAVE
    if args.save:
        SAVE = args.save
    elif scenario in ("talk", "trade", "trade2", "recruit", "recruitnew", "browse", "exit"):
        SAVE = "user/save/bunker.sav"
    elif scenario in ("quest", "shoptalk", "levelup"):
        SAVE = "user/save/QuestBarter.sav"
    if args.test == "soak":
        min_frames = args.soak_frames // 50 - 1
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, ok, detail))

    # Preconditions: same mod build and same save in both copies.
    for d in [HOST_DIR, JOIN_DIR] + extras:
        shutil.copyfile(DLL, os.path.join(d, "netlog.dll"))
        r = os.path.join(d, "core", *RECEIVED.split("/"))
        if d != HOST_DIR and os.path.exists(r):
            os.remove(r)
    fetch_test = args.test == "fetch_save"
    join_save = OTHER_SAVE if fetch_test else SAVE
    host_file = os.path.join(HOST_DIR, "core", *SAVE.split("/"))
    join_file = os.path.join(JOIN_DIR, "core", *join_save.split("/"))
    received = os.path.join(JOIN_DIR, "core", *RECEIVED.split("/"))
    if os.path.exists(received):
        os.remove(received)
    if fetch_test:
        ok = os.path.exists(host_file) and os.path.exists(join_file) and sha(host_file) != sha(join_file)
        check("precondition: the joiner's save differs from the host's (only the transfer can sync)", ok,
              f"host {SAVE}, joiner {join_save}")
    else:
        ok = os.path.exists(host_file) and os.path.exists(join_file) and sha(host_file) == sha(join_file)
        check("precondition: identical save in both copies", ok, SAVE)
    for d in extras:
        f = os.path.join(d, "core", *join_save.split("/"))
        check(f"precondition: {os.path.basename(os.path.dirname(d))} has the joiner's save",
              os.path.exists(f) and sha(f) == sha(join_file))
        ok = ok and checks[-1][1]
    if not ok:
        return report(args, checks, None)

    # Back up the play settings. A file that still has a [test] section is a leftover from an interrupted run,
    # not the owner's settings: restore the plain play config instead (once this backed up a test ini).
    backups = {}
    join_default = f"[coop]\r\nmode=join\r\nhost={HOST_IP}\r\n"
    for d, default in [(HOST_DIR, "[coop]\r\nmode=host\r\n"), (JOIN_DIR, join_default)] + [(e, join_default) for e in extras]:
        ini = os.path.join(d, "ftcoop.ini")
        text = read(ini) if os.path.exists(ini) else None
        backups[d] = default if text is not None and "[test]" in text else text
        log = os.path.join(d, "netlog.txt")
        if os.path.exists(log):
            os.remove(log)
    share = not args.mutate_share_squad
    fetch = not args.mutate_fetch
    menu = args.test in MENU_TESTS
    dropin = args.test == "dropin"
    write_ini(HOST_DIR, "host", scenario, actor, args.timeout + (DROPIN_DELAY_S if dropin else 0), share,
              args.soak_frames, SAVE, fetch, menu=menu, joiners=0 if dropin else args.joiners, hold=args.hold_frames)
    for i, d in enumerate([JOIN_DIR] + extras, start=1):
        write_ini(d, "join", scenario, actor, args.timeout, share, args.soak_frames, join_save, fetch,
                  not args.mutate_follow, menu=menu, index=i, hold=args.hold_frames)

    # The host's own quick-save must survive re-syncs (they go through the quick-save slot).
    host_quick = os.path.join(HOST_DIR, "core", "user", "save", "QuickSave.sav")
    quick_before = sha(host_quick) if os.path.exists(host_quick) else None
    join_quick = os.path.join(JOIN_DIR, "core", "user", "save", "QuickSave.sav")  # talk_join quick-saves on the joiner
    join_quick_before = sha(join_quick) if os.path.exists(join_quick) else None
    procs = []
    try:
        procs.append(subprocess.Popen([os.path.join(HOST_DIR, "BOS.exe")], cwd=HOST_DIR))
        time.sleep(DROPIN_DELAY_S if dropin else 2)
        procs.append(subprocess.Popen([os.path.join(JOIN_DIR, "BOS.exe")], cwd=JOIN_DIR))
        for d in extras:
            time.sleep(2)
            procs.append(subprocess.Popen([os.path.join(d, "BOS.exe")], cwd=d))
        deadline = time.time() + args.timeout + 30 + 2 * len(extras)
        while time.time() < deadline and any(p.poll() is None for p in procs):
            time.sleep(1)
        codes = []
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
                codes.append("killed (hung)")
            else:
                codes.append(p.returncode)
    finally:
        for d, text in backups.items():
            ini = os.path.join(d, "ftcoop.ini")
            if text is None:
                os.remove(ini)
            else:
                with open(ini, "w", newline="") as f:
                    f.write(text)

    host, join = read(os.path.join(HOST_DIR, "netlog.txt")), read(os.path.join(JOIN_DIR, "netlog.txt"))
    extra_logs = [read(os.path.join(d, "netlog.txt")) for d in extras]
    if actor == "all" and scenario == "trade":  # the race: exactly one joiner gets the merchant; checked as the trader below
        joiner_logs = [join] + extra_logs
        winners = [i for i, l in enumerate(joiner_logs) if "co-op: .?AVWSpeech@@ opened for this player" in l]
        check("race: every joiner talked to the merchant", len(extras) >= 1
              and all("bot: TALK (the owner's talk command, to NPC 96" in l for l in joiner_logs))
        check("race: exactly one joiner got the conversation", len(winners) == 1, f"winners {winners}")
        losers = [l for i, l in enumerate(joiner_logs) if i not in winners]
        # A loser either sent its talk and lost at the host (dropped; the winner's claim tells it), or got the
        # winner's claim before sending and was refused on its own copy.
        sent = [l for l in losers if "co-op: claim " in l and " sent for NPC 96" in l]
        check("race: the host relayed one claim and dropped the talk of every loser that sent one",
              len(re.findall(r"host: dropped player \d+'s talk to NPC 96", host)) == len(sent)
              and len(re.findall(r"co-op: NPC 96 claimed by", host)) == 1, f"{len(sent)} of {len(losers)} losers sent")
        check("race: every loser was told someone else got there first, and kept no conversation",
              all("co-op: NPC 96 claimed by another player" in l and "opened for this player" not in l
                  and ("notice: Co-op: another player got to them first" in l
                       or "refused this player's talk to NPC 96" in l) for l in losers))
        if len(winners) == 1 and winners[0] > 0:  # put the winner first
            w = winners[0]
            join, extra_logs[w - 1] = extra_logs[w - 1], join
            codes[1], codes[w + 1] = codes[w + 1], codes[1]
        actor = "join"
    check("both copies exited cleanly (code 0)", codes[:2] == [0, 0], f"host={codes[0]} joiner={codes[1]}")
    # Mode asserts: guard against a hollow pass where a copy silently ran single-player.
    hooks = "main-menu hook ok, pump hook ok"
    check("host became the co-op host", "co-op: hosting" in host and hooks in host)
    check("joiner became a co-op joiner", f"co-op: joining {HOST_IP}" in join and hooks in join)
    if menu:
        check("Start Co-op button added to the Multiplayer screen", "menu: Start Co-op button added" in host)
        check("host pressed Start Co-op", "menu: Multiplayer > Start Co-op" in host and "co-op mode at start: off" in host)
        check("host's DirectPlay session is named [Co-op] <name>", re.search(r'session "\[Co-op\] [^"]+", DPTCPNetwork', host) is not None)
        check("joiner's session list showed the host's co-op session", 'bot: the session list shows "[Co-op]' in join)
        check("joiner found the host over UDP and joined that session",
              re.search(r'menu: Multiplayer > Join: co-op session "\[Co-op\][^"]*" at 127\.0\.0\.1', join) is not None
              and "co-op mode at start: off" in join)
        check("Join button enabled with no session in the list", "Join button enabled" in join)
        check("joiner showed the waiting message, which closed by itself when the host was ready",
              "co-op: showing the waiting message" in join and "closing the waiting message" in join
              and 'co-op: loading the host\'s save "user/save/ftcoop_received.sav"' in join)
    if not dropin:
        m = re.search(r"bot: (\d+) players ready", host)
        check("host started with 2 ready players", bool(m) and int(m.group(1)) >= 2, m.group(0) if m else "no start line")
    check("joiner connected to the host's session", "join: connected to the host's session" in join)
    f_host = re.search(rf"bot: in game, first frame (\d+), mode host, scenario {scenario}", host)
    f_join = re.search(rf"bot: in game, first frame (\d+), mode join, scenario {scenario}", join)
    if dropin:  # the joiner enters at the host's generation-2 session: compare that session's first frame
        f_host = re.search(r"bot: session 2 first frame (\d+)", host)
        f_join = re.search(r"bot: session 1 first frame (\d+)", join)
    check(f"both reached the game on the same first frame, scenario {scenario}",
          bool(f_host and f_join) and f_host.group(1) == f_join.group(1),
          f"host={f_host.group(1) if f_host else None} joiner={f_join.group(1) if f_join else None}")

    # Whole-map oracle: every Actor, every shared snapshot frame.
    s_host, s_join = snapshots(host), snapshots(join)
    shared = sorted(set(s_host) & set(s_join))
    talk_frame = free_frame = None
    if scenario == "drift":  # session 1 differs on purpose from the joiner's nudge; session 2 must not
        nd = re.search(r"bot: DRIFT moved NPC \d+ on this copy only, at frame (\d+)", join)
        talk_frame = int(nd.group(1)) if nd else None
        fx = re.search(r"co-op: drift correction applied here too at frame (\d+)", host)
        fix_frame = int(fx.group(1)) if fx else None
        shared = [k for k in shared if k[0] >= 2 or (talk_frame is not None and k[1] < talk_frame)
                  or (fix_frame is not None and k[1] > fix_frame)]
    if scenario in ("talk", "quest", "shoptalk", "levelup", "trade", "trade2", "recruit", "recruitnew", "browse"):
        # Before the talk both copies match; from the talk the talker's game differs until the other copy applied the
        # talker's delta (no reload); from the frame after that, everything must match again.
        # (a level-up does not hold the others: its changes land there up to a second later, so those frames are skipped)
        tf = re.search(r"bot: (?:TALK|LEVEL pressing the Level button) .* at frame (\d+)", host if actor != "join" else join)
        talk_frame = int(tf.group(1)) if tf else None
        ff = [int(x) for x in re.findall(r"co-op: NPC 9?6?\d* is free again \([^)]*\) at frame (-?\d+)", host + join)]
        free_frame = max(ff) if ff else None
        shared = [k for k in shared if k[0] >= 2 or (talk_frame is not None and k[1] < talk_frame)
                  or (free_frame is not None and k[1] > free_frame + 1)]
    check(f"at least {min_frames} shared snapshot frames", len(shared) >= min_frames,
          f"{len(shared)} shared, host {len(s_host)}, joiner {len(s_join)}")
    mismatch = [f for f in shared if s_host[f] != s_join[f]]
    detail = f"{len(shared)} frames, {sum(len(s_host[f]) for f in shared)} actor states"
    if mismatch:
        f = mismatch[0]
        diff = [i for i in set(s_host[f]) | set(s_join[f]) if s_host[f].get(i) != s_join[f].get(i)]
        detail = f"first mismatch (session, frame) {f}: {len(diff)} actors differ, e.g. id {diff[0]} host {s_host[f].get(diff[0])} joiner {s_join[f].get(diff[0])}"
    check("every actor identical on both copies at every shared frame", bool(shared) and not mismatch, detail)
    i_host, i_join = inventories(host), inventories(join)
    inv_n = sum(len(i_host.get(f, {})) for f in shared)
    inv_bad = [f for f in shared if i_host.get(f, {}) != i_join.get(f, {})]
    detail = f"{len(shared)} frames, {inv_n} inventories"
    if inv_bad:
        f = inv_bad[0]
        a, b = i_host.get(f, {}), i_join.get(f, {})
        diff = [i for i in set(a) | set(b) if a.get(i) != b.get(i)]
        detail = f"first mismatch {f}: {len(diff)} differ, e.g. id {diff[0]} host {a.get(diff[0])} joiner {b.get(diff[0])}"
    check("every inventory identical on both copies at every shared frame", inv_n > 0 and not inv_bad, detail)
    q_host, q_join = squads(host), squads(join)
    q_bad = [f for f in shared if q_host.get(f) != q_join.get(f) or not q_host.get(f)]
    e_host, e_join = (dict(((int(m.group(1)), int(m.group(2))), m.group(3))
                           for m in re.finditer(r"ETAB s=(\d+) f=(\d+) n=\d+ hash=(\S+)", log)) for log in (host, join))
    e_bad = [f for f in shared if e_host.get(f) != e_join.get(f) or not e_host.get(f)]
    check("same entity-table bookkeeping on both copies at every shared frame", bool(shared) and not e_bad,
          f"first mismatch {e_bad[0]}: host {e_host.get(e_bad[0])} joiner {e_join.get(e_bad[0])}" if e_bad
          else f"{len(shared)} frames")
    st_host, st_join = tables(host, "STOCK"), tables(join, "STOCK")
    st_bad = [f for f in shared if st_host.get(f) != st_join.get(f) or not st_host.get(f)]
    check("same campaign stock list (what the quartermaster sells) on both copies at every shared frame",
          bool(shared) and not st_bad, f"first mismatch {st_bad[0]}" if st_bad else f"{len(shared)} frames")
    v_host, v_join = tables(host, "VARS"), tables(join, "VARS")
    v_bad = [f for f in shared if v_host.get(f) != v_join.get(f) or not v_host.get(f)]
    check("same quest variables on both copies at every shared frame", bool(shared) and not v_bad,
          f"first mismatch {v_bad[0]}: host {v_host.get(v_bad[0])} joiner {v_join.get(v_bad[0])}" if v_bad
          else f"{len(shared)} frames")
    a_host, a_join = tables(host, "ATTR"), tables(join, "ATTR")
    a_bad = [f for f in shared if a_host.get(f) != a_join.get(f) or not a_host.get(f)]
    check("same character attributes (stats, skills, perks) on both copies at every shared frame", bool(shared) and not a_bad,
          f"first mismatch {a_bad[0]}" if a_bad else f"{len(shared)} frames")
    check("same squad on both copies at every shared frame", bool(shared) and not q_bad,
          f"first mismatch {q_bad[0]}: host {q_host.get(q_bad[0])} joiner {q_join.get(q_bad[0])}" if q_bad
          else f"{len(shared)} frames")

    # Scenario asserts: the action really happened and had an effect.
    if scenario == "move":
        mover = host if actor == "host" else join
        mv = re.search(r"bot: MOVE id=(\d+)", mover)
        check(f"the {actor} bot issued the move", bool(mv), mv.group(0) if mv else "no MOVE line")
        if mv:
            uid = int(mv.group(1))
            d = (travelled(s_host, uid), travelled(s_join, uid))
            check(f"moved unit {uid} travelled > {MIN_MOVE} on both copies", min(d) > MIN_MOVE,
                  f"host {d[0]:.2f}, joiner {d[1]:.2f}")
        # Selection dots: the joiner is blue; when it narrows its selection, both copies' bars show it.
        check("the host gave the joiner the colour blue", re.search(r"host: .*'s colour is blue", host) is not None)
        check("the joiner's bot narrowed its selection", "bot: the joiner selects only its 2nd and 3rd characters" in join)
        if "bot: the joiner selects only its 2nd and 3rd characters" in join:
            sq = re.search(r"dots: red ([\d,]+) \| blue ([\d,]+)", host)
            narrowed = [m.group(1) for m in re.finditer(r"dots: red [\d,]+ \| blue ([\d,]+)", host)]
            full = sq.group(1).split(",") if sq else []
            want = ",".join(full[1:3])
            check("the host's bar shows the joiner's new selection (blue on its 2nd and 3rd characters)",
                  bool(want) and want in narrowed, f"wanted blue {want}; host showed {narrowed}")
            check("the joiner's bar shows the same", f"| blue {want} " in join, want)
    elif scenario == "simul":
        mv_h, mv_j = re.search(r"bot: MOVE id=(\d+)", host), re.search(r"bot: MOVE id=(\d+)", join)
        check("both bots issued a move on the same frame", bool(mv_h and mv_j))
        for who, mv in (("host's", mv_h), ("joiner's", mv_j)):
            if mv:
                uid = int(mv.group(1))
                d = (travelled(s_host, uid), travelled(s_join, uid))
                check(f"{who} unit {uid} travelled > {MIN_MOVE} on both copies", min(d) > MIN_MOVE,
                      f"host {d[0]:.2f}, joiner {d[1]:.2f}")
    elif scenario == "attack":
        at = re.search(r"bot: ATTACK id=(\d+) target=(\d+) hp=(-?\d+)", host)
        check("the host bot ordered an attack", bool(at), at.group(0) if at else "no ATTACK line")
        if at:
            tid, hp0 = int(at.group(2)), int(at.group(3))
            last = shared[-1] if shared else None
            hp_end = s_host[last].get(tid, (0, 0, 0, None))[3] if last else None
            check(f"target {tid} lost HP (identical on both copies)", hp_end is not None and hp_end < hp0,
                  f"hp {hp0} -> {hp_end}")
    elif scenario == "dropin":
        check("host played alone before the joiner came (generation-1 snapshots, host only)",
              any(k[0] == 1 for k in s_host) and not any(k[0] == 1 for k in s_join))
        check("host noticed the joiner mid-session", re.search(r"host: \S+ joined \(id", host) is not None)
        check("host re-synced for the joiner", "joined mid-session): quick-save" in host)
        n = host.count("host: re-sync (")
        check("exactly one re-sync (no re-sync loop)", n == 1, f"{n} re-syncs")
        check("re-sync save served as ftcoop_resync.sav, not the player's QuickSave.sav",
              'loading save "user/save/ftcoop_resync.sav", serving it to joiners as generation 2' in host)
        quick_after = sha(host_quick) if os.path.exists(host_quick) else None
        check("host's own QuickSave.sav unchanged by the re-sync", quick_before == quick_after
              and ("restored the player's QuickSave.sav" in host or "removed the re-sync's" in host)
              and "QuickSave.sav - FAILED" not in host)
        check("notices shown: joiner arrival on the host, 'in the host's game' on the joiner",
              "notice: Co-op: Unnamed joined mid-session" in host and "notice: Co-op: in the host's game" in join
              and "notice: exception" not in host + join)
        check("host served the re-sync save as generation 2", "serving it to joiners as generation 2" in host)
        check("joiner waited for the host's lobby, then loaded generation 2",
              "host's co-op lobby is open" in join
              and re.search(r'co-op: loading (the host\'s save )?"user/save/ftcoop_received.sav"', join) is not None
              and "generation 2) as user/save/ftcoop_received.sav" in join)
        g2 = [k for k in shared if k[0] == 2]
        check("at least 4 shared generation-2 frames, all identical",
              len(g2) >= 4 and all(s_host[k] == s_join[k] for k in g2), f"{len(g2)} generation-2 frames")
    elif scenario == "talk":
        talker, other = (host, join) if actor == "host" else (join, host)
        check(f"the {actor} bot started the conversation and its copy saw its own talk command",
              "bot: TALK" in talker and "co-op: this player started a conversation" in talker)
        if actor == "join":
            check("host saw the joiner's talk command", re.search(r"host: player \d+ started a conversation", host) is not None)
        check(f"the conversation stayed open on the {actor}'s copy", "co-op: .?AVWSpeech@@ opened for this player" in talker)
        check("the other copy closed it", "AVWSpeech@@ opened for another player's conversation: closing it here" in other
              and "opened for this player" not in other)
        ch = re.search(r"bot: LOCAL CHANGE id=(\d+) hp (-?\d+)->(-?\d+)", talker)
        check(f"the {actor}'s copy changed its game while talking", bool(ch), ch.group(0) if ch else "no LOCAL CHANGE line")
        check("conversation ended, the talker brought the others up to date",
              "bot: ending the conversation" in talker
              and "co-op: private screens closed, bringing the other players up to date" in talker)
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
        qv = re.search(r'bot: QUEST VAR (\w+) \d+ "([^"]+)" "[^"]*" -> "([^"]+)" \(reads "([^"]*)"\)', talker)
        check(f"the {actor} changed a quest variable in the conversation", bool(qv) and qv.group(3) == qv.group(4),
              qv.group(0) if qv else "no QUEST VAR line")
        if qv:
            check("the other copy set the same quest variable from the delta",
                  f'co-op: delta: {qv.group(1)} variable "{qv.group(2)}" = "{qv.group(3)}"' in other)
        ap = re.search(r"co-op: delta applied: .* (\d+) conflicts", other)
        check("the other copy applied the delta without conflicts and freed the NPC",
              bool(ap) and ap.group(1) == "0" and re.search(r"co-op: NPC \d+ is free again \(delta\)", other) is not None,
              ap.group(0) if ap else "no delta applied line")
        if ch:
            uid, hp1 = int(ch.group(1)), int(ch.group(3))
            after = [k for k in shared if free_frame is not None and k[1] > free_frame + 1]
            hps = {s_host[k].get(uid, (0, 0, 0, None))[3] for k in after} | {s_join[k].get(uid, (0, 0, 0, None))[3] for k in after}
            check(f"after the conversation both copies carry the {actor}'s change (unit {uid} hp {hp1})",
                  len(after) >= 4 and hps == {hp1}, f"{len(after)} frames, hp seen {sorted(hps, key=str)}")
    elif scenario == "browse":
        tr, other = (host, join) if actor == "host" else (join, host)
        check(f"the {actor} talked to the merchant, its copy kept the conversation",
              "bot: TALK (the owner's talk command, to NPC 96" in tr and "co-op: .?AVWSpeech@@ opened for this player" in tr)
        check("the barter screen opened on the trader's copy", re.search(r"bot: BARTER screen up \(mode [67]\)", tr) is not None)
        check("the other player's talk to the busy merchant was refused on its copy",
              "bot: the other player tries to talk to the merchant too" in other
              and "co-op: refused this player's talk to NPC 96" in other)
        if SHOP_NO_PAUSE:  # the shop's stock was made on every copy: browsing changes nothing anyone needs
            sh = re.search(r"co-op: shop: the deal is (\d+) ops \((\d+) table entries\)", tr)
            check("the trader's copy found no deal to send, only the table's bookkeeping",
                  bool(sh) and int(sh.group(1)) - (1 if int(sh.group(2)) else 0) == 0, sh.group(0) if sh else "no deal line")
            check("the other copy made the shop's stock itself and closed it at the shop's close",
                  "stock made here without a window" in other and "co-op: shop closed" in other)
        else:
            ch = re.search(r"co-op: private screens changed: inventories (\w+), squad (\w+); delta: 0 new, 0 gone, 0 stack counts, 0 inventories, 0 HP", tr)
            check("the trader's copy found nothing changed but entity bookkeeping",
                  bool(ch) and ch.groups() == ("no", "no"), ch.group(0) if ch else "no diff line")
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
        sent = re.search(r"co-op: sending (\d+) co-op command\(s\): (.*)", tr)
        check("the trader sent the bookkeeping and the unlock as co-op commands", bool(sent) and "no reload" in sent.group(2),
              sent.group(0) if sent else "no send line")
        ap = re.search(r"co-op: delta applied: .* (\d+) conflicts", other)
        check("the other copy applied it without conflicts and freed the merchant",
              "co-op: NPC 96 is free again" in other
              and ((bool(ap) and ap.group(1) == "0") or "nothing changed" in (sent.group(2) if sent else "")),
              ap.group(0) if ap else "")
        later = [k for k in shared if k[0] == 1]
        check("at least 6 shared frames, all identical (positions, HP, inventories, squad, entity table)",
              len(later) >= 6, f"{len(later)} frames")
    elif scenario in ("recruit", "recruitnew"):
        # A recruit visit, no reload (recruit: dismiss two, take one back; recruitnew: someone never recruited).
        rec, other = (host, join) if actor == "host" else (join, host)
        check(f"the {actor} talked to the recruiter and its copy kept the conversation",
              "bot: TALK (the owner's talk command, to NPC 81" in rec and "co-op: .?AVWSpeech@@ opened for this player" in rec)
        check("the other copy closed it", "AVWSpeech@@ opened for another player's conversation: closing it here" in other)
        sq = {m.group(1): m.group(2) for m in
              re.finditer(r"bot: SQUAD (BEFORE|AFTER REMOVE|AFTER ADD|AFTER DONE|SYNCED) n=\d+ ids=(\S*)", rec)}
        pk = re.search(r"bot: RECRUIT choosing pool member (\d+)", rec)
        if scenario == "recruitnew":
            check("the recruit is someone new from the list, not a member just dismissed",
                  bool(pk) and pk.group(1) in sq.get("AFTER ADD", "").split(",")
                  and pk.group(1) not in sq.get("BEFORE", "").split(","),
                  f"picked {pk.group(1) if pk else None}, after add {sq.get('AFTER ADD')}")
        else:
            check("the recruit screen dismissed and added squad members",
                  bool(sq.get("AFTER REMOVE")) and bool(sq.get("AFTER ADD"))
                  and len(sq["AFTER REMOVE"].split(",")) < len(sq.get("BEFORE", "").split(","))
                  and sq.get("SYNCED") != sq.get("BEFORE"), f"before {sq.get('BEFORE')}, synced {sq.get('SYNCED')}")
        # Every copy makes the recruit screen's candidates on the same frame (every copy is held while it is up): the
        # recruiter's copy with its window, the others without one. The changes then go out as a delta: no reload.
        made_r = re.search(r"recruit: the screen's constructor: (\d+) made([^;]*);", rec)
        made_o = re.search(r"recruit: another player's recruit screen, made here: (\d+) made([^;]*);", other)
        check("the other copy made the recruit screen's candidates too, the same entities in the same slots",
              bool(made_r) and bool(made_o) and made_r.groups() == made_o.groups() and int(made_r.group(1)) > 0,
              f"{made_r.group(1) if made_r else None} on the recruiter's copy, {made_o.group(1) if made_o else None} on the other")
        check("the other copy closed its screen before applying the delta",
              "co-op: the other player's recruit screen closed here too" in other)
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join and "full re-sync" not in rec)
        ap = re.search(r"co-op: delta applied: .* (\d+) conflicts", other)
        check("the other copy applied the delta without conflicts", bool(ap) and ap.group(1) == "0",
              ap.group(0) if ap else "no delta applied line")
        after = [k for k in shared if free_frame is not None and k[1] > free_frame + 1]
        check("at least 4 shared frames after the visit, all identical", len(after) >= 4, f"{len(after)} frames")
        seen = {q_host.get(k) for k in after} | {q_join.get(k) for k in after}
        check("after the visit both copies have the new squad", bool(after) and seen == {sq.get("SYNCED")},
              f"want {sq.get('SYNCED')}, seen {sorted(seen, key=str)}")
    elif scenario == "trade2":
        # Two visits to the same quartermaster: first the host trades, then the joiner (and it sells an item too).
        # Closing a shop rebuilds the campaign's stock list from what is left; the second shop is made from that list.
        for who, log in (("host", host), ("joiner", join)):
            check(f"the {who} opened the shop once for itself and once for the other player",
                  len(re.findall(r"co-op: shop of NPC 96 open for this player", log)) == 1
                  and len(re.findall(r"co-op: shop of NPC 96 open for another player", log)) == 1)
            ds = re.findall(r"bot: DEAL buyer \d+ caps (\d+) -> (\d+)", log)
            check(f"the {who}'s deal went through", len(ds) == 1 and int(ds[0][0]) > int(ds[0][1]), str(ds))
            ap = re.findall(r"co-op: delta applied: .* (\d+) conflicts", log)
            check(f"the {who} applied the other player's deal without conflicts", len(ap) >= 1 and all(c == "0" for c in ap), str(ap))
        check("the second deal also sold one of the buyer's items", "bot: SELL the buyer's" in join)
        so = [re.search(r"stock made here without a window \((\d+) items\)", l) for l in (host, join)]
        check("both shops were made on the other copy with stock", all(so) and all(int(m.group(1)) > 0 for m in so),
              str([m.group(1) if m else None for m in so]))
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
        check("nobody held the game", "co-op: holding the game" not in host + join)
        after_frames = [k for k in shared if free_frame is not None and k[1] > free_frame + 1]
        check("at least 4 shared frames after the second shop, all identical", len(after_frames) >= 4, f"{len(after_frames)} frames")
    elif scenario == "trade":
        trader, other = (host, join) if actor == "host" else (join, host)
        s1 = [k for k in shared if k[0] == 1]
        check("session 1 matched on both copies until the talk (moves of both players)", len(s1) >= 4 and talk_frame is not None,
              f"{len(s1)} session-1 frames before frame {talk_frame}")
        for who, log in (("non-trading side's", other), ("trader's", trader)):
            mv = re.search(r"bot: MOVE id=(\d+)", log)
            uid = int(mv.group(1)) if mv else None
            d = (travelled({k: v for k, v in s_host.items() if k in s1}, uid),
                 travelled({k: v for k, v in s_join.items() if k in s1}, uid)) if mv else (0, 0)
            check(f"the {who} squad move: unit {uid} travelled > {MIN_MOVE} on both copies", min(d) > MIN_MOVE,
                  f"host {d[0]:.2f}, joiner {d[1]:.2f}")
        check(f"the {actor} talked to the merchant and its copy saw its own talk command",
              "bot: TALK (the owner's talk command, to NPC 96" in trader and "co-op: this player started a conversation" in trader)
        if actor == "join":
            check("host saw the joiner's talk command", re.search(r"host: player \d+ started a conversation", host) is not None)
        check(f"the conversation stayed open on the {actor}'s copy", "co-op: .?AVWSpeech@@ opened for this player" in trader)
        check("the other copy closed it", "AVWSpeech@@ opened for another player's conversation: closing it here" in other
              and "opened for this player" not in other)
        check("the conversation's Barter button opened the barter screen on the trader's copy only",
              re.search(r"bot: BARTER screen up \(mode [67]\)", trader) is not None and "BARTER" not in other
              and not re.search(r"ui: window \S*Barter", other))
        of = re.search(r'bot: OFFER the merchant\'s \d+ "[^"]*" \(value \d+\) for (\d+) of the buyer\'s \d+ caps: offered [1-9]\d* and [1-9]', trader)
        check("the trader put caps and a merchant's item on the barter table", bool(of), of.group(0) if of else "no OFFER line")
        dl = re.search(r"bot: DEAL buyer (\d+) caps (\d+) -> (\d+)", trader)
        paid = int(of.group(1)) if of else None
        check("the barter screen's Offer button made the deal: the buyer paid its caps",
              bool(dl) and paid is not None and int(dl.group(2)) - int(dl.group(3)) == paid, dl.group(0) if dl else "no DEAL line")
        tr = re.search(r"bot: DEAL buyer (\d+)", trader)
        after = dict((int(m.group(1)), m.group(2)) for m in re.finditer(r"bot: INV AFTER id=(\d+) items=(\S*)", trader))
        before = dict((int(m.group(1)), m.group(2)) for m in re.finditer(r"bot: INV BEFORE id=(\d+) items=(\S*)", trader))
        check("the trade changed both inventories", len(after) == 2 and all(after[i] != before.get(i) for i in after),
              f"before {before}, after {after}")
        # The merchant's shop stock exists only while the barter screen is up: compare with the trader's game when it
        # brought the others up to date, after the screens closed. The squad member must still have the trade then.
        synced = dict((int(m.group(1)), m.group(2)) for m in re.finditer(r"bot: INV SYNCED id=(\d+) items=(\S*)", trader))
        member = int(tr.group(1)) if tr else None
        check("the trader's squad member kept the trade after the barter screen closed",
              member in synced and synced[member] == after.get(member), f"synced {synced.get(member)}")
        check("the trader bought a kind of item it did not carry (a new stack)", "bot: buying a NEW item kind" in trader)
        nw = re.search(r"co-op: private screens changed: .*delta: (\d+) new", trader)
        check("the trader's delta carries the new entity",
              (bool(nw) and int(nw.group(1)) >= 1) or "co-op: shop: new stack" in trader, nw.group(0) if nw else "")
        if SHOP_NO_PAUSE:  # the shop paused nobody: every other copy made the stock itself and kept running
            so = re.search(r"co-op: shop of NPC 96 open for another player at frame (\d+): stock made here without a window \((\d+) items\)", other)
            sc = re.search(r"co-op: shop closed at frame (\d+) \(the others' deal applied here\)", other)
            check("the other copy made the shop's stock itself, without a window", bool(so) and int(so.group(2)) > 0,
                  so.group(0) if so else "no headless shop line")
            check("the other copy kept running while the shop was open (it closed frames later)",
                  bool(so and sc) and int(sc.group(1)) > int(so.group(1)) + 5,
                  f"open {so.group(1) if so else None}, close {sc.group(1) if sc else None}")
            check("nobody held the game for the shop", "co-op: holding the game" not in host + join
                  and "pause counter World+0x2db = 1 at the conversation" not in trader)
            check("the other copy could not order the trading character (refused, with a notice)",
                  "bot: tries to move the trading squad member" in other
                  and re.search(r"co-op: refused this player's order \w+ for squad member \d+: it is trading", other) is not None
                  and "notice: Co-op: that character is trading" in other)
            check("the other copy could not talk to the recruiter while the shop was open",
                  "co-op: refused this player's talk to recruiter 81: a player is trading" in other
                  and "notice: Co-op: a player is trading - recruit when they are done" in other)
            check("the shopper's own pauses were swallowed and balanced",
                  "co-op: swallowed the game's pause" in trader and "(balance 0)" in trader and "NOT ZERO" not in trader)
        qv = re.search(r'bot: QUEST VAR (\w+) \d+ "([^"]+)" "[^"]*" -> "([^"]+)" \(reads "([^"]*)"\)', trader)
        check("the trader changed a quest variable while bartering", bool(qv) and qv.group(3) == qv.group(4),
              qv.group(0) if qv else "no QUEST VAR line")
        if qv:
            check("the other copy set the same quest variable from the delta",
                  f'co-op: delta: {qv.group(1)} variable "{qv.group(2)}" = "{qv.group(3)}"' in other)
        check("barter closed, the trader brought the others up to date",
              "bot: leaving the barter screen" in trader
              and "co-op: private screens closed, bringing the other players up to date" in trader
              and "co-op: sending" in trader and "the delta, no reload" in trader)
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
        if FORCE_FILE_SERVER:
            check("the delta went through the host's file server (over the lockstep limit set for the test)",
                  "on the host's file server for the others to fetch" in trader
                  and re.search(r"co-op: fetched the delta from the host's file server: [1-9]\d* bytes", other) is not None)
        ap = re.search(r"co-op: delta applied: (\d+) created, (\d+) destroyed, (\d+) stack counts, (\d+) inventories, (\d+) HP, (\d+) conflicts(.*)", other)
        check("the other copy applied the delta without conflicts", bool(ap) and ap.group(6) == "0" and "MALFORMED" not in ap.group(7),
              ap.group(0) if ap else "no delta applied line")
        check("the other copy created the new entity", bool(ap) and int(ap.group(1)) >= 1, ap.group(0) if ap else "")
        after_frames = [k for k in shared if free_frame is not None and k[1] > free_frame + 1]
        check("at least 4 shared frames after the delta, all identical", len(after_frames) >= 4, f"{len(after_frames)} frames")
        for uid, want in sorted(synced.items()):
            seen = {i_host.get(k, {}).get(uid) for k in after_frames} | {i_join.get(k, {}).get(uid) for k in after_frames}
            check(f"after the delta both copies carry the trade: inventory of {uid}", bool(after_frames) and seen == {want},
                  f"want {want}, seen {sorted(seen, key=str)}")
    elif scenario == "exit":
        mover = host if actor == "host" else join
        check(f"the {actor} walked the whole squad onto the exit grid",
              len(re.findall(r"bot: SQUAD MOVE \d+ members", mover)) >= 1)
        check("the mission's own exit trigger took both copies to the world map",
              "ui: window .?AVWCampaignWindow@@ first ticks" in host and "ui: window .?AVWCampaignWindow@@ first ticks" in join)
        check("the joiner's world map choice was refused (only the host chooses)",
              "bot: the joiner tries to choose a location on the world map" in join
              and "co-op: refused this player's world map choice" in join)
        check("the host chose a location", "bot: WORLD MAP -> " in host)
        check("host's mission started alone and re-synced",
              "host: session without a load" in host and "host: re-sync (session started without a co-op lobby)" in host)
        check("joiner followed the host to generation 2 from the world map",
              "join: following the host to generation 2 from the world map" in join)
        g2 = [k for k in shared if k[0] == 2]
        check("at least 4 shared snapshot frames in generation 2, all identical", len(g2) >= 4, f"{len(g2)} frames")
    elif scenario == "transition":
        check("both bots ended the mission (ActionEndMission -> World Map)",
              'bot: END MISSION -> "World Map"' in host and 'bot: END MISSION -> "World Map"' in join)
        check("host bot started a mission from the world map", "bot: WORLD MAP -> " in host)
        check("host's mission started alone and re-synced",
              "host: session without a load" in host and "host: re-sync (session started without a co-op lobby)" in host)
        check("host served the re-sync save as generation 2", "serving it to joiners as generation 2" in host)
        check("joiner followed the host to generation 2 from the world map",
              "join: following the host to generation 2 from the world map" in join)
        g2 = [k for k in shared if k[0] == 2]
        check("at least 4 shared snapshot frames in generation 2, all identical",
              len(g2) >= 4 and all(s_host[k] == s_join[k] for k in g2), f"{len(g2)} generation-2 frames")
    elif scenario == "resync":
        check("host re-synced: quick-save then quick-load",
              "host: re-sync (bot: mid-mission re-sync): quick-save" in host and "quick-save written" in host)
        check("host served the re-sync save as generation 2", "serving it to joiners as generation 2" in host)
        check("joiner followed the host to generation 2 from the mission",
              "join: following the host to generation 2 from the mission" in join)
        check("joiner rejoined the host's session 2", join.count("join: connected to the host's session") >= 2,
              f"{join.count('join: connected to the host')} connects")
        s2 = [k for k in shared if k[0] == 2]
        check("at least 4 shared snapshot frames in session 2, all identical",
              len(s2) >= 4 and all(s_host[k] == s_join[k] for k in s2), f"{len(s2)} session-2 frames")
    elif scenario == "quest":
        talker, other = (host, join) if actor == "host" else (join, host)
        check(f"the {actor} talked to the quest giver and got the briefing (the Pipboy) on its copy",
              "to NPC 266" in talker and "co-op: .?AVWPipboy@@ opened for this player" in talker)
        check("the other copy closed its briefing and told its player",
              "co-op: .?AVWPipboy@@ opened for another player's conversation: closing it here" in other
              and "notice: Co-op: another player got a mission briefing" in other)
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
        qv = re.search(r'bot: QUEST VAR (\w+) \d+ "([^"]+)" "[^"]*" -> "([^"]+)"', talker)
        check("the other copy set the conversation's quest variable from the delta",
              bool(qv) and f'co-op: delta: {qv.group(1)} variable "{qv.group(2)}" = "{qv.group(3)}"' in other,
              qv.group(0) if qv else "no QUEST VAR line")
        ap = re.search(r"co-op: delta applied: .* (\d+) conflicts", other)
        check("the other copy applied it without conflicts", bool(ap) and ap.group(1) == "0", ap.group(0) if ap else "")
    elif scenario == "shoptalk":
        check("the host opened its shop", "co-op: shop of NPC 96 open for this player" in host)
        check("the joiner's talk to the quest giver during the shop was refused, with a notice",
              "bot: the joiner talks to the quest giver while the host shops" in join
              and "co-op: refused this player's talk to NPC 266: a player is trading" in join
              and "notice: Co-op: a player is trading - talk to them when they are done" in join)
        check("after the shop the joiner talked to the quest giver and got the briefing",
              "bot: the shop is done: the joiner talks to the quest giver again" in join
              and "co-op: .?AVWPipboy@@ opened for this player" in join)
        aps = [m.group(1) for log in (host, join) for m in re.finditer(r"co-op: delta applied: .* (\d+) conflicts", log)]
        check("both changes (the deal, the conversation) applied on the other copy without conflicts",
              len(aps) >= 2 and set(aps) == {"0"}, aps)
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
    elif scenario == "levelup" and actor == "all":
        won = [who for who, log in (("host", host), ("joiner", join)) if re.search(r"co-op: character screen of \d+ is ours", log)]
        lost = [who for who, log in (("host", host), ("joiner", join)) if "another player's came first" in log]
        check("both players opened the same character's screen at once; exactly one got it", len(won) == 1 and len(lost) == 1,
              f"won {won}, lost {lost}")
        if lost:
            loser = host if lost[0] == "host" else join
            check("the other was told and its screen closed",
                  "notice: Co-op: another player is levelling up that character" in loser and "ending mission-screen mode 3" in loser)
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
    elif scenario == "levelup":
        lv, other = (host, join) if actor == "host" else (join, host)
        # A joiner levelling up leaves the host playing: the host must be refused that character's screen and the
        # recruiter. The host levelling up holds every game (records flow only through the host's ticks), so a joiner
        # can neither learn of the lock nor act until it is over; its same-character screen is the race (levelrace).
        if actor == "join":
            check("while it levelled up, the other player's screen of that character closed again, with a notice",
                  "bot: the other player opens the same character's screen while it levels up" in other
                  and "co-op: refused this player's character screen of" in other
                  and "notice: Co-op: another player is levelling up that character" in other)
            check("and its talk to the recruiter was refused, with a notice",
                  "co-op: refused this player's talk to recruiter" in other
                  and "notice: Co-op: a player is levelling up - recruit when they are done" in other)
        check(f"the {actor} opened the character screen for a level-up and chose a perk",
              "co-op: .?AVWCharacter@@ opened for this player" in lv and "perk dialog: pressing Done" in lv)
        runs = re.findall(r"co-op: delta: character \d+'s attributes", lv)
        ap = re.search(r"co-op: delta applied: .* (\d+) conflicts, \d+ variables, (\d+) attribute runs", other)
        check("the level-up's attribute changes went to the other copy and applied without conflicts",
              bool(runs) and bool(ap) and ap.group(1) == "0" and int(ap.group(2)) == len(runs),
              f"{len(runs)} runs sent; {ap.group(0) if ap else 'no delta applied line'}")
        check("no reload: no re-sync, no upload, no new generation",
              "host: re-sync (" not in host and "join: quick-save for the host" not in join
              and "join: following the host to generation 2" not in join)
    elif scenario == "joinload":
        check("the joiner pressed quick-load and it was refused, with a notice",
              "bot: the joiner presses quick-load" in join
              and re.search(r'join: refused this player.s load of "[^"]+": only the host loads', join) is not None
              and "notice: Co-op: only the host can load a game" in join)
        check("no reload anywhere: no re-sync, no new generation, the joiner stayed in the session",
              "host: re-sync (" not in host and "join: following the host to generation 2" not in join
              and "game session returned to the main menu" not in join)
    elif scenario == "drift":
        check("the joiner's bot moved an NPC on its copy only", talk_frame is not None)
        dd = re.findall(r"co-op: drift: .*'s game differs from ours at frame (\d+)", host)
        check("the host saw the joiner's game differ, from the next checked frame",
              bool(dd) and talk_frame is not None and int(dd[0]) - talk_frame <= 2 * 25, dd[:3])
        check("the host sent its positions instead of reloading", "co-op: drift: sent every actor's position and HP" in host
              and "host: re-sync (" not in host and "join: following the host to generation 2" not in join)
        fx = re.search(r"co-op: drift correction applied here too at frame (\d+)", host)
        ap = re.search(r"co-op: a conversation's changes applied at frame (\d+) \(theirs\)", join)
        check("both copies took the correction at the same frame", bool(fx) and bool(ap) and fx.group(1) == ap.group(1),
              f"host {fx.group(1) if fx else None}, joiner {ap.group(1) if ap else None}")
        after = [k for k in shared if fx and k[1] > int(fx.group(1))]
        check("at least 4 shared frames after it, all identical", len(after) >= 4
              and all(s_host[k] == s_join[k] for k in after), f"{len(after)} frames")
        check("no drift after the correction", not re.search(r"drift: .*differs from ours at frame (\d+)", host)
              or all(int(m) <= int(fx.group(1)) for m in re.findall(r"differs from ours at frame (\d+)", host)) if fx else False)
    elif scenario == "soak":
        n_h, n_j = len(re.findall(r"bot: MOVE id=", host)), len(re.findall(r"bot: MOVE id=", join))
        expected = max(1, (args.soak_frames - 30) // 240)
        check(f"both bots issued at least {expected} moves each", n_h >= expected and n_j >= expected,
              f"host {n_h}, joiner {n_j}")
        # An idle soak passes frame equality trivially: require real walking (summed over snapshots).
        movers = {int(x) for x in re.findall(r"bot: MOVE id=(\d+)", host + join)}
        frames = sorted(s_host)
        walked = 0.0
        for uid in movers:
            path = [s_host[f][uid][:3] for f in frames if uid in s_host[f]]
            walked += sum(math.dist(a, b) for a, b in zip(path, path[1:]))
        need = 8.0 * (n_h + n_j) * 0.5
        check(f"squad walked at least {need:.0f} units in total", walked >= need, f"{walked:.1f} units")

    if fetch_test:
        check("host served its save to the joiner", re.search(r'host: sent "user/save/New Save Game.sav" \(\d+ bytes', host) is not None)
        rx = re.search(r"join: received host's save .* \((\d+) bytes", join)
        size_ok = bool(rx) and int(rx.group(1)) == os.path.getsize(host_file)
        check("joiner received the host's save, full size", size_ok, rx.group(0) if rx else "no receive line")
        check("received file is byte-identical to the host's save", os.path.exists(received) and sha(received) == sha(host_file))
        check("joiner loaded the received file instead of its own save",
              'co-op: loading "user/save/ftcoop_received.sav"' in join)
    # Extra joiners: in the game, nothing crashed, and every actor matches the host at every shared frame.
    for n, (d, log) in enumerate(zip(extras, extra_logs), start=2):
        who = f"joiner {n}"
        if scenario == "simul":  # each joiner ordered its own squad member (member <index>) on the same frame
            mv = re.search(r"bot: MOVE id=(\d+)", log)
            uids = {int(x.group(1)) for x in re.finditer(r"bot: MOVE id=(\d+)", host + join)}
            uid = int(mv.group(1)) if mv else None
            dist = travelled(s_host, uid) if uid is not None else 0.0
            check(f"{who} ordered its own unit and it travelled > {MIN_MOVE} on the host",
                  uid is not None and uid not in uids and dist > MIN_MOVE, f"unit {uid}, {dist:.2f}")
        check(f"{who} exited cleanly (code 0)", codes[n] == 0, f"code {codes[n]}")
        check(f"{who} became a co-op joiner and connected", f"co-op: joining {HOST_IP}" in log
              and "join: connected to the host's session" in log)
        s_x = snapshots(log)
        sh = sorted(set(s_host) & set(s_x))
        if scenario in ("trade", "trade2", "recruit", "recruitnew", "browse"):  # the same frames as for the first joiner
            sh = [k for k in sh if k in set(shared)]
        bad = [f for f in sh if s_host[f] != s_x[f]]
        check(f"{who}: at least {min_frames} shared snapshot frames, every actor identical to the host",
              len(sh) >= min_frames and not bad, f"{len(sh)} shared, {len(bad)} differ")
        i_x, q_x, e_x, v_x = inventories(log), squads(log), tables(log, "ETAB"), tables(log, "VARS")
        for what, a, b in (("inventories", i_host, i_x), ("squad", q_host, q_x), ("entity table", e_host, e_x),
                           ("quest variables", v_host, v_x)):
            bad = [f for f in sh if a.get(f) != b.get(f)]
            check(f"{who}: same {what} as the host at every shared frame", bool(sh) and not bad,
                  f"first mismatch {bad[0]}" if bad else f"{len(sh)} frames")
        if scenario in ("trade", "recruit", "recruitnew"):
            ap = re.search(r"co-op: delta applied: .* (\d+) conflicts(.*)", log)
            check(f"{who}: applied the delta without conflicts", bool(ap) and ap.group(1) == "0" and "MALFORMED" not in ap.group(2),
                  ap.group(0) if ap else "no delta applied line")
        check(f"{who}: no out-of-sync report or crash",
              "type=207(" not in log and not re.search(r"EXCEPTION 0xc0000005|bot: FAIL|bot: TIMEOUT", log))
    if extras:
        m = re.search(r"bot: (\d+) players ready", host)
        check(f"host started with {1 + args.joiners} ready players", bool(m) and int(m.group(1)) >= 1 + args.joiners,
              m.group(0) if m else "no start line")
    # Random state (World+0x729) every frame: the first frame a copy differs from the host is where it diverged.
    def rng(log):
        return {(int(m.group(1)), int(m.group(2))): m.group(3)
                for m in re.finditer(r"RNG s=(\d+) f=(\d+) v=([0-9a-f]+)", log)}
    r_host = rng(host)
    for who, log in [("joiner", join)] + [(f"joiner {n}", l) for n, l in enumerate(extra_logs, start=2)]:
        r_x = rng(log)
        common = sorted(k for k in set(r_host) & set(r_x) if scenario not in ("talk", "quest", "shoptalk", "levelup", "trade", "trade2", "recruit", "recruitnew", "browse", "drift") or k[0] >= 2
                        or (talk_frame is not None and k[1] < talk_frame)
                        or (free_frame is not None and k[1] > free_frame + 1))
        diff = [k for k in common if r_host[k] != r_x[k]]
        # A real divergence persists (the random sequence never re-converges). A one-frame blip, seen on the frame the
        # host quick-saves for a re-sync while the joiner still runs the old session, is reported, not failed.
        nxt = {common[i]: common[i + 1] for i in range(len(common) - 1)}
        lasting = [k for k in diff if k in nxt and nxt[k] in diff and nxt[k][0] == k[0]]
        blips = [k for k in diff if k not in lasting]
        check(f"{who}: random state matches the host's (no lasting divergence)", bool(common) and not lasting,
              f"{len(common)} frames" + (f", diverges from (gen, frame) {lasting[0]}" if lasting else "")
              + (f", one-frame blips at {blips[:3]}" if blips else ""))
    for name, log in (("host", host), ("joiner", join)):
        check(f"{name}: no out-of-sync report (msg 0xcf)", "type=207(" not in log)
        check(f"{name}: no crash or bot failure", not re.search(r"EXCEPTION 0xc0000005|bot: FAIL|bot: TIMEOUT", log))
    if extra_logs:  # selection dots with 3-4 players: a colour each, and every copy shows every player
        colours = ["red", "blue", "yellow", "green"][:2 + len(extra_logs)]
        given = re.findall(r"host: .*'s colour is (\w+)", host)
        check(f"the host gave the joiners {', '.join(colours[1:])}, one each", sorted(given) == sorted(colours[1:]), given)
        for who, log in [("host", host), ("joiner", join)] + [(f"joiner {n}", l) for n, l in enumerate(extra_logs, start=2)]:
            shows = [re.findall(r"(?:^|\| )(\w+) [\d,]+", m.group(1)) for m in re.finditer(r"dots: (.*)", log)]
            check(f"{who}'s squad bar showed all {len(colours)} players' dots", colours in shows,
                  shows[-1] if shows else "no dots line")
    return report(args, checks, (host, join, *extra_logs))


def report(args, checks, logs):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = args.test + ("-mutated" if args.mutate_share_squad or getattr(args, "mutate_fetch", False)
                         or getattr(args, "mutate_follow", False) else "")
    if getattr(args, "joiners", 1) > 1:
        name += f"-{1 + args.joiners}p"
    out = os.path.join(RUNS, f"{stamp}_{name}")
    os.makedirs(out, exist_ok=True)
    if logs:
        names = ["host_netlog.txt", "joiner_netlog.txt"] + [f"joiner{n}_netlog.txt" for n in range(2, len(logs))]
        for fname, text in zip(names, logs):
            with open(os.path.join(out, fname), "w", encoding="latin1") as f:
                f.write(text)
    lines = [f"{'PASS' if ok else 'FAIL'}  {n}" + (f"  [{d}]" if d else "") for n, ok, d in checks]
    result = "PASS" if checks and all(ok for _, ok, _ in checks) else "FAIL"
    lines.append(f"RESULT: {result}  ({name}, {sum(ok for _, ok, _ in checks)}/{len(checks)} checks)")
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"logs: {out}")
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
