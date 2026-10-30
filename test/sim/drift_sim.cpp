// Drift check simulator: the host's drift decisions (src/drift_judge.h, the same code the mod runs) against made-up
// copies, no game. Each copy's hashes come from a small state; scenarios change those states the way the owner's
// nights did (a long shop, copies that ran other sessions, a real split, the same drift back) and joiners' checks
// reach the host late, each joiner by its own delay. Every scenario runs again with the 0.10.5 behaviour, which must
// fail its expectation (the simulator would otherwise pass for nothing).
// Build and run: test\sim\run_sim.bat (needs the x86 or x64 MSVC tools on the path, as src\build.bat).
#include <cstdio>
#include <vector>
#include "../../src/drift_judge.h"

namespace {

struct Copy {
    uint32_t table = 0, used = 0, inv = 0;  // what differs from the host's (0: the same)
    int delay = 1;                          // frames until the host has its check
};

struct Scenario {
    const char* name;
    int joiners;
    int delays[3];
    int frames;
    int shop_from, shop_to;   // the host shops (the shopper's game differs meanwhile, the same after the close)
    int skew_joiners;         // these joiners' entity serials differ from the start
    int split_at;             // joiner 1 makes other entities from here (a correction can't fix it, a reload does)
    int split_again;          // ... and again after the reload, this many frames later (0: never)
    int back_after;           // the same free-list drift comes back this many frames after a correction (0: never)
    int want_fixes, want_resyncs;
    bool mutate_shop, mutate_fix;  // the 0.10.5 behaviour, which must fail the expectation
};

struct Result {
    int fixes = 0, resyncs = 0, drops = 0;
};

Result Run(const Scenario& sc, bool mutated) {
    drift::Judge judge;
    if (mutated) {
        judge.skip_shop = !sc.mutate_shop;
        judge.drop_before_fix = !sc.mutate_fix;
    }
    std::vector<Copy> copies(sc.joiners);
    for (int i = 0; i < sc.joiners; i++) copies[i].delay = sc.delays[i];
    long gen = 1;
    int frame_base = 0;   // a load restarts the frame count
    int hold_at = -1;     // the correction's hold acts at this frame
    int back_at = -1;     // the drift comes back here
    int split_at = sc.split_at;
    struct Pending {
        int arrive, joiner;
        drift::Rec rec;
    };
    std::vector<Pending> pending;
    drift::Rec ring[8] = {};
    int ring_next = 0;
    Result r;
    auto skew = [&]() {
        for (int i = 0; i < sc.skew_joiners && i < sc.joiners; i++) copies[i].table = 0x1000 + i;
    };
    skew();
    for (int t = 0; t < sc.frames; t++) {
        int f = t - frame_base;  // this generation's frame
        uint32_t now = static_cast<uint32_t>(t) * 70;  // 14 frames a second, as the owner's copies ran
        bool shop = t >= sc.shop_from && t < sc.shop_to;
        if (split_at >= 0 && t == split_at) copies[0].used = 0x77, copies[0].inv = 0x77;
        if (back_at >= 0 && t == back_at) copies[0].table = 0x2000, back_at = -1;
        if (hold_at >= 0 && f == hold_at) {  // the host's values put every copy right (not other entities)
            for (Copy& c : copies) c.table = 0, c.inv = c.used ? c.inv : 0;
            judge.CorrectionDone(gen, f);
            hold_at = -1;
            if (sc.back_after) back_at = t + sc.back_after;
        }
        if (f > 0 && f % drift::kEvery == 0) {
            drift::Rec host = {gen, f, uint32_t(f * 7), uint32_t(f * 11), uint32_t(f * 13), uint32_t(f * 17), 1, shop};
            if (shop) host.table ^= 1, host.used ^= 1, host.inv ^= 1;  // the shop's stock and offers, the shopper's only
            ring[ring_next] = host;
            ring_next = (ring_next + 1) % 8;
            for (int i = 0; i < sc.joiners; i++) {
                drift::Rec j = {gen, f, uint32_t(f * 7), uint32_t(f * 11) ^ copies[i].table, uint32_t(f * 13) ^ copies[i].used,
                                uint32_t(f * 17) ^ copies[i].inv, 1, false};
                pending.push_back({t + copies[i].delay, i, j});
            }
        }
        for (size_t k = 0; k < pending.size();) {
            if (pending[k].arrive > t) {
                k++;
                continue;
            }
            const drift::Rec* mine = nullptr;
            for (const drift::Rec& h : ring)
                if (h.gen == pending[k].rec.gen && h.frame == pending[k].rec.frame) mine = &h;
            uint8_t kinds;
            judge.Compare(100 + pending[k].joiner, gen, pending[k].rec, mine, &kinds);
            pending.erase(pending.begin() + k);
        }
        bool busy = shop;  // as the mod: a correction asked for is not busy until its hold acts (and then only briefly)
        drift::Action a = judge.Decide(now, busy);
        if (a == drift::Action::Correct) {
            r.fixes++;
            judge.CorrectionStarted(gen, f);
            hold_at = f + 3;  // the hold record acts a few frames later on every copy (the owner's log: 3 frames)
        } else if (a == drift::Action::Resync) {
            r.resyncs++;
            gen++;  // everyone loads the host's save: a new generation, frames from 0, the same game
            frame_base = t + 1;
            hold_at = -1;
            pending.clear();  // checks of the old generation are never compared
            for (Copy& c : copies) c.table = c.used = c.inv = 0;  // (the serial is the host's after a load now)
            judge.NewSession();
            if (sc.split_again) split_at = t + sc.split_again;
        } else if (a == drift::Action::Drop) {
            r.drops++;
        }
    }
    return r;
}

}  // namespace

int main() {
    const Scenario scenarios[] = {
        // The owner's shops: the host shops 30 s; the joiners' games match after the close.
        {"a long shop: no correction, no reload", 2, {2, 9}, 1200, 300, 720, 0, -1, 0, 0, 0, 0, true, false},
        // After every mission load with 3 players: both joiners' serials differ; their reports come 2 and 9 frames late.
        {"two joiners with the same drift: one correction, no reload", 2, {2, 4}, 600, -1, -1, 2, -1, 0, 0, 1, 0, false, true},
        // Other entities on one copy: only a reload can fix it.
        {"other entities on a joiner: one reload", 1, {3}, 600, -1, -1, 0, 200, 0, 0, 0, 1, false, false},
        // The same drift back within 10 s of its correction: a reload.
        {"the same drift back soon after a correction: a reload", 1, {3}, 800, -1, -1, 1, -1, 0, 60, 1, 1, false, false},
        // Other entities again soon after the reload: no second reload within the minute.
        {"a split again within a minute of a reload: no second reload", 1, {3}, 900, -1, -1, 0, 200, 150, 0, 0, 1, false, false},
    };
    int failed = 0, checks = 0;
    for (const Scenario& sc : scenarios) {
        Result r = Run(sc, false);
        bool ok = r.fixes == sc.want_fixes && r.resyncs == sc.want_resyncs;
        checks++;
        failed += !ok;
        printf("%s  %s  [corrections %d, reloads %d, dropped %d; want %d, %d]\n", ok ? "PASS" : "FAIL", sc.name, r.fixes,
               r.resyncs, r.drops, sc.want_fixes, sc.want_resyncs);
        if (sc.mutate_shop || sc.mutate_fix) {
            Result m = Run(sc, true);
            bool caught = m.fixes != sc.want_fixes || m.resyncs != sc.want_resyncs;
            checks++;
            failed += !caught;
            printf("%s  ... with the 0.10.5 %s, it fails  [corrections %d, reloads %d]\n", caught ? "PASS" : "FAIL",
                   sc.mutate_shop ? "checks during shops" : "reports from before a correction", m.fixes, m.resyncs);
        }
    }
    printf("RESULT: %s  (drift_sim, %d/%d checks)\n", failed ? "FAIL" : "PASS", checks - failed, checks);
    return failed ? 1 : 0;
}
