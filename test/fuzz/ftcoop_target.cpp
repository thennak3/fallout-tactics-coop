// Fallout Tactics co-op on a fake world, for the hub's fuzzer (coop-anything tools/fuzz): 2 to 4 copies in lockstep,
// shops, joins, loads, drift checks, corrections and reloads, with desyncs forced on purpose. The decisions are the
// mod's own code: src/drift_judge.h (when a drift is a drift, correct or reload) and src/session_sync.h (what every copy
// takes from the host at a session's start, when a re-sync may save). The world around them is a model of what those
// decisions see in the game; the bot tests in test/run_test.py check the model against the game.
//
// The model (what the game does, as far as sync is concerned):
//   - The host relays every command with the frame it acts at; every copy runs every frame's commands in order.
//   - Entity table: slots with sequence numbers and a free list; making an entity takes the head (and the next serial),
//     destroying one puts it back at the head with its sequence + 1. The serial counter is not in a save.
//   - The quartermaster's stock: a list on every copy. A shop makes one entity per stocked kind, plus the offer
//     inventories, on every copy from its own list; the shopper's copy also splits a stack meanwhile (an entity only
//     it has, freed before the deal). The deal (the buyer's inventory, the stock, and the entity table's bookkeeping:
//     the free list and its sequence numbers, as the mod's "N table entries", and the entity serial, BookSerial) acts
//     at the close.
//     Pessimistic: the stock is not in a save (a joiner's own list survives a load), as the owner's night looked.
//   - Item moves wait while a shop is open, and run the frame after it closed.
//   - A shopper that leaves (or has left by the time its shop opens): the host closes its shop for everyone with an
//     empty close, at a frame (shop_sync.inc CloseAbandonedShop); its purchases are gone with it.
//   - Drift check: every 25 frames each copy hashes positions, the free list (with the serial), which slots are used,
//     inventories. Joiners poll the host every 250 ms with their last checks; the host decides (drift::Judge).
//   - Correction: a hold record; every copy holds after its frame; the host sends positions, free list, serial and
//     inventories; a copy goes on when it has them, or gives up after 8 s. It cannot make or destroy entities.
//   - Reload: everyone waits for every copy to load the host's save; a new generation from frame 0.
//
// Conversations (the fuzzer's Hit op: copy m talks to NPC 300 + a % 3 for 1 to 15 s): 300 and 302 are story NPCs
// (held: from the talk's frame every copy waits until the talker is done, the talker's own world too), 301 a trader
// (nobody waits). The talk goes to the host, which passes the first on an NPC and drops the others (talk_locks.h);
// every copy marks the NPC busy at the talk's frame and refuses its own player's talk to a busy NPC. The talker's done
// frees the NPC (a held one: on arrival, nobody runs frames meanwhile; a trader: at its frame). The host stops waiting
// for a joiner's conversation when it is done, when every joiner is gone, or after 10 minutes (HostHoldTimeout). A
// session's start clears every lock (ClearNpcLocks, ClearHostLocks). A talker that leaves in the middle: the host
// ends its conversation for everyone (CloseAbandonedTalks). One talk at a time per player (a second order replaces the
// first in the game: ReleaseSupersededTalk; here it is not sent).
//
// Faults (the fuzzer's Spawn op, a % 5, on a joiner): 0 its serial runs ahead, 1 its stock is emptied, 2 it makes an
// entity the others don't (only a reload mends it), 3 its free list's first two slots swap (a correction mends it),
// 4 every joiner's serial runs ahead at once (copies that ran other sessions: their reports race each other).
// Activate opens a shop for 1 to 30 s; Pickup moves an item between squad members; Move moves one; Join and Leave;
// Load is the host's quick-load. heal() ends the session with one more reload, so what a session start mends is mended.
//
// Oracles beyond the fuzzer's own: frozen (a copy held for a correction that never came), needless_repair (a correction
// or a drift reload while every copy was the same as the host), apart_after_save (a save's write on the saver's copy only), apart_after_load (a copy not the same as the host 10
// frames into a session, with no fault since it started), saved_during_shop (a re-sync's save while a shop is open).
//
// Mutation twins, the bugs of 0.10.5 to 0.10.7: no_shop_skip, no_drop_before_fix, no_serial_sync, no_stock_sync,
// no_resync_gate, no_abandoned_close (found by this fuzzer: a shopper that left kept the shop open everywhere),
// no_host_filter (the host passes every talk), no_lock_clear (0.10.3: locks kept across a session start),
// no_solo_release (0.10.5: a host on its own stays held after its conversation), no_abandoned_talk (found by this
// fuzzer: a talker that left kept the NPC locked and everyone waiting).
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <set>
#include <string>
#include "coopfuzz/fuzz.h"
#include "coopfuzz/rng.h"
#include "../../src/drift_judge.h"
#include "../../src/session_sync.h"
#include "../../src/talk_locks.h"

namespace coopfuzz {
namespace {

const uint32_t kFrameMs = 70;   // 14 frames a second, as the owner's copies ran
const int kSlots = 96;
const int kKinds = 6;
const int kActors = 4;
const uint32_t kPollMs = 250;
const uint32_t kFixGiveUpMs = 8000;
const uint32_t kLoadMs = 3000;
const uint64_t kSaveHoldMaxMs = 600000;  // kSaveHoldMaxMs: a saver gone quiet, everyone goes on
const int kExecDelay = 2;  // a command acts this many frames after the host has it

enum CmdType : uint8_t { CMove, CGive, CShopOpen, CShopClose, CFixHold, CSerial, CStock, CJoinersApart, CTalk, CTalkDone, CSaveHold };

struct Cmd {
    CmdType type = CMove;
    int a = 0, b = 0, c = 0;
    int origin = 0;
    uint32_t id = 0;
    uint32_t value = 0;
    uint8_t payload[session::kStockChunk] = {};
    int inv[kKinds] = {};
    int stock[kKinds] = {};
    std::vector<uint16_t> seqs;   // a deal: every slot's sequence number and next free slot, and the free list's head
    std::vector<int> nexts;
    int head = 0;
};

struct Slot {
    bool used = false;
    uint16_t seq = 0;
    int next = 0;
};

struct World {
    std::vector<Slot> slots;
    int head = 1;
    uint32_t serial = 1000;
    int pos[kActors][2] = {};
    int inv[kActors][kKinds] = {};
    int stock[kKinds] = {};
    void Reset() {
        slots.assign(kSlots, Slot());
        for (int i = 1; i < kSlots; i++) slots[i].next = i + 1 < kSlots ? i + 1 : 0;
        head = 1;
        for (int a = 0; a < kActors; a++) {
            pos[a][0] = 100 * a, pos[a][1] = 50;
            for (int k = 0; k < kKinds; k++) inv[a][k] = (a + k) % 3;
        }
        for (int k = 0; k < kKinds; k++) stock[k] = 3 + k;
    }
    int Make() {
        int s = head;
        if (!s) return 0;
        head = slots[s].next;
        slots[s].used = true;
        serial++;
        return s;
    }
    void Destroy(int s) {
        if (s <= 0 || !slots[s].used) return;
        slots[s].used = false;
        slots[s].seq++;
        slots[s].next = head;
        head = s;
    }
    static uint32_t Fnv(uint32_t h, uint32_t v) {
        for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xff)) * 16777619u;
        return h;
    }
    uint32_t PosHash() const {
        uint32_t h = 2166136261u;
        for (int a = 0; a < kActors; a++) h = Fnv(Fnv(h, pos[a][0]), pos[a][1]);
        return h;
    }
    uint32_t TableHash() const {
        uint32_t h = Fnv(2166136261u, head);
        h = Fnv(h, serial);
        for (int i = 1; i < kSlots; i++)
            if (!slots[i].used) h = Fnv(Fnv(Fnv(h, i), slots[i].next), slots[i].seq);
        return h;
    }
    uint32_t UsedHash() const {
        uint32_t h = 2166136261u;
        for (int i = 1; i < kSlots; i++)
            if (slots[i].used) h = Fnv(Fnv(h, i), slots[i].seq);
        return h;
    }
    uint32_t InvHash() const {  // actors' inventories (the stock list is not in the game's check either)
        uint32_t h = 2166136261u;
        for (int a = 0; a < kActors; a++)
            for (int k = 0; k < kKinds; k++) h = Fnv(h, inv[a][k]);
        return h;
    }
    uint32_t StockHash() const {
        uint32_t h = 2166136261u;
        for (int k = 0; k < kKinds; k++) h = Fnv(h, stock[k]);
        return h;
    }
    int StockText(char* out, int cap) const {
        int n = 0;
        for (int k = 0; k < kKinds; k++) n += snprintf(out + n, cap - n, "item%d\t%d\n", k, stock[k]);
        return n;
    }
};

struct Delivery {
    uint64_t at;
    int frame;
    long gen;
    std::vector<Cmd> cmds;
};

struct Poll {
    uint64_t at;
    int from;
    std::vector<drift::Rec> recs;
};

struct Hashes {
    uint32_t pos, table, used, inv, stock;
    bool shop;
    bool operator==(const Hashes& o) const {
        return pos == o.pos && table == o.table && used == o.used && inv == o.inv && stock == o.stock;
    }
};
Hashes HashesOf(const World& w, bool shop) { return {w.PosHash(), w.TableHash(), w.UsedHash(), w.InvHash(), w.StockHash(), shop}; }

struct Copy {
    World w;
    int frame = 0;
    bool connected = true;
    bool loading = false;
    bool held = false;            // held for a correction
    bool loaded = false;          // took the save of the reload under way
    std::map<int, int> npc_lock;  // NPC -> the copy talking to it (NpcLock)
    bool conv_wait = false;       // held for another copy's conversation
    bool talking = false;         // this copy's player is in a conversation
    bool talk_pending = false;    // ... asked for, not opened (or dropped) yet
    int pending_npc = 0;
    uint64_t gone_wait_since = 0;
    uint64_t stock_apart_since = 0;
    int stock_apart_frames = 0;  // frames run while apart: the drift checks need frames, not time (a held conversation has none)
    bool gone_wait_said = false;
    bool talk_held = false;       // ... a held one: its own world waits too (g_talker_hold)
    int talk_npc = 0;
    uint64_t talk_until = 0;
    std::map<int, Hashes> frames;  // this copy's hashes at each frame of this session
    std::map<int, World> worlds;   // and its world, for the reports (what differed, from when)
    uint64_t held_since = 0;
    std::deque<Delivery> stream;  // host -> this copy: frames, in order (reliable, like DirectPlay's guaranteed)
    uint64_t last_delivery = 0;
    std::vector<drift::Rec> ring;
    uint64_t next_poll = 0;
    // a shop open on this copy
    bool shop = false;
    int shop_shopper = -1, shop_buyer = 0;
    std::vector<int> shop_made;
    int shop_split = 0;
    std::vector<Cmd> held_cmds;   // item moves waiting for the shop's close
    bool run_held = false;
    session::StockAssembler stock_in;
    // shopper side
    uint64_t shop_close_at = 0;
    bool my_shop = false;
    // saving in step (save_sync.inc)
    bool save_requested = false;  // asked, the hold not acted yet
    bool save_hold = false;       // held for a save: no frames
    bool save_mine = false;       // this copy saves
    int save_saver = -1;
    int save_hold_frame = 0;
    uint64_t save_hold_since = 0, save_done_at = 0;
    uint64_t save_gone_since = 0;
    bool save_gone_said = false;
    bool save_stuck_said = false;
    uint64_t save_done_sent = 0;  // when this copy (the saver) sent its done: the stuck oracle
    // a saver's done that came before this copy's hold frame (g_save_early_*)
    int early_saver = -1;
    bool early_saved = false;
    uint64_t early_tick = 0;
    // NPCs whose gone talker's done came first (g_cancelled[8]), 10 s each; one_cancel_slot: only the last
    std::vector<std::pair<int, uint64_t>> cancelled;
    int cancelled_next = 0;
};

class FtTarget : public TargetBase {
public:
    FtTarget(const Scenario& s, const std::set<std::string>& bugs) : n_(s.machines), rng_(s.seed * 7919 + 17), bugs_(bugs) {
        link_ = s.link;
        if (!s.profile.empty()) linkProfile(s.profile, link_);
        judge_.skip_shop = !bugs.count("no_shop_skip");
        judge_.drop_before_fix = !bugs.count("no_drop_before_fix");
        copies_.resize(n_);
        for (int m = 0; m < n_; m++) {
            copies_[m].w.Reset();
            copies_[m].w.serial = 1000 + 37 * m;  // copies that ran other sessions before: their counters differ
            copies_[m].next_poll = rng_.range(0, kPollMs);  // each joiner polls on its own clock
        }
        StartSession(true);
    }

    uint64_t now() const override { return now_; }
    int machines() const override { return n_; }
    bool connected(int m) const override { return copies_[m].connected; }
    uint64_t repairs(int) const override { return fixes_ + drift_reloads_; }

    void apply(const Op& op) override {
        int m = std::max(0, std::min(n_ - 1, op.m));
        switch (op.kind) {
            case OpKind::Move: {
                Cmd c;
                c.type = CMove, c.a = op.a % kActors, c.b = op.b, c.c = op.c;
                Request(m, c);
                counters_["op_move"]++;
                break;
            }
            case OpKind::Pickup:
            case OpKind::Drop: {
                Cmd c;
                c.type = CGive, c.a = op.a % kActors, c.b = (op.a + 1) % kActors, c.c = op.b % kKinds;
                Request(m, c);
                counters_["op_give"]++;
                break;
            }
            case OpKind::Activate: {
                // a quarter of them: back to back (five a session), the drift checks between them were too few to see two
                // in a row (probe_undetected)
                if (!Quarter(op) || any_shop_ || !copies_[m].connected || copies_[m].loading) break;
                Cmd c;
                c.type = CShopOpen, c.a = op.a % kActors, c.b = m, c.c = 1 + (op.b % 30);
                Request(m, c);
                any_shop_ = true;  // one shop at a time (the merchant's lock)
                counters_["op_shop"]++;
                Trace("copy %d asks for a shop (%d s)", m, c.c);
                break;
            }
            case OpKind::Hit: {  // a conversation: copy m talks to NPC 300 + a % 3
                if (!Quarter(op) || !copies_[m].connected || copies_[m].loading || copies_[m].talking || copies_[m].talk_pending) break;
                Copy& c = copies_[m];
                int npc = 300 + op.a % 3;
                auto l = c.npc_lock.find(npc);
                if (l != c.npc_lock.end() && l->second != m) {  // RefuseLockedTalk
                    Trace("copy %d's talk to NPC %d refused here: copy %d is talking to them", m, npc, l->second);
                    bool done_on_way = false;  // the talker's done is still on its way to this copy: it is told on arrival
                    for (const DoneDue& d : done_due_) done_on_way |= d.m == m && d.npc == npc;
                    // ... or its done is in the schedule, for a frame that does not run while another conversation holds the
                    // game (a non-held talk's done acts at its frame): the click is refused until then (counted, no fault)
                    for (auto& f : schedule_)
                        for (const Cmd& sc : f.second) done_on_way |= sc.type == CTalkDone && sc.a == npc;
                    for (const Delivery& dl : copies_[m].stream)  // ... or on its way to this copy in the frame stream
                        for (const Cmd& sc : dl.cmds) done_on_way |= sc.type == CTalkDone && sc.a == npc;
                    if (done_on_way) counters_["refused_while_done_waits"]++;
                    if (!TalkTaken(npc) && !done_on_way)
                        Violate("refused_free_npc", "copy " + std::to_string(m) + "'s talk to NPC " + std::to_string(npc) +
                                                        " refused while nobody is talking to them");
                    break;
                }
                Cmd t;
                t.type = CTalk, t.a = npc, t.b = m, t.c = 1 + op.b % 15;
                Request(m, t);
                c.talk_pending = true, c.pending_npc = npc;
                counters_["op_talk"]++;
                Trace("copy %d talks to NPC %d (%d s)", m, npc, t.c);
                break;
            }
            case OpKind::Save: {  // a save: an even a is a quick-save (300 ms), else the save screen (1 to 10 s)
                Copy& c = copies_[m];
                if (!Quarter(op) || !c.connected || c.loading || c.save_requested || c.save_mine) break;
                Cmd q;
                q.type = CSaveHold, q.a = m, q.b = op.a % 2 == 0 ? 1 : 2;
                q.c = q.b == 1 ? 300 : 1000 * (1 + op.b % 10);
                q.value = static_cast<uint32_t>(faults_);  // faults so far: the watch ignores a save with another
                Request(m, q);
                c.save_requested = true;
                counters_["op_save"]++;
                Trace("copy %d asks to save (%s, %d ms)", m, q.b == 1 ? "quick-save" : "save screen", q.c);
                break;
            }
            case OpKind::Spawn: {  // a forced desync on a joiner's copy
                // a quarter of them too: with every Spawn a fault, a split was nearly always among them, and the drifts
                // a correction mends (the owner's) were never alone
                // ... except every joiner's serial at once (the owner's night: their reports race a correction)
                if ((op.a % 5 != 4 && !Quarter(op)) || m == 0 || !copies_[m].connected || copies_[m].loading) break;
                Fault(m, op.a % 5);
                break;
            }
            case OpKind::Join:  // a quarter of the generator's joins, leaves and loads: drifts get time to be seen
                if (Quarter(op) && m != 0 && !copies_[m].connected) {
                    Copy& c = copies_[m];
                    c.connected = true;
                    c.loading = true;  // in the lobby until it has loaded a save of the host's (a reload brings it in)
                    c.w.serial += 5000 + rng_.range(0, 5000);  // its own game ran meanwhile
                    if (op.a == 1 || rng_.range(0, 1)) for (int& k : c.w.stock) k = 0;  // and may have no stock at all
                    counters_["op_join"]++;
                    Trace("copy %d joins", m);
                    if (reload_pending_ || loading_until_) join_again_ = true;  // that reload's save is already made
                    else StartReload(false, "a joiner arrived");
                }
                break;
            case OpKind::Leave:
                if (Quarter(op) && m != 0 && copies_[m].connected && connectedCount() > 1) {  // down to a host alone
                    copies_[m].connected = false;
                    copies_[m].held = false;
                    bool saver_mid_save = copies_[0].save_hold && copies_[0].save_saver == m && !copies_[0].save_mine;
                    copies_[m].save_requested = copies_[m].save_mine = copies_[m].save_hold = false;  // the others' holds stay
                    gone_.insert(m);
                    if (saver_mid_save && !bugs_.count("no_abandoned_save")) {  // CloseAbandonedSave: nothing saved
                        Trace("the host ends copy %d's save for everyone (nothing saved)", m);
                        for (int j = 0; j < n_; j++)
                            if (copies_[j].connected) save_done_due_.push_back({j == 0 ? now_ : now_ + Latency(), j, m, false});
                    }
                    counters_["op_leave"]++;
                    Trace("copy %d leaves%s", m, copies_[m].my_shop ? " in the middle of its own shop" : copies_[m].shop ? " with a shop open" : "");
                    if (copies_[0].shop && copies_[0].shop_shopper == m) CloseAbandoned();
                    copies_[m].talking = copies_[m].talk_pending = copies_[m].talk_held = false;
                    copies_[m].conv_wait = false, copies_[m].npc_lock.clear();  // a copy that comes back starts from nothing
                    if (!bugs_.count("no_abandoned_talk"))  // CloseAbandonedTalks: its NPCs' done, for everyone
                        for (int npc = 300; npc < 303; npc++)
                            if (host_locks_.Held(static_cast<uint16_t>(npc), static_cast<uint32_t>(now_)) && TalkOwner(npc) == m) {
                                host_locks_.Free(static_cast<uint16_t>(npc));
                                Trace("the host ends copy %d's conversation with NPC %d for everyone", m, npc);
                                for (int j = 0; j < n_; j++)
                                    if (copies_[j].connected) done_due_.push_back({j == 0 ? now_ : now_ + Latency(), j, npc, m, true});
                            }
                }
                break;
            case OpKind::Load:
                if (!Quarter(op)) break;
                counters_["op_load"]++;
                Trace("the host quick-loads");
                StartReload(false, "the host's quick-load");
                break;
            default:
                break;
        }
    }

    void advance(uint64_t toMs) override {
        while (now_ < toMs) {
            now_ = std::min<uint64_t>(now_ + 10, toMs);
            Step();
        }
    }

    void heal() override {  // no more ops; one more session start mends what a session start mends
        frozen_ = true;
        final_reload_ = true;
        StartReload(false, "the end of the session");
    }

    bool quiescent() const override {
        if (!frozen_ || final_reload_ || reload_pending_ || loading_until_ || resync_wait_) return false;
        if (any_shop_ || judge_.reason || fix_pending_ || host_hold_npc_ || !done_due_.empty() || !save_done_due_.empty()) return false;
        // Lockstep frames and polls never stop: quiet is the drift machinery idle with every copy 160 frames (six checks)
        // into the session that heal() started.
        for (const Copy& c : copies_)
            if (c.connected && (c.held || c.loading || c.frame < 160 || c.talking || c.conv_wait || c.talk_held ||
                              c.save_hold || c.save_requested))
                return false;
        return true;
    }

    std::vector<Thing> view(int m) const override {
        std::vector<Thing> v;
        const World& w = copies_[m].w;
        for (int a = 0; a < kActors; a++) {
            Thing t;
            t.netId = 0x100 + a;
            t.type = 1;
            t.x = w.pos[a][0], t.y = w.pos[a][1];
            int sum = 0;
            for (int k = 0; k < kKinds; k++) sum = sum * 7 + w.inv[a][k];
            t.hp = sum;
            v.push_back(t);
        }
        Thing q;  // the quartermaster: his stock
        q.netId = 0x200;
        q.type = 1;
        q.hp = static_cast<int>(w.StockHash() & 0x7fffffff);
        v.push_back(q);
        return v;
    }

    uint64_t digest(int m, int cat) const override {
        const World& w = copies_[m].w;
        switch (cat) {
            case 0: return (static_cast<uint64_t>(w.TableHash()) << 32) | w.UsedHash();
            case 1: return w.InvHash();
            case 2: return w.StockHash();
            default: return w.PosHash();
        }
    }

    void markQuiescent() override {}

    std::vector<Violation> nativeViolations() override {
        std::vector<Violation> v = violations_;
        if (frozen_ && quiescent()) {  // nothing talks: every lock must be free (0.10.3: one survived a reload)
            for (int m = 0; m < n_; m++)
                for (auto& l : copies_[m].npc_lock)
                    if (copies_[m].connected) {
                        v.push_back({"lock_leak", "copy " + std::to_string(m) + " still has NPC " + std::to_string(l.first) +
                                                      " locked for copy " + std::to_string(l.second) + " || trace" + Recent(), now_});
                        break;
                    }
            for (int npc = 300; npc < 303; npc++)
                if (host_locks_.Held(static_cast<uint16_t>(npc), static_cast<uint32_t>(now_)))
                    v.push_back({"lock_leak", "the host still holds NPC " + std::to_string(npc) + " || trace" + Recent(), now_});
        }
        if (frozen_ && !quiescent()) {  // what keeps it busy, for the report
            std::string d = "busy:";
            if (final_reload_) d += " final_reload";
            if (reload_pending_) d += " reload_pending";
            if (loading_until_) d += " loading";
            if (resync_wait_) d += " resync_waiting_for_shop";
            if (any_shop_) d += " shop";
            if (judge_.reason) d += " drift_reason";
            if (host_hold_npc_) d += " host_waits_for_conversation";
            if (!save_done_due_.empty()) d += " save_done_on_its_way";
            if (fix_pending_) d += " fix_pending";
            for (int m = 0; m < n_; m++) {
                const Copy& c = copies_[m];
                if (!c.connected) continue;
                if (c.held) d += " held" + std::to_string(m);
                if (c.loading) d += " loading" + std::to_string(m);
                if (c.frame < 160) d += " frame" + std::to_string(m) + "=" + std::to_string(c.frame);
                if (c.shop) d += " shop_on" + std::to_string(m);
                if (c.talking) d += " talking" + std::to_string(m);
                if (c.conv_wait) d += " conv_wait" + std::to_string(m);
                if (c.talk_held) d += " talk_held" + std::to_string(m);
                if (c.save_hold) d += " save_hold" + std::to_string(m) + "(saver " + std::to_string(c.save_saver) + (c.save_mine ? ", mine" : "") + ")";
                if (c.save_requested) d += " save_asked" + std::to_string(m);
            }
            v.push_back({"stuck", d + " || trace" + Recent(), now_});
        }
        return v;
    }

private:
    // The session's events, for the reports: every violation carries the last ones (run_fuzz.py groups by them).
    std::vector<std::string> trace_;
    void Trace(const char* fmt, ...) {
        char line[160];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        char stamped[200];
        snprintf(stamped, sizeof(stamped), "%llu.%01llus %s", static_cast<unsigned long long>(now_ / 1000),
                 static_cast<unsigned long long>(now_ % 1000 / 100), line);
        trace_.push_back(stamped);
    }
    std::string Recent(int n = 16) const {
        std::string t;
        for (size_t i = trace_.size() > static_cast<size_t>(n) ? trace_.size() - n : 0; i < trace_.size(); i++) t += " | " + trace_[i];
        return t;
    }
    void Violate(const char* invariant, const std::string& detail) { violations_.push_back({invariant, detail + " || trace" + Recent(), now_}); }

    // Joins, leaves and loads carry no arguments: a quarter of them by their time (they come often enough to mend every
    // drift before it is checked twice, and the corrections never ran).
    static bool Quarter(const Op& op) { return ((op.t * 2654435761u) >> 30) == 0; }

    int connectedCount() const {
        int k = 0;
        for (const Copy& c : copies_) k += c.connected;
        return k;
    }
    uint32_t Latency() { return rng_.range(link_.minLatencyMs, std::max(link_.minLatencyMs, link_.maxLatencyMs)); }

    // ---- a copy asks the host for a command: it acts kExecDelay frames after the host has it
    struct Pending {
        uint64_t at;
        Cmd cmd;
    };
    void Request(int m, Cmd c) {
        c.origin = m;
        c.id = ++next_id_;
        pending_.push_back({now_ + (m == 0 ? 0 : Latency()), c});
    }
    void HostCommand(Cmd c) {
        if (!c.id) c.id = ++next_id_;  // the host's own commands too: every message its own id
        if (c.type == CTalk) {  // HostFilterTalks (talk_locks.h); no_host_filter: the host passes every talk anyway
            int holder = -1;
            bool pass = host_locks_.Pass(static_cast<uint16_t>(c.a), c.b, static_cast<uint32_t>(now_), &holder);
            if (pass || bugs_.count("no_host_filter")) {
                if (pass) talk_owner_[c.a] = c.b;
            } else {
                Trace("host dropped copy %d's talk to NPC %d: copy %d has them", c.b, c.a, holder);
                copies_[c.b].talk_pending = false;  // the winner's claim tells it: someone got there first
                counters_["talk_dropped"]++;
                return;
            }
        }
        if (c.type == CTalkDone) host_locks_.Done(static_cast<uint16_t>(c.a), c.b);
        if (c.type == CTalkDone && Held(c.a)) {  // a held conversation's done: on arrival (nobody runs frames)
            for (int m = 0; m < n_; m++)
                if (copies_[m].connected) done_due_.push_back({m == 0 ? now_ : now_ + Latency(), m, c.a, c.b, false});
            return;
        }
        schedule_[copies_[0].frame + kExecDelay].push_back(c);
    }

    void Fault(int m, int kind) {
        World& w = copies_[m].w;
        static const char* names[] = {"serial ahead", "stock emptied", "an entity only it has", "free list swapped", "every joiner's serial ahead"};
        Trace("fault on copy %d: %s", m, names[kind]);
        faults_++;
        fault_gen_ = gen_;
        switch (kind) {
            case 0: w.serial += 1000; counters_["fault_serial"]++; break;
            case 1: for (int& k : w.stock) k = 0; counters_["fault_stock"]++; break;
            case 2: w.Make(); counters_["fault_split"]++; break;
            case 4: {  // at one frame on every joiner, as the owner's (apart from the session's first frame on both)
                Cmd c;
                c.type = CJoinersApart;
                HostCommand(c);
                counters_["fault_all_serial"]++;
                break;
            }
            default: {
                int a = w.head, b = a ? w.slots[a].next : 0;
                if (a && b) {
                    w.slots[a].next = w.slots[b].next;
                    w.slots[b].next = a;
                    w.head = b;
                }
                counters_["fault_freelist"]++;
                break;
            }
        }
    }

    // ---- sessions and reloads
    void StartSession(bool first) {
        gen_++;
        for (Copy& c : copies_) {
            c.frame = 0;
            c.ring.clear();
            c.stream.clear();
            c.last_delivery = 0;
            c.shop = false;
            c.my_shop = false;
            c.shop_made.clear();
            c.held_cmds.clear();
            c.held = false;
            c.stock_in = session::StockAssembler();
            c.frames.clear();
            c.worlds.clear();
        }
        schedule_.clear();
        pending_.clear();
        polls_.clear();
        any_shop_ = false;
        host_shop_busy_until_ = 0;
        fix_pending_ = false;
        judge_.NewSession();
        host_frames_.clear();
        // A load closes every screen (DropPrivateScreens); a session's start clears every lock (ClearNpcLocks)
        talk_owner_.clear();
        for (Copy& c : copies_) {
            c.talking = c.talk_held = c.conv_wait = c.talk_pending = false;
            if (!bugs_.count("no_lock_clear")) c.npc_lock.clear();
        }
        if (!bugs_.count("no_lock_clear")) host_locks_.Clear();
        host_hold_npc_ = 0;
        done_due_.clear();
        save_done_due_.clear();
        save_watches_.clear();
        for (Copy& c : copies_) {
            c.save_requested = c.save_hold = c.save_mine = false;
            c.save_saver = -1;
            c.save_gone_since = 0, c.save_gone_said = false;
            c.save_done_sent = 0, c.early_saver = -1, c.cancelled.clear(), c.cancelled_next = 0, c.save_stuck_said = false;
        }
        gone_.clear();
        // What the host sends at every session's start (settings_sync.inc), acting at the same frame everywhere.
        if (!bugs_.count("no_serial_sync")) {
            Cmd c;
            c.type = CSerial;
            c.value = session::SessionSerial(copies_[0].w.serial);
            schedule_[3].push_back(c);
        }
        if (!bugs_.count("no_stock_sync")) SendStock(3);
        (void)first;
    }

    void StartReload(bool drift_reload, const char* why) {
        (void)why;
        if (reload_pending_ || loading_until_) return;
        reload_pending_ = true;
        reload_counts_ = drift_reload;
        resync_wait_ = 0;
    }

    void ReloadStep() {
        if (!reload_pending_) return;
        // HostResyncStep: the save waits for the host's shop (session_sync.h).
        bool busy = HostShopBusy();
        if (!resync_wait_ && busy) resync_wait_ = now_;
        session::Gate g = bugs_.count("no_resync_gate") ? session::Gate::Save
                                                         : session::ResyncGate(busy, false, resync_wait_ ? static_cast<uint32_t>(now_ - resync_wait_) : 0);
        if (g == session::Gate::Wait) return;
        Trace("the host saves for a %s", reload_counts_ ? "drift reload" : "reload");
        if (busy) Violate("saved_during_shop", "a re-sync saved while a shop was open (the merchant's inventory is set aside)");
        resync_wait_ = 0;
        reload_pending_ = false;
        if (reload_counts_) drift_reloads_++;
        counters_["reloads"]++;
        // Everyone loads the host's save: entities and inventories; not the serial, not the stock (pessimistic).
        World saved = copies_[0].w;
        for (Copy& c : copies_) {
            if (!c.connected) continue;
            uint32_t serial = c.w.serial;
            int stock[kKinds];
            std::copy(c.w.stock, c.w.stock + kKinds, stock);
            c.w = saved;
            c.w.serial = serial;
            std::copy(stock, stock + kKinds, c.w.stock);
            c.loading = true;
            c.loaded = true;
        }
        loading_until_ = now_ + kLoadMs + link_.maxLatencyMs * 2;
    }

    // ---- the host's view of shops (ShopBusy): from the open frame to the frame after the close went round
    bool HostShopBusy() const { return copies_[0].shop || copies_[0].frame < host_shop_busy_until_; }

    // ---- one command at its frame, on one copy
    void Execute(int m, const Cmd& c) {
        Copy& cp = copies_[m];
        World& w = cp.w;
        switch (c.type) {
            case CMove:
                w.pos[c.a][0] = c.b % 1000, w.pos[c.a][1] = c.c % 1000;
                break;
            case CGive:
                if (cp.shop) {  // HoldForShop: item moves wait for the shop's close
                    cp.held_cmds.push_back(c);
                    break;
                }
                if (w.inv[c.a][c.c] > 0) w.inv[c.a][c.c]--, w.inv[c.b][c.c]++;
                break;
            case CShopOpen: {
                if (m == 0) Trace("shop of copy %d opens at frame %d", c.b, cp.frame);
                if (m != 0) {  // made from this copy's own list: it must be the host's (her night: 0 items, the host's N)
                    auto hw = copies_[0].worlds.find(cp.frame - 1);   // apart 10 s or more: the drift check had time
                    if (hw != copies_[0].worlds.end() && hw->second.StockHash() != w.StockHash() && cp.stock_apart_since &&
                        cp.stock_apart_frames >= 143)
                        Violate("shop_stock_apart", "copy " + std::to_string(m) + " makes the shop from another stock than the host's (" +
                                                        std::to_string(StockItems(w)) + " items here, " +
                                                        std::to_string(StockItems(hw->second)) + " on the host)");
                }
                if (m == 0 && !copies_[c.b].connected) pending_abandon_ = true;  // gone before its shop opened
                cp.shop = true;
                cp.shop_shopper = c.b;
                cp.shop_buyer = c.a;
                cp.shop_made.clear();
                for (int k = 0; k < kKinds; k++)
                    if (w.stock[k] > 0) cp.shop_made.push_back(w.Make());  // the stock, from this copy's own list
                for (int i = 0; i < 3; i++) cp.shop_made.push_back(w.Make());  // the offer inventories
                cp.shop_split = 0;
                if (m == c.b) {  // the shopper: its screen, a stack split for the offer table (this copy only)
                    cp.my_shop = true;
                    cp.shop_close_at = now_ + static_cast<uint64_t>(c.c) * 1000;
                    cp.shop_split = w.Make();
                    for (int k = 0; k < kKinds; k++)  // it buys the first thing in stock
                        if (w.stock[k] > 0) {
                            w.stock[k]--, w.inv[c.a][k]++;
                            break;
                        }
                }
                if (m == 0) host_shop_busy_until_ = INT32_MAX;
                break;
            }
            case CShopClose: {
                if (!cp.shop) break;
                bool abandoned = c.c == 1;
                if (cp.shop_split) w.Destroy(cp.shop_split);  // freed before the deal is measured
                cp.shop_split = 0;
                if (m != c.origin && !abandoned) {  // the others take the deal
                    std::copy(c.inv, c.inv + kKinds, w.inv[c.a]);
                    std::copy(c.stock, c.stock + kKinds, w.stock);
                }
                if (m != c.origin && !abandoned && !c.seqs.empty()) {  // and its table bookkeeping, measured before the
                    for (int i = 0; i < kSlots; i++)                   // stock went (the close below frees it alike)
                        if (!w.slots[i].used) w.slots[i].seq = c.seqs[i], w.slots[i].next = c.nexts[i];
                    w.head = c.head;
                    w.serial = c.value;
                }
                logApply(m, (static_cast<uint64_t>(gen_) << 40) | c.id, 0x200, 0, c.id, c.origin, ApplyKind::Holder, true, static_cast<uint32_t>(gen_), "deal");
                for (auto it = cp.shop_made.rbegin(); it != cp.shop_made.rend(); ++it) w.Destroy(*it);
                cp.shop_made.clear();
                cp.shop = false;
                cp.my_shop = false;
                cp.run_held = true;  // the frame after: the waiting item moves
                if (m == 0) {
                    Trace("shop of copy %d closes at frame %d", c.origin, cp.frame);
                    host_shop_busy_until_ = cp.frame + 1;
                    any_shop_ = false;
                }
                break;
            }
            case CFixHold:
                cp.held = true;
                cp.held_since = now_;
                if (m == 0) {
                    fix_hold_seen_ = true;
                    fix_frame_ = cp.frame;
                    fix_snapshot_ = true;  // the values from the end of this frame (the hold is from its end)
                    for (int j = 1; j < n_; j++)
                        if (copies_[j].connected) fix_due_[j] = now_ + Latency();
                    fix_release_host_at_ = now_ + link_.maxLatencyMs;
                }
                break;
            case CSerial:
                w.serial = c.value;
                break;
            case CTalk: {
                bool cancelled = false;
                for (auto& e : cp.cancelled) cancelled |= e.first == c.a && now_ < e.second;
                if (m != c.b && cancelled) {  // TalkCancelled: its talker is gone
                    if (m == 0) Trace("copy %d's conversation with NPC %d is over already (its talker is gone)", c.b, c.a);
                    break;
                }
                cp.npc_lock[c.a] = c.b;
                if (m == c.b) cp.talk_pending = false;
                if (m == c.b && cp.connected) {
                    for (int j = 0; j < n_; j++)  // the talker's own conversation opens: nobody else may be in one with them
                        if (j != m && TalkActiveBy(c.a, j))
                            Violate("two_talkers", "NPC " + std::to_string(c.a) + ": copies " + std::to_string(j) + " and " +
                                                       std::to_string(m) + " both talk to them");
                    cp.talking = true, cp.talk_npc = c.a, cp.talk_until = now_ + static_cast<uint64_t>(c.c) * 1000;
                    cp.talk_held = Held(c.a);
                } else if (Held(c.a)) {
                    cp.conv_wait = true;
                    if (m == 0) host_hold_npc_ = c.a, host_hold_since_ = now_;
                }
                if (m == 0)
                    Trace("copy %d's conversation with NPC %d opens at frame %d%s", c.b, c.a, cp.frame, Held(c.a) ? " (everyone waits)" : "");
                if (m == 0 && gone_.count(c.b) && !bugs_.count("no_cancel_mark") && TalkOwner(c.a) == c.b) {  // (a second done is harmless)  // CloseTalkOfGone
                    host_locks_.Free(static_cast<uint16_t>(c.a));
                    talk_owner_.erase(c.a);
                    Trace("the talker of NPC %d is gone: the host ends its conversation at once", c.a);
                    for (int j = 0; j < n_; j++)
                        if (copies_[j].connected) done_due_.push_back({j == 0 ? now_ : now_ + Latency(), j, c.a, c.b, true});
                }
                break;
            }
            case CTalkDone:  // a trader's: at its frame
                FreeNpc(m, c.a, c.b);
                break;
            case CJoinersApart:
                if (m != 0) w.serial += 700;
                if (m == 0) {
                    Trace("every joiner's serial runs ahead from frame %d", cp.frame);
                    apart_watch_ = now_, apart_gen_ = gen_;
                }
                break;
            case CSaveHold:  // SaveHoldAtFrame, the same frame on every copy
                if (cp.save_hold) {  // another save's hold acts already: this one is not served
                    if (m == c.a && cp.save_requested) {
                        cp.save_requested = false;
                        Trace("copy %d: another player is saving", m);
                    }
                    break;
                }
                cp.save_hold = true;
                cp.save_saver = c.a;
                cp.save_mine = m == c.a && cp.save_requested;
                if (cp.save_mine) cp.save_requested = false;
                cp.save_hold_frame = cp.frame;
                cp.save_hold_since = now_;
                cp.save_gone_since = 0, cp.save_gone_said = false;
                if (cp.save_mine) cp.save_done_at = now_ + static_cast<uint64_t>(c.c), cp.save_done_sent = 0;
                if (m == 0) {
                    save_watches_.push_back({gen_, cp.frame + 10, c.value});
                    Trace("every copy holds after frame %d (copy %d saves: %s)", cp.frame, c.a, c.b == 1 ? "a quick-save" : "the save screen");
                }
                if (m == 0 && gone_.count(c.a) && !bugs_.count("no_gone_saver_at_hold")) {  // the saver left before this hold acted
                    Trace("the saver of this hold is gone already: the host ends the save for everyone");
                    for (int j = 0; j < n_; j++)
                        if (copies_[j].connected) save_done_due_.push_back({j == 0 ? now_ : now_ + Latency(), j, c.a, false});
                }
                if (!cp.save_mine && cp.early_saver == c.a && now_ - cp.early_tick < 30000) {  // its done came first
                    cp.early_saver = -1;
                    if (cp.early_saved && !bugs_.count("save_alone")) WriteEffect(cp.w);
                    ReleaseSave(m, "the saver's done came first");
                }
                break;
            case CStock:
                if (cp.stock_in.Feed(c.a, c.b, c.payload)) {
                    int k = 0;
                    session::ForEachStockLine(cp.stock_in.text, [&](const char*, int count) {
                        if (k < kKinds) w.stock[k++] = count;
                    });
                }
                break;
        }
    }

    // What a save changes in the running game of the copy that writes it
    static void WriteEffect(World& w) {
        w.serial += 3;
        w.Destroy(w.Make());
    }
    void ReleaseSave(int m, const char* why) {
        Copy& c = copies_[m];
        if (!c.save_hold) return;
        c.save_hold = false;
        c.save_mine = false;
        if (m == 0) judge_.CorrectionDone(gen_, c.save_hold_frame);
        Trace("copy %d goes on after the save (%s)", m, why);
    }
    struct SaveDone {
        uint64_t at;
        int m, saver;
        bool saved;  // r[5] & 1: the saver wrote a save, ours follows
    };
    struct SaveWatch {
        long gen;
        int frame;
        uint32_t faults;
    };

    static bool Held(int npc) { return npc != 301; }
    static int StockItems(const World& w) {
        int n = 0;
        for (int k = 0; k < kKinds; k++) n += w.stock[k] > 0 ? w.stock[k] : 0;
        return n;
    }
    int TalkOwner(int npc) const {  // the host's note of who claimed it (NoteTalkOwner)
        auto it = talk_owner_.find(npc);
        return it == talk_owner_.end() ? -1 : it->second;
    }
    bool TalkActiveBy(int npc, int talker) const {
        return talker >= 0 && talker < n_ && copies_[talker].connected && copies_[talker].talking && copies_[talker].talk_npc == npc;
    }
    bool TalkActive(int npc) const {
        for (int m = 0; m < n_; m++)
            if (TalkActiveBy(npc, m)) return true;
        return false;
    }
    // Taken: someone talks to it on some copy, or the host has passed someone's talk to it (it opens a little later on
    // the copies that lag behind).
    bool TalkTaken(int npc) const {
        for (const Copy& c : copies_)
            if (c.connected && c.talk_pending && c.pending_npc == npc) return true;  // a talk on its way
        return TalkActive(npc) || host_locks_.Held(static_cast<uint16_t>(npc), static_cast<uint32_t>(now_));
    }
    void FreeNpc(int m, int npc, int talker, bool gone = false) {  // the talker's done reached copy m
        Copy& c = copies_[m];
        if (gone && !bugs_.count("no_cancel_mark")) {
            size_t slots = bugs_.count("one_cancel_slot") ? 1 : 8;
            if (c.cancelled.size() < slots) c.cancelled.resize(slots, {0, 0});
            c.cancelled[c.cancelled_next] = {npc, now_ + 10000};
            c.cancelled_next = (c.cancelled_next + 1) % static_cast<int>(slots);
        }
        auto l = c.npc_lock.find(npc);
        if (l != c.npc_lock.end() && l->second == talker) c.npc_lock.erase(l);
        if (c.conv_wait) {  // goes on once no held conversation of another copy is left
            bool other = false;
            for (auto& l : c.npc_lock) other |= Held(l.first) && l.second != m;
            if (!other) c.conv_wait = false;
        }
        if (m == 0 && host_hold_npc_ == npc) host_hold_npc_ = 0;
    }
    struct DoneDue {
        uint64_t at;
        int m, npc, talker;
        bool gone;  // r[21] = kTalkerGone
    };

    // SendStock: the host's list in records, acting at one frame on every copy (at a session's start, and on a drift of
    // the stock alone: Action::Restock)
    void SendStock(int at_frame) {
        char text[session::kStockMaxText + 1];
        int n = copies_[0].w.StockText(text, sizeof(text));
        int chunks = session::StockChunks(n);
        for (int i = 0; i < chunks; i++) {
            Cmd c;
            c.type = CStock, c.a = i, c.b = chunks;
            session::StockChunk(text, n, i, c.payload);
            c.id = ++next_id_;
            schedule_[at_frame].push_back(c);
        }
    }

    void CloseAbandoned() {  // CloseAbandonedShop: the host's empty close, at a frame
        if (bugs_.count("no_abandoned_close")) return;
        Trace("the host closes the gone shopper's shop");
        Cmd c;
        c.type = CShopClose, c.c = 1, c.origin = 0;
        HostCommand(c);
    }

    void StepFrame(int m, const std::vector<Cmd>& cmds) {
        Copy& c = copies_[m];
        c.frame++;
        if (c.run_held) {
            c.run_held = false;
            std::vector<Cmd> held;
            held.swap(c.held_cmds);
            for (const Cmd& h : held) Execute(m, h);
        }
        for (const Cmd& cmd : cmds) Execute(m, cmd);
        if (m == 0 && fix_snapshot_) {
            fix_snapshot_ = false;
            fix_world_ = c.w;
        }
        if (m == 0 && pending_abandon_) {
            pending_abandon_ = false;
            CloseAbandoned();
        }
        c.worlds[c.frame] = c.w;
        if (c.worlds.size() > 400) c.worlds.erase(c.worlds.begin());
        if (m == 0) {
            host_frames_[c.frame] = HashesOf(c.w, HostShopBusy() || c.shop);
            if (host_frames_.size() > 600) host_frames_.erase(host_frames_.begin());
        } else {
            auto hw = copies_[0].worlds.find(c.frame);  // since when this copy's stock is not the host's
            if (hw != copies_[0].worlds.end() && hw->second.StockHash() != c.w.StockHash()) {
                if (!c.stock_apart_since) c.stock_apart_since = now_;
                c.stock_apart_frames++;
            } else if (hw != copies_[0].worlds.end()) {
                c.stock_apart_since = 0, c.stock_apart_frames = 0;
            }
            c.frames[c.frame] = HashesOf(c.w, false);
            if (c.frames.size() > 600) c.frames.erase(c.frames.begin());
        }
        if (m != 0 && c.frame == 10 && fault_gen_ != gen_) {  // a session's start leaves every copy as the host
            auto hf = host_frames_.find(10);
            if (hf != host_frames_.end() && !hf->second.shop && !(HashesOf(c.w, false) == hf->second))
                Violate("apart_after_load", "copy " + std::to_string(m) + " differs from the host 10 frames into a session" +
                                                Diff(HashesOf(c.w, false), hf->second));
        }
        if (m != 0) {  // apart_after_save: 10 frames after a save's hold, the same as the host (no fault since it was asked)
            for (const SaveWatch& sw : save_watches_) {
                if (sw.gen != gen_ || sw.frame != c.frame || sw.faults != faults_) continue;
                auto hf = host_frames_.find(c.frame);
                auto h0 = host_frames_.find(sw.frame - 10), c0 = c.frames.find(sw.frame - 10);  // apart before the save: not its doing
                if (h0 != host_frames_.end() && c0 != c.frames.end() && !(c0->second == h0->second)) continue;
                if (hf != host_frames_.end() && !hf->second.shop && !(HashesOf(c.w, false) == hf->second))
                    Violate("apart_after_save", "copy " + std::to_string(m) + " differs from the host 10 frames after a save's hold" +
                                                    Diff(HashesOf(c.w, false), hf->second) + Detail(m, c.frame));
            }
        }
        if (c.frame % drift::kEvery == 0) {
            drift::Rec r = {gen_, c.frame, c.w.PosHash(), c.w.TableHash(), c.w.UsedHash(), c.w.InvHash(), 1,
                            bugs_.count("no_stock_check") ? 0u : c.w.StockHash(), m == 0 && HostShopBusy()};
            c.ring.push_back(r);
            if (c.ring.size() > 8) c.ring.erase(c.ring.begin());
        }
    }

    static std::string Diff(const Hashes& a, const Hashes& b) {
        std::string d;
        if (a.pos != b.pos) d += " positions";
        if (a.table != b.table) d += " free-list/serial";
        if (a.used != b.used) d += " entities";
        if (a.inv != b.inv) d += " inventories";
        if (a.stock != b.stock) d += " stock";
        return d.empty() ? "" : ":" + d;
    }

    std::string DriftDiff() const {  // what really differed at the drift's frame, and whether that frame was a shop's
        if (drift_from_ < 0) return "";
        auto h = host_frames_.find(drift_frame_);
        auto j = copies_[drift_from_].frames.find(drift_frame_);
        if (h == host_frames_.end() || j == copies_[drift_from_].frames.end()) return ", not kept";
        Hashes mine = j->second;
        mine.stock = h->second.stock;
        std::string d = Diff(mine, h->second);
        return (h->second.shop ? ", in a shop" : "") + (d.empty() ? std::string(", the same") : d) + Detail(drift_from_, drift_frame_);
    }

    // The first thing that differs between copy m and the host at a frame, and the first frame it differed at.
    static std::string FirstDiff(const World& a, const World& b) {
        char d[120] = "";
        for (int x = 0; x < kActors && !d[0]; x++)
            for (int k = 0; k < kKinds && !d[0]; k++)
                if (a.inv[x][k] != b.inv[x][k]) snprintf(d, sizeof(d), "actor %d item %d: %d here, %d on the host", x, k, a.inv[x][k], b.inv[x][k]);
        if (!d[0] && a.serial != b.serial) snprintf(d, sizeof(d), "serial %u here, %u on the host", a.serial, b.serial);
        if (!d[0] && a.head != b.head) snprintf(d, sizeof(d), "free head %d here, %d on the host", a.head, b.head);
        for (int i = 1; i < kSlots && !d[0]; i++)
            if (a.slots[i].used != b.slots[i].used || a.slots[i].seq != b.slots[i].seq || (!a.slots[i].used && a.slots[i].next != b.slots[i].next))
                snprintf(d, sizeof(d), "slot %d: used %d seq %d next %d here, used %d seq %d next %d on the host", i, a.slots[i].used,
                         a.slots[i].seq, a.slots[i].next, b.slots[i].used, b.slots[i].seq, b.slots[i].next);
        for (int x = 0; x < kActors && !d[0]; x++)
            if (a.pos[x][0] != b.pos[x][0] || a.pos[x][1] != b.pos[x][1]) snprintf(d, sizeof(d), "actor %d position", x);
        return d;
    }
    std::string Detail(int m, int frame) const {
        const Copy& c = copies_[m];
        auto mine = c.worlds.find(frame);
        auto host = copies_[0].worlds.find(frame);
        if (mine == c.worlds.end() || host == copies_[0].worlds.end()) return "";
        std::string what = FirstDiff(mine->second, host->second);
        if (what.empty()) return "";
        // back to where that same field (the first difference's subject, before the colon) parted
        std::string field = what.substr(0, what.find(':'));
        int since = frame;
        for (int f = frame - 1; f > 0; f--) {
            auto a = c.worlds.find(f), b = copies_[0].worlds.find(f);
            if (a == c.worlds.end() || b == copies_[0].worlds.end()) break;
            if (FirstDiff(a->second, b->second).substr(0, field.size()) != field) break;
            since = f;
        }
        return " [" + what + ", since frame " + std::to_string(since) + "]";
    }

    // At the host's decision, from the drift that led to it (the joiner and frame of its second differing check):
    // needless when its evidence was bad: a check from a shop's frames (the shopper's own split stack and purchase are
    // its alone then), or a report older than the correction already under way, of what a correction mends (the
    // owner's night: the second joiner's report reloaded everyone). Something else mending the copy while the host
    // waited (a shop's deal, a second fault undoing the first) is not the decision's fault: the host can't know.
    bool RepairNeeded(uint8_t kinds) const {
        if (drift_from_ < 0) return true;
        if (fix_gen_seen_ == gen_ && drift_frame_ <= fix_start_frame_ && !(kinds & drift::kEntities)) return false;
        auto h = host_frames_.find(drift_frame_);
        return h == host_frames_.end() || !h->second.shop;
    }

    void Step() {
        // loading: everyone starts the new session together
        if (loading_until_ && now_ >= loading_until_) {
            loading_until_ = 0;
            for (Copy& c : copies_)
                if (c.loaded) c.loading = false, c.loaded = false;  // a joiner that came during the load waits for the next
            StartSession(false);
            Trace("session %ld starts", gen_);
            final_reload_ = false;
            if (join_again_) {
                join_again_ = false;
                StartReload(false, "a joiner arrived during a reload");
            }
        }
        if (loading_until_) return;
        ReloadStep();
        if (loading_until_) return;

        // commands reaching the host
        for (size_t i = 0; i < pending_.size();) {
            if (pending_[i].cmd.origin != 0 && !copies_[pending_[i].cmd.origin].connected) {  // sent by a copy since gone:
                pending_.erase(pending_.begin() + i);                                           // lost with its connection
                continue;
            }
            if (pending_[i].at > now_) {
                ++i;
                continue;
            }
            HostCommand(pending_[i].cmd);
            pending_.erase(pending_.begin() + i);
        }
        // the shopper leaves its shop: the deal goes out
        for (int m = 0; m < n_; m++) {
            Copy& c = copies_[m];
            if (c.connected && c.my_shop && c.shop && now_ >= c.shop_close_at) {  // a copy that left does nothing
                c.my_shop = false;
                Cmd d;
                d.type = CShopClose, d.a = c.shop_buyer;
                if (c.shop_split) c.w.Destroy(c.shop_split), c.shop_split = 0;  // freed before the deal is measured
                for (int i = 0; i < kSlots; i++) d.seqs.push_back(c.w.slots[i].seq), d.nexts.push_back(c.w.slots[i].next);
                d.head = c.w.head;
                d.value = c.w.serial;
                std::copy(c.w.inv[c.shop_buyer], c.w.inv[c.shop_buyer] + kKinds, d.inv);
                std::copy(c.w.stock, c.w.stock + kKinds, d.stock);
                Request(m, d);
            }
        }
        // conversations: the talker is done
        for (int m = 0; m < n_; m++) {
            Copy& c = copies_[m];
            if (!c.connected || !c.talking || now_ < c.talk_until) continue;
            c.talking = false;
            bool alone = m == 0 && connectedCount() == 1;
            if (!(alone && bugs_.count("no_solo_release"))) c.talk_held = false;  // ReleaseTalkerHold (0.10.5: not alone)
            Trace("copy %d is done talking to NPC %d", m, c.talk_npc);
            Cmd d;
            d.type = CTalkDone, d.a = c.talk_npc, d.b = m;
            Request(m, d);
        }
        for (size_t i = 0; i < done_due_.size();) {
            if (done_due_[i].at > now_) {
                ++i;
                continue;
            }
            DoneDue d = done_due_[i];
            done_due_.erase(done_due_.begin() + i);
            if (copies_[d.m].connected) FreeNpc(d.m, d.npc, d.talker, d.gone);
        }
        // saves: the saver is done (the write changes its game); its done goes to everyone, as it arrives
        for (int m = 0; m < n_; m++) {
            Copy& c = copies_[m];
            if (!c.connected || !c.save_hold || !c.save_mine || now_ < c.save_done_at) continue;
            WriteEffect(c.w);
            c.save_done_sent = now_;
            Trace("copy %d's save is done", m);
            for (int j = 0; j < n_; j++)
                if (j != m && copies_[j].connected) save_done_due_.push_back({j == 0 ? now_ : now_ + Latency(), j, m, true});
            ReleaseSave(m, "it saved");
        }
        for (size_t i = 0; i < save_done_due_.size();) {
            if (save_done_due_[i].at > now_) {
                ++i;
                continue;
            }
            SaveDone d = save_done_due_[i];
            save_done_due_.erase(save_done_due_.begin() + i);
            Copy& c = copies_[d.m];
            if (!c.connected || d.saver == d.m) continue;
            if (!c.save_hold) {  // SaveDoneArrived: before this copy's hold frame, kept for it
                if (!bugs_.count("no_early_save_done")) {
                    c.early_saver = d.saver, c.early_saved = d.saved, c.early_tick = now_;
                    Trace("copy %d: the saver's done came before its hold: kept", d.m);
                }
                continue;
            }
            if (c.save_saver != d.saver) continue;
            if (d.saved && !bugs_.count("save_alone")) WriteEffect(c.w);  // save_alone: only the saver wrote
            ReleaseSave(d.m, d.saved ? "the saver is done" : "the saver left: nothing saved");
        }
        for (int m = 0; m < n_; m++) {
            Copy& c = copies_[m];
            if (c.connected && c.save_hold && !c.save_mine && now_ - c.save_hold_since > kSaveHoldMaxMs) ReleaseSave(m, "the saver went quiet");
            // the saver is gone: nothing but those 10 minutes ends the wait
            if (c.connected && c.save_hold && !c.save_mine && c.save_saver >= 0 && !copies_[c.save_saver].connected) {
                if (!c.save_gone_since) c.save_gone_since = now_;
                if (now_ - c.save_gone_since > 5000 && !c.save_gone_said) {
                    c.save_gone_said = true;
                    Violate("waits_for_gone", "copy " + std::to_string(m) + " has waited 5 s for the save of copy " +
                                                  std::to_string(c.save_saver) + ", which is gone");
                }
            } else {
                c.save_gone_since = 0, c.save_gone_said = false;
            }
        }
        // a save held 20 s after its saver (connected) has sent its done: the done was lost on the way
        for (int m = 0; m < n_; m++) {
            Copy& c = copies_[m];
            if (!c.connected || !c.save_hold || c.save_mine || c.save_saver < 0 || !copies_[c.save_saver].connected) continue;
            uint64_t done = copies_[c.save_saver].save_done_sent;
            if (done && now_ - std::max(done, c.save_hold_since) > 20000 && !c.save_stuck_said) {
                c.save_stuck_said = true;
                Violate("save_hold_stuck", "copy " + std::to_string(m) + " holds 20 s for the save of copy " +
                                               std::to_string(c.save_saver) + ", which is connected and done");
            }
        }
        // A copy waiting for a conversation whose talker is gone: nothing but 10 minutes would end it (a reload hides it)
        for (int m = 0; m < n_; m++) {
            Copy& c = copies_[m];
            int talker = -1;
            if (c.connected && c.conv_wait)
                for (auto& l : c.npc_lock)
                    if (Held(l.first) && l.second != m) talker = l.second;
            if (talker >= 0 && !copies_[talker].connected) {
                if (!c.gone_wait_since) c.gone_wait_since = now_;
                if (now_ - c.gone_wait_since > 5000 && !c.gone_wait_said) {
                    c.gone_wait_said = true;
                    Violate("waits_for_gone", "copy " + std::to_string(m) + " has waited 5 s for the conversation of copy " +
                                                  std::to_string(talker) + ", which is gone");
                }
            } else {
                c.gone_wait_since = 0, c.gone_wait_said = false;
            }
        }
        if (host_hold_npc_) {  // HostHoldTimeout: every joiner gone, or 10 minutes
            bool alone = connectedCount() == 1;
            if (alone || now_ - host_hold_since_ > talk::kLockMaxMs) {
                Trace("the host stops waiting for NPC %d's conversation (%s)", host_hold_npc_,
                      alone ? "the other player left" : "waited too long");
                copies_[0].conv_wait = false;
                host_hold_npc_ = 0;
            }
        }
        // the host: a frame every kFrameMs unless held
        Copy& h = copies_[0];
        if (h.held && fix_release_host_at_ && now_ >= fix_release_host_at_) {
            h.held = false;
            fix_release_host_at_ = 0;
            judge_.CorrectionDone(gen_, fix_frame_);
        }
        if (!h.held && !h.conv_wait && !h.talk_held && !h.save_hold && now_ >= next_host_frame_) {
            next_host_frame_ = now_ + kFrameMs;
            std::vector<Cmd> cmds;
            auto it = schedule_.find(h.frame + 1);
            if (it != schedule_.end()) {
                cmds = it->second;
                schedule_.erase(it);
            }
            StepFrame(0, cmds);
            for (int m = 1; m < n_; m++) {
                Copy& c = copies_[m];
                if (!c.connected) continue;
                uint64_t at = std::max(c.last_delivery, now_ + Latency());
                c.last_delivery = at;
                c.stream.push_back({at, h.frame, gen_, cmds});
            }
        }
        // joiners: run the frames they have, unless held
        for (int m = 1; m < n_; m++) {
            Copy& c = copies_[m];
            if (!c.connected || c.loading) continue;
            if (c.held) {
                auto due = fix_due_.find(m);
                if (due != fix_due_.end() && now_ >= due->second) {  // the host's values: positions, free list, serial, inventories
                    World& w = c.w;
                    for (int a = 0; a < kActors; a++) {
                        std::copy(fix_world_.pos[a], fix_world_.pos[a] + 2, w.pos[a]);
                        std::copy(fix_world_.inv[a], fix_world_.inv[a] + kKinds, w.inv[a]);
                    }
                    w.serial = fix_world_.serial;
                    w.head = fix_world_.head;
                    for (int i = 1; i < kSlots; i++)
                        if (!w.slots[i].used && !fix_world_.slots[i].used) w.slots[i] = fix_world_.slots[i];
                    logApply(m, (static_cast<uint64_t>(gen_) << 40) | (0x8000000000ull) | fix_frame_, -1, 0, fix_frame_, 0, ApplyKind::Reset, true,
                             static_cast<uint32_t>(gen_), "correction");
                    fix_due_.erase(due);
                    c.held = false;
                    Trace("copy %d has the correction", m);
                } else if (now_ - c.held_since >= kFixGiveUpMs) {
                    Violate("frozen", "copy " + std::to_string(m) + " held 8 s for a correction that never came");
                    c.held = false;
                    fix_due_.erase(m);
                }
                if (c.held) continue;
            }
            while (!c.stream.empty() && c.stream.front().at <= now_ && !c.held && !c.conv_wait && !c.talk_held &&
                   !c.save_hold) {
                Delivery d = c.stream.front();
                c.stream.pop_front();
                if (d.gen != gen_) continue;
                StepFrame(m, d.cmds);
            }
            if (now_ >= c.next_poll) {  // the selection poll carries the last checks (each joiner on its own clock)
                c.next_poll = now_ + kPollMs - 20 + rng_.range(0, 40);
                polls_.push_back({now_ + Latency(), m, c.ring});
            }
        }
        if (fix_pending_ && fix_hold_seen_ && fix_due_.empty() && !h.held) fix_pending_ = fix_hold_seen_ = false;
        // the host's file server: joiners' checks
        for (size_t i = 0; i < polls_.size();) {
            if (polls_[i].at > now_) {
                ++i;
                continue;
            }
            Poll p = polls_[i];
            polls_.erase(polls_.begin() + i);
            if (!copies_[p.from].connected) continue;
            for (const drift::Rec& j : p.recs) {
                const drift::Rec* mine = nullptr;
                for (const drift::Rec& r : copies_[0].ring)
                    if (r.gen == j.gen && r.frame == j.frame) mine = &r;
                uint8_t kinds;
                drift::Verdict verdict = judge_.Compare(100 + p.from, gen_, j, mine, &kinds);
                if (race_at_ && p.from != race_from_ && j.frame == race_frame_ && bugs_.count("probe_race")) {
                    static const char* vn[] = {"old", "shop", "match", "first miss", "drifted"};
                    char d[200];
                    snprintf(d, sizeof(d), "the other joiner's check of frame %d came %llu ms after the correction was asked: %s%s%s",
                             j.frame, static_cast<unsigned long long>(now_ - race_at_), vn[static_cast<int>(verdict)],
                             fix_hold_seen_ ? ", the hold had acted" : ", before the hold acted", fix_pending_ ? "" : ", the correction over");
                    Violate("probe", d);
                    race_at_ = 0;
                }
                if (kinds) Trace("check of copy %d, frame %d differs (%s)", p.from, j.frame,
                                 verdict == drift::Verdict::Drifted ? "drifted" : "first miss");
                if (fix_pending_ && kinds) {
                    counters_["cov_differing_report_during_correction"]++;
                    if (bugs_.count("probe_late_report")) Violate("probe", "a differing report during a correction");
                }
                if (fix_pending_ && verdict == drift::Verdict::Drifted) counters_["cov_drift_during_correction"]++, late_drift_ = true;
                if (verdict == drift::Verdict::Drifted) drift_from_ = p.from, drift_frame_ = j.frame;
            }
        }
        // the host's main thread: act on a drift (DriftCheckHost)
        // As DriftCheckHost: busy with a shop, a re-sync, or its own correction's hold (g_fix_hold, from the hold's frame
        // on, not from when it was asked for: the window the owner's second report came in).
        bool busy = HostShopBusy() || reload_pending_ || h.held;
        uint8_t decided_kinds = judge_.reason_kinds;
        drift::Action a = judge_.Decide(static_cast<uint32_t>(now_), busy);
        if (late_drift_ && a != drift::Action::None && a != drift::Action::Wait) {
            static const char* names[] = {"none", "wait", "correct", "resync", "drop"};
            counters_[std::string("cov_after_late_drift_") + names[static_cast<int>(a)] +
                      ((decided_kinds & drift::kEntities) ? "_entities" : "")]++;
            late_drift_ = false;
        }
        if (apart_watch_ && (apart_gen_ != gen_ || a == drift::Action::Correct || a == drift::Action::Resync)) apart_watch_ = 0;
        if (apart_watch_ && now_ - apart_watch_ > 6000 && bugs_.count("probe_undetected")) {
            Violate("probe", "every joiner apart 6 s ago, and nothing done");
            apart_watch_ = 0;
        }
        bool needless = (a == drift::Action::Correct || a == drift::Action::Resync) && !RepairNeeded(decided_kinds);
        if (a == drift::Action::Wait && fix_pending_) counters_["cov_drift_waiting_for_correction"]++;
        if (a == drift::Action::Correct) {
            int apart = 0;
            for (int j = 1; j < n_; j++) {
                auto hf = host_frames_.find(copies_[j].frame);
                if (copies_[j].connected && !copies_[j].loading && hf != host_frames_.end() && !hf->second.shop &&
                    HashesOf(copies_[j].w, false).table != hf->second.table)
                    apart++;
            }
            if (apart >= 2) {
                counters_["cov_correction_with_two_apart"]++;
                race_at_ = now_, race_frame_ = drift_frame_, race_from_ = drift_from_;
            }
            Trace("drift (copy %d, frame %d%s): correction", drift_from_, drift_frame_, DriftDiff().c_str());
            if (needless) Violate("needless_repair", "a correction while every copy was the same as the host");
            fixes_++;
            counters_["corrections"]++;
            judge_.CorrectionStarted(gen_, h.frame);
            fix_gen_seen_ = gen_, fix_start_frame_ = h.frame;
            Cmd c;
            c.type = CFixHold;
            HostCommand(c);
            fix_pending_ = true;
        } else if (a == drift::Action::Resync) {
            Trace("drift (copy %d, frame %d%s): reload", drift_from_, drift_frame_, DriftDiff().c_str());
            if (needless) Violate("needless_repair", "a drift reload while every copy was the same as the host");
            counters_["drift_reloads"]++;
            StartReload(true, "a drift");
        } else if (a == drift::Action::Restock) {
            Trace("drift (copy %d, frame %d): the stock only: the host's list again", drift_from_, drift_frame_);
            counters_["restocks"]++;
            SendStock(copies_[0].frame + kExecDelay);
        } else if (a == drift::Action::Drop) {
            counters_["drift_dropped"]++;
        }
    }

    int n_;
    Rng rng_;
    std::set<std::string> bugs_;
    LinkParams link_;
    drift::Judge judge_;
    std::vector<Copy> copies_;
    std::map<int, std::vector<Cmd>> schedule_;  // the host's frame -> commands acting then
    std::vector<Pending> pending_;
    std::vector<Poll> polls_;
    std::map<int, uint64_t> fix_due_;
    World fix_world_;
    int fix_frame_ = 0;
    uint64_t fix_release_host_at_ = 0;
    bool fix_pending_ = false, fix_hold_seen_ = false;
    std::vector<Violation> violations_;
    std::map<int, Hashes> host_frames_;  // the host's hashes at each frame of this session
    long fault_gen_ = -1;
    int drift_from_ = -1, drift_frame_ = 0;
    bool pending_abandon_ = false;
    std::set<int> gone_;  // the host's note of the copies that left this session (g_gone_joiners)
    talk::HostLocks host_locks_;
    int host_hold_npc_ = 0;
    uint64_t host_hold_since_ = 0;
    std::vector<DoneDue> done_due_;
    std::vector<SaveDone> save_done_due_;
    std::vector<SaveWatch> save_watches_;
    std::map<int, int> talk_owner_;  // the host: NPC -> who claimed it
    bool late_drift_ = false;
    bool fix_snapshot_ = false;
    uint64_t race_at_ = 0, apart_watch_ = 0;
    long apart_gen_ = 0;  // probe_race: a correction asked while two joiners were apart
    int race_frame_ = 0, race_from_ = -1;
    long fix_gen_seen_ = -1;  // the last correction asked for: its generation and the host's frame then
    int fix_start_frame_ = 0;  // the joiner and frame of the drift the host acts on
    uint64_t now_ = 0, next_host_frame_ = 0;
    uint64_t loading_until_ = 0, resync_wait_ = 0;
    bool reload_pending_ = false, reload_counts_ = false, join_again_ = false;
    bool frozen_ = false, final_reload_ = false;
    bool any_shop_ = false;
    int host_shop_busy_until_ = 0;
    long gen_ = 0;
    uint32_t next_id_ = 0;
    uint64_t faults_ = 0, fixes_ = 0, drift_reloads_ = 0;
};

// probe_late_report is no bug: it reports the moment a differing check reaches the host during a correction, so the
// triage shows how that comes about.
const char* kBugs[] = {"no_shop_skip", "no_drop_before_fix", "no_serial_sync", "no_stock_sync", "no_resync_gate", "no_abandoned_close",
                       "probe_late_report", "probe_race", "probe_undetected",
                       "no_host_filter", "no_lock_clear", "no_solo_release", "no_abandoned_talk", "no_stock_check", "save_alone", "no_abandoned_save", "no_early_save_done", "no_cancel_mark", "one_cancel_slot", "no_gone_saver_at_hold"};

std::unique_ptr<ITarget> makeFt(const Scenario& s, std::string* err) {
    std::set<std::string> bugs;
    for (const std::string& b : s.bugs) {
        bool known = false;
        for (const char* k : kBugs) known |= b == k;
        if (!known) {
            if (err) *err = "unknown bug class for target ftcoop: " + b;
            return nullptr;
        }
        bugs.insert(b);
    }
    return std::make_unique<FtTarget>(s, bugs);
}

const TargetRegistrar kRegisterFt(TargetInfo{
    "ftcoop", "Fallout Tactics co-op on a fake world: lockstep, shops, drift checks, corrections, reloads, forced desyncs",
    {"no_shop_skip", "no_drop_before_fix", "no_serial_sync", "no_stock_sync", "no_resync_gate", "no_abandoned_close", "probe_late_report", "probe_race", "probe_undetected", "no_host_filter",
     "no_lock_clear", "no_solo_release", "no_abandoned_talk", "no_stock_check", "save_alone", "no_abandoned_save", "no_early_save_done", "no_cancel_mark", "one_cancel_slot", "no_gone_saver_at_hold"}, makeFt});

}  // namespace
}  // namespace coopfuzz
