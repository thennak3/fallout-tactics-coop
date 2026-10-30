// ---- The drift check's decisions, with nothing of the game in them (drift.inc feeds and acts on them; the
// simulator test/sim/drift_sim.cpp drives them with made-up copies, no game needed) ----
// The host compares each joiner's hashes of a frame with its own; two checked frames in a row that differ are a
// drift. When nobody is busy the host corrects it (every copy holds, the host sends its values), or reloads everyone
// if the entities themselves differ, or if the same drift is back within 10 s of a correction; reloads at most once a
// minute.
#pragma once
#include <cstdint>
#include <cstring>

namespace drift {

enum : uint8_t { kPos = 1, kTable = 2, kInv = 4, kAttr = 8, kEntities = 16 };
const uint32_t kFixWindowMs = 10000;  // a drift back within this of a correction: a reload
const uint32_t kSpacingMs = 60000;    // reloads at most this often
const int kEvery = 25;                // a check every 25 frames

struct Rec {
    long gen;
    int frame;
    uint32_t pos;    // every actor's position and HP
    uint32_t table;  // the free list (order, seq numbers) and the entity serial counter
    uint32_t used;   // which slots hold an entity, and their seq numbers: two copies that differ here made other things
    uint32_t inv;    // every inventory (actors and containers)
    uint32_t attr;   // every character's attributes (stats, skills, perks)
    bool shop;       // host: a shop was open (ShopBusy): this frame is not compared
};

inline uint8_t Kinds(const Rec& m, const Rec& j) {
    return (m.pos != j.pos ? kPos : 0) | (m.table != j.table ? kTable : 0) | (m.used != j.used ? kEntities : 0) |
           (m.inv != j.inv ? kInv : 0) | (m.attr != j.attr ? kAttr : 0);
}

enum class Verdict {
    Old,      // from before the last correction or load, or already compared: ignored
    Shop,     // a shop was open on the host at that frame: not compared
    Match,
    Differs,  // one check differs
    Drifted,  // the second in a row: the host acts (Decide)
};

enum class Action { None, Wait, Correct, Resync, Drop };

struct Judge {
    // What can be turned off, for the simulator's mutations (the bugs of 0.10.5).
    bool skip_shop = true;          // checks made during a shop are not compared
    bool drop_before_fix = true;    // a correction drops the reports from before it

    struct Seen {
        unsigned id;
        long gen;  // last_frame belongs to this generation (a load restarts the frame count)
        int last_frame, misses;
        uint8_t kinds;
    };
    Seen seen[8] = {};
    long fix_gen = -1;  // comparisons start again after fix_frame, in this generation
    int fix_frame = 0;
    bool reason = false;  // a drift waits for the host's main thread (Decide)
    unsigned reason_id = 0;
    uint8_t reason_kinds = 0;
    uint32_t fix_tick = 0, last_resync = 0;

    // A joiner's check of one frame, against the host's own of that frame (host: null when it has none yet; the
    // check is then compared on a later poll). kinds: what differed.
    Verdict Compare(unsigned id, long serve_gen, const Rec& j, const Rec* host, uint8_t* kinds) {
        *kinds = 0;
        Seen* s = nullptr;
        for (Seen& x : seen)
            if (x.id == id) s = &x;
        for (Seen& x : seen)
            if (!s && !x.id) s = &x;
        if (!s) return Verdict::Old;
        s->id = id;
        if (j.gen != serve_gen || (j.gen == fix_gen && j.frame <= fix_frame)) return Verdict::Old;
        if (s->gen != j.gen) s->gen = j.gen, s->last_frame = 0, s->misses = 0, s->kinds = 0;
        if (j.frame <= s->last_frame || !host) return Verdict::Old;
        s->last_frame = j.frame;
        // While a shop is open the shopper's game has what the others get at its close (the stock, the offers): every
        // check differed, and the report made at the close reloaded everyone before the deal could be compared (the
        // owner's night, 0.10.5: a reload after nearly every shop). The checks after the close count.
        if (skip_shop && host->shop) {
            s->misses = 0;
            s->kinds = 0;
            return Verdict::Shop;
        }
        uint8_t k = Kinds(*host, j);
        *kinds = k;
        if (!k) {
            s->misses = 0;
            s->kinds = 0;
            return Verdict::Match;
        }
        s->kinds |= k;
        if (++s->misses >= 2 && !reason) {
            reason = true;
            reason_id = id;
            reason_kinds = s->kinds;
            s->misses = 0;
            s->kinds = 0;
            return Verdict::Drifted;
        }
        return Verdict::Differs;
    }

    // Host, main thread: what to do about a drift now. busy: a conversation, shop, re-sync or correction is under way.
    // Correct and Resync clear the drift; the caller starts them (and calls CorrectionStarted for a correction).
    Action Decide(uint32_t now, bool busy) {
        if (!reason) return Action::None;
        if (busy) return Action::Wait;
        uint8_t k = reason_kinds;
        reason = false;
        reason_kinds = 0;
        // The correction, unless other entities exist on that copy (it can't make or destroy them) or the same drift
        // is back soon after one.
        if (!(k & kEntities) && (!fix_tick || now - fix_tick > kFixWindowMs)) {
            fix_tick = now;
            return Action::Correct;
        }
        if (last_resync && now - last_resync < kSpacingMs) return Action::Drop;
        last_resync = now;
        return Action::Resync;
    }

    // The host asked every copy to hold at about this frame. The correction puts every copy right: another joiner's
    // report from before it, often on its way already, is no new drift. Taken as one, it came within the 10 s after a
    // correction and reloaded everyone (the owner's night, after every mission load with 3 players). The hold's own
    // frame replaces this when it is over.
    void CorrectionStarted(long gen, int frame) {
        if (!drop_before_fix) return;
        fix_gen = gen;
        fix_frame = frame + 2 * kEvery;
        reason = false;
        reason_kinds = 0;
    }

    void CorrectionDone(long gen, int hold_frame) {  // the hashes made before the hold: not compared again
        fix_gen = gen;
        fix_frame = hold_frame + 1;
    }

    void NewSession() {  // a drift seen in the old session is gone with it
        reason = false;
        reason_kinds = 0;
    }
};

}  // namespace drift
