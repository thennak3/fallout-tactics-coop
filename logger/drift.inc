// ---- Drift check: the host finds a joiner whose game has drifted out of step, and puts it right ----
// Included from netlog.cpp after dots.inc. Every copy hashes its game every kDriftEvery frames (at the client tick,
// the same point of the frame on every copy), one hash per part: positions and HP, the entity table's free list, which
// slots are used, inventories, character attributes. Joiners send their latest hashes with their selection poll
// (dots.inc, "SEL ... H<gen>:<frame>:<pos>:<table>:<used>:<inv>:<attr>,..."); the host compares them with its own.
// Two checked frames in a row that differ: the host is right (owner, 2026-10-23: "let's patch it automatically and
// assume the host is right"). If the same entities exist on both, every copy holds after one frame (kFixHold), the host
// sends what differed as it has it (positions, HP, inventories, attributes, the free list) and the others take it as it
// arrives, then everyone goes on. Other entities (something died on one copy only), or the same drift back within 10 s
// of a correction: a re-sync (quick-save, quick-load, everyone loads it), at most once a minute. What drifts: NPC shots
// at the squad landed on one copy only (chaos runs 2026-10-23: same random draws up to the shot, every time); a 0-damage
// hit still made effect entities on one copy only, and the free list's order parted from there.
// Like lockstep games' desync checks (a hash per tick, per part, so a mismatch names what broke) but cheaper: the game
// state we watch is small, so instead of the usual full reload the host's values are sent for the part that differs.
const int kDriftEvery = 25;
const int kDriftRing = 8;
const DWORD kDriftSpacingMs = 60000;
enum : uint8_t { kDriftPos = 1, kDriftTable = 2, kDriftInv = 4, kDriftAttr = 8, kDriftEntities = 16 };
struct DriftRec {
    long gen;
    int frame;
    uint32_t pos;    // every actor's position and HP
    uint32_t table;  // the free list (order, seq numbers) and the entity serial counter
    uint32_t used;   // which slots hold an entity, and their seq numbers: two copies that differ here made other things
    uint32_t inv;    // every inventory (actors and containers)
    uint32_t attr;   // every character's attributes (stats, skills, perks)
};
DriftRec g_drift_ring[kDriftRing] = {};
int g_drift_next = 0;
SRWLOCK g_drift_lock = SRWLOCK_INIT;
char g_drift_reason[96] = "";  // host: set by the file server thread, acted on by the main thread
uint8_t g_drift_kinds = 0;     // host: what differed (kDrift*), with the reason
const DWORD kDriftFixWindowMs = 10000;  // a drift back within this of a correction: a reload
DWORD g_drift_fix_tick = 0;  // the last correction
int g_drift_fix_frame = 0;   // comparisons start again after this frame, in this generation (every load restarts
long g_drift_fix_gen = -1;   // the frame count: kept across loads, one correction stopped every later comparison)
DWORD g_drift_last_resync = 0;
const DWORD kLockBlocksResyncMs = 120000;  // an NPC lock this old holds up no re-sync (a held host still does)
void StartWorldFix(uint8_t* world, uint8_t kinds);

// HP too (owner, 2026-10-23: "sounds like we need a regular use this ID or seed sync as well"): a hit that landed on
// one copy only (actor 683, frame 3742) left its HP 2 apart, it died at frame 3856 on one copy only, and its death's
// entities split the entity table from there.
uint32_t PosHash(uint8_t* world) {
    uint32_t h = 2166136261u;
    for (int i = 1; i < EntityCount(world); i++) {
        if (!IsActor(world, i)) continue;
        h = Fnv(h, EntityPos(world, static_cast<uint16_t>(i)), 12);
        int hp = ActorHp(world, static_cast<uint16_t>(i));
        h = Fnv(h, &hp, sizeof(hp));
        h = Fnv(h, &i, sizeof(i));
    }
    return h;
}

bool SlotFree(uint8_t* e, void* dummy, int i) {
    void* p = *reinterpret_cast<void**>(e + i * 8);
    return !p || p == dummy;
}

uint32_t TableHash(uint8_t* world) {
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    uint32_t h = Fnv(2166136261u, world + kWorldFreeHead, 2);
    h = Fnv(h, reinterpret_cast<void*>(kEntitySerial), 4);
    for (int i = 0; i < EntityCount(world); i++)
        if (SlotFree(e, dummy, i)) h = Fnv(Fnv(h, &i, sizeof(i)), e + i * 8 + 4, 4);  // next_free, seq
    return h;
}

uint32_t UsedHash(uint8_t* world) {
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    uint32_t h = 2166136261u;
    for (int i = 0; i < EntityCount(world); i++)
        if (!SlotFree(e, dummy, i)) h = Fnv(Fnv(h, &i, sizeof(i)), e + i * 8 + 6, 2);  // seq
    return h;
}

uint32_t InvHash(uint8_t* world) {
    uint32_t h = 2166136261u;
    char s[2048];
    for (int i = 1; i < EntityCount(world); i++) {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(EntityById(world, static_cast<uint16_t>(i)));
        if (vt != kActorVtable && vt != kContainerVtable) continue;
        if (InvString(world, static_cast<uint16_t>(i), s, sizeof(s))) h = Fnv(Fnv(h, &i, sizeof(i)), s, strlen(s));
        EquipString(world, static_cast<uint16_t>(i), s, sizeof(s));  // hands and armour too
        h = Fnv(h, s, strlen(s));
    }
    return h;
}

uint32_t AttrHash(uint8_t* world) {
    uint32_t h = 2166136261u;
    for (int i = 1; i < EntityCount(world); i++)
        if (IsActor(world, i)) h = Fnv(h, EntityById(world, static_cast<uint16_t>(i)) + kActorAttr, kActorAttrLen);
    return h;
}

// Main thread, every client tick.
void DriftFrame(uint8_t* world) {
    static int last = -1;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    if (frame == last || frame % kDriftEvery) return;
    last = frame;
    DriftRec r = {g_coop_host ? static_cast<long>(g_serve_gen) : g_loaded_gen, frame, PosHash(world), TableHash(world),
                  UsedHash(world), InvHash(world), AttrHash(world)};
    AcquireSRWLockExclusive(&g_drift_lock);
    g_drift_ring[g_drift_next] = r;
    g_drift_next = (g_drift_next + 1) % kDriftRing;
    ReleaseSRWLockExclusive(&g_drift_lock);
}

// Joiner: " H<gen>:<frame>:<pos>:<table>:<used>:<inv>:<attr>,..." for the selection poll.
int FormatDrift(char* out, size_t cap) {
    int n = sprintf_s(out, cap, " H");
    AcquireSRWLockShared(&g_drift_lock);
    for (const DriftRec& r : g_drift_ring)
        if (r.frame)
            n += sprintf_s(out + n, cap - n, "%ld:%d:%x:%x:%x:%x:%x,", r.gen, r.frame, r.pos, r.table, r.used, r.inv,
                           r.attr);
    ReleaseSRWLockShared(&g_drift_lock);
    return n;
}

void KindsText(uint8_t k, char* out, size_t cap) {
    out[0] = 0;
    const char* names[] = {"positions/HP", "free list", "inventories", "attributes", "entities"};
    for (int i = 0; i < 5; i++)
        if (k & (1 << i)) {
            if (out[0]) strcat_s(out, cap, ", ");
            strcat_s(out, cap, names[i]);
        }
}

// Host, file server thread: a joiner's hashes, from the "H" part of its poll.
void DriftFromJoiner(unsigned id, const char* name, const char* p) {
    struct Seen {
        unsigned id;
        long gen;  // last_frame belongs to this generation (a load restarts the frame count)
        int last_frame, misses;
        int same;    // matching checks since the last "matches" line (a silent check looks like none)
        DWORD said;
        uint8_t kinds;
    };
    static Seen seen[8] = {};
    Seen* s = nullptr;
    for (Seen& x : seen)
        if (x.id == id || (!s && !x.id)) s = &x;
    for (Seen& x : seen)
        if (x.id == id) s = &x;
    if (!s) return;
    s->id = id;
    while (*p && *p != 'H') p++;
    if (*p) p++;
    long gen;
    int frame, used;
    DriftRec j = {};
    while (sscanf_s(p, "%ld:%d:%x:%x:%x:%x:%x,%n", &gen, &frame, &j.pos, &j.table, &j.used, &j.inv, &j.attr, &used) == 7) {
        p += used;
        if (gen != g_serve_gen || (gen == g_drift_fix_gen && frame <= g_drift_fix_frame)) continue;
        if (s->gen != gen) s->gen = gen, s->last_frame = 0, s->misses = 0, s->kinds = 0;
        if (frame <= s->last_frame) continue;
        bool mine = false;
        DriftRec m = {};
        AcquireSRWLockShared(&g_drift_lock);
        for (const DriftRec& r : g_drift_ring)
            if (r.gen == gen && r.frame == frame) {
                mine = true;
                m = r;
            }
        ReleaseSRWLockShared(&g_drift_lock);
        if (!mine) continue;  // not ours yet (or long gone): compared on a later poll
        s->last_frame = frame;
        uint8_t k = (m.pos != j.pos ? kDriftPos : 0) | (m.table != j.table ? kDriftTable : 0) |
                    (m.used != j.used ? kDriftEntities : 0) | (m.inv != j.inv ? kDriftInv : 0) |
                    (m.attr != j.attr ? kDriftAttr : 0);
        if (!k) {
            s->misses = 0;
            s->kinds = 0;
            s->same++;
            if (GetTickCount() - s->said >= 60000) {
                Log("co-op: drift: %s's game matches ours (%d checks, last at frame %d, generation %ld)", name, s->same,
                    frame, gen);
                s->same = 0;
                s->said = GetTickCount();
            }
            continue;
        }
        char what[96];
        KindsText(k, what, sizeof(what));
        Log("co-op: drift: %s's game differs from ours at frame %d (generation %ld): %s", name, frame, gen, what);
        s->kinds |= k;
        if (++s->misses >= 2 && !g_drift_reason[0]) {
            g_drift_kinds = s->kinds;
            sprintf_s(g_drift_reason, "%s's game drifted out of step", name);
            s->misses = 0;
            s->kinds = 0;
        }
    }
}

// Host, main thread, every client tick: put a drifted joiner right when nobody is busy.
void DriftCheckHost() {
    extern bool g_private_open;
    if (!g_drift_reason[0] || g_private_open || g_resync_state != 0) return;
    for (const NpcLock& l : g_npc_locks)  // someone's conversation or shop is (about to be) open; a lock left over
        if (l.npc && GetTickCount() - l.since < kLockBlocksResyncMs) return;  // from a lost talk no longer blocks it
    DWORD now = GetTickCount();
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint8_t kinds = g_drift_kinds;
    g_drift_kinds = 0;
    // The correction, unless other entities exist on that copy (it can't make or destroy them) or the same drift is
    // back soon after one.
    if (world && !(kinds & kDriftEntities) && (!g_drift_fix_tick || now - g_drift_fix_tick > kDriftFixWindowMs)) {
        g_drift_reason[0] = 0;
        g_drift_fix_tick = now;
        StartWorldFix(world, kinds);
        return;
    }
    if (g_drift_last_resync && now - g_drift_last_resync < kDriftSpacingMs) {
        g_drift_reason[0] = 0;
        return;
    }
    static char reason[96];
    strcpy_s(reason, g_drift_reason);
    g_drift_reason[0] = 0;
    g_drift_last_resync = now;
    Notice("Co-op: %s - saving and reloading to bring everyone back together", reason);
    RequestResync(reason);
}

// ---- The correction (owner, 2026-10-22: "handle drift by force updating rather than reloading"). The host sends a
// kFixHold record; on every copy it acts at the same frame, and from the end of that frame no copy steps another (the
// world's step gate 0x635ce0 says no, at both of its calls in the update 0x4fbf50; nothing changes meanwhile, no
// effect entity comes or goes). The host then sends its values for what differed (marker 0xfffe, taken as it
// arrives); a joiner that has applied them, and the host once its own records came back, step on. The values are
// absolute: 'P' position, 'H' HP, 'N'+'I' every inventory, 'A' every character's attributes, 'F' the free list.
const uintptr_t kStepGate = 0x635ce0;                     // char __thiscall(Client*): may the world step a frame
const uintptr_t kStepGateCalls[] = {0x4fc060, 0x4fc0c5};  // before the time is added, and in the step loop
const DWORD kFixHoldMaxMs = 8000;
bool g_fix_hold = false;
bool g_fix_sent = false;
uint8_t g_fix_kinds = 0;
int g_fix_frame = 0;
DWORD g_fix_since = 0;

char __fastcall StepGateHook(void* client, void* edx) {
    if (g_fix_hold) return 0;
    return reinterpret_cast<char(__thiscall*)(void*)>(kStepGate)(client);
}

void StartWorldFix(uint8_t* world, uint8_t kinds) {
    if (g_fix_hold) return;
    uint8_t rec[1][27];
    CoopRecord(rec[0], kFixHold, 0);
    rec[0][5] = kinds;
    SendCoopRecords(rec, 1);
    char what[96];
    KindsText(kinds, what, sizeof(what));
    Log("co-op: drift: asking every copy to hold for a correction (%s) at frame %d", what,
        *reinterpret_cast<int*>(world + kWorldFrame));
}

void FixHoldAtFrame(const uint8_t* r) {  // CoopCommandAtFrame, the same frame on every copy
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world || g_fix_hold) return;
    g_fix_hold = true;
    g_fix_sent = false;
    g_fix_kinds = r[5];
    g_fix_frame = *reinterpret_cast<int*>(world + kWorldFrame);
    g_fix_since = GetTickCount();
    Log("co-op: drift: holding after frame %d for the host's correction", g_fix_frame);
}

void ReleaseFixHold(const char* why) {
    if (!g_fix_hold) return;
    g_fix_hold = false;
    g_drift_fix_frame = g_fix_frame + 1;  // the hashes made before it: not compared again
    g_drift_fix_gen = g_coop_host ? static_cast<long>(g_serve_gen) : g_loaded_gen;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world) *reinterpret_cast<float*>(world + 0x2e7) = *reinterpret_cast<float*>(world + 0x2f7);  // no frames owed
    Log("co-op: drift: going on (%s, held %lu ms)", why, GetTickCount() - g_fix_since);
}

void SendWorldFix(uint8_t* world, uint8_t kinds);

void FixHoldTick() {  // the mission screen's tick (no frames run while held)
    if (!g_fix_hold) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (g_coop_host && !g_fix_sent && world) {
        g_fix_sent = true;
        SendWorldFix(world, g_fix_kinds);
    }
    if (g_fix_hold && GetTickCount() - g_fix_since > kFixHoldMaxMs) ReleaseFixHold("waited too long for the correction");
}

// The free list from its head, in order, with each slot's seq; `term` is the last slot's next_free.
int ReadFreeOrder(uint8_t* world, uint16_t* slots, uint16_t* seqs, int max, uint16_t* term) {
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    int count = EntityCount(world), n = 0;
    uint16_t at = *reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
    *term = at;
    while (at < count && at != 0 && SlotFree(e, dummy, at) && n < max) {
        slots[n] = at;
        seqs[n++] = *reinterpret_cast<uint16_t*>(e + at * 8 + 6);
        at = *reinterpret_cast<uint16_t*>(e + at * 8 + 4);
        *term = at;
    }
    return n;
}

// 'F' on this copy: its free slots chained in the host's order (then any others it has free, lowest first), seq
// numbers and the serial counter as the host has them.
void ApplyFreeOrder(uint8_t* world, const uint8_t* p, int n, uint32_t serial, uint16_t term) {
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    int count = EntityCount(world);
    static uint16_t order[8192];
    static uint8_t placed[8192];
    int k = 0;
    memset(placed, 0, sizeof(placed));
    for (int i = 0; i < n; i++) {
        uint16_t slot, seq;
        memcpy(&slot, p + i * 4, 2);
        memcpy(&seq, p + i * 4 + 2, 2);
        if (slot >= count || slot >= 8192 || placed[slot] || !SlotFree(e, dummy, slot)) continue;
        *reinterpret_cast<uint16_t*>(e + slot * 8 + 6) = seq;
        placed[slot] = 1;
        order[k++] = slot;
    }
    int extra = 0;
    for (int i = 1; i < count && i < 8192; i++)
        if (!placed[i] && SlotFree(e, dummy, i)) {
            order[k++] = static_cast<uint16_t>(i);
            extra++;
        }
    for (int i = 0; i < k; i++)
        *reinterpret_cast<uint16_t*>(e + order[i] * 8 + 4) = i + 1 < k ? order[i + 1] : term;
    *reinterpret_cast<uint16_t*>(world + kWorldFreeHead) = k ? order[0] : term;
    *reinterpret_cast<uint32_t*>(kEntitySerial) = serial;
    Log("co-op: drift: free list as the host has it: %d slots in its order, %d more of ours", k - extra, extra);
}

void SendWorldFix(uint8_t* world, uint8_t kinds) {
    static uint8_t buf[2 * 1024 * 1024], z[2 * 1024 * 1024 + 65536];
    DeltaWriter w = {buf, 0, sizeof(buf), false};
    int n = 0, invs = 0, attrs = 0;
    for (int i = 1; i < EntityCount(world); i++) {
        if (!IsActor(world, i)) continue;
        w.U8('P');
        w.U16(static_cast<uint16_t>(i));
        w.Put(EntityPos(world, static_cast<uint16_t>(i)), 12);
        w.U8('H');
        w.U16(static_cast<uint16_t>(i));
        w.U32(static_cast<uint32_t>(ActorHp(world, static_cast<uint16_t>(i))));
        n++;
        if (kinds & kDriftAttr) {
            w.U8('A');
            w.U16(static_cast<uint16_t>(i));
            w.U16(0);
            w.U16(static_cast<uint16_t>(kActorAttrLen));
            w.Put(EntityById(world, static_cast<uint16_t>(i)) + kActorAttr, kActorAttrLen);
            attrs++;
        }
    }
    if (kinds & kDriftInv) {
        for (int i = 1; i < EntityCount(world); i++) {
            uintptr_t vt = *reinterpret_cast<uintptr_t*>(EntityById(world, static_cast<uint16_t>(i)));
            if (vt != kActorVtable && vt != kContainerVtable) continue;
            uint16_t items[200];
            int counts[200];
            char s[2048];
            if (!InvString(world, static_cast<uint16_t>(i), s, sizeof(s))) continue;
            int k = InvItems(world, static_cast<uint16_t>(i), items, counts, 200);
            for (int j = 0; j < k; j++) {
                w.U8('N');
                w.U16(items[j]);
                w.U32(static_cast<uint32_t>(counts[j]));
            }
            w.U8('I');
            w.U16(static_cast<uint16_t>(i));
            PutIdList(w, world, items, k);
            invs++;
        }
    }
    int frees = 0;
    if (kinds & kDriftTable) {
        static uint16_t slots[8192], seqs[8192];
        uint16_t term;
        frees = ReadFreeOrder(world, slots, seqs, 8192, &term);
        w.U8('F');
        w.U16(static_cast<uint16_t>(frees));
        w.U32(*reinterpret_cast<uint32_t*>(kEntitySerial));
        w.U16(term);
        for (int i = 0; i < frees; i++) {
            w.U16(slots[i]);
            w.U16(seqs[i]);
        }
    }
    w.U8('E');
    if (w.overflow) {
        Log("co-op: drift: the correction does not fit (%d bytes): not sent", w.len);
        ReleaseFixHold("no correction");
        return;
    }
    uint32_t raw = static_cast<uint32_t>(w.len);
    uint32_t zn = Compress(buf, raw, z, sizeof(z));
    bool packed = zn > 0 && zn < raw;
    const uint8_t* body = packed ? z : buf;
    uint32_t size = packed ? zn : raw;
    uint32_t hash = Fnv(2166136261u, body, size);
    static uint8_t recs[kMaxChunks + 8][27];
    int nrec = 0;
    bool via_file = (size + kChunk - 1) / kChunk > static_cast<uint32_t>(g_lockstep_chunks);
    if (via_file) {
        EnterCriticalSection(&g_serve_lock);
        StoreBigDelta(body, size, hash);
        LeaveCriticalSection(&g_serve_lock);
    } else {
        for (uint32_t off = 0; off < size; off += kChunk) {
            int c = size - off > static_cast<uint32_t>(kChunk) ? kChunk : static_cast<int>(size - off);
            CoopRecord(recs[nrec], kSyncChunk, static_cast<uint8_t>(c));
            memcpy(recs[nrec] + 5, body + off, c);
            nrec++;
        }
    }
    g_delta_mine = true;
    CoopRecord(recs[nrec], kSyncDone, 0);
    uint16_t marker = 0xfffe;  // "NPC" 0xfffe: the correction while every copy holds (HandleCoopCommand)
    memcpy(recs[nrec] + 5, &marker, 2);
    recs[nrec][7] = static_cast<uint8_t>(kDiffApplied);
    recs[nrec][8] = (packed ? 1 : 0) | (via_file ? 2 : 0);  // taken as it arrives
    memcpy(recs[nrec] + 9, &size, 4);
    memcpy(recs[nrec] + 13, &hash, 4);
    memcpy(recs[nrec] + 17, &raw, 4);
    nrec++;
    SendCoopRecords(recs, nrec);
    Log("co-op: drift: sent the correction: %d actors' positions and HP, %d inventories, %d attribute sets, free list of "
        "%d (%u bytes%s)", n, invs, attrs, frees, size, via_file ? ", file server" : "");
}

bool TakeWorldFix(const uint8_t** data, int* len) { return false; }  // the at-frame correction before the hold: gone

// ---- Exploration (test mode): every entity made or destroyed, with the game code that asked (its return address), so
// a session's two logs show the first allocation or destroy that happened on one copy only.
// World 0x4e50c0 __thiscall(World*, Entity*) ret 4 puts an entity in a slot; World::DestroyEntity 0x4e4a30
// __thiscall(World*, EntityID*) ret 4 marks one.
const uint8_t kAllocPrologue[] = {0x8b, 0x54, 0x24, 0x04, 0x83, 0xec, 0x0c};
const uint8_t kDestroyPrologue[] = {0xa1, 0x64, 0xc5, 0x8b, 0x00, 0x53};
uint8_t* g_alloc_trampoline = nullptr;
uint8_t* g_destroy_trampoline = nullptr;

void __fastcall EntityAllocHook(uint8_t* world, void* edx, uint8_t* ent) {
    void* caller = _ReturnAddress();
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(g_alloc_trampoline)(world, edx, ent);
    if (ent) Log("ent: made %u %s at frame %d by %p", *reinterpret_cast<uint16_t*>(ent + 4), RttiName(ent) + 4,
                 *reinterpret_cast<int*>(world + kWorldFrame), caller);
}

void __fastcall EntityDestroyHook(uint8_t* world, void* edx, uint16_t* id) {
    void* caller = _ReturnAddress();
    uint16_t slot = id ? id[1] : 0;
    Log("ent: destroy %u %s at frame %d by %p", slot, slot < EntityCount(world) ? RttiName(EntityById(world, slot)) + 4 : "?",
        *reinterpret_cast<int*>(world + kWorldFrame), caller);
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(g_destroy_trampoline)(world, edx, id);
}

// And every damage the game deals an actor (Actor vtable 0x80c1d0, FTSE names): a hit, poison, an overdose, with its
// caller. A session where one copy's actor lost HP the other's didn't (683 at frame 3742: no shot, same random numbers
// on both) shows here which code did it. Their arguments aren't known: a thunk logs and jumps on with every register
// and the stack as they were, and the first three stack words go in the log.
const struct {
    int slot;
    uintptr_t fn;
    const char* name;
} kDamageSlots[] = {{300, 0x54c910, "ApplyDamage"},         {533, 0x57c000, "ApplyPoisonDamage"},
                    {534, 0x57c400, "ApplyOverdoseDamage"}, {296, 0x54a980, "BeginAttackAnEntity"},
                    {297, 0x54b0a0, "BeginAttackALocation"}};
const int kDamageSlotCount = sizeof(kDamageSlots) / sizeof(kDamageSlots[0]);
void* g_damage_orig[kDamageSlotCount] = {};  // the slots' functions (PatchSlot)

void __stdcall DamageLog(int which, uint8_t* actor, void* caller, const uint32_t* args) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    Log("dmg: %s actor %u hp %d at frame %d by %p args %08x %08x %08x", kDamageSlots[which].name,
        *reinterpret_cast<uint16_t*>(actor + 4), *reinterpret_cast<int*>(actor + kActorHp),
        world ? *reinterpret_cast<int*>(world + kWorldFrame) : -1, caller, args[0], args[1], args[2]);
}

// pushad (32 bytes): the return address at esp+32, the first argument at esp+36.
#define DAMAGE_THUNK(i)                                            __declspec(naked) void DamageThunk##i() {                           __asm pushad                                                    __asm lea eax, [esp + 36]                                       __asm push eax                                                  __asm push dword ptr [esp + 36]                                 __asm push ecx                                                  __asm push i                                                    __asm call DamageLog                                            __asm popad                                                     __asm jmp dword ptr [g_damage_orig + 4 * i]                 }
DAMAGE_THUNK(0)
DAMAGE_THUNK(1)
DAMAGE_THUNK(2)
DAMAGE_THUNK(3)
DAMAGE_THUNK(4)
#undef DAMAGE_THUNK

// And every random draw: the game's random state is World+0x729 (0x505b00 hands out a pointer to it; a draw steps it,
// x * 0x343fd + 0x269ec3). Each frame starts it again at seed + frame number (0x4fc1f0), so equal values at frame start
// (the RNG log) say nothing about the draws within it: one extra draw on one copy changes every later roll in that frame
// there (the same shot did 6 damage on the host, 5 on the joiner, chaos run 2026-10-23). Per frame: how many draws, and
// how many from each caller; the two logs' first differing line names the code that draws on one copy only.
const uint8_t kRandPrologue[] = {0xa0, 0x8b, 0xdd, 0x8b, 0x00, 0x56};  // mov al, [0x8bdd8b]; push esi
uint8_t* g_rand_trampoline = nullptr;
struct RandCaller {
    uintptr_t at;
    int n;
};
RandCaller g_rand_callers[64];
bool g_in_fire = false;  // from a fire routine's first draw to the end of the frame (the ray visit log below)
uint32_t g_rand_order = 0;  // the frame's draws in order: who drew, and for which entity (ECX of the caller is not known: the
                            // caller's return address and the actor its stack names come in)
int g_rand_ncallers = 0, g_rand_frame = -1, g_rand_total = 0, g_rand_outside = 0;  // outside: not in the world tick

// Each draw's actor: the first pointer to one of this frame's actors on the drawing code's stack.
uintptr_t g_actor_ptrs[256];
uint16_t g_actor_ids[256];
int g_actor_n = 0;
char g_rand_seq[4096];
int g_rand_seq_len = 0;

void CollectActorPtrs(uint8_t* world) {
    g_actor_n = 0;
    for (int i = 1; i < EntityCount(world) && g_actor_n < 256; i++)
        if (IsActor(world, i)) {
            g_actor_ptrs[g_actor_n] = reinterpret_cast<uintptr_t>(EntityById(world, static_cast<uint16_t>(i)));
            g_actor_ids[g_actor_n++] = static_cast<uint16_t>(i);
        }
}

uint16_t DrawActor(const uint32_t* sp) {
    for (int k = 0; k < 64; k++) {
        uint32_t v;
        __try { v = sp[k]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        for (int j = 0; j < g_actor_n; j++)
            if (g_actor_ptrs[j] == v) return g_actor_ids[j];
    }
    return 0;
}

void FlushRandFrame(uint32_t end_state) {
    if (g_rand_frame < 0 || !g_rand_total) return;
    char line[2048];
    int len = sprintf_s(line, "RNGC f=%d n=%d out=%d end=%08x order=%08x", g_rand_frame, g_rand_total, g_rand_outside,
                        end_state, g_rand_order);
    for (int i = 0; i < g_rand_ncallers && len < 1990; i++)
        len += sprintf_s(line + len, sizeof(line) - len, " %06x:%d", static_cast<unsigned>(g_rand_callers[i].at), g_rand_callers[i].n);
    Log("%s", line);
    if (g_rand_seq_len) Log("RNGQ f=%d%s", g_rand_frame, g_rand_seq);
}

void* __fastcall RandStateHook(uint8_t* world, void* edx) {
    uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    if (frame != g_rand_frame) {
        // The state as the last frame left it (the frame start's reset draws here first): code that steps it without
        // 0x505b00 (0x5046b0 and 0x69f330 write World+0x729 directly) shows here as the same draws with another end.
        FlushRandFrame(*reinterpret_cast<uint32_t*>(world + 0x729));
        g_rand_frame = frame;
        g_rand_ncallers = g_rand_total = g_rand_outside = 0;
        g_rand_order = 2166136261u;
        g_in_fire = false;
        g_rand_seq_len = 0;
        g_rand_seq[0] = 0;
        CollectActorPtrs(world);
    }
    g_rand_total++;
    if (!g_in_world_tick) g_rand_outside++;
    if (caller == 0x611aa9) g_in_fire = true;
    uint16_t who = DrawActor(reinterpret_cast<uint32_t*>(_AddressOfReturnAddress()));
    g_rand_order = Fnv(g_rand_order, &caller, sizeof(caller));
    g_rand_order = Fnv(g_rand_order, &who, sizeof(who));
    if (g_rand_seq_len < static_cast<int>(sizeof(g_rand_seq)) - 24)
        g_rand_seq_len += sprintf_s(g_rand_seq + g_rand_seq_len, sizeof(g_rand_seq) - g_rand_seq_len, " %06x:%u",
                                    static_cast<unsigned>(caller), who);
    int i = 0;
    while (i < g_rand_ncallers && g_rand_callers[i].at != caller) i++;
    if (i == g_rand_ncallers && i < 64) g_rand_callers[g_rand_ncallers++] = {caller, 0};
    if (i < 64) g_rand_callers[i].n++;
    if (caller >= 0x61a420 && caller < 0x61a9c0 && (i == 64 || g_rand_callers[i].n == 1)) {
        // An attack resolving (0x61a420: to-hit, damage): the BOS.exe return addresses on the stack, to see what set it off.
        char chain[600];
        size_t o = 0;
        uint32_t* sp = reinterpret_cast<uint32_t*>(_AddressOfReturnAddress());
        for (int k = 0, found = 0; k < 4000 && found < 40; k++) {
            uint32_t v;
            __try { v = sp[k]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (v >= 0x401000 && v < 0x7fc000) {
                o += sprintf_s(chain + o, sizeof(chain) - o, " %06x", v);
                found++;
            }
        }
        Log("RNGS f=%d attack resolves%s:%s", frame, g_in_world_tick ? "" : " OUTSIDE the world tick", chain);
    }
    return reinterpret_cast<void*(__fastcall*)(void*, void*)>(g_rand_trampoline)(world, edx);
}

// And every animation step (0x5e2b00, __thiscall(Entity*): the next command of the entity's animation list, +0x117 ..
// +0x11b, index +0x135, ms per step +0x137; a negative command can fire the attack): two copies that start an attack
// on the same frame with the same clock should step it alike (chaos run 2026-10-23: NPC 204's shot fired on one copy).
const uint8_t kAnimPrologue[] = {0x83, 0xec, 0x10, 0x53, 0x56, 0x8b, 0xf1};  // sub esp, 10h; push ebx; push esi; mov esi, ecx
uint8_t* g_anim_trampoline = nullptr;

void __fastcall AnimStepHook(uint8_t* ent, void* edx) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint16_t id = *reinterpret_cast<uint16_t*>(ent + 4);
    if (world && IsActor(world, id)) {
        const int16_t* list = *reinterpret_cast<int16_t**>(ent + 0x117);
        const int16_t* end = *reinterpret_cast<int16_t**>(ent + 0x11b);
        int n = list ? static_cast<int>(end - list) : 0;
        uint32_t h = list ? Fnv(2166136261u, list, n * 2) : 0;
        Log("ANIM f=%d id=%u idx=%u n=%d ms=%u list=%08x%s by %p", *reinterpret_cast<int*>(world + kWorldFrame), id,
            *reinterpret_cast<uint16_t*>(ent + 0x135), n, *reinterpret_cast<uint16_t*>(ent + 0x137), h,
            g_in_world_tick ? "" : " OUTSIDE", _ReturnAddress());
    }
    reinterpret_cast<void(__fastcall*)(void*, void*)>(g_anim_trampoline)(ent, edx);
}

// And what a firing's rays find: the ray query's visitor 0x5110a0 (__thiscall(Query*, Object*) ret 4, from the bullet's
// query vtable 0x8095cc) skips an object whose +2 bit 0 or +3 bit 3 is set (unless the query's +5 / +4 says otherwise) or
// that this ray already stamped (+0x3d == query +0x37). Logged from a fire routine's first draw (0x611aa9) to the end of
// that frame: two copies with the same shooter, clock and random draws whose bullets find different bystanders.
const uint8_t kRayVisitPrologue[] = {0xa0, 0x81, 0xdd, 0x8b, 0x00};  // mov al, [0x8bdd81]
uint8_t* g_ray_visit_trampoline = nullptr;

void __fastcall RayVisitHook(uint8_t* query, void* edx, uint8_t* obj) {
    if (g_in_fire && g_in_world_tick && obj) __try {  // 0x8bdd81 set: the visitor returns at once (the object unread)
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        // Its entity: the slot whose object holds this address (a map object inside an entity), and the offset.
        int id = -1;
        unsigned off = 0;
        for (int i = 1; i < EntityCount(world) && id < 0; i++) {
            uintptr_t e = reinterpret_cast<uintptr_t>(EntityById(world, static_cast<uint16_t>(i)));
            if (e == *reinterpret_cast<uintptr_t*>(Entries(world))) continue;  // an empty slot: the dummy entity
            uintptr_t o = reinterpret_cast<uintptr_t>(obj);
            if (o >= e && o < e + 0x1100) {
                id = i;
                off = static_cast<unsigned>(o - e);
            }
        }
        const char* name = id > 0 ? RttiName(EntityById(world, static_cast<uint16_t>(id))) : "?";
        if (strlen(name) > 4) name += 4;
        char raw[0x48 * 2 + 1];
        for (int k = 0; k < 0x48; k++) sprintf_s(raw + k * 2, 3, "%02x", obj[k]);
        Log("RAYB %s", raw);
        Log("RAY f=%d sw=%u obj=%d+%x %s flags=%02x/%02x q45=%02x/%02x stamp=%08x qid=%08x",
            world ? *reinterpret_cast<int*>(world + kWorldFrame) : -1, *reinterpret_cast<uint8_t*>(0x8bdd81), id, off, name, obj[2], obj[3], query[4], query[5], *reinterpret_cast<uint32_t*>(obj + 0x3d),
            *reinterpret_cast<uint32_t*>(query + 0x37));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("RAY object %p unreadable", obj);
    }
    uint8_t before[0x40];
    bool logging = g_in_fire && g_in_world_tick;
    if (logging) memcpy(before, query, sizeof(before));
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(g_ray_visit_trampoline)(query, edx, obj);
    if (logging) {  // what the visit changed in the query (+6: the ray stops; the hit's fields)
        char d[256];
        int n = 0;
        for (int k = 0; k < 0x40 && n < 220; k++)
            if (query[k] != before[k] && k != 0x37) n += sprintf_s(d + n, sizeof(d) - n, " +%x:%02x>%02x", k, before[k], query[k]);
        if (n) Log("RAYR%s", d);
    }
}

// And who really calls the hit routine 0x613230 (stack scans showed stale frames): its return address, at entry.
const uint8_t kHitPrologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};  // push ebp; mov ebp, esp; push -1
void* g_hit_trampoline = nullptr;
void __stdcall HitEntryLog(uintptr_t caller) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    Log("HIT f=%d 0x613230 called from %06x", world ? *reinterpret_cast<int*>(world + kWorldFrame) : -1,
        static_cast<unsigned>(caller));
}
__declspec(naked) void HitEntryThunk() {
    __asm pushad
    __asm push dword ptr [esp + 32]
    __asm call HitEntryLog
    __asm popad
    __asm jmp dword ptr [g_hit_trampoline]
}

void InstallEntityLogHooks() {  // test mode only (after the test config is read)
    g_hit_trampoline = HookWithTrampoline(0x613230, kHitPrologue, sizeof(kHitPrologue), (void*)&HitEntryThunk);
    Log("co-op: hit entry log hook %s", g_hit_trampoline ? "ok" : "FAILED");
    g_ray_visit_trampoline = HookWithTrampoline(0x5110a0, kRayVisitPrologue, sizeof(kRayVisitPrologue), (void*)&RayVisitHook);
    Log("co-op: ray visit log hook %s", g_ray_visit_trampoline ? "ok" : "FAILED");
    g_anim_trampoline = HookWithTrampoline(0x5e2b00, kAnimPrologue, sizeof(kAnimPrologue), (void*)&AnimStepHook);
    Log("co-op: animation step log hook %s", g_anim_trampoline ? "ok" : "FAILED");
    g_rand_trampoline = HookWithTrampoline(0x505b00, kRandPrologue, sizeof(kRandPrologue), (void*)&RandStateHook);
    Log("co-op: world tick hook %s", PatchCall(kWorldTickCall, kWorldTick, (void*)&WorldTickHook) ? "ok" : "FAILED");
    Log("co-op: random draw log hook %s", g_rand_trampoline ? "ok" : "FAILED");
    g_alloc_trampoline = HookWithTrampoline(0x4e50c0, kAllocPrologue, sizeof(kAllocPrologue), (void*)&EntityAllocHook);
    g_destroy_trampoline = HookWithTrampoline(0x4e4a30, kDestroyPrologue, sizeof(kDestroyPrologue), (void*)&EntityDestroyHook);
    Log("co-op: entity log hooks %s, %s", g_alloc_trampoline ? "ok" : "FAILED", g_destroy_trampoline ? "ok" : "FAILED");
    void* thunks[] = {(void*)&DamageThunk0, (void*)&DamageThunk1, (void*)&DamageThunk2, (void*)&DamageThunk3,
                      (void*)&DamageThunk4};
    int ok = 0;
    for (int i = 0; i < kDamageSlotCount; i++)
        ok += PatchSlot(kActorVtable + 4 * kDamageSlots[i].slot, kDamageSlots[i].fn, thunks[i], &g_damage_orig[i]);
    Log("co-op: damage and attack log hooks %d of %d", ok, kDamageSlotCount);
}
