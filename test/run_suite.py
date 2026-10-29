"""Fallout Tactics co-op suite: every two-copy test, plus mutation runs that must fail.

Usage: python -I run_suite.py
Prints one line per test and `SUITE: PASS` only if every normal test passed and every mutation failed.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# (name, args, expected RESULT)
SUITE = [
    ("host_move", ["host_move", "--timeout", "60"], "PASS"),
    ("join_move", ["join_move", "--timeout", "60"], "PASS"),
    ("join_move without shared-squad fix (mutation)", ["join_move", "--mutate-share-squad", "--timeout", "60"], "FAIL"),
    ("simul", ["simul", "--timeout", "60"], "PASS"),
    ("attack", ["attack", "--timeout", "70"], "PASS"),
    ("fetch_save (joiner starts from a different save)", ["fetch_save", "--timeout", "60"], "PASS"),
    ("fetch_save without the transfer (mutation)", ["fetch_save", "--mutate-fetch", "--timeout", "60"], "FAIL"),
    ("resync (mid-mission save + reload, joiner follows)", ["resync", "--timeout", "90"], "PASS"),
    ("resync without following (mutation)", ["resync", "--mutate-follow", "--timeout", "90"], "FAIL"),
    ("drift_host (the joiner's game drifts on purpose; the host notices and corrects it, no reload)", ["drift_host", "--timeout", "240"], "PASS"),
    ("drift_equip (the joiner empties a hand on its copy only; the host corrects it, no reload)", ["drift_equip", "--timeout", "240"], "PASS"),
    ("quest_host (QuestBarter.sav: the host talks to the quest giver; only it sees the briefing)", ["quest_host", "--timeout", "180"], "PASS"),
    ("quest_join (the same from the joiner)", ["quest_join", "--timeout", "180"], "PASS"),
    ("shoptalk (the host shops with scrip; the joiner's talk waits until the shop is done)", ["shoptalk", "--timeout", "180"], "PASS"),
    ("levelup_host (the host levels up: a perk; the joiner gets the stats)", ["levelup_host", "--timeout", "120"], "PASS"),
    ("levelup_join (the joiner levels up; the host is refused that character's screen and the recruiter)", ["levelup_join", "--timeout", "120"], "PASS"),
    ("levelrace (both open the same character's screen at once: one wins)", ["levelrace", "--timeout", "120"], "PASS"),
    ("joinload (the joiner presses quick-load mid-mission: refused, only the host loads)", ["joinload", "--timeout", "120"], "PASS"),
    ("transition (mission -> world map -> mission)", ["transition", "--timeout", "90"], "PASS"),
    ("menu_join (roles from the Multiplayer menu)", ["menu_join", "--timeout", "90"], "PASS"),
    ("dropin (joiner arrives mid-session)", ["dropin", "--timeout", "120"], "PASS"),
    ("talk_host (bunker: the host talks, changes HP and a quest variable; the joiner gets a delta, no reload)", ["talk_host"], "PASS"),
    ("talk_join (the same from the joiner)", ["talk_join"], "PASS"),
    ("browse_host (bunker: the host opens barter, buys nothing; no reload; the joiner is refused the merchant)", ["browse_host"], "PASS"),
    ("browse_join (the same from the joiner; the host waits for it)", ["browse_join"], "PASS"),
    ("trade_host (bunker: both move, the host barters with the merchant, no reload: the joiner gets the delta)", ["trade_host"], "PASS"),
    ("trade_join (bunker: both move, the joiner barters, its game goes to the host)", ["trade_join"], "PASS"),
    ("trade2_host (two visits: the host trades, then the joiner trades and sells; the stock lists stay the same)",
     ["trade2_host", "--timeout", "360"], "PASS"),
    ("trade_host with the old held shop (--shop-pause)", ["trade_host", "--shop-pause"], "PASS"),
    ("savemenu (bunker: the host sits in the game menu and its save screen, then quick-saves; the joiner keeps playing)",
     ["savemenu", "--save", "user/save/bunker.sav", "--timeout", "120"], "PASS"),
    ("savemenu with saves written alone (--save-alone, the old way): must drift",
     ["savemenu", "--save", "user/save/bunker.sav", "--timeout", "120", "--save-alone"], "FAIL"),
    ("recruit_host (bunker: the host dismisses two, takes one back; every copy makes the candidates, no reload)", ["recruit_host"], "PASS"),
    ("recruit_join (the same from the joiner)", ["recruit_join"], "PASS"),
    ("recruitnew_host (bunker: the host recruits someone new, no reload)", ["recruitnew_host"], "PASS"),
    ("recruitnew_join (the same from the joiner)", ["recruitnew_join"], "PASS"),
    ("trade_race (4 players: every joiner talks to the merchant at once; one trades, everyone matches)",
     ["trade_race", "--joiners", "3", "--timeout", "420"], "PASS"),
    ("trade_host, 4 players (a new stack and a quest variable reach all three joiners, through the host's file server)",
     ["trade_host", "--joiners", "3", "--timeout", "420"], "PASS"),
    ("recruitnew_host, 4 players (every joiner makes the candidates and applies the recruit)",
     ["recruitnew_host", "--joiners", "3", "--timeout", "420"], "PASS"),
    ("exit_host (bunker exit grid -> world map; the joiner is refused, the host picks, the joiner follows)", ["exit_host", "--timeout", "180"], "PASS"),
    ("exit_join (the same with the joiner walking the squad out)", ["exit_join", "--timeout", "180"], "PASS"),
    ("menu_join --menu-cancel (the host cancels the Load Game screen Start Co-op opens: no crash)",
     ["menu_join", "--menu-cancel", "--timeout", "150"], "PASS"),
    ("encounter (world map -> random encounter; the joiner follows)", ["encounter", "--timeout", "300"], "PASS"),
    ("mission_ping (the host, then the joiner, middle-click in the mission: both flags on both copies, fading)",
     ["mission_ping", "--timeout", "180"], "PASS"),
    ("worldmap (the host travels, the joiner watches and can't; pings; fog of war the same)", ["worldmap", "--timeout", "300"], "PASS"),
    ("talk_host --grow-table (the entity table grows during the conversation: no reload)", ["talk_host", "--grow-table", "--timeout", "240"], "PASS"),
    ("talk_join --grow-table (the same from the joiner)", ["talk_join", "--grow-table", "--timeout", "240"], "PASS"),
    ("talk2 (host and joiner talk to two NPCs at the same moment)", ["talk2", "--timeout", "300"], "PASS"),
    ("talk2same (both talk with the same character: one talk is lost, its NPC freed)", ["talk2same", "--timeout", "300"], "PASS"),
    ("trade_host --inv-chaos (the joiner gives and equips while the host shops)", ["trade_host", "--inv-chaos", "--timeout", "300"], "PASS"),
    ("trade_join --inv-chaos (the host gives and equips while the joiner shops)", ["trade_join", "--inv-chaos", "--timeout", "300"], "PASS"),
    ("loot (containers.sav: the joiner takes all, the host one stack)",
     ["loot", "--save", "user/save/containers.sav", "--timeout", "300"], "PASS"),
    ("chaos (containers.sav: every copy acts at random, equips included; no drift)",
     ["chaos", "--save", "user/save/containers.sav", "--hold-frames", "2000", "--timeout", "420"], "PASS"),
    ("turnbased, Squad (containers.sav: the players take turns ending the squad's turn)",
     ["turnbased", "--save", "user/save/containers.sav", "--hold-frames", "4000", "--timeout", "300"], "PASS"),
    ("turnbased, Individual, 3 players", ["turnbased", "--save", "user/save/containers.sav", "--hold-frames", "4000",
                                          "--timeout", "420", "--joiners", "2", "--host-turnbased", "1"], "PASS"),
    ("turnbased, the joiner's own option differs (the host's settings win)",
     ["turnbased", "--save", "user/save/containers.sav", "--hold-frames", "4000", "--timeout", "300", "--joiner-turnbased", "1"], "PASS"),
    ("tbauto (combat starts by itself)", ["tbauto", "--save", "user/save/containers.sav", "--hold-frames", "4000", "--timeout", "300"], "PASS"),
]
# Long run, not in the default suite: python -I run_test.py soak --soak-frames 4000 --timeout 420


def main():
    ok_all = True
    for name, args, expected in SUITE:
        out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "run_test.py"), *args],
                             capture_output=True, text=True).stdout
        result = next((l for l in out.splitlines() if l.startswith("RESULT:")), "RESULT: none (no result line)")
        got = "PASS" if result.startswith("RESULT: PASS") else "FAIL"
        ok = got == expected
        ok_all &= ok
        print(f"{'ok  ' if ok else 'BAD '} {name}: expected {expected}, got {result[len('RESULT: '):]}")
    print(f"SUITE: {'PASS' if ok_all else 'FAIL'}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
