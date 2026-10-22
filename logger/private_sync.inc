// ---- Private screens without a reload: the talker's changes reach the other copies as lockstep commands ----
// Included from bot_scenarios.inc (needs its entity helpers). A conversation (and the barter / recruit screens it
// leads to) runs on the talker's copy only (see PrivateScreenTick). When it opens, that copy takes a snapshot; when
// the screens close, it compares:
//   - mission and campaign variables (World +0x386.. / +0x396.., *0x8bdd2c +0x2b.. / +0x3b..; FTSE World.h/.cpp),
//   - the squad, every Actor's HP and inventory,
//   - the entity table (World+0x743, entries {ptr, i16 next_free, i16 seq}; free-list head u16 at World+0x74f).
// Variables changed (a dialogue choice): the old save and reload. Otherwise the talker writes a delta and sends it as
// co-op commands; every other copy applies it on the same frame:
//   'D' slot: destroy the entity there (vtable slot 10, DestructEntity);
//   'C' slot seq serial len bytes: an entity the others do not have (a recruit, a new stack). The bytes are the game's
//       own serialization (0x5eabd0, as the level editor's undo and the save-entity-to-file 0x5ea630 use it); the
//       receiver writes them to a file and creates the entity with MakeEntity 0x5ea400 (FTSE CreateEntity), with the
//       free list pointed at the talker's slot first, so the entity lands in the same slot;
//   'N' slot count: a stack's count (Collectable +0x1ad);
//   'I' owner n {slot, seq}: an Actor's inventory list, in order (Inventory entity +0x1a9 list, +0x1ad count);
//   'Q' n {slot, seq}: the squad (players[1] +0x4b list);
//   'H' slot hp: an Actor's HP;
//   'B' count head serial n {slot, next_free, seq}: the entity table's bookkeeping, so all copies hand out the same
//       slots afterwards (opening barter alone creates and destroys the shop's stock), and the entity serial counter
//       0x8be148.
// Co-op command 0x30 (a 27-byte ControllerCommandStruct; never reaches the game: the Actor command hook takes it):
//   [0] 0x30, [1..2] actor (the talker's squad member, so the server passes it on), [3] kind, [4] n, [5..6] npc,
//   kSyncChunk: [7..8] chunk index, [9..26] n (<= 18) bytes of the delta;
//   kSyncDone: [7] kDiff*, [9..12] delta size, [13..16] its FNV hash.
// The same commands lock the NPC: a talk command (0x17) to an NPC locks it on every copy (the command runs on every
// copy on the same frame), kSyncDone unlocks it. A copy refuses its own player's talk to a locked NPC; the host drops
// a talk to a locked NPC from anyone else (two players clicking at the same moment).
const uint8_t kCoopCmd = 0x30;
enum { kSyncChunk = 1, kSyncDone = 3 };
enum { kDiffNone = 0, kDiffApplied = 1, kDiffFull = 2 };
const size_t kWorldFreeHead = 0x74f;
const uintptr_t kEntitySerial = 0x8be148;
const int kMaxSlots = 8192;
const int kChunk = 18;
const int kMaxDelta = 256 * 1024;

struct SlotState { uint8_t used; int16_t next_free, seq; };
SlotState g_snap_slots[kMaxSlots];
int g_snap_count = 0;
uint16_t g_snap_head = 0;
uint32_t g_snap_vars = 0, g_snap_inv = 0, g_snap_squad = 0;
bool g_snap_taken = false;
uint16_t g_private_npc = 0;  // whom this copy's player is talking to
struct ActorSnap { uint16_t id; int hp; float pos[3]; char inv[2048]; };
const int kMaxActorSnaps = 256;
ActorSnap g_snap_actors[kMaxActorSnaps];
int g_snap_nactors = 0;
uint16_t g_snap_squad_ids[16];
int g_snap_nsquad = 0;

uint32_t Fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

uint32_t HashWide(uint32_t h, const wchar_t* s) {
    if (!s) return Fnv(h, "\0", 1);
    return Fnv(h, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

uint32_t HashVarList(uint32_t h, wchar_t** keys, wchar_t** keys_end, wchar_t** vals) {
    for (int i = 0; keys && keys + i < keys_end && i < 20000; i++) {
        h = HashWide(h, keys[i]);
        h = HashWide(h, vals ? vals[i] : nullptr);
    }
    return h;
}

uint32_t HashVars(uint8_t* world) {
    uint32_t h = 2166136261u;
    __try {
        h = HashVarList(h, *reinterpret_cast<wchar_t***>(world + 0x396), *reinterpret_cast<wchar_t***>(world + 0x39a),
                        *reinterpret_cast<wchar_t***>(world + 0x386));
        uint8_t* cv = *reinterpret_cast<uint8_t**>(0x8bdd2c);
        if (cv)
            h = HashVarList(h, *reinterpret_cast<wchar_t***>(cv + 0x3b), *reinterpret_cast<wchar_t***>(cv + 0x3f),
                            *reinterpret_cast<wchar_t***>(cv + 0x2b));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h ^= 0xdeadbeef;
    }
    return h;
}

uint32_t HashInventories(uint8_t* world) {
    uint32_t h = 2166136261u;
    char s[2048];
    for (int i = 1; i < EntityCount(world); i++) {
        if (!IsActor(world, i)) continue;
        int hp = ActorHp(world, static_cast<uint16_t>(i));
        h = Fnv(Fnv(h, &i, sizeof(i)), &hp, sizeof(hp));
        if (InvString(world, static_cast<uint16_t>(i), s, sizeof(s))) h = Fnv(h, s, strlen(s));
    }
    return h;
}

uint32_t HashSquad(uint8_t* world) {
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    return Fnv(Fnv(2166136261u, &n, sizeof(n)), squad, n * sizeof(uint16_t));
}

void ReadSlots(uint8_t* world, SlotState* out, int* count) {
    uint8_t* e = Entries(world);
    int n = EntityCount(world);
    if (n > kMaxSlots) n = kMaxSlots;
    void* dummy = *reinterpret_cast<void**>(e);
    for (int i = 0; i < n; i++) {
        void* p = *reinterpret_cast<void**>(e + i * 8);
        out[i].used = p && p != dummy;
        out[i].next_free = *reinterpret_cast<int16_t*>(e + i * 8 + 4);
        out[i].seq = *reinterpret_cast<int16_t*>(e + i * 8 + 6);
    }
    *count = n;
}

void TakePrivateSnapshot() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    ReadSlots(world, g_snap_slots, &g_snap_count);
    g_snap_head = *reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
    g_snap_vars = HashVars(world);
    g_snap_inv = HashInventories(world);
    g_snap_squad = HashSquad(world);
    g_snap_nsquad = GetSquad(world, g_snap_squad_ids, 16);
    g_snap_nactors = 0;
    for (int i = 1; i < EntityCount(world) && g_snap_nactors < kMaxActorSnaps; i++) {
        if (!IsActor(world, i)) continue;
        ActorSnap& a = g_snap_actors[g_snap_nactors++];
        a.id = static_cast<uint16_t>(i);
        a.hp = ActorHp(world, a.id);
        memcpy(a.pos, EntityPos(world, a.id), sizeof(a.pos));
        if (!InvString(world, a.id, a.inv, sizeof(a.inv))) a.inv[0] = 0;
    }
    g_snap_taken = true;
    Log("co-op: snapshot for the private screens: %d entity slots, head %u, vars %08x, inventories %08x, squad %08x, "
        "%d actors", g_snap_count, g_snap_head, g_snap_vars, g_snap_inv, g_snap_squad, g_snap_nactors);
}

// ---- the game's own entity serialization (see the header) ----
// Returns the FOTString buffer holding the bytes (the game's heap) and their length; nullptr on failure.
uint8_t* SerializeEntity(uint8_t* world, uint16_t id, int* len) {
    *len = 0;
    __try {
        uint32_t* sb = reinterpret_cast<uint32_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(0x6c4dd0)(0x14));
        reinterpret_cast<void(__fastcall*)(void*)>(0x426830)(sb);
        sb[2] = 0xffffffff;
        sb[0] = 0x7fde40;
        uint8_t* str = static_cast<uint8_t*>(MakeFOTString(""));
        sb[3] = reinterpret_cast<uint32_t>(str);
        (*reinterpret_cast<int*>(str - 0xc))++;
        sb[4] = 0;
        sb[0] = 0x7fde04;
        static uint8_t ar[256];
        memset(ar, 0, sizeof(ar));
        reinterpret_cast<void(__thiscall*)(void*, int, void*, int)>(0x704060)(ar, 1, sb, 1);
        char ok = reinterpret_cast<char(__cdecl*)(void*, uint32_t)>(0x5eabd0)(ar, FullId(world, id));
        uint8_t* buf = reinterpret_cast<uint8_t*>(sb[3]);
        if (!ok || !buf) return nullptr;
        *len = *reinterpret_cast<int*>(buf - 4);
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("co-op: exception 0x%08x serializing entity %u", GetExceptionCode(), id);
        return nullptr;
    }
}

const char kEntityTmpName[] = "user/save/ftcoop_entity.tmp";
const char kEntityTmpPath[] = "core\\user\\save\\ftcoop_entity.tmp";

// Create the entity in `bytes` in slot `slot` (free here): the free list's head is pointed at it first.
uint16_t CreateEntityInSlot(uint8_t* world, uint16_t slot, int16_t seq, const uint8_t* bytes, uint32_t len) {
    FILE* f = nullptr;
    if (fopen_s(&f, kEntityTmpPath, "wb") != 0 || !f) {
        Log("co-op: cannot write %s", kEntityTmpPath);
        return 0;
    }
    fwrite(bytes, 1, len, f);
    fclose(f);
    uint8_t* e = Entries(world);
    uint16_t* head = reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
    *reinterpret_cast<int16_t*>(e + slot * 8 + 4) = static_cast<int16_t>(*head);
    *head = slot;
    uint32_t out[2] = {};
    void* name = MakeFOTString(kEntityTmpName);
    __try {
        reinterpret_cast<void*(__cdecl*)(uint32_t*, void**, int)>(0x5ea400)(out, &name, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("co-op: exception 0x%08x creating entity for slot %u", GetExceptionCode(), slot);
        return 0;
    }
    uint16_t got = static_cast<uint16_t>(out[0] >> 16);
    *reinterpret_cast<int16_t*>(e + slot * 8 + 6) = seq;
    return got;
}

// ---- sending ----
void SendCoopRecords(const uint8_t (*recs)[27], int n) {
    if (!g_client_ptr || n <= 0) return;
    static uint8_t m[6 + 27 * 64];
    for (int done = 0; done < n;) {
        int k = n - done > 64 ? 64 : n - done;
        *reinterpret_cast<uint32_t*>(m) = 2;
        *reinterpret_cast<uint16_t*>(m + 4) = static_cast<uint16_t>(k);
        memcpy(m + 6, recs[done], 27 * k);
        reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(kClientSend)(g_client_ptr, m, 6 + 27 * k, 1);
        done += k;
    }
}

void CoopRecord(uint8_t* r, uint8_t kind, uint8_t n) {
    memset(r, 0, 27);
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint16_t squad[16];
    uint16_t actor = world && GetSquad(world, squad, 16) > 0 ? squad[0] : 0;
    r[0] = kCoopCmd;
    memcpy(r + 1, &actor, 2);
    r[3] = kind;
    r[4] = n;
    memcpy(r + 5, &g_private_npc, 2);
}

// ---- building the delta (the talker's copy) ----
struct DeltaWriter {
    uint8_t* buf;
    int len, cap;
    bool overflow;
    void Put(const void* p, int n) {
        if (len + n > cap) {
            overflow = true;
            return;
        }
        memcpy(buf + len, p, n);
        len += n;
    }
    void U8(uint8_t v) { Put(&v, 1); }
    void U16(uint16_t v) { Put(&v, 2); }
    void U32(uint32_t v) { Put(&v, 4); }
};

void ParseInv(const char* s, uint16_t* items, int* counts, int* n, int max) {
    *n = 0;
    if (!s || !*s || *s == '-' || *s == '?') return;
    for (const char* p = s; *p && *n < max;) {
        char* end;
        items[*n] = static_cast<uint16_t>(strtoul(p, &end, 10));
        counts[*n] = *end == ':' ? strtol(end + 1, &end, 10) : 0;
        (*n)++;
        p = *end == ',' ? end + 1 : end;
        if (end == p && *p) break;
    }
}

const ActorSnap* SnapOf(uint16_t id) {
    for (int i = 0; i < g_snap_nactors; i++)
        if (g_snap_actors[i].id == id) return &g_snap_actors[i];
    return nullptr;
}

void PutIdList(DeltaWriter& w, uint8_t* world, const uint16_t* ids, int n) {
    w.U16(static_cast<uint16_t>(n));
    for (int i = 0; i < n; i++) {
        w.U16(ids[i]);
        w.U16(static_cast<uint16_t>(FullId(world, ids[i]) & 0xffff));
    }
}

// Main thread, when the private screens have closed (the talker's copy). Builds and sends the delta and the unlock.
// Returns kDiffFull when the caller must fall back to the save and reload.
uint8_t g_delta[kMaxDelta];
bool g_delta_mine = false;  // this copy sent the delta now in flight: do not apply it to ourselves

int PrivateScreensDiff() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    int result = kDiffNone;
    DeltaWriter w = {g_delta, 0, kMaxDelta, false};
    if (!world || !g_snap_taken) {
        Log("co-op: no snapshot of the private screens: full re-sync");
        result = kDiffFull;
    } else {
        static SlotState now[kMaxSlots];
        int count = 0;
        ReadSlots(world, now, &count);
        uint16_t head = *reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
        uint32_t vars = HashVars(world), inv = HashInventories(world), squad = HashSquad(world);
        int news = 0, gone = 0, counts = 0, invs = 0, hps = 0, moved = 0;
        if (vars != g_snap_vars || count != g_snap_count) {
            Log("co-op: private screens changed %s: full re-sync", vars != g_snap_vars ? "quest variables" : "the entity table size");
            result = kDiffFull;
        } else {
            // 'D': gone or replaced
            for (int i = 1; i < count; i++)
                if (g_snap_slots[i].used && (!now[i].used || now[i].seq != g_snap_slots[i].seq)) {
                    w.U8('D');
                    w.U16(static_cast<uint16_t>(i));
                    gone++;
                }
            // 'C': new (bytes from the game's serializer)
            for (int i = 1; i < count && !w.overflow; i++)
                if (now[i].used && (!g_snap_slots[i].used || now[i].seq != g_snap_slots[i].seq)) {
                    int n = 0;
                    uint8_t* bytes = SerializeEntity(world, static_cast<uint16_t>(i), &n);
                    if (!bytes || n <= 0) {
                        Log("co-op: could not serialize new entity %u: full re-sync", i);
                        result = kDiffFull;
                        break;
                    }
                    w.U8('C');
                    w.U16(static_cast<uint16_t>(i));
                    w.U16(static_cast<uint16_t>(now[i].seq));
                    w.U32(*reinterpret_cast<uint32_t*>(EntityById(world, static_cast<uint16_t>(i)) + 0x5d));
                    w.U32(static_cast<uint32_t>(n));
                    w.Put(bytes, n);
                    news++;
                    Log("co-op: new entity %u (%s): %d bytes", i, RttiName(EntityById(world, static_cast<uint16_t>(i))), n);
                }
            // 'N', 'I', 'H': stacks, inventory lists and HP of every Actor that changed
            uint16_t sq_now[16];
            int nsq_now = GetSquad(world, sq_now, 16), nsq_before = g_snap_nsquad;
            const uint16_t* sq_before = g_snap_squad_ids;
            static uint16_t a_items[200], b_items[200];
            static int a_counts[200], b_counts[200];
            char s[2048];
            for (int i = 1; i < count; i++) {
                if (!IsActor(world, i)) continue;
                uint16_t id = static_cast<uint16_t>(i);
                const ActorSnap* before = SnapOf(id);
                int hp = ActorHp(world, id);
                // Position and HP only for the squad (before or after): the screens place recruits; NPC positions keep
                // moving a little between frames (walk animation) even while the world is paused.
                bool squad_member = InList(sq_before, nsq_before, id) || InList(sq_now, nsq_now, id);
                if (!squad_member) {
                } else if (!before || before->hp != hp) {
                    w.U8('H');
                    w.U16(id);
                    w.U32(static_cast<uint32_t>(hp));
                    hps++;
                }
                float* pos = EntityPos(world, id);
                float jump = before ? fabsf(pos[0] - before->pos[0]) + fabsf(pos[1] - before->pos[1]) +
                                         fabsf(pos[2] - before->pos[2])
                                   : 1e9f;
                // A walking character creeps a fraction of a unit even while the world is paused (trade_join); the
                // screens place a recruit tens of units away. Only a jump counts.
                if (squad_member && jump > 2.0f) {
                    Log("co-op: delta: squad member %u moved to (%.3f,%.3f,%.3f)", id, pos[0], pos[1], pos[2]);
                    w.U8('P');  // a recruit placed next to the squad, an NPC a script moved
                    w.U16(id);
                    w.Put(pos, sizeof(float) * 3);
                    hps++;
                }
                if (!InvString(world, id, s, sizeof(s))) continue;
                if (before && strcmp(before->inv, s) == 0) continue;
                int na = 0, nb = 0;
                ParseInv(before ? before->inv : "", b_items, b_counts, &nb, 200);
                ParseInv(s, a_items, a_counts, &na, 200);
                for (int k = 0; k < na; k++) {  // counts of stacks that are not new
                    bool fresh = !g_snap_slots[a_items[k]].used || g_snap_slots[a_items[k]].seq != now[a_items[k]].seq;
                    int was = -1;
                    for (int j = 0; j < nb; j++)
                        if (b_items[j] == a_items[k]) was = b_counts[j];
                    if (!fresh && was != a_counts[k]) {
                        w.U8('N');
                        w.U16(a_items[k]);
                        w.U32(static_cast<uint32_t>(a_counts[k]));
                        counts++;
                    }
                }
                w.U8('I');
                w.U16(id);
                PutIdList(w, world, a_items, na);
                invs++;
            }
            if (squad != g_snap_squad) {
                uint16_t ids[16];
                int n = GetSquad(world, ids, 16);
                w.U8('Q');
                PutIdList(w, world, ids, n);
            }
            // 'B': the table's bookkeeping
            int nb = 0;
            for (int i = 0; i < count; i++)
                if (now[i].next_free != g_snap_slots[i].next_free || now[i].seq != g_snap_slots[i].seq) nb++;
            w.U8('B');
            w.U16(static_cast<uint16_t>(count));
            w.U16(head);
            w.U32(*reinterpret_cast<uint32_t*>(kEntitySerial));
            w.U16(static_cast<uint16_t>(nb));
            for (int i = 0; i < count; i++)
                if (now[i].next_free != g_snap_slots[i].next_free || now[i].seq != g_snap_slots[i].seq) {
                    w.U16(static_cast<uint16_t>(i));
                    w.U16(static_cast<uint16_t>(now[i].next_free));
                    w.U16(static_cast<uint16_t>(now[i].seq));
                }
            moved = nb;
            w.U8('E');
            if (w.overflow) {
                Log("co-op: delta over %d bytes: full re-sync", kMaxDelta);
                result = kDiffFull;
            } else if (result != kDiffFull) {
                bool any = news || gone || counts || invs || hps || squad != g_snap_squad || moved || head != g_snap_head;
                result = any ? kDiffApplied : kDiffNone;
            }
            Log("co-op: private screens changed: inventories %s, squad %s; delta: %d new, %d gone, %d stack counts, "
                "%d inventories, %d HP, %d table entries, free head %u -> %u, %d bytes",
                inv != g_snap_inv ? "yes" : "no", squad != g_snap_squad ? "yes" : "no", news, gone, counts, invs, hps,
                moved, g_snap_head, head, w.len);
        }
    }
    // Records: the delta in chunks (only when applied), then the unlock with what happened.
    static uint8_t recs[kMaxDelta / kChunk + 8][27];
    int nrec = 0;
    if (result == kDiffApplied) {
        for (int off = 0; off < w.len; off += kChunk) {
            int n = w.len - off > kChunk ? kChunk : w.len - off;
            CoopRecord(recs[nrec], kSyncChunk, static_cast<uint8_t>(n));
            uint16_t idx = static_cast<uint16_t>(off / kChunk);
            memcpy(recs[nrec] + 7, &idx, 2);
            memcpy(recs[nrec] + 9, g_delta + off, n);
            nrec++;
        }
        g_delta_mine = true;
    }
    CoopRecord(recs[nrec], kSyncDone, 0);
    recs[nrec][7] = static_cast<uint8_t>(result);
    uint32_t size = result == kDiffApplied ? static_cast<uint32_t>(w.len) : 0;
    uint32_t hash = Fnv(2166136261u, g_delta, size);
    memcpy(recs[nrec] + 9, &size, 4);
    memcpy(recs[nrec] + 13, &hash, 4);
    nrec++;
    Log("co-op: sending %d co-op command(s): %s", nrec,
        result == kDiffNone ? "nothing changed, no reload" : result == kDiffApplied ? "the delta, no reload"
                                                                                    : "unlock (full re-sync follows)");
    SendCoopRecords(recs, nrec);
    g_snap_taken = false;
    return result;
}

// ---- applying the delta (every other copy) ----
struct DeltaReader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad;
    bool Has(int n) { return !bad && p + n <= end; }
    uint8_t U8() { if (!Has(1)) { bad = true; return 0; } return *p++; }
    uint16_t U16() { uint16_t v = 0; if (!Has(2)) { bad = true; return 0; } memcpy(&v, p, 2); p += 2; return v; }
    uint32_t U32() { uint32_t v = 0; if (!Has(4)) { bad = true; return 0; } memcpy(&v, p, 4); p += 4; return v; }
};

// Make a std::list {next, prev, EntityID} with sentinel `head` hold exactly `ids` (new nodes from the game's heap).
int RewriteIdList(uint8_t* head, const uint16_t* slots, const uint16_t* seqs, int n) {
    uint8_t* nodes[256];
    int have = 0;
    for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node && node != head && have < 256;
         node = *reinterpret_cast<uint8_t**>(node))
        nodes[have++] = node;
    for (int i = have; i < n; i++) {  // append
        uint8_t* node = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(0x6c4dd0)(12));
        uint8_t* last = *reinterpret_cast<uint8_t**>(head + 4);
        *reinterpret_cast<uint8_t**>(node) = head;
        *reinterpret_cast<uint8_t**>(node + 4) = last;
        *reinterpret_cast<uint8_t**>(last) = node;
        *reinterpret_cast<uint8_t**>(head + 4) = node;
        nodes[i] = node;
    }
    for (int i = n; i < have; i++) {  // unlink the rest
        uint8_t* node = nodes[i];
        uint8_t* next = *reinterpret_cast<uint8_t**>(node);
        uint8_t* prev = *reinterpret_cast<uint8_t**>(node + 4);
        *reinterpret_cast<uint8_t**>(prev) = next;
        *reinterpret_cast<uint8_t**>(next + 4) = prev;
        reinterpret_cast<void(__cdecl*)(void*)>(0x6c4e50)(node);
    }
    for (int i = 0; i < n; i++) {
        *reinterpret_cast<uint16_t*>(nodes[i] + 8) = seqs[i];
        *reinterpret_cast<uint16_t*>(nodes[i] + 10) = slots[i];
    }
    return have;
}

bool ReadIdList(DeltaReader& r, uint16_t* slots, uint16_t* seqs, int* n, int max) {
    *n = r.U16();
    if (*n > max) return false;
    for (int i = 0; i < *n; i++) {
        slots[i] = r.U16();
        seqs[i] = r.U16();
    }
    return !r.bad;
}

const uint8_t* g_reapply_b = nullptr;  // the delta's 'B' op, written again on the next frame
int g_reapply_wait = 0;

void ApplyBookkeeping(uint8_t* world, const uint8_t* p) {
    uint16_t count, head, nb;
    uint32_t serial;
    memcpy(&count, p, 2);
    memcpy(&head, p + 2, 2);
    memcpy(&serial, p + 4, 4);
    memcpy(&nb, p + 8, 2);
    uint8_t* e = Entries(world);
    for (int i = 0; i < nb; i++) {
        uint16_t slot, next_free, seq;
        memcpy(&slot, p + 10 + i * 6, 2);
        memcpy(&next_free, p + 12 + i * 6, 2);
        memcpy(&seq, p + 14 + i * 6, 2);
        if (slot < EntityCount(world)) {
            *reinterpret_cast<uint16_t*>(e + slot * 8 + 4) = next_free;
            *reinterpret_cast<uint16_t*>(e + slot * 8 + 6) = seq;
        }
    }
    *reinterpret_cast<uint16_t*>(world + kWorldFreeHead) = head;
    *reinterpret_cast<uint32_t*>(kEntitySerial) = serial;
}

// Main thread, at the start of every frame: the deferred destroys of the last delta have run; write its bookkeeping
// again so the free list is the talker's.
void ReapplyBookkeeping() {
    if (!g_reapply_b) return;
    if (g_reapply_wait-- > 0) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world) {
        uint16_t was = *reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
        ApplyBookkeeping(world, g_reapply_b);
        Log("co-op: delta bookkeeping written again after the frame (free head %u -> %u)", was,
            *reinterpret_cast<uint16_t*>(world + kWorldFreeHead));
    }
    g_reapply_b = nullptr;
}

void ApplyDelta(const uint8_t* data, int len) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    DeltaReader r = {data, data + len, false};
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    int created = 0, destroyed = 0, counts = 0, invs = 0, hps = 0, conflicts = 0;
    static uint16_t slots[256], seqs[256];
    int n = 0;
    for (uint8_t op = r.U8(); !r.bad && op != 'E'; op = r.U8()) {
        if (op == 'D') {
            uint16_t slot = r.U16();
            uint8_t* ent = slot < EntityCount(world) ? *reinterpret_cast<uint8_t**>(e + slot * 8) : nullptr;
            if (ent && ent != dummy) {
                reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(ent))[10])(ent);
                destroyed++;
            }
        } else if (op == 'C') {
            uint16_t slot = r.U16();
            int16_t seq = static_cast<int16_t>(r.U16());
            uint32_t serial = r.U32(), size = r.U32();
            if (!r.Has(static_cast<int>(size))) break;
            void* p = *reinterpret_cast<void**>(e + slot * 8);
            if (p && p != dummy) {
                Log("co-op: slot %u is in use here: cannot create the talker's entity", slot);
                conflicts++;
            } else {
                uint16_t got = CreateEntityInSlot(world, slot, seq, r.p, size);
                e = Entries(world);
                if (got != slot) {
                    Log("co-op: the talker's entity for slot %u landed in slot %u", slot, got);
                    conflicts++;
                } else {
                    *reinterpret_cast<uint32_t*>(EntityById(world, slot) + 0x5d) = serial;
                    created++;
                }
            }
            r.p += size;
        } else if (op == 'N') {
            uint16_t slot = r.U16();
            int count = static_cast<int>(r.U32());
            uint8_t* ent = EntityById(world, slot);
            if (ent && ent != dummy) {
                *reinterpret_cast<int*>(ent + kItemCount) = count;
                counts++;
            }
        } else if (op == 'H') {
            uint16_t id = r.U16();
            int hp = static_cast<int>(r.U32());
            if (IsActor(world, id)) {
                *reinterpret_cast<int*>(EntityById(world, id) + kActorHp) = hp;
                hps++;
            }
        } else if (op == 'P') {
            uint16_t id = r.U16();
            float pos[3] = {};
            if (!r.Has(12)) break;
            memcpy(pos, r.p, 12);
            r.p += 12;
            if (IsActor(world, id)) {
                uint8_t* ent = EntityById(world, id);
                memcpy(ent + kEntityPos, pos, sizeof(pos));
                __try {  // the map grid (World+4), as the recruit screen updates it (0x4d6980)
                    reinterpret_cast<void(__thiscall*)(void*, void*)>(0x6e9bc0)(world + 4, ent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("co-op: exception 0x%08x moving actor %u on the map grid", GetExceptionCode(), id);
                }
                hps++;
            }
        } else if (op == 'I') {
            uint16_t owner = r.U16();
            if (!ReadIdList(r, slots, seqs, &n, 256)) break;
            uint8_t* actor = EntityById(world, owner);
            uint16_t inv_id = actor ? *reinterpret_cast<uint16_t*>(actor + kActorInventory + 2) : 0;
            uint8_t* invent = inv_id ? EntityById(world, inv_id) : nullptr;
            if (!invent || *reinterpret_cast<uintptr_t*>(invent) != kInventoryVtable) {
                Log("co-op: actor %u has no inventory here", owner);
                conflicts++;
                continue;
            }
            RewriteIdList(*reinterpret_cast<uint8_t**>(invent + kInventoryList), slots, seqs, n);
            *reinterpret_cast<int*>(invent + 0x1ad) = n;
            invs++;
        } else if (op == 'Q') {
            if (!ReadIdList(r, slots, seqs, &n, 16)) break;
            uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
            uint8_t* lhead = *reinterpret_cast<uint8_t**>(begin + 1 * kPlayerSize + kPlayerMembers);
            int* size = reinterpret_cast<int*>(begin + 1 * kPlayerSize + kPlayerMembers + 4);
            int had = RewriteIdList(lhead, slots, seqs, n);
            Log("co-op: squad rewritten: %d -> %d members (list size field was %d)", had, n, *size);
            if (*size == had) *size = n;
        } else if (op == 'B') {
            g_reapply_b = r.p;
            g_reapply_wait = 1;  // the destroys finish during the next frame: write it again before the one after
            uint16_t count = r.U16(), head = r.U16();
            uint32_t serial = r.U32();
            int nb = r.U16();
            e = Entries(world);
            for (int i = 0; i < nb && !r.bad; i++) {
                uint16_t slot = r.U16(), next_free = r.U16(), seq = r.U16();
                if (slot < EntityCount(world)) {
                    *reinterpret_cast<uint16_t*>(e + slot * 8 + 4) = next_free;
                    *reinterpret_cast<uint16_t*>(e + slot * 8 + 6) = seq;
                }
            }
            *reinterpret_cast<uint16_t*>(world + kWorldFreeHead) = head;
            *reinterpret_cast<uint32_t*>(kEntitySerial) = serial;
            if (count != EntityCount(world)) {
                Log("co-op: entity table size here %d, the talker's %u", EntityCount(world), count);
                conflicts++;
            }
        } else {
            Log("co-op: unknown delta op 0x%02x", op);
            r.bad = true;
        }
    }
    Log("co-op: delta applied: %d created, %d destroyed, %d stack counts, %d inventories, %d HP, %d conflicts%s",
        created, destroyed, counts, invs, hps, conflicts, r.bad ? ", MALFORMED" : "");
}

// ---- receiving: every copy, on the same frame (Actor vtable slot 510, ActOnControllerCommand) ----
const uintptr_t kActorActSlot = 0x80c1d0 + 510 * 4;
const uintptr_t kActorAct = 0x5747e0;
typedef void(__fastcall* ActFn)(void*, void*, uint8_t*);
ActFn g_actor_act = nullptr;

struct NpcLock { uint16_t npc; bool mine; DWORD since; };
const DWORD kLockMaxMs = 10 * 60 * 1000;
uint16_t g_host_hold_npc = 0;  // host: the game is held for a joiner's conversation with this NPC
uint16_t g_last_other_npc = 0;  // the NPC of the last talk command from another player
DWORD g_host_hold_since = 0;

// Host, when another player's conversation opens here too (PrivateScreenTick closes it): the joiner's world stops
// while its private screens are up; if the host played on, the joiner would replay those frames with its changes
// already made (recruit_join: an NPC walked round a moved squad member and the copies drifted apart). So the host
// holds the game (the pause counter, as the barter screen raises it) until the joiner's kSyncDone reaches the server
// (HostFilterTalks). Held from the conversation's opening, not the talk command: the talker first turns to the NPC,
// and the joiner needs those frames. A host conversation already stops everyone: the ticks come from its world.
void HostHoldForOther() {
    if (g_host_hold_npc || !g_last_other_npc) return;
    g_host_hold_npc = g_last_other_npc;
    g_host_hold_since = GetTickCount();
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x4fc800)(*reinterpret_cast<uint8_t**>(kWorldPtr), 1);  // World+0x2db
    Log("co-op: holding the game while another player talks to NPC %u", g_host_hold_npc);
    Notice("Co-op: another player is talking to someone - the game waits for them");
}

void HostReleaseHold(const char* why) {
    if (!g_host_hold_npc) return;
    Log("co-op: the game goes on (%s, held %lu ms)", why, GetTickCount() - g_host_hold_since);
    g_host_hold_npc = 0;
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x4fc800)(*reinterpret_cast<uint8_t**>(kWorldPtr), 0);  // World+0x2db
}

void HostHoldTimeout() {  // window ticks (no frames run while held): a joiner that never finishes, or left
    bool alone = g_coop_host && LiveJoiners() == 0;
    if (g_host_hold_npc && (GetTickCount() - g_host_hold_since > kLockMaxMs || alone))
        HostReleaseHold(alone ? "the other player left" : "waited too long");
}
NpcLock g_npc_locks[16];

NpcLock* FindLock(uint16_t npc) {
    for (auto& l : g_npc_locks)
        if (l.npc == npc && GetTickCount() - l.since < kLockMaxMs) return &l;
    return nullptr;
}

extern volatile DWORD g_local_talk_tick;
uint16_t g_local_talk_npc = 0;  // the NPC of this copy's last talk command (client-send hook)

uint8_t g_rx_delta[kMaxDelta];
int g_rx_len = 0;

void HandleCoopCommand(const uint8_t* r) {
    uint8_t kind = r[3], n = r[4];
    uint16_t npc;
    memcpy(&npc, r + 5, 2);
    if (kind == kSyncChunk) {
        uint16_t idx;
        memcpy(&idx, r + 7, 2);
        int off = idx * kChunk;
        if (n <= kChunk && off + n <= kMaxDelta) {
            memcpy(g_rx_delta + off, r + 9, n);
            if (off + n > g_rx_len) g_rx_len = off + n;
        }
    } else if (kind == kSyncDone) {
        uint32_t size, hash;
        memcpy(&size, r + 9, 4);
        memcpy(&hash, r + 13, 4);
        if (r[7] == kDiffApplied) {
            if (g_delta_mine) {
                Log("co-op: our delta went round (%u bytes): the other copies apply it now", size);
            } else if (static_cast<int>(size) != g_rx_len || Fnv(2166136261u, g_rx_delta, size) != hash) {
                Log("co-op: delta incomplete (%d of %u bytes, hash %s): not applied", g_rx_len, size,
                    Fnv(2166136261u, g_rx_delta, g_rx_len) == hash ? "ok" : "bad");
            } else {
                ApplyDelta(g_rx_delta, static_cast<int>(size));
            }
        }
        g_delta_mine = false;
        g_rx_len = 0;
        if (NpcLock* l = FindLock(npc)) l->npc = 0;
        if (npc == g_host_hold_npc) HostReleaseHold("the other player is done");
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        Log("co-op: NPC %u is free again (%s) at frame %d", npc,
            r[7] == kDiffNone ? "nothing changed" : r[7] == kDiffApplied ? "delta" : "full re-sync",
            world ? *reinterpret_cast<int*>(world + kWorldFrame) : -1);
    }
}

void __fastcall ActorActHook(void* actor, void* edx, uint8_t* cmd) {
    if (cmd && cmd[0] == kCoopCmd) return;  // handled when it arrived (CoopFromNetwork)
    if (cmd && cmd[0] == 0x17 && (g_coop_host || g_coop_join)) {  // a talk: that NPC is busy until kSyncDone
        uint16_t npc;
        memcpy(&npc, cmd + 5, 2);
        bool mine = npc == g_local_talk_npc && GetTickCount() - g_local_talk_tick < 10000;
        NpcLock* l = FindLock(npc);
        if (!l)
            for (auto& s : g_npc_locks)
                if (!s.npc || GetTickCount() - s.since >= kLockMaxMs) {
                    l = &s;
                    break;
                }
        if (l) {
            l->npc = npc;
            l->mine = mine;
            l->since = GetTickCount();
        }
        Log("co-op: NPC %u is busy with %s", npc, mine ? "this player" : "another player");
        if (!mine) g_last_other_npc = npc;
    }
    g_actor_act(actor, edx, cmd);
}

// Client send: this player's talk to an NPC someone else is talking to never leaves this copy.
bool RefuseLockedTalk(uint8_t* msg, int len) {
    if (len < 6 || *reinterpret_cast<uint32_t*>(msg) != 2) return false;
    int count = *reinterpret_cast<uint16_t*>(msg + 4);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len; i++) {
        uint8_t* r = msg + 6 + i * 27;
        if (r[0] != 0x17) continue;
        uint16_t npc;
        memcpy(&npc, r + 5, 2);
        NpcLock* l = FindLock(npc);
        if (l && !l->mine) {
            Log("co-op: refused this player's talk to NPC %u: another player is talking to them", npc);
            Notice("Co-op: another player is talking to them - try again when they are done");
            return true;
        }
        g_local_talk_npc = npc;
    }
    return false;
}

// Host, server receive: two players talking to the same NPC at the same moment. The first talk relayed wins until
// its talker's kSyncDone comes through; a talk from anyone else to that NPC is dropped from the message.
struct HostLock { uint16_t npc; int sender; DWORD since; };
HostLock g_host_locks[16];

int HostFilterTalks(uint8_t* msg, int len, int sender) {
    if (len < 6 || *reinterpret_cast<uint32_t*>(msg) != 2) return len;
    int count = *reinterpret_cast<uint16_t*>(msg + 4);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len;) {
        uint8_t* r = msg + 6 + i * 27;
        uint16_t npc;
        memcpy(&npc, r + 5, 2);
        if (r[0] == kCoopCmd && r[3] == kSyncDone) {
            for (auto& l : g_host_locks)
                if (l.npc == npc && l.sender == sender) l.npc = 0;
        } else if (r[0] == 0x17) {
            HostLock* held = nullptr;
            for (auto& l : g_host_locks)
                if (l.npc == npc && GetTickCount() - l.since < kLockMaxMs) held = &l;
            if (held && held->sender != sender) {
                Log("host: dropped player %d's talk to NPC %u: player %d is talking to them", sender, npc, held->sender);
                memmove(r, r + 27, len - (r + 27 - msg));
                len -= 27;
                count--;
                *reinterpret_cast<uint16_t*>(msg + 4) = static_cast<uint16_t>(count);
                continue;
            }
            if (!held)
                for (auto& l : g_host_locks)
                    if (!l.npc || GetTickCount() - l.since >= kLockMaxMs) {
                        held = &l;
                        break;
                    }
            if (held) {
                held->npc = npc;
                held->sender = sender;
                held->since = GetTickCount();
            }
        }
        i++;
    }
    return len;
}


// Co-op commands are applied the moment they arrive, not on their lockstep frame: every copy but the talker's holds
// its game from the frame the conversation opened (the same frame everywhere), so applying on arrival puts the change
// on that frame on every copy, before the next frame runs (ticks run a few frames ahead, so the lockstep frame would
// be too late). The host takes them from its server receive (a client's msg 2, records at +6); a joiner from the tick
// as it arrives (S->C type 1: 32-byte header, then 27-byte records).
void CoopRecords(const uint8_t* recs, int n) {
    for (int i = 0; i < n; i++) {
        const uint8_t* r = recs + i * 27;
        if (r[0] != kCoopCmd) continue;
        __try {
            HandleCoopCommand(r);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("co-op: exception 0x%08x in a co-op command", GetExceptionCode());
        }
    }
}

void CoopFromNetwork(const uint8_t* msg, int len, bool server_side) {
    if (len < 6) return;
    uint32_t type = *reinterpret_cast<const uint32_t*>(msg);
    if (server_side && type == 2) {
        int n = *reinterpret_cast<const uint16_t*>(msg + 4);
        if (6 + n * 27 <= len) CoopRecords(msg + 6, n);
    } else if (!server_side && type == 1 && len > 32 && (len - 32) % 27 == 0) {
        CoopRecords(msg + 32, (len - 32) / 27);
    }
}
