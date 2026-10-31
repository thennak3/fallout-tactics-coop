"""Read netlog.txt files without reading them: the events that matter, in order, and the known bad patterns.

  python -I test/netlog_tool.py summary host.txt [joiner.txt ...]     # sessions, reloads, drifts, shops, talks, problems
  python -I test/netlog_tool.py timeline host.txt [--from 300 --to 420] [--only shop,drift] [joiner.txt ...]
  python -I test/netlog_tool.py problems host.txt [joiner.txt ...]    # only the flagged patterns, with where they are
  python -I test/netlog_tool.py selftest                              # every pattern on test/netlog_samples

Several logs (the host's and its joiners'): each line is labelled with its log's name, and the timeline merges them by
time (each log's clock starts at its own game's start, so lines line up by session: a session's start is the anchor).
Raw packet dumps, window ticks and per-frame bot lines are left out. Standard library only.
"""
import argparse
import os
import re
import sys

# kind, regex (on the text after the time stamp). The first match names the line's kind.
EVENTS = [
    ("session", r"^session \d+ started"),
    ("session", r"^notice: Co-op: (\d+ player\(s\) in this game|in the host's game)"),
    ("join", r"joined mid-session|^host: sent \".*\" .* to a joiner|^join: following the host"),
    ("leave", r"^notice: Co-op: .* left"),
    ("reload", r"^host: re-sync \(|^join: the host's session ended|waiting for the shop to close|saving anyway"),
    ("drift", r"^co-op: drift: (?!test:|.*matches ours)"),
    ("correction", r"correction|going on \("),
    ("shop", r"shop of NPC|gamble of NPC|^co-op: shop|Barter pressed|Gamble pressed|deal is|a player is trading"),
    ("talk", r"started a conversation|claim .* (sent|for NPC)|NPC \d+ (is|claimed)|conversation of|holding the game while"
             r"|private screens|free again|stops waiting|talking to someone|got to them first"),
    ("stock", r"quartermaster stock|stock made here|entity serial"),
    ("save", r"^co-op: save:"),
    ("refused", r"refused|^host: dropped player"),
    ("pause", r"swallowed the game's|NOT ZERO|pause changes go through"),
    ("error", r"exception|FAIL|crash|NOT ZERO"),
    ("notice", r"^notice:"),
]
SKIP = re.compile(r"^(S->C|C->S) |^ui: window |^\s*rec\d |^(SNAP|POSH|RNG|SQUAD|ETAB|FREE|ETABX|VARS|ATTR|STOCK|INV) |"
                  r"drift: test:|^tablestep:|^bot: (?!FAIL)")
LINE = re.compile(r"^\s*(\d+\.\d+) (.*)$")


class Event:
    def __init__(self, log, t, kind, text, lineno):
        self.log, self.t, self.kind, self.text, self.lineno = log, t, kind, text, lineno


def parse(path):
    name = os.path.basename(os.path.dirname(os.path.abspath(path))) + "/" + os.path.basename(path) \
        if os.path.basename(path) == "netlog.txt" else os.path.basename(path)
    events = []
    with open(path, encoding="latin1", errors="replace") as f:
        for n, raw in enumerate(f, 1):
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            t, text = float(m.group(1)), m.group(2)
            if SKIP.search(text):
                continue
            kind = next((k for k, rx in EVENTS if re.search(rx, text)), None)
            if kind:
                events.append(Event(name, t, kind, text, n))
    return events


def sessions(events):
    """[(start time, label)] of each session start in one log."""
    out = []
    for e in events:
        m = re.match(r"session (\d+) started", e.text)
        if m:
            out.append((e.t, int(m.group(1))))
    return out


# ---- the known bad patterns (what cost play nights before), each: (name, function(events by log) -> [(event, why)])
def p_drift_after_shop(logs):
    out = []
    for evs in logs.values():
        last_close = None
        for e in evs:
            if e.kind == "shop" and "shop closed at frame" in e.text:
                last_close = e
            elif e.kind == "reload" and "drifted out of step" in e.text and last_close and e.t - last_close.t < 5:
                out.append((e, f"a drift reload {e.t - last_close.t:.1f} s after a shop closed (line {last_close.lineno})"))
    return out


def p_empty_headless_shop(logs):
    out = []
    for evs in logs.values():
        for e in evs:
            m = re.search(r"(shop|gamble) of NPC (\d+) open for another player .* \(0 items\)", e.text)
            if m and m.group(2) == "96":  # the quartermaster always has stock
                out.append((e, "the quartermaster's shop made with no stock here: this copy's stock list is empty"))
    return out


def p_correction_gave_up(logs):
    return [(e, "every copy held for a correction that never came") for evs in logs.values() for e in evs
            if "waited too long for the correction" in e.text]


def p_double_report(logs):
    out = []
    for evs in logs.values():
        for i, e in enumerate(evs):
            if "asking every copy to hold for a correction" in e.text:
                for f in evs[i + 1:i + 8]:
                    if f.kind == "reload" and "drifted out of step" in f.text and f.t - e.t < 2:
                        out.append((f, f"a reload {f.t - e.t:.2f} s after a correction was asked for (a second report)"))
                        break
    return out


def p_lonely_session(logs):
    return [(e, "a session started without a co-op lobby (a reload follows)") for evs in logs.values() for e in evs
            if "session started without a co-op lobby" in e.text]


def p_left_mid(logs):
    out = []
    for evs in logs.values():
        open_shop = None
        for e in evs:
            if "open for another player" in e.text:
                open_shop = e
            elif "shop closed at frame" in e.text:
                open_shop = None
            elif e.kind == "leave" and open_shop:
                closed = any("closes for everyone" in f.text for f in evs if e.t <= f.t <= e.t + 30)
                if not closed:
                    out.append((e, f"a player left with a shop open (line {open_shop.lineno}) and nothing closed it"))
    return out


def p_refused(logs):
    return [(e, "an action refused") for evs in logs.values() for e in evs if e.kind == "refused"]


def p_pause_balance(logs):
    return [(e, "the game's pauses did not balance") for evs in logs.values() for e in evs if "NOT ZERO" in e.text]


def p_errors(logs):
    return [(e, "an error") for evs in logs.values() for e in evs if e.kind == "error" and "NOT ZERO" not in e.text]


def p_stock_apart(logs):
    """Every copy's stock hash at each session start should be the host's."""
    out = []
    by_session = {}
    for name, evs in logs.items():
        cur, pending = None, None
        for e in evs:
            m = re.match(r"session (\d+) started", e.text)
            if m:
                cur = int(m.group(1))
                if pending:  # logged just before its session's line
                    by_session.setdefault(cur, {})[name] = pending
                    pending = None
            m = re.search(r"quartermaster stock (is the host's|now the host's|at the session start): (\d+) kinds, (\d+) items, hash (\w+)", e.text)
            if m and m.group(1) == "at the session start":
                pending = (m.group(4), e)
            elif m and cur is not None:  # the host's list, put in a few frames into the session
                by_session.setdefault(cur, {})[name] = (m.group(4), e)
    for s, copies in sorted(by_session.items()):
        hashes = {h for h, _ in copies.values()}
        if len(hashes) > 1:
            e = next(iter(copies.values()))[1]
            out.append((e, f"session {s}: the copies' quartermaster stock differs ({', '.join(f'{n}={h}' for n, (h, _) in copies.items())})"))
    return out


PATTERNS = [
    ("drift reload after a shop", p_drift_after_shop),
    ("quartermaster shop with no stock", p_empty_headless_shop),
    ("stock apart between copies", p_stock_apart),
    ("correction that never came", p_correction_gave_up),
    ("reload right after a correction", p_double_report),
    ("session without a co-op lobby", p_lonely_session),
    ("player left mid-shop, never closed", p_left_mid),
    ("refused actions", p_refused),
    ("pause balance", p_pause_balance),
    ("errors", p_errors),
]


def load(paths):
    return {p: parse(p) for p in paths}


def label(evs_by_path):
    return {p: (evs[0].log if evs else os.path.basename(p)) for p, evs in evs_by_path.items()}


def cmd_summary(paths):
    logs = load(paths)
    for p, evs in logs.items():
        name = label(logs)[p]
        counts = {}
        for e in evs:
            counts[e.kind] = counts.get(e.kind, 0) + 1
        ss = sessions(evs)
        span = f"{evs[0].t:.0f}-{evs[-1].t:.0f} s" if evs else "empty"
        print(f"== {name}: {span}, {len(ss)} sessions")
        print("   " + ", ".join(f"{k} {counts[k]}" for k in sorted(counts)))
        reloads = [e for e in evs if e.kind == "reload" and "re-sync (" in e.text]
        why = {}
        for e in reloads:
            m = re.search(r"re-sync \((.*?)\)", e.text)
            k = re.sub(r"^\w+'s ", "<player>'s ", m.group(1)) if m else e.text
            why[k] = why.get(k, 0) + 1
        for k, n in sorted(why.items(), key=lambda kv: -kv[1]):
            print(f"   reload x{n}: {k}")
    cmd_problems(paths, logs)


def cmd_problems(paths, logs=None):
    logs = logs or load(paths)
    by_name = {label(logs)[p]: evs for p, evs in logs.items()}
    found = False
    for title, fn in PATTERNS:
        hits = fn(by_name)
        if not hits:
            continue
        found = True
        print(f"-- {title}: {len(hits)}")
        for e, why in hits[:6]:
            print(f"   {e.log} {e.t:9.3f} (line {e.lineno}) {why}: {e.text[:110]}")
        if len(hits) > 6:
            print(f"   ... {len(hits) - 6} more")
    if not found:
        print("-- no known bad pattern")


def cmd_timeline(paths, t_from, t_to, only):
    logs = load(paths)
    merged = [e for evs in logs.values() for e in evs
              if (t_from is None or e.t >= t_from) and (t_to is None or e.t <= t_to) and (not only or e.kind in only)]
    merged.sort(key=lambda e: (e.t, e.log))
    width = max((len(e.log) for e in merged), default=4)
    for e in merged:
        print(f"{e.t:9.3f} {e.log:<{width}} {e.kind:<10} {e.text[:150]}")


def cmd_selftest():
    """The samples are anonymized lines from the owner's night (0.10.5, 3 players): every pattern must find its case,
    and none may find anything in the clean sample."""
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "netlog_samples")
    night = load([os.path.join(here, "night_host.txt"), os.path.join(here, "night_joiner.txt")])
    clean = load([os.path.join(here, "clean_host.txt")])
    by_name = {label(night)[p]: evs for p, evs in night.items()}
    clean_by = {label(clean)[p]: evs for p, evs in clean.items()}
    failed = 0
    for title, fn in PATTERNS:
        if title in ("refused actions", "errors"):  # catch-alls, not night patterns
            continue
        hit, false_hit = fn(by_name), fn(clean_by)
        ok = bool(hit) and not false_hit
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {title}: {len(hit)} in the night's sample, {len(false_hit)} in the clean one")
    print(f"RESULT: {'FAIL' if failed else 'ALL OK'}")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["summary", "timeline", "problems", "selftest"])
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--from", dest="t_from", type=float)
    ap.add_argument("--to", dest="t_to", type=float)
    ap.add_argument("--only", help="comma-separated kinds: " + ", ".join(sorted({k for k, _ in EVENTS})))
    args = ap.parse_args()
    if args.command == "selftest":
        return cmd_selftest()
    if not args.logs:
        print("give one or more netlog files")
        return 2
    for p in args.logs:
        if not os.path.exists(p):
            print(f"no such log: {p}")
            return 2
    if args.command == "summary":
        cmd_summary(args.logs)
    elif args.command == "problems":
        cmd_problems(args.logs)
    else:
        cmd_timeline(args.logs, args.t_from, args.t_to, set(args.only.split(",")) if args.only else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
