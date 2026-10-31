"""Fuzz the mod's sync decisions on a fake world, no game: test/fuzz/ftcoop_target.cpp on the hub's fuzzer.

The fuzzer is coop-anything's tools/fuzz (https://github.com/cranebeaker/coop-anything, MIT). Point COOPFUZZ_DIR at
its folder (default: the hub this checkout sits in, ../../../../../tools/fuzz from here). It needs cmake and the
Visual Studio 2022 Build Tools.

It triages by itself: failures are grouped by what broke and why (the violation, and what the target's event trace
says was going on), with one shrunk event trace per group; a mutation twin counts as caught only when the invariant
it is meant to break does.

Usage:
  python -I test/fuzz/run_fuzz.py                      # 300 sessions, 2 to 4 players, then every mutation twin
  python -I test/fuzz/run_fuzz.py --count 2000 --profile bad_wifi
  python -I test/fuzz/run_fuzz.py --bug no_stock_sync  # one twin
Arguments it doesn't know go to the fuzzer as they are.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FUZZ = os.path.abspath(os.environ.get("COOPFUZZ_DIR") or os.path.join(HERE, "..", "..", "..", "..", "..", "tools", "fuzz"))
BUILD = os.path.join(HERE, "build")
OUT = os.path.join(HERE, "out")
TARGET = os.path.join(HERE, "ftcoop_target.cpp")
# Each twin is a bug of 0.10.5 / 0.10.6 switched back on, and the invariant that must catch it.
TWINS = {
    "no_shop_skip": "needless_repair",        # checks during a shop counted: a reload after the shop
    "no_drop_before_fix": "needless_repair",  # a second joiner's old report reloads after the correction mended it
    "no_serial_sync": "apart_after_load",     # copies apart from the first frame of every session
    "no_stock_sync": "apart_after_load",      # a joiner's own (empty) stock after a session's start: her night
    "no_stock_check": "shop_stock_apart",     # a stock apart mid-session is never mended until the next session
    "no_resync_gate": "saved_during_shop",    # a re-sync's save in the middle of a shop
    "no_abandoned_close": "stuck",            # a shopper that left: its shop open everywhere, a reload waits for it
    "no_host_filter": "two_talkers",          # two players clicking one NPC at once both get the conversation
    "no_lock_clear": "lock_leak",             # 0.10.3: an NPC stays locked after a reload
    "no_solo_release": "stuck",               # 0.10.5: a host on its own can't move after its conversation
    "no_abandoned_talk": "waits_for_gone",    # a talker that left: its NPC locked and everyone waiting
    "save_alone": "apart_after_save",         # 0.10.2: a save written on the saver's copy only
    "no_abandoned_save": "waits_for_gone",    # a saver that left mid-save: everyone holds for 10 minutes
    "no_early_save_done": "save_hold_stuck",  # a done before a lagging copy's hold frame was dropped: held 10 minutes
    "no_gone_saver_at_hold": "waits_for_gone",  # a saver gone before its hold acted: nobody ends the hold
    "no_cancel_mark": "waits_for_gone",       # a talk relayed before its talker left reaches its frame afterwards
    "no_level_race": "two_levellers",         # two players open one character's level screen: the second wins too
    "no_level_clear": "level_lock_leak",      # a session start keeps the character locks
    "no_headless_recruit": "recruit_apart",   # the other copies do not make the recruit screen's entities
    "joiner_travels": "two_destinations",     # a joiner's world map travel click is not refused
    "joiner_rolls": "map_apart",              # a joiner rolls its own encounter: another map than the host's
    "no_abandoned_level": "level_lock_leak",  # a leveller that left keeps its character lock on every copy
    "no_recruit_drop_on_load": "stale_screen",  # a load leaves the headless recruit screen up: freed in the next world
    "any_done_closes_recruit": "recruit_apart",  # another conversation's done closes a visit's recruit screen early
    "no_forget_gone": "live_talk_cancelled",  # a joiner back with the same id stays in the gone set: its talk is ended at once
    "no_rejoin_as_leave": "rejoin_before_notice",  # a joiner back inside 15 s: the host keeps what it held for the old self
}
# Fixtures: one exact situation each, written out and replayed over 20 seeds (the seed decides latencies and the joiners'
# poll clocks). Clean, every seed must pass; with its twin, at least one seed must break the twin's invariant. Random
# sessions are too noisy for some races (a shop's deal or a load mends a fault before the race can happen).
def quarter(t):
    """Joins, leaves, loads, shops and most faults pass the target's thinning only at some times (Quarter())."""
    while ((t * 2654435761) & 0xffffffff) >> 30 != 0:
        t += 1
    return t


FIXTURES = {
    # the host shops for 7 s: the checks in the middle of it differ from the joiners' (its purchase is its alone until the deal)
    "host_shop_checks": ("no_shop_skip", 3, 15000, lambda: [
        {"t": quarter(1000), "op": "activate", "m": 0, "a": 0, "b": 6}]),
    # the owner's night: every joiner apart from the same frame; the second joiner's report reaches the host after it
    # asked for the correction the first one's started
    "two_joiners_same_drift": ("no_drop_before_fix", 3, 9000, lambda: [
        {"t": 600, "op": "spawn", "m": 1, "a": 4}]),
    # two players click the same story NPC at the same moment: one gets the conversation, the other is told
    "two_click_one_npc": ("no_host_filter", 3, 9000, lambda: [
        {"t": quarter(800), "op": "hit", "m": 1, "a": 0, "b": 5},
        {"t": quarter(800), "op": "hit", "m": 2, "a": 0, "b": 5}]),
    # the owner's wife's night: a joiner comes back without the quartermaster's stock, then the host shops
    "joiner_without_stock": ("no_stock_sync", 3, 12000, lambda: [
        {"t": quarter(300), "op": "leave", "m": 2},
        {"t": quarter(1500), "op": "join", "m": 2, "a": 1},
        {"t": quarter(7000), "op": "activate", "m": 0, "a": 0, "b": 4}]),
    # a joiner's stock goes wrong in the middle of a session, and the host shops 12 s later: the drift check mends it
    "stock_lost_mid_session": ("no_stock_check", 3, 18000, lambda: [
        {"t": quarter(1000), "op": "spawn", "m": 2, "a": 1},
        {"t": quarter(13000), "op": "activate", "m": 0, "a": 0, "b": 4}]),
    # a joiner talks to a story NPC (everyone waits) and leaves in the middle of it
    "talker_leaves": ("no_abandoned_talk", 3, 30000, lambda: [
        {"t": quarter(500), "op": "hit", "m": 1, "a": 0, "b": 12},
        {"t": quarter(2500), "op": "leave", "m": 1}]),
    # the host opens the save screen: every copy holds at one frame and writes the same save
    "save_in_step": ("save_alone", 3, 12000, lambda: [
        {"t": quarter(800), "op": "save", "m": 0, "a": 1, "b": 3}]),
    # a joiner opens the save screen and leaves in the middle of it: the host ends the save for everyone, nothing saved
    "saver_leaves": ("no_abandoned_save", 3, 30000, lambda: [
        {"t": quarter(500), "op": "save", "m": 1, "a": 1, "b": 9},
        {"t": quarter(2500), "op": "leave", "m": 1}]),
    # a slow link, the host quick-saves: its done can reach a copy before that copy's hold frame
    "save_done_early": ("no_early_save_done", 3, 45000, lambda: [
        {"t": quarter(800), "op": "save", "m": 0, "a": 0, "b": 0}], {"maxLatencyMs": 2500}),
    # a joiner talks to a story NPC and leaves a moment later: the talk reaches its frame after the talker is gone. The host
    # notices a leave 15 s late, so its frame has to wait that long: another joiner's 15 s conversation holds the host
    "talk_then_leave": ("no_cancel_mark", 3, 60000, lambda: [
        {"t": quarter(300), "op": "hit", "m": 2, "a": 0, "b": 14},
        {"t": quarter(quarter(300) + 200), "op": "hit", "m": 1, "a": 2, "b": 5},
        {"t": quarter(quarter(300) + 400), "op": "leave", "m": 1}], {"minLatencyMs": 100, "maxLatencyMs": 100}),
    # a joiner asks to save and leaves before the hold acts: the host is held by another joiner's 15 s conversation, so the
    # hold's frame comes after the host has noticed the leave (15 s)
    "saver_leaves_before_hold": ("no_gone_saver_at_hold", 3, 45000, lambda: [
        {"t": quarter(300), "op": "hit", "m": 2, "a": 0, "b": 14},
        {"t": quarter(quarter(300) + 200), "op": "save", "m": 1, "a": 1, "b": 3},
        {"t": quarter(quarter(300) + 400), "op": "leave", "m": 1}], {"minLatencyMs": 100, "maxLatencyMs": 100}),
    # a joiner opens a shop for 10 s and leaves in the middle of it
    "shopper_leaves": ("no_abandoned_close", 3, 25000, lambda: [
        {"t": quarter(500), "op": "activate", "m": 1, "a": 0, "b": 9},
        {"t": quarter(2500), "op": "leave", "m": 1}]),
    # two joiners open one character's level screen at the same moment: one gets it, the other's screen closes
    "level_race": ("no_level_race", 3, 15000, lambda: [
        {"t": quarter(800), "op": "drop", "m": 1, "a": 0, "b": 1, "c": 5},
        {"t": quarter(800), "op": "drop", "m": 2, "a": 0, "b": 1, "c": 5}]),
    # a joiner levels up for 8 s and the host quick-loads in the middle: a session start clears the character locks
    "level_then_load": ("no_level_clear", 3, 30000, lambda: [
        {"t": quarter(500), "op": "drop", "m": 1, "a": 2, "b": 1, "c": 7},
        {"t": quarter(3000), "op": "load", "m": 0}]),
    # a joiner recruits (browses) for 8 s: the other copies make the screen's candidates and free them at its close
    "recruit_visit": ("no_headless_recruit", 3, 20000, lambda: [
        {"t": quarter(800), "op": "hit", "m": 1, "a": 3, "b": 7}]),
    # a joiner's world map travel click and choice are refused; the host travels and the joiners follow it
    "joiner_clicks_map": ("joiner_travels", 3, 25000, lambda: [
        {"t": quarter(500), "op": "move", "m": 0, "c": 1},
        {"t": 1500, "op": "move", "m": 0, "c": 2, "a": 900, "b": 900},
        {"t": 3000, "op": "move", "m": 1, "c": 2, "a": 100, "b": 100},
        {"t": 3500, "op": "move", "m": 2, "c": 1}]),
    # a long trip on the world map: the host rolls its encounters, the joiners' hooks refuse theirs
    "joiner_rolls_encounter": ("joiner_rolls", 3, 25000, lambda: [
        {"t": quarter(500), "op": "move", "m": 0, "c": 1},
        {"t": 1500, "op": "move", "m": 0, "c": 2, "a": 1000, "b": 0}]),
    # a joiner levels up and leaves: the host closes its character screen for everyone when it notices (15 s)
    "leveller_leaves": ("no_abandoned_level", 3, 35000, lambda: [
        {"t": quarter(500), "op": "drop", "m": 1, "a": 0, "b": 1, "c": 9},
        {"t": quarter(2500), "op": "leave", "m": 1},
        {"t": quarter(26000), "op": "hit", "m": 0, "a": 3, "b": 5}]),
    # a joiner recruits (browse, 8 s) and the host quick-loads in the middle: the load closes the screens made for it (else the next visit's done frees them in the new world)
    "reload_during_recruit": ("no_recruit_drop_on_load", 3, 20000, lambda: [
        {"t": quarter(500), "op": "hit", "m": 1, "a": 3, "b": 7},
        {"t": quarter(3500), "op": "load", "m": 0},
        {"t": quarter(9000), "op": "hit", "m": 0, "a": 3, "b": 1}]),
    # a joiner's talk to a trader is passed by the host, then the joiner leaves; the host recruits for 15 s, and the gone
    # talker's done comes (at the notice) in the middle of the visit: it must not close the visit's screen
    "done_during_recruit": ("any_done_closes_recruit", 3, 25000, lambda: [
        {"t": quarter(100), "op": "hit", "m": 1, "a": 1, "b": 1},
        {"t": quarter(quarter(100) + 100), "op": "leave", "m": 1},
        {"t": quarter(300), "op": "hit", "m": 0, "a": 3, "b": 14}]),
    # a joiner opens a shop (or a conversation) and leaves, then comes back within the 15 s: the host takes it as a leave
    "shopper_rejoins": ("no_rejoin_as_leave", 3, 20000, lambda: [
        {"t": quarter(500), "op": "activate", "m": 1, "a": 0, "b": 9},
        {"t": quarter(2500), "op": "leave", "m": 1},
        {"t": quarter(5000), "op": "join", "m": 1, "a": 1}]),
    # a joiner leaves, comes back inside the delay, plays, then talks to a story NPC: its conversation opens and ends normally
    "rejoin_then_talk": ("no_forget_gone", 3, 30000, lambda: [
        {"t": quarter(500), "op": "leave", "m": 1},
        {"t": quarter(2500), "op": "join", "m": 1, "a": 1},
        {"t": quarter(14000), "op": "hit", "m": 1, "a": 0, "b": 3}]),
    "talker_rejoins": ("no_rejoin_as_leave", 3, 20000, lambda: [
        {"t": quarter(500), "op": "hit", "m": 1, "a": 0, "b": 12},
        {"t": quarter(2500), "op": "leave", "m": 1},
        {"t": quarter(5000), "op": "join", "m": 1, "a": 1}]),
}
def run_fixtures(exe, only=None):
    """Returns (failed, the twins some fixture caught)."""
    failed = False
    caught_by_fixture = set()
    folder = os.path.join(OUT, "fixtures")
    os.makedirs(folder, exist_ok=True)
    for name, (twin, machines, duration, ops, *extra) in FIXTURES.items():
        if only and twin not in only:
            continue
        results = {}
        for mutated in (False, True):
            broke = []
            for seed in range(1, 21):
                ops_ = [dict({"b": 0, "c": 0, "d": 0, "a": 0}, **o) for o in ops()]
                scen = {"format": "coopfuzz-1", "seed": seed, "machines": machines, "durationMs": duration,
                        "link": dict({"minLatencyMs": 5, "maxLatencyMs": 60, "loss": 0, "duplicate": 0, "corrupt": 0},
                                     **(extra[0] if extra else {})),
                        "target": "ftcoop", "mutations": [twin] if mutated else [], "ops": ops_}
                path = os.path.join(folder, f"{name}{'_' + twin if mutated else ''}_seed{seed}.json")
                with open(path, "w", encoding="utf-8") as f:
                    json.dump(scen, f, indent=1)
                r = subprocess.run([exe, "--replay", path, "--no-shrink"],
                                   capture_output=True, text=True, errors="replace")
                if r.returncode != 0:  # a replay prints its violations ("VIOLATION <invariant> <detail>")
                    inv = sorted({l.split()[1] for l in r.stdout.splitlines() if l.startswith("VIOLATION ")})
                    broke.append((seed, inv))
            results[mutated] = broke
        want = TWINS[twin]
        clean_ok = not results[False]
        caught = [s for s, inv in results[True] if want in inv]
        print(f"{'PASS' if clean_ok else 'FAIL'}  fixture {name}: clean on 20 seeds"
              + ("" if clean_ok else f" (broke: {results[False][:3]})"))
        print(f"{'PASS' if caught else 'FAIL'}  fixture {name} with {twin}: {want} on {len(caught)} of 20 seeds"
              + ("" if caught else f" (only {results[True][:3]})"))
        failed |= not clean_ok or not caught
        if caught:
            caught_by_fixture.add(twin)
    return failed, caught_by_fixture


VS_CMAKE = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"


def cmake():
    found = shutil.which("cmake")
    if found:
        return found
    exe = os.path.join(VS_CMAKE, "cmake.exe")
    return exe if os.path.exists(exe) else None


def build():
    exe = cmake()
    if not exe or not os.path.isdir(FUZZ):
        print(f"needs cmake and the hub's tools/fuzz (COOPFUZZ_DIR, now {FUZZ})")
        return None
    sources = TARGET.replace("\\", "/")
    if subprocess.run([exe, "-S", FUZZ, "-B", BUILD, f"-DCOOPFUZZ_TARGET_SOURCES={sources}"],
                      stdout=subprocess.DEVNULL).returncode != 0:
        return None
    r = subprocess.run([exe, "--build", BUILD, "--config", "Release", "--target", "coopfuzz"],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        print("\n".join(l for l in r.stdout.splitlines() if "error" in l)[:3000])
        return None
    path = os.path.join(BUILD, "Release", "coopfuzz.exe")
    return path if os.path.exists(path) else os.path.join(BUILD, "coopfuzz")


def fresh(folder):
    """A run's own folder, emptied of old reports (only report and scenario files it wrote)."""
    os.makedirs(folder, exist_ok=True)
    for f in glob.glob(os.path.join(folder, "fail_*.json")):
        os.remove(f)
    return folder


def reports(folder):
    """One report per failing seed: the shrunk one when there is one."""
    best = {}
    for path in glob.glob(os.path.join(folder, "fail_seed*.report.json")):
        m = re.search(r"fail_seed(\d+)_", os.path.basename(path))
        seed = int(m.group(1)) if m else -1
        if seed not in best or ".min." in path:
            best[seed] = path
    out = []
    for seed, path in sorted(best.items()):
        with open(path, encoding="utf-8") as f:
            out.append((seed, json.load(f)))
    return out


def cause(v):
    """What broke and why, without the numbers: the grouping key."""
    detail, _, trace = v["detail"].partition(" || trace")
    head = re.sub(r"\d+", "#", detail)[:120]
    events = [e.split("s ", 1)[-1] for e in trace.split(" | ") if e.strip()]
    hint = next((e for e in reversed(events) if any(w in e for w in ("leaves", "joins", "fault", "drift", "quick-loads"))), "")
    return f"{v['invariant']}: {head}" + (f" (last: {re.sub(r'[0-9]+', '#', hint)})" if hint else "")


def triage(runs):
    groups = {}
    for seed, rep in runs:
        for v in rep.get("violations", []):
            if v["invariant"] == "not_quiescent" and any(x["invariant"] == "stuck" for x in rep["violations"]):
                continue  # "stuck" says why it never went quiet
            groups.setdefault(cause(v), []).append((seed, v))
    for key, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        seeds = sorted({s for s, _ in items})
        print(f"  {len(seeds):3d} seed(s)  {key}")
        print(f"             seeds {', '.join(map(str, seeds[:8]))}{' ...' if len(seeds) > 8 else ''}")
        _, v = items[0]
        trace = v["detail"].partition(" || trace")[2]
        for e in [e for e in trace.split(" | ") if e.strip()][-16:]:
            print(f"               {e}")
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--bug", action="append", default=[])
    ap.add_argument("--no-twins", action="store_true", help="only the clean sessions")
    args, rest = ap.parse_known_args()
    exe = build()
    if not exe:
        return 2
    failed = False
    if not args.bug:
        folder = fresh(os.path.join(OUT, "clean"))
        r = subprocess.run([exe, "--target", "ftcoop", "--out", folder, "--count", str(args.count), "--max-fail", "20",
                            "--quiet"] + rest, capture_output=True, text=True, errors="replace")
        summary = next((l for l in r.stdout.splitlines() if l.startswith("coopfuzz:")), r.stdout.strip()[-200:])
        print(f"{'PASS' if r.returncode == 0 else 'FAIL'}  clean sessions  [{summary}]")
        if r.returncode != 0:
            failed = True
            triage(reports(folder))
    by_fixture = set()
    if not args.no_twins:
        fx_failed, by_fixture = run_fixtures(exe, args.bug or None)
        failed |= fx_failed
    twins = args.bug or ([] if args.no_twins else list(TWINS))
    for twin in twins:  # "a+b": several bug classes in one run (a probe with the twin it looks into)
        parts = twin.split("+")
        want = next((TWINS[p] for p in parts if p in TWINS), None) if len(parts) == 1 or not any(p.startswith("probe") for p in parts) else None
        folder = fresh(os.path.join(OUT, twin.replace("+", "_and_")))
        bugs = sum((["--bug", p] for p in parts), [])
        subprocess.run([exe, "--target", "ftcoop", "--out", folder, "--count", str(args.count), "--max-fail", "3", "--quiet"]
                       + bugs + rest, capture_output=True, text=True, errors="replace")
        runs = reports(folder)
        hits = [(s, v) for s, rep in runs for v in rep.get("violations", []) if v["invariant"] == want]
        caught = bool(hits)
        if not caught and twin in by_fixture:  # too rare for random sessions; its fixture caught it
            print(f"ok    twin {twin}: random sessions miss it, its fixture catches it")
            continue
        failed |= not caught
        if not want:  # a probe: show what it found
            print(f"probe {twin}:")
            triage(runs)
            continue
        if caught:
            s, v = hits[0]
            print(f"PASS  twin {twin} caught by {want} (seed {s}): {v['detail'].partition(' || ')[0][:110]}")
        else:
            other = sorted({v["invariant"] for _, rep in runs for v in rep.get("violations", [])})
            print(f"FAIL  twin {twin} NOT caught by {want}" + (f" (only by {', '.join(other)})" if other else ""))
    print(f"RESULT: {'FAIL' if failed else 'ALL OK'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
