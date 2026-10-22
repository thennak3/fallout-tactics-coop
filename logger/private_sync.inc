// ---- Private screens without a reload: the talker's changes reach the other copies as lockstep commands ----
// Included from bot_scenarios.inc (needs its entity helpers). A conversation (and the barter / recruit screens it
// leads to) runs on the talker's copy only (see PrivateScreenTick). When it opens, that copy takes a snapshot; when
// the screens close, it compares:
//   - mission and campaign variables (World +0x386.. / +0x396.., *0x8bdd2c +0x2b.. / +0x3b..; FTSE World.h/.cpp),
//   - the squad, every Actor's HP and inventory,
//   - the entity table (World+0x743, entries {ptr, i16 next_free, i16 seq}; free-list head u16 at World+0x74f).
// The talker writes a delta and sends it as co-op commands; every other copy applies it on the same frame (only a change
// the delta cannot carry, such as a new entity-table size, falls back to the old save and reload):
//   'V' campaign index key value: a mission or campaign variable, set as a trigger sets it (SetGameVar);
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
//   kSyncChunk: [5..26] n (<= 22) bytes of the compressed delta, in order;
//   kSyncDone: [5..6] npc, [7] kDiff*, [8] bit 1: compressed (LZNT1, ntdll RtlCompressBuffer), bit 2: no chunks, the
//   bytes are on the host's file server (a joiner talker uploaded them first; the others fetch them), [9..12] the sent size,
//   [13..16] its FNV hash, [17..20] the delta's size before compression.
// A new recruit is a whole character (Actor 12.7 KB serialized, with AI, inventory, armour, weapons 23 KB): sent raw it
// took 1288 commands in one tick and the game stopped ("Invalid number of commands passed", limit 1000).
// The same commands lock the NPC: a talk command (0x17) to an NPC locks it on every copy (the command runs on every
// copy on the same frame), kSyncDone unlocks it. Whose conversation it is: every player's talk to an NPC is the same
// command (the shared squad's member, the same NPC), so each player sends a claim first, kTalkClaim: [7..10] a random
// number of its own. The host passes on only the first claim and talk for a free NPC, so every copy learns the
// winner from the claim it relays (four bots talking to the merchant at once: all three joiners took it as theirs). A copy refuses its own player's talk to a locked NPC; the host drops
// a talk to a locked NPC from anyone else (two players clicking at the same moment).
const uint8_t kCoopCmd = 0x30;
enum { kSyncChunk = 1, kSyncDone = 3, kTalkClaim = 4, kShopOpen = 5, kShopClose = 6, kRecruitOpen = 7 };  // 5, 6: shop_sync.inc; 7: recruit_sync.inc
bool g_recruit_synced = false;  // a recruit screen is up and every copy made its candidates (recruit_sync.inc)
void HandleRecruitOpen(const uint8_t* r);  // recruit_sync.inc
void CloseHeadlessRecruit();
uint16_t g_shop_new[16];  // test mode: a shop deal's new stacks, followed through the close (shop_sync.inc)
int g_shop_new_n = 0;
bool g_applying_at_frame = false;  // ApplyDelta from a delta at its frame: no second write of the bookkeeping
enum { kDiffNone = 0, kDiffApplied = 1, kDiffFull = 2 };
const size_t kWorldFreeHead = 0x74f;
const uintptr_t kEntitySerial = 0x8be148;
const int kMaxSlots = 8192;
const int kChunk = 22;        // payload bytes per chunk command ([5..26]; chunks arrive in order on one stream)
const int kMaxChunks = 800;   // the game allows 1000 commands in one tick (0x63bc10, commands.cpp line 22)
int g_recreate_recruits = 0;  // [coop] recreate_recruits: experimental, see PrivateScreensDiff
int g_lockstep_chunks = 300;  // [coop] lockstep_chunks: bigger deltas go through the host's file server (DPUT / DGET)
const int kMaxDelta = 4 * 1024 * 1024;  // a few recruits with full packs fit (the file-server path has no lockstep limit)

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

// One variable list: keys and values are parallel arrays of wide strings (mission: World +0x396/+0x39a keys, +0x386
// values; campaign: *0x8bdd2c +0x3b/+0x3f keys, +0x2b values; FTSE World.h).
const int kMaxVars = 4096;
struct VarList { wchar_t** keys; wchar_t** vals; int n; };

bool GetVarList(uint8_t* world, int campaign, VarList* v) {
    __try {
        uint8_t* base = campaign ? *reinterpret_cast<uint8_t**>(0x8bdd2c) : world;
        v->keys = v->vals = nullptr;
        v->n = 0;
        if (!base) return campaign != 0;  // no campaign loaded: an empty list
        v->keys = *reinterpret_cast<wchar_t***>(base + (campaign ? 0x3b : 0x396));
        wchar_t** end = *reinterpret_cast<wchar_t***>(base + (campaign ? 0x3f : 0x39a));
        v->vals = *reinterpret_cast<wchar_t***>(base + (campaign ? 0x2b : 0x386));
        v->n = v->keys ? static_cast<int>(end - v->keys) : 0;
        return v->n >= 0 && v->n <= kMaxVars;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t VarHash(const VarList& v, int i) {
    return HashWide(HashWide(2166136261u, v.keys[i]), v.vals ? v.vals[i] : nullptr);
}

// Plain ASCII only (variable names and their values are short numbers and names); false for anything else.
bool NarrowVar(const wchar_t* s, char* out, int cap, int* len) {
    *len = 0;
    for (; s && *s; s++) {
        if (*s < 0x20 || *s > 0x7e || *len + 1 >= cap) return false;
        out[(*len)++] = static_cast<char>(*s);
    }
    out[*len] = 0;
    return true;
}

// As a trigger's "set variable" does (FTSE World::SetVariable): a command object, vtable 0x837b24, run by 0x67ee30.
bool SetGameVar(wchar_t* key, const char* value, bool campaign) {
#pragma pack(push, 1)
    struct {
        uint32_t vtable;
        uint32_t unknown_0;
        char unknown_0a[3];
        wchar_t* key;
        wchar_t* value;
        bool campaign;
    } cmd = {};
#pragma pack(pop)
    cmd.vtable = 0x837b24;
    cmd.key = key;
    cmd.value = static_cast<wchar_t*>(MakeFOTString(value));
    cmd.campaign = campaign;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(0x67ee30)(&cmd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("co-op: exception 0x%08x setting a variable", GetExceptionCode());
        return false;
    }
    return true;
}

uint32_t g_snap_var_h[2][kMaxVars];
int g_snap_nvars[2] = {-1, -1};

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


// Test mode: every actor's +0x117 flags byte that is not 0 (an NPC AI state the recruit screen changed on one copy).
void LogAiFlags(const char* when) {
    if (!g_test.enabled) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    char line[2048] = "";
    size_t len = 0;
    for (int i = 1; i < EntityCount(world) && len + 16 < sizeof(line); i++)
        if (IsActor(world, i) && EntityById(world, static_cast<uint16_t>(i))[0x117])
            len += sprintf_s(line + len, sizeof(line) - len, " %d:%02x", i, EntityById(world, static_cast<uint16_t>(i))[0x117]);
    Log("AIFLAGS %s frame %d:%s", when, *reinterpret_cast<int*>(world + kWorldFrame), line[0] ? line : " none");
    // and every actor's exact position bits, to see whether positions move while the world is paused
    static char pl[16384];
    size_t pn = 0;
    for (int i = 1; i < EntityCount(world) && pn + 48 < sizeof(pl); i++) {
        if (!IsActor(world, i)) continue;
        uint32_t q[3];
        memcpy(q, EntityPos(world, static_cast<uint16_t>(i)), 12);
        pn += sprintf_s(pl + pn, sizeof(pl) - pn, " %d:%08x,%08x,%08x", i, q[0], q[1], q[2]);
    }
    Log("POSBITS %s frame %d:%s", when, *reinterpret_cast<int*>(world + kWorldFrame), pl);
}

void TakePrivateSnapshot() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    ReadSlots(world, g_snap_slots, &g_snap_count);
    g_snap_head = *reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
    g_snap_vars = HashVars(world);
    for (int c = 0; c < 2; c++) {
        VarList v;
        g_snap_nvars[c] = GetVarList(world, c, &v) ? v.n : -1;
        for (int i = 0; i < g_snap_nvars[c]; i++) g_snap_var_h[c][i] = VarHash(v, i);
    }
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
    LogAiFlags("at the snapshot");
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

// Create the entity in `bytes` at the free list's head as the caller arranged it; returns the slot it took (0: failed).
uint16_t CreateEntityChained(uint8_t* world, const uint8_t* bytes, uint32_t len) {
    FILE* f = nullptr;
    if (fopen_s(&f, kEntityTmpPath, "wb") != 0 || !f) {
        Log("co-op: cannot write %s", kEntityTmpPath);
        return 0;
    }
    fwrite(bytes, 1, len, f);
    fclose(f);
    uint32_t out[2] = {};
    void* name = MakeFOTString(kEntityTmpName);
    __try {
        reinterpret_cast<void*(__cdecl*)(uint32_t*, void**, int)>(0x5ea400)(out, &name, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("co-op: exception 0x%08x creating an entity", GetExceptionCode());
        return 0;
    }
    return static_cast<uint16_t>(out[0] >> 16);
}

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

// An entity's template name (Entity +0x199, a FOTString: "pullTabs.ent"): made from its bytes on another copy, it took
// the temporary file's name instead ("ftcoop_entity.tmp"), and a quartermaster's stock list, rebuilt from the names of
// what is left in the shop, then differed (trade, shop_no_pause). So 'C' carries it: u16 length, ASCII.
void PutEntityName(DeltaWriter& w, uint8_t* ent) {
    char name[260] = "";
    int len = 0;
    __try {
        const wchar_t* nm = ent ? *reinterpret_cast<wchar_t**>(ent + 0x199) : nullptr;
        for (; nm && nm[len] && len < 259; len++) name[len] = nm[len] < 0x80 ? static_cast<char>(nm[len]) : '?';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        len = 0;
    }
    w.U16(static_cast<uint16_t>(len));
    w.Put(name, len);
}

void SetEntityName(uint8_t* ent, const uint8_t* name, int len) {
    if (!ent || len <= 0 || len > 259) return;
    char buf[260];
    memcpy(buf, name, len);
    buf[len] = 0;
    void* str = MakeFOTString(buf);
    reinterpret_cast<void*(__thiscall*)(void*, void**)>(kStringAssign)(ent + 0x199, &str);
}

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
uint8_t g_delta_z[kMaxDelta + 4096];
int g_delta_b_off = -1;  // where the delta's 'B' op starts in g_delta
extern const uint8_t* g_reapply_b;
extern int g_reapply_wait;
// The ops after 'W', run after the next frame's bookkeeping (ReapplyBookkeeping) on every copy, the talker too.
const uint8_t* g_deferred_ops = nullptr;
const uint8_t* g_deferred_end = nullptr;
bool g_deferred_mine = false;
const uint8_t* DeferredStart(const uint8_t* delta, int len);
bool g_delta_mine = false;

// LZNT1 through ntdll (on every Windows; COMPRESSION_FORMAT_LZNT1 = 2). 0 on failure.
typedef LONG(NTAPI* RtlCompressFn)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
typedef LONG(NTAPI* RtlDecompressFn)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG);
typedef LONG(NTAPI* RtlWorkSpaceFn)(USHORT, PULONG, PULONG);
uint32_t Compress(const uint8_t* src, uint32_t n, uint8_t* dst, uint32_t cap) {
    static RtlCompressFn comp = nullptr;
    static void* work = nullptr;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (!comp && nt) {
        comp = reinterpret_cast<RtlCompressFn>(GetProcAddress(nt, "RtlCompressBuffer"));
        RtlWorkSpaceFn ws = reinterpret_cast<RtlWorkSpaceFn>(GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize"));
        ULONG a = 0, b = 0;
        if (ws && ws(2, &a, &b) == 0) work = HeapAlloc(GetProcessHeap(), 0, a);
    }
    ULONG out = 0;
    if (!comp || !work || comp(2, const_cast<PUCHAR>(src), n, dst, cap, 4096, &out, work) != 0) return 0;
    return out;
}
uint32_t Decompress(const uint8_t* src, uint32_t n, uint8_t* dst, uint32_t cap) {
    static RtlDecompressFn dec = nullptr;
    if (!dec) dec = reinterpret_cast<RtlDecompressFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlDecompressBuffer"));
    ULONG out = 0;
    if (!dec || dec(2, dst, cap, const_cast<PUCHAR>(src), n, &out) != 0) return 0;
    return out;
}  // this copy sent the delta now in flight: do not apply it to ourselves

// What PrivateScreensDiff looks at: 0 everything (a held conversation); 1 the quest variables only (a conversation
// that paused nobody: the world ran meanwhile); 2 a shop's deal only (shop_sync.inc: the buyer's and merchant's
// inventories). And the kind of its last record (kShopClose for a shop).
int g_diff_mode = 0;
uint8_t g_done_kind = kSyncDone;
struct ShopState;
extern ShopState g_shop;
int ShopDealDiff(DeltaWriter& w, uint8_t* world, const SlotState* now, int count);  // below

int PrivateScreensDiff() {
    LogAiFlags("at the diff");
    g_delta_b_off = -1;
    int mode = g_diff_mode;
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
        int news = 0, gone = 0, counts = 0, invs = 0, hps = 0, moved = 0, nvars = 0;
        // 'V' campaign index key value: a changed mission or campaign variable (a dialogue choice, a script).
        const char* var_fail = nullptr;
        for (int c = 0; c < 2 && vars != g_snap_vars && !var_fail; c++) {
            VarList v;
            if (!GetVarList(world, c, &v) || v.n != g_snap_nvars[c]) {
                var_fail = "the number of quest variables";
                break;
            }
            for (int i = 0; i < v.n && !var_fail; i++) {
                if (VarHash(v, i) == g_snap_var_h[c][i]) continue;
                char key[128], val[256];
                int kl = 0, vl = 0;
                if (!NarrowVar(v.keys[i], key, sizeof(key), &kl) || !NarrowVar(v.vals ? v.vals[i] : nullptr, val, sizeof(val), &vl)) {
                    var_fail = "a quest variable that is not plain text";
                    break;
                }
                w.U8('V');
                w.U8(static_cast<uint8_t>(c));
                w.U16(static_cast<uint16_t>(i));
                w.U16(static_cast<uint16_t>(kl));
                w.Put(key, kl);
                w.U16(static_cast<uint16_t>(vl));
                w.Put(val, vl);
                nvars++;
                Log("co-op: delta: %s variable %d \"%s\" = \"%s\"", c ? "campaign" : "mission", i, key, val);
            }
        }
        // The recruit screen makes its candidates as real characters (at 0,0,0) on this copy only while it is open,
        // and making and destroying them leaves something NPCs read different here: with positions, random state and
        // entity table identical everywhere, an NPC still walked another way 200 frames later (recruit_host, about
        // one run in three). Until every copy makes them too (the screen's allocation run everywhere), a recruit
        // visit is the old save and reload.
        extern bool g_private_recruit;
        if (g_private_recruit && !g_recreate_recruits && !g_recruit_synced) var_fail = "the squad at the recruit screen";
        if (mode == 1 && !var_fail) {  // the variables only
            w.U8('E');
            result = nvars ? kDiffApplied : kDiffNone;
            Log("co-op: a conversation that paused nobody changed %d variables", nvars);
        } else if (mode == 2) {  // the variables written above, then the deal
            int n = ShopDealDiff(w, world, now, count);
            w.U8('E');
            result = n || nvars ? kDiffApplied : kDiffNone;
        } else if (var_fail || count != g_snap_count) {
            Log("co-op: private screens changed %s: full re-sync", var_fail ? var_fail : "the entity table size");
            result = kDiffFull;
        } else {
            // A destroy is deferred: DestructEntity (vtable slot 10) sets +0x141 and the entity leaves its slot during
            // the next frame (its next_free and seq stay). Such an entity counts as gone here, and its slot goes into the
            // bookkeeping, so the copies agree once the destroys have finished (recruit: a dismissed member's gear was
            // still pending when the screens closed, and the other copies kept it).
            static bool pending[kMaxSlots];
            auto read_pending = [&]() {
                for (int i = 0; i < count; i++) {
                    uint8_t* ent = now[i].used ? *reinterpret_cast<uint8_t**>(Entries(world) + i * 8) : nullptr;
                    pending[i] = ent && ent[0x141] != 0;
                }
            };
            read_pending();
            // A new character (a recruit) cannot be copied slot for slot: made from its bytes, its AI, inventories and
            // gear come out of the free list in another order, and even as other entities, than the recruit screen
            // made them. So every copy makes it the same way: this copy keeps its bytes, destroys the screen's
            // character (its children go with it), and after the next frame, with every copy's table written the
            // same ('B'), all of them, this one too, make it from the bytes ('W': ops run then; 'R': make one).
            static uint8_t* new_bytes[16];
            static int new_size[16];
            static uint16_t new_slot[16];
            int nnew = 0;
            uint32_t first_serial = 0xffffffffu;
            for (int i = 1; i < count && nnew < 16; i++)
                if (now[i].used && !pending[i] && (!g_snap_slots[i].used || now[i].seq != g_snap_slots[i].seq) &&
                    IsActor(world, i)) {
                    int n = 0;
                    uint8_t* bytes = SerializeEntity(world, static_cast<uint16_t>(i), &n);
                    if (!bytes || n <= 0) {
                        Log("co-op: could not serialize new character %u: full re-sync", i);
                        result = kDiffFull;
                        break;
                    }
                    new_bytes[nnew] = bytes;
                    new_size[nnew] = n;
                    new_slot[nnew++] = static_cast<uint16_t>(i);
                    uint32_t ser = *reinterpret_cast<uint32_t*>(EntityById(world, static_cast<uint16_t>(i)) + 0x5d);
                    if (ser < first_serial) first_serial = ser;
                }
            // Not finished, so off by default ([coop] recreate_recruits=1): made this way, the new character matched on
            // every copy, slot for slot, but destroying the recruit screen's own character changed an NPC's AI state on
            // this copy only (NPC 46 +0x117; it then walked another way). A new character means the old save and
            // reload for now.
            if (nnew && !g_recreate_recruits) {
                Log("co-op: a new character joined the squad (%u): full re-sync", new_slot[0]);
                result = kDiffFull;
                nnew = 0;
            }
            for (int k = 0; k < nnew; k++) {
                uint8_t* ent = EntityById(world, new_slot[k]);
                Log("co-op: new character %u (%d bytes): every copy makes it from its bytes after the next frame",
                    new_slot[k], new_size[k]);
                reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(ent))[10])(ent);
            }
            if (nnew) {  // whatever came with them and is not marked yet (serial 0: an AI made without one)
                for (int i = 1; i < count; i++) {
                    uint8_t* ent = now[i].used ? *reinterpret_cast<uint8_t**>(Entries(world) + i * 8) : nullptr;
                    if (!ent || ent[0x141] || (g_snap_slots[i].used && now[i].seq == g_snap_slots[i].seq)) continue;
                    uint32_t ser = *reinterpret_cast<uint32_t*>(ent + 0x5d);
                    if (ser != 0 && ser < first_serial) continue;  // made before the recruit: a trade's
                    Log("co-op: new entity %u (%s) goes with the new character", i, RttiName(ent));
                    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(ent))[10])(ent);
                }
                read_pending();
            }
            // 'D': gone, replaced or being destroyed
            for (int i = 1; i < count; i++)
                if (g_snap_slots[i].used && (!now[i].used || now[i].seq != g_snap_slots[i].seq || pending[i])) {
                    w.U8('D');
                    w.U16(static_cast<uint16_t>(i));
                    gone++;
                }
            // 'C': new (bytes from the game's serializer), in the order the talker's game made them (serial +0x5d): an
            // entity's serialization holds its children (an Actor's AI, inventory, gear), so creating it on another
            // copy makes them too, from the free list in that order; the receiver chains its free list through the
            // talker's slots in this order (ApplyNewEntities) so they land where the talker has them.
            static uint16_t fresh[kMaxSlots];
            int nfresh = 0;
            for (int i = 1; i < count; i++)
                if (now[i].used && !pending[i] && (!g_snap_slots[i].used || now[i].seq != g_snap_slots[i].seq))
                    fresh[nfresh++] = static_cast<uint16_t>(i);
            auto serial_of = [&](uint16_t id) { return *reinterpret_cast<uint32_t*>(EntityById(world, id) + 0x5d); };
            std::sort(fresh, fresh + nfresh, [&](uint16_t a, uint16_t b) { return serial_of(a) < serial_of(b); });
            for (int f = 0; f < nfresh && !w.overflow; f++) {
                int i = fresh[f];
                {
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
                    PutEntityName(w, EntityById(world, static_cast<uint16_t>(i)));
                    news++;
                    uint8_t* ent = EntityById(world, static_cast<uint16_t>(i));
                    Log("co-op: new entity %u (%s): %d bytes, serial %u%s", i, RttiName(ent), n, serial_of(static_cast<uint16_t>(i)),
                        IsActor(world, i) ? "" : "");
                    if (IsActor(world, i))
                        Log("co-op: new actor %u: inventory link +0x1006 = %08x", i, *reinterpret_cast<uint32_t*>(ent + kActorInventory));
                }
            }
            // 'N', 'I', 'H': stacks, inventory lists and HP of every Actor that changed
            uint16_t sq_now[16];
            int nsq_now = GetSquad(world, sq_now, 16), nsq_before = g_snap_nsquad;
            const uint16_t* sq_before = g_snap_squad_ids;
            static uint16_t a_items[200], b_items[200];
            static int a_counts[200], b_counts[200];
            char s[2048];
            for (int i = 1; i < count; i++) {
                if (!IsActor(world, i) || pending[i]) continue;
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
            if (squad != g_snap_squad && !nnew) {
                uint16_t ids[16];
                int n = GetSquad(world, ids, 16);
                w.U8('Q');
                PutIdList(w, world, ids, n);
            }
            // 'B': the table's bookkeeping
            int nb = 0;
            for (int i = 0; i < count; i++)
                if (now[i].next_free != g_snap_slots[i].next_free || now[i].seq != g_snap_slots[i].seq || pending[i]) nb++;
            w.U8('B');
            g_delta_b_off = w.len;
            w.U16(static_cast<uint16_t>(count));
            w.U16(head);
            w.U32(*reinterpret_cast<uint32_t*>(kEntitySerial));
            w.U16(static_cast<uint16_t>(nb));
            for (int i = 0; i < count; i++)
                if (now[i].next_free != g_snap_slots[i].next_free || now[i].seq != g_snap_slots[i].seq || pending[i]) {
                    w.U16(static_cast<uint16_t>(i));
                    w.U16(static_cast<uint16_t>(now[i].next_free));
                    w.U16(static_cast<uint16_t>(now[i].seq));
                }
            moved = nb;
            if (nnew) {
                w.U8('W');  // from here on, after the next frame
                for (int k = 0; k < nnew; k++) {
                    w.U8('R');
                    w.U32(static_cast<uint32_t>(new_size[k]));
                    w.Put(new_bytes[k], new_size[k]);
                    news++;
                }
                uint16_t ids[16];
                int n = GetSquad(world, ids, 16);
                w.U8('Q');  // the squad, with 0xfff0 + k for the k-th new character
                w.U16(static_cast<uint16_t>(n));
                for (int i = 0; i < n; i++) {
                    int k = 0;
                    while (k < nnew && new_slot[k] != ids[i]) k++;
                    w.U16(k < nnew ? static_cast<uint16_t>(0xfff0 + k) : ids[i]);
                    w.U16(k < nnew ? 0 : static_cast<uint16_t>(FullId(world, ids[i]) & 0xffff));
                }
                g_deferred_mine = true;
            }
            w.U8('E');
            if (w.overflow) {
                Log("co-op: delta over %d bytes: full re-sync", kMaxDelta);
                result = kDiffFull;
            } else if (result != kDiffFull) {
                bool any = news || gone || counts || invs || hps || nvars || squad != g_snap_squad || moved ||
                           head != g_snap_head;
                result = any ? kDiffApplied : kDiffNone;
            }
            Log("co-op: private screens changed: inventories %s, squad %s; delta: %d new, %d gone, %d stack counts, "
                "%d inventories, %d HP, %d variables, %d table entries, free head %u -> %u, %d bytes",
                inv != g_snap_inv ? "yes" : "no", squad != g_snap_squad ? "yes" : "no", news, gone, counts, invs, hps,
                nvars, moved, g_snap_head, head, w.len);
        }
    }
    // Records: the delta compressed, in chunks (only when applied), then the unlock with what happened.
    const uint8_t* body = g_delta;
    uint32_t size = 0, raw = result == kDiffApplied ? static_cast<uint32_t>(w.len) : 0;
    bool packed = false;
    if (result == kDiffApplied) {
        uint32_t z = Compress(g_delta, raw, g_delta_z, sizeof(g_delta_z));
        packed = z > 0 && z < raw;
        body = packed ? g_delta_z : g_delta;
        size = packed ? z : raw;
        Log("co-op: delta %u bytes, %s %u bytes: %u chunk commands", raw, packed ? "compressed to" : "sent as", size,
            (size + kChunk - 1) / kChunk);
    }
    bool via_file = false;
    if (result == kDiffApplied && (size + kChunk - 1) / kChunk > static_cast<uint32_t>(g_lockstep_chunks)) {
        // Too big for lockstep: the host's file server holds it; one command tells the others to fetch it.
        uint32_t h = Fnv(2166136261u, body, size);
        if (g_coop_host) {
            EnterCriticalSection(&g_serve_lock);
            StoreBigDelta(body, size, h);
            LeaveCriticalSection(&g_serve_lock);
            via_file = true;
        } else {
            via_file = PutBigDelta(body, size, h);
        }
        Log("co-op: delta over %d commands: %s", g_lockstep_chunks,
            via_file ? "on the host's file server for the others to fetch" : "upload FAILED, full re-sync");
        if (!via_file) {
            result = kDiffFull;
            size = raw = 0;
        }
    }
    static uint8_t recs[kMaxChunks + 8][27];
    int nrec = 0;
    if (result == kDiffApplied) {
        for (uint32_t off = 0; !via_file && off < size; off += kChunk) {
            int n = size - off > static_cast<uint32_t>(kChunk) ? kChunk : static_cast<int>(size - off);
            CoopRecord(recs[nrec], kSyncChunk, static_cast<uint8_t>(n));
            memcpy(recs[nrec] + 5, body + off, n);
            nrec++;
        }
        g_delta_mine = true;
        // The talker too: its own destroys (a dismissed member, the shop's stock) finish during the next frame and
        // change its free list after this diff; write the bookkeeping it sent again after that frame, as the others
        // do (recruit_join: the recruiter's entity table differed from everyone else's from then on).
        if (g_delta_b_off >= 0 && mode == 0) {  // a shop's bookkeeping is written before its close, once
            g_reapply_b = g_delta + g_delta_b_off;
            g_reapply_wait = 1;
        }
        if (g_deferred_mine) {
            g_deferred_ops = DeferredStart(g_delta, w.len);
            g_deferred_end = g_delta + w.len;
        }
        g_deferred_mine = false;
    }
    CoopRecord(recs[nrec], g_done_kind, 0);
    recs[nrec][7] = static_cast<uint8_t>(result);
    recs[nrec][8] = (packed ? 1 : 0) | (via_file ? 2 : 0) | (mode != 0 ? 4 : 0);  // 4: applied at its frame
    uint32_t hash = Fnv(2166136261u, body, size);
    memcpy(recs[nrec] + 9, &size, 4);
    memcpy(recs[nrec] + 13, &hash, 4);
    memcpy(recs[nrec] + 17, &raw, 4);
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
// Find where the ops after 'W' start in a delta this copy wrote (the talker): walk the ops.
const uint8_t* DeferredStart(const uint8_t* delta, int len) {
    // The talker wrote 'W' right after the 'B' op; 'B' = count u16, head u16, serial u32, n u16, n x 6 bytes.
    if (g_delta_b_off < 0) return nullptr;
    const uint8_t* b = delta + g_delta_b_off;
    uint16_t nb;
    memcpy(&nb, b + 8, 2);
    const uint8_t* p = b + 10 + nb * 6;
    return p < delta + len && *p == 'W' ? p + 1 : nullptr;
}

// After the next frame, on every copy: make the new characters from their bytes (the same table everywhere, so the
// same slots everywhere), then the squad with them in it.
void ApplyDeferred(uint8_t* world) {
    DeltaReader r = {g_deferred_ops, g_deferred_end, false};
    g_deferred_ops = g_deferred_end = nullptr;
    uint16_t made[16] = {};
    int nmade = 0;
    for (uint8_t op = r.U8(); !r.bad && op != 'E'; op = r.U8()) {
        if (op == 'R') {
            uint32_t size = r.U32();
            if (!r.Has(static_cast<int>(size))) break;
            uint16_t got = CreateEntityChained(world, r.p, size);
            r.p += size;
            if (nmade < 16) made[nmade++] = got;
            Log("co-op: delta: made new character %d in slot %u (%s)", nmade - 1, got,
                got ? RttiName(EntityById(world, got)) : "-");
        } else if (op == 'Q') {
            static uint16_t slots[16], seqs[16];
            int n = r.U16();
            if (n > 16) break;
            for (int i = 0; i < n; i++) {
                slots[i] = r.U16();
                seqs[i] = r.U16();
                if (slots[i] >= 0xfff0) {
                    int k = slots[i] - 0xfff0;
                    slots[i] = k < nmade ? made[k] : 0;
                    seqs[i] = static_cast<uint16_t>(FullId(world, slots[i]) & 0xffff);
                }
            }
            if (r.bad) break;
            uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
            uint8_t* lhead = *reinterpret_cast<uint8_t**>(begin + 1 * kPlayerSize + kPlayerMembers);
            int* lsize = reinterpret_cast<int*>(begin + 1 * kPlayerSize + kPlayerMembers + 4);
            int had = RewriteIdList(lhead, slots, seqs, n);
            if (*lsize == had) *lsize = n;
            Log("co-op: delta: squad written with the new characters: %d -> %d members", had, n);
        } else {
            Log("co-op: unknown deferred delta op 0x%02x", op);
            break;
        }
    }
}

// The delta's destroys ('D'), checked when the bookkeeping is written again: an entity still in its slot then is
// destroyed again and the bookkeeping waits one more frame (recruit: 5 of 12 entities the talker no longer had were
// still in their slots on the other copies a frame after their destroy).
uint16_t g_destroyed[512];
int g_ndestroyed = 0, g_destroy_rounds = 0;

void ReapplyBookkeeping() {
    if (!g_reapply_b) return;
    if (g_reapply_wait-- > 0) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world && g_ndestroyed && g_destroy_rounds < 3) {
        uint8_t* e = Entries(world);
        void* dummy = *reinterpret_cast<void**>(e);
        int left = 0;
        for (int i = 0; i < g_ndestroyed; i++) {
            uint8_t* ent = g_destroyed[i] < EntityCount(world) ? *reinterpret_cast<uint8_t**>(e + g_destroyed[i] * 8) : nullptr;
            if (!ent || ent == dummy) continue;
            Log("co-op: entity %u (%s) is still there after its destroy: destroying it again", g_destroyed[i], RttiName(ent));
            __try {
                reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(ent))[10])(ent);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("co-op: exception 0x%08x destroying entity %u again", GetExceptionCode(), g_destroyed[i]);
            }
            left++;
        }
        g_destroy_rounds++;
        if (left) {
            g_reapply_wait = 0;  // the bookkeeping after the frame these destroys finish in
            return;
        }
    }
    g_ndestroyed = 0;
    g_destroy_rounds = 0;
    if (world) {
        uint16_t was = *reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
        ApplyBookkeeping(world, g_reapply_b);
        Log("co-op: delta bookkeeping written again after the frame (free head %u -> %u)", was,
            *reinterpret_cast<uint16_t*>(world + kWorldFreeHead));
        if (g_deferred_ops) ApplyDeferred(world);
        LogAiFlags("after the bookkeeping");
    }
    g_reapply_b = nullptr;
}

struct NewEntity { uint16_t slot; int16_t seq; uint32_t serial, size; const uint8_t* bytes; const uint8_t* name; int nlen; };

// Creates that wait for their slot (its old entity's destroy finishes during the next frame), with the bookkeeping the
// talker had for that slot ('B'), written again once made.
struct DeferredNew { NewEntity ne; uint8_t* copy; bool have_b; uint16_t next_free; int16_t seq; };
DeferredNew g_deferred_new[16];
int g_ndeferred_new = 0;
uint8_t* g_frame_b = nullptr;  // a delta-at-its-frame's bookkeeping, written at the start of the next frame

bool DeferCreate(const NewEntity& ne) {
    if (g_ndeferred_new == 16 || !g_applying_at_frame) return false;
    DeferredNew& d = g_deferred_new[g_ndeferred_new++];
    d = {};
    d.copy = static_cast<uint8_t*>(malloc(ne.size + ne.nlen + 1));
    memcpy(d.copy, ne.bytes, ne.size);
    memcpy(d.copy + ne.size, ne.name, ne.nlen);
    d.ne = ne;
    d.ne.bytes = d.copy;
    d.ne.name = d.copy + ne.size;
    return true;
}

// An entity keeps its own EntityID too (a GenericItem at +0x146: {seq, slot}). Made here with the seq the table gave
// it, then given the talker's seq in the table, it no longer matched itself and the game would not destroy it (the
// paid ringpulls stayed when the shop's stock was destroyed, shop_no_pause). Each {made seq, slot} pair in its first
// 0x400 bytes becomes {talker's seq, slot}.
void FixSelfId(uint8_t* ent, uint16_t slot, int16_t made, int16_t seq) {
    if (!ent || made == seq) return;
    int patched = 0;
    for (int off = 8; off + 4 <= 0x400; off++) {
        int16_t sq;
        uint16_t sl;
        __try {
            memcpy(&sq, ent + off, 2);
            memcpy(&sl, ent + off + 2, 2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (sq != made || sl != slot) continue;
        memcpy(ent + off, &seq, 2);
        patched++;
        if (g_test.enabled) Log("co-op: entity %u keeps its own id at +0x%x: seq %d -> %d", slot, off, made, seq);
    }
    if (!patched && g_test.enabled) Log("co-op: entity %u: no copy of its own id found (seq %d -> %d)", slot, made, seq);
}

// Take a free slot out of the free list (head World+0x74f, links in each entry's next_free).
void UnlinkFree(uint8_t* world, uint16_t slot) {
    uint8_t* e = Entries(world);
    uint16_t* head = reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
    int n = EntityCount(world);
    if (*head == slot) {
        *head = *reinterpret_cast<uint16_t*>(e + slot * 8 + 4);
        return;
    }
    for (int cur = *head, steps = 0; cur > 0 && cur < n && steps < n; steps++) {
        uint16_t next = *reinterpret_cast<uint16_t*>(e + cur * 8 + 4);
        if (next == slot) {
            *reinterpret_cast<uint16_t*>(e + cur * 8 + 4) = *reinterpret_cast<uint16_t*>(e + slot * 8 + 4);
            return;
        }
        cur = next;
    }
}

// Create the talker's new entities in their slots. For each one not yet made (by its parent), the free list is chained
// through the slots still free, in the talker's order, so its children take the next ones as on the talker's copy.
// Returns the number of conflicts (a slot in use beforehand, an entity that landed elsewhere, a slot left empty).
int ApplyNewEntities(uint8_t* world, NewEntity* ne, int nn, int* made, int* kids) {
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    auto used = [&](uint16_t slot) {
        e = Entries(world);
        void* p = slot < EntityCount(world) ? *reinterpret_cast<void**>(e + slot * 8) : nullptr;
        return p && p != dummy;
    };
    int conflicts = 0;
    static bool done[1024];
    for (int k = 0; k < nn; k++) {
        done[k] = false;
        if (!used(ne[k].slot)) continue;
        uint8_t* occupant = EntityById(world, ne[k].slot);
        if (occupant[0x141] && DeferCreate(ne[k])) {  // freed and reused within the deal: made after the destroy is done
            Log("co-op: slot %u still holds something being destroyed: the talker's entity is made next frame", ne[k].slot);
            done[k] = true;
            continue;
        }
        Log("co-op: slot %u is in use here: cannot create the talker's entity", ne[k].slot);
        conflicts++;
        done[k] = true;
    }
    *made = *kids = 0;
    for (int k = 0; k < nn; k++) {
        if (done[k]) continue;
        uint16_t* head = reinterpret_cast<uint16_t*>(world + kWorldFreeHead);
        for (int j = k; j < nn; j++)  // each slot out of wherever it is in the free list, so it is not handed out twice
            if (!done[j] && !used(ne[j].slot)) UnlinkFree(world, ne[j].slot);
        uint16_t old = *head;
        int last = -1;
        for (int j = nn - 1; j >= k; j--) {  // head -> ne[k] -> the next free ones -> the old head
            if (done[j] || used(ne[j].slot)) continue;
            *reinterpret_cast<int16_t*>(e + ne[j].slot * 8 + 4) = static_cast<int16_t>(last < 0 ? old : ne[last].slot);
            last = j;
        }
        *head = ne[k].slot;
        uint16_t got = CreateEntityChained(world, ne[k].bytes, ne[k].size);
        e = Entries(world);
        if (got) SetEntityName(EntityById(world, got), ne[k].name, ne[k].nlen);
        if (got != ne[k].slot) {
            Log("co-op: the talker's entity for slot %u landed in slot %u", ne[k].slot, got);
            conflicts++;
        }
        for (int j = k; j < nn; j++) {  // it and whatever came with it
            if (done[j] || !used(ne[j].slot)) continue;
            done[j] = true;
            int16_t made_seq = *reinterpret_cast<int16_t*>(e + ne[j].slot * 8 + 6);
            *reinterpret_cast<int16_t*>(e + ne[j].slot * 8 + 6) = ne[j].seq;
            FixSelfId(EntityById(world, ne[j].slot), ne[j].slot, made_seq, ne[j].seq);
            *reinterpret_cast<uint32_t*>(EntityById(world, ne[j].slot) + 0x5d) = ne[j].serial;
            if (j == k) (*made)++;
            else (*kids)++;
            if (g_applying_at_frame && g_shop_new_n < 16) g_shop_new[g_shop_new_n++] = ne[j].slot;
        }
    }
    for (int k = 0; k < nn; k++) {
        if (!done[k]) {
            Log("co-op: the talker's entity for slot %u was not made here", ne[k].slot);
            conflicts++;
            continue;
        }
        uint8_t* ent = EntityById(world, ne[k].slot);
        Log("co-op: delta: new entity %u here: %s, serial %u", ne[k].slot, RttiName(ent), ne[k].serial);
        if (IsActor(world, ne[k].slot))
            Log("co-op: delta: new actor %u: inventory link +0x1006 = %08x", ne[k].slot,
                *reinterpret_cast<uint32_t*>(ent + kActorInventory));
    }
    return conflicts;
}

void ApplyDelta(const uint8_t* data, int len) {
    LogAiFlags("before the delta");
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    DeltaReader r = {data, data + len, false};
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    int created = 0, destroyed = 0, counts = 0, invs = 0, hps = 0, vars = 0, conflicts = 0;
    static uint16_t slots[256], seqs[256];
    int n = 0;
    for (uint8_t op = r.U8(); !r.bad && op != 'E'; op = r.U8()) {
        if (op == 'D') {
            uint16_t slot = r.U16();
            uint8_t* ent = slot < EntityCount(world) ? *reinterpret_cast<uint8_t**>(e + slot * 8) : nullptr;
            if (ent && ent != dummy && !ent[0x141]) {  // not one already being destroyed
                if (g_test.enabled) Log("co-op: delta: destroying entity %u (%s)", slot, RttiName(ent));
                static uint8_t before[0x200];
                if (g_test.enabled) memcpy(before, ent, sizeof(before));
                reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(ent))[10])(ent);
                if (g_test.enabled) {  // what a pending destroy marks on the entity (it finishes next frame)
                    char diff[512] = "";
                    size_t dl = 0;
                    for (int k = 0; k < static_cast<int>(sizeof(before)) && dl + 16 < sizeof(diff); k++)
                        if (before[k] != ent[k]) dl += sprintf_s(diff + dl, sizeof(diff) - dl, " +%x:%02x>%02x", k, before[k], ent[k]);
                    Log("co-op: delta: destroy marked entity %u:%s", slot, diff[0] ? diff : " nothing in +0..+0x1ff");
                }
                destroyed++;
                if (g_ndestroyed < 512) g_destroyed[g_ndestroyed++] = slot;
            }
        } else if (op == 'C') {
            // All the new entities (consecutive 'C' ops, in the talker's creation order), created together.
            static NewEntity ne[1024];
            int nn = 0;
            for (bool more = true; more && nn < 1024;) {
                NewEntity& x = ne[nn];
                x.slot = r.U16();
                x.seq = static_cast<int16_t>(r.U16());
                x.serial = r.U32();
                x.size = r.U32();
                if (!r.Has(static_cast<int>(x.size))) {
                    r.bad = true;
                    break;
                }
                x.bytes = r.p;
                r.p += x.size;
                x.nlen = r.U16();
                if (!r.Has(x.nlen)) {
                    r.bad = true;
                    break;
                }
                x.name = r.p;
                r.p += x.nlen;
                nn++;
                more = r.Has(1) && *r.p == 'C';
                if (more) r.p++;
            }
            if (r.bad) break;
            int made = 0, kids = 0;
            conflicts += ApplyNewEntities(world, ne, nn, &made, &kids);
            created += made + kids;
            e = Entries(world);
            Log("co-op: delta: %d new entities: %d created from their bytes, %d came with their parent", nn, made, kids);
        } else if (op == 'W') {
            g_deferred_ops = r.p;
            g_deferred_end = r.end;
            break;
        } else if (op == 'V') {
            uint8_t c = r.U8();
            uint16_t i = r.U16(), kl = r.U16();
            if (!r.Has(kl)) break;
            char key[128] = "", val[256] = "";
            memcpy(key, r.p, kl < sizeof(key) - 1 ? kl : sizeof(key) - 1);
            r.p += kl;
            uint16_t vl = r.U16();
            if (!r.Has(vl)) break;
            memcpy(val, r.p, vl < sizeof(val) - 1 ? vl : sizeof(val) - 1);
            r.p += vl;
            VarList v;
            char here[128];
            int hl = 0;
            if (!GetVarList(world, c, &v) || i >= v.n || !NarrowVar(v.keys[i], here, sizeof(here), &hl) ||
                strcmp(here, key) != 0) {
                Log("co-op: %s variable %u is not \"%s\" here", c ? "campaign" : "mission", i, key);
                conflicts++;
            } else if (SetGameVar(v.keys[i], val, c != 0)) {
                Log("co-op: delta: %s variable \"%s\" = \"%s\"", c ? "campaign" : "mission", key, val);
                vars++;
                // Our own screens are open too (one shops while another talks): the variable's new value goes into
                // our snapshot, so our own diff at close carries only what this player changed.
                extern bool g_private_open;
                VarList now;
                if (g_private_open && i < g_snap_nvars[c] && GetVarList(world, c, &now) && i < now.n) {
                    g_snap_var_h[c][i] = VarHash(now, i);
                    Log("co-op: our open screens' snapshot takes the other player's \"%s\"", key);
                }
            }
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
                __try {  // the map (World+4), as the recruit screen places a recruit (0x4d6980): 0x6e5ab0 when
                         // World+0x1a5 is set, then 0x6e9bc0. Without the first the entity kept its old bounds
                         // (+0x08..+0x1c, +0x50..+0x6c) and NPCs walked round it where it had been.
                    if (*reinterpret_cast<uint32_t*>(world + 0x1a5))
                        reinterpret_cast<void(__thiscall*)(void*, void*)>(0x6e5ab0)(world + 4, ent);
                    reinterpret_cast<void(__thiscall*)(void*, void*)>(0x6e9bc0)(world + 4, ent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("co-op: exception 0x%08x moving actor %u on the map grid", GetExceptionCode(), id);
                }
                hps++;
            }
        } else if (op == 'I') {
            uint16_t owner = r.U16();
            if (!ReadIdList(r, slots, seqs, &n, 256)) break;
            uint8_t* actor = IsActor(world, owner) ? EntityById(world, owner) : nullptr;
            uint16_t inv_id = actor ? *reinterpret_cast<uint16_t*>(actor + kActorInventory + 2) : 0;
            uint8_t* invent = inv_id && inv_id < EntityCount(world) ? EntityById(world, inv_id) : nullptr;
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
        } else if (op == 'B' && g_applying_at_frame && (destroyed || g_ndeferred_new)) {
            // Destroys just started finish during the next frame and would change these entries again: the
            // bookkeeping (and the shop's close after it) waits for the start of that frame (RunDeferredCreates).
            uint16_t nb_;
            memcpy(&nb_, r.p + 8, 2);
            int n = 10 + nb_ * 6;
            if (!r.Has(n)) break;
            free(g_frame_b);
            g_frame_b = static_cast<uint8_t*>(malloc(n));
            memcpy(g_frame_b, r.p, n);
            r.p += n;
            Log("co-op: delta: the bookkeeping waits for this frame's destroys (%d entries)", nb_);
        } else if (op == 'B') {
            if (!g_applying_at_frame) {
                g_reapply_b = r.p;
                g_reapply_wait = 1;  // the destroys finish during the next frame: write it again before the one after
            }
            uint16_t count = r.U16(), head = r.U16();
            uint32_t serial = r.U32();
            int nb = r.U16();
            e = Entries(world);
            for (int i = 0; i < nb && !r.bad; i++) {
                uint16_t slot = r.U16(), next_free = r.U16(), seq = r.U16();
                for (int d = 0; d < g_ndeferred_new; d++)
                    if (g_deferred_new[d].ne.slot == slot) {
                        g_deferred_new[d].have_b = true;
                        g_deferred_new[d].next_free = next_free;
                        g_deferred_new[d].seq = static_cast<int16_t>(seq);
                    }
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
    Log("co-op: delta applied: %d created, %d destroyed, %d stack counts, %d inventories, %d HP, %d conflicts, "
        "%d variables%s", created, destroyed, counts, invs, hps, conflicts, vars, r.bad ? ", MALFORMED" : "");
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
    LogAiFlags("holding");
    g_host_hold_npc = g_last_other_npc;
    g_host_hold_since = GetTickCount();
    WorldPause(*reinterpret_cast<uint8_t**>(kWorldPtr), true);  // World+0x2db
    Log("co-op: holding the game while another player talks to NPC %u", g_host_hold_npc);
    Notice("Co-op: another player is talking to someone - the game waits for them");
}

void HostReleaseHold(const char* why) {
    if (!g_host_hold_npc) return;
    Log("co-op: the game goes on (%s, held %lu ms)", why, GetTickCount() - g_host_hold_since);
    g_host_hold_npc = 0;
    WorldPause(*reinterpret_cast<uint8_t**>(kWorldPtr), false);  // World+0x2db
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

// Test mode, after a private-screen delta: every 10th frame (the same frames on every copy) the raw memory of a few
// actors, as "EDUMP f= id= off= hex" rows of 64 bytes, to find the fields the delta does not carry.
int g_dump_until = 0;
void DebugDumpActors() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world || !g_dump_until) return;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    if (frame > g_dump_until) {
        g_dump_until = 0;
        return;
    }
    static int last = -1;
    if (frame % 10 != 0 || frame == last) return;
    last = frame;
    // Every actor, every 40 frames (the same frames on every copy)
    if (frame % 40 != 0) return;
    for (int id = 1; id < EntityCount(world); id++) {
        if (!IsActor(world, id)) continue;
        uint8_t* e = EntityById(world, id);
        for (int off = 0; off < 0x1080; off += 64) {
            char hex[64 * 2 + 1];
            __try {
                for (int i = 0; i < 64; i++) sprintf_s(hex + i * 2, 3, "%02x", e[off + i]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            Log("EDUMP f=%d id=%u off=%04x %s", frame, id, off, hex);
        }
    }
}

uint32_t g_my_claim = 0;         // the claim number sent with this player's last talk
uint16_t g_my_claim_npc = 0;
uint16_t g_claim_npc = 0;        // the last claim relayed: its NPC, whether it was ours, when it came
bool g_claim_mine = false;
DWORD g_claim_tick = 0;

// Client send, before this player's talk goes out: its claim, through the same send (the host relays both in order).
void SendTalkClaim(void* client, const uint8_t* msg, int len) {
    int count = *reinterpret_cast<const uint16_t*>(msg + 4);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len; i++) {
        const uint8_t* t = msg + 6 + i * 27;
        if (t[0] != 0x17) continue;
        uint16_t npc;
        memcpy(&npc, t + 5, 2);
        // Unique per player and per talk: the high-resolution counter and the process id, well mixed (rand() runs
        // the same sequence on every copy, and the tick counts of copies started together are close).
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        uint32_t seed[3] = {qpc.LowPart, static_cast<uint32_t>(qpc.HighPart), GetCurrentProcessId()};
        uint32_t h = Fnv(2166136261u, seed, sizeof(seed));
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        g_my_claim = h;
        if (!g_my_claim) g_my_claim = 1;
        g_my_claim_npc = npc;
        uint8_t m[6 + 27] = {};
        *reinterpret_cast<uint32_t*>(m) = 2;
        *reinterpret_cast<uint16_t*>(m + 4) = 1;
        uint8_t* r = m + 6;
        r[0] = kCoopCmd;
        memcpy(r + 1, t + 1, 2);  // the talker, so the server passes it on like the talk
        r[3] = kTalkClaim;
        memcpy(r + 5, &npc, 2);
        memcpy(r + 7, &g_my_claim, 4);
        reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(kClientSend)(client, m, sizeof(m), 1);
        Log("co-op: claim %08x sent for NPC %u", g_my_claim, npc);
        return;
    }
}

void QueueFrameDelta(uint8_t kind, bool ok, bool mine, const uint8_t* data, int len);  // shop_sync.inc
void CoopCommandAtFrame(const uint8_t* r);                                              // shop_sync.inc

void HandleCoopCommand(const uint8_t* r) {
    uint8_t kind = r[3], n = r[4];
    uint16_t npc;
    memcpy(&npc, r + 5, 2);
    if (kind == kTalkClaim) {
        uint32_t claim;
        memcpy(&claim, r + 7, 4);
        g_claim_npc = npc;
        g_claim_mine = claim == g_my_claim && npc == g_my_claim_npc;
        g_claim_tick = GetTickCount();
        Log("co-op: NPC %u claimed by %s (claim %08x)", npc, g_claim_mine ? "this player" : "another player", claim);
        if (!g_claim_mine && npc == g_my_claim_npc && g_my_claim) {  // our talk lost the race
            g_local_talk_tick = 0;
            g_my_claim = 0;
            Notice("Co-op: another player got to them first - try again when they are done");
        }
        return;
    }
    if (kind == kRecruitOpen) {
        HandleRecruitOpen(r);
        return;
    }
    if (kind == kSyncDone) CloseHeadlessRecruit();  // the recruiter's screen closed before its diff: ours too, first
    if (kind == kSyncChunk) {
        if (n <= kChunk && g_rx_len + n <= kMaxDelta) {
            memcpy(g_rx_delta + g_rx_len, r + 5, n);
            g_rx_len += n;
        }
    } else if (kind == kSyncDone || kind == kShopClose) {
        uint32_t size, hash, raw;
        memcpy(&size, r + 9, 4);
        memcpy(&hash, r + 13, 4);
        memcpy(&raw, r + 17, 4);
        bool queued = false;
        if (r[7] == kDiffApplied) {
            static uint8_t plain[kMaxDelta];
            uint32_t got = 0;
            if ((r[8] & 2) && !g_delta_mine) {  // the bytes are on the host's file server
                if (g_coop_host) {
                    EnterCriticalSection(&g_serve_lock);
                    if (g_big_delta && g_big_hash == hash && g_big_size <= static_cast<uint32_t>(kMaxDelta)) {
                        memcpy(g_rx_delta, g_big_delta, g_big_size);
                        g_rx_len = static_cast<int>(g_big_size);
                    }
                    LeaveCriticalSection(&g_serve_lock);
                } else {
                    g_rx_len = static_cast<int>(GetBigDelta(hash, g_rx_delta, kMaxDelta));
                }
                Log("co-op: fetched the delta from the host's file server: %d bytes", g_rx_len);
            }
            if (g_delta_mine) {
                Log("co-op: our delta went round (%u bytes): the other copies apply it now", size);
            } else if (static_cast<int>(size) != g_rx_len || Fnv(2166136261u, g_rx_delta, size) != hash) {
                Log("co-op: delta incomplete (%d of %u bytes, hash %s): not applied", g_rx_len, size,
                    Fnv(2166136261u, g_rx_delta, g_rx_len) == hash ? "ok" : "bad");
            } else if ((r[8] & 1) && ((got = Decompress(g_rx_delta, size, plain, sizeof(plain))) != raw || raw > kMaxDelta)) {
                Log("co-op: delta did not decompress (%u of %u bytes): not applied", got, raw);
            } else if (r[8] & 4) {  // at its frame (shop_sync.inc)
                QueueFrameDelta(kind, true, false, (r[8] & 1) ? plain : g_rx_delta, static_cast<int>((r[8] & 1) ? raw : size));
                queued = true;
            } else {
                ApplyDelta((r[8] & 1) ? plain : g_rx_delta, static_cast<int>((r[8] & 1) ? raw : size));
            }
        }
        if ((r[8] & 4) && !queued) QueueFrameDelta(kind, false, g_delta_mine, nullptr, 0);  // ours, or nothing to apply
        g_delta_mine = false;
        g_rx_len = 0;
        if (kind == kShopClose) return;  // the conversation goes on: its NPC stays busy
        if (r[8] & 4) return;  // freed at its frame on every copy (CoopCommandAtFrame): arrival differs per copy
        if (g_test.enabled) {  // test mode: dump some actors' memory for 100 frames, to diff the copies
            uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr);
            g_dump_until = w ? *reinterpret_cast<int*>(w + kWorldFrame) + 250 : 0;
        }
        if (NpcLock* l = FindLock(npc)) l->npc = 0;
        if (npc == g_host_hold_npc) HostReleaseHold("the other player is done");
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        Log("co-op: NPC %u is free again (%s) at frame %d", npc,
            r[7] == kDiffNone ? "nothing changed" : r[7] == kDiffApplied ? "delta" : "full re-sync",
            world ? *reinterpret_cast<int*>(world + kWorldFrame) : -1);
    }
}

void __fastcall ActorActHook(void* actor, void* edx, uint8_t* cmd) {
    if (cmd && cmd[0] == kCoopCmd) {  // mostly handled when it arrived (CoopFromNetwork); some act at their frame
        CoopCommandAtFrame(cmd);
        return;
    }
    if (cmd && cmd[0] == 0x17 && (g_coop_host || g_coop_join)) {  // a talk: that NPC is busy until kSyncDone
        uint16_t npc;
        memcpy(&npc, cmd + 5, 2);
        // The relayed claim says whose talk this is; without one (an older copy), this player's recent talk.
        bool mine = g_claim_npc == npc ? g_claim_mine
                                       : npc == g_local_talk_npc && GetTickCount() - g_local_talk_tick < 10000;
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
        // A recruiter (FTSE slot 252, IsRecruitMaster) changes the squad everyone plays: everyone waits, as before.
        // Anyone else, with shop_no_pause: nobody waits, and the talker's own world keeps running.
        extern bool g_shop_no_pause, g_my_conv_light, g_other_conv_light, g_suppress_pause;
        extern int g_suppressed;
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        uint8_t* who = world && npc < EntityCount(world) ? EntityById(world, npc) : nullptr;
        bool recruiter = who && reinterpret_cast<bool(__thiscall*)(void*)>((*reinterpret_cast<void***>(who))[0x3f0 / 4])(who);
        bool quartermaster = who && reinterpret_cast<bool(__thiscall*)(void*)>((*reinterpret_cast<void***>(who))[0x3f4 / 4])(who);
        bool light = g_shop_no_pause && quartermaster && !recruiter;
        if (mine) {
            g_my_conv_light = light;
            g_diff_mode = light ? 1 : 0;
            if (light) {
                g_suppress_pause = true;
                g_suppressed = 0;
            }
        } else {
            g_other_conv_light = light;
        }
        Log("co-op: NPC %u is %s: %s", npc, recruiter ? "a recruiter" : quartermaster ? "a quartermaster" : "someone else",
            light ? "nobody waits" : "everyone waits while the conversation is up");
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
        // Also an NPC someone else's claim already won, before their talk has run here (the race: a claim can
        // arrive just before this player's own talk goes out).
        bool claimed = g_claim_npc == npc && !g_claim_mine && GetTickCount() - g_claim_tick < 10000;
        if ((l && !l->mine) || claimed) {
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
        } else if (r[0] == 0x17 || (r[0] == kCoopCmd && r[3] == kTalkClaim)) {
            HostLock* held = nullptr;
            for (auto& l : g_host_locks)
                if (l.npc == npc && GetTickCount() - l.since < kLockMaxMs) held = &l;
            if (held && held->sender != sender) {
                Log("host: dropped player %d's %s NPC %u: player %d is talking to them", sender,
                    r[0] == 0x17 ? "talk to" : "claim on", npc, held->sender);
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

// Test bot, the trader's copy, during the barter screen: change one quest variable the way a script would, so the delta
// must carry it ('V'). The first variable holding a number goes up by one (mission first, then campaign); failing
// that, the first plain-text one gets "1" (or "2" if it was "1").
void BotChangeQuestVar(uint8_t* world) {
    for (int pass = 0; pass < 2; pass++)
    for (int c = 0; c < 2; c++) {
        VarList v;
        if (!GetVarList(world, c, &v)) {
            if (pass == 0) Log("bot: QUEST VAR no %s variable list", c ? "campaign" : "mission");
            continue;
        }
        if (pass == 0) {
            char list[512] = "";
            size_t len = 0;
            for (int i = 0; i < v.n && i < 6 && len + 80 < sizeof(list); i++) {
                char key[64] = "?", val[64] = "?";
                int kl = 0, vl = 0;
                NarrowVar(v.keys[i], key, sizeof(key), &kl);
                if (v.vals) NarrowVar(v.vals[i], val, sizeof(val), &vl);
                len += sprintf_s(list + len, sizeof(list) - len, " %s=\"%s\"", key, val);
            }
            Log("bot: QUEST VAR %s variables: %d (vals %p):%s", c ? "campaign" : "mission", v.n, v.vals, list);
        }
        for (int i = 0; i < v.n; i++) {
            char key[128], val[256];
            int kl = 0, vl = 0;
            if (!NarrowVar(v.keys[i], key, sizeof(key), &kl) || !NarrowVar(v.vals ? v.vals[i] : nullptr, val, sizeof(val), &vl))
                continue;
            char* end;
            long x = strtol(val, &end, 10);
            bool number = vl && !*end;
            if (pass == 0 && !number) continue;
            char now[32];
            if (number) sprintf_s(now, "%ld", x + 1);
            else strcpy_s(now, strcmp(val, "1") == 0 ? "2" : "1");
            bool ok = SetGameVar(v.keys[i], now, c != 0);
            char after[256] = "?";
            VarList w;
            int al = 0;
            if (GetVarList(world, c, &w) && i < w.n) NarrowVar(w.vals[i], after, sizeof(after), &al);
            Log("bot: QUEST VAR %s %d \"%s\" \"%s\" -> \"%s\" (reads \"%s\"%s)", c ? "campaign" : "mission", i, key, val,
                now, after, ok ? "" : ", set FAILED");
            return;
        }
    }
    Log("bot: QUEST VAR none to change");
}

#include "shop_sync.inc"

// The deal (diff mode 2): only the buyer's and the merchant's inventories (the merchant's is the shop's stock now),
// measured from the shop's opening, when the stock existed on every copy. New stacks (a split) are made from their bytes;
// stacks gone (merged) are destroyed; no bookkeeping: the world ran on every copy meanwhile.
int ShopDealDiff(DeltaWriter& w, uint8_t* world, const SlotState* now, int count) {
    g_shop_new_n = 0;
    uint16_t owners[2] = {SlotOf(g_shop.buyer), SlotOf(g_shop.merchant)};
    static uint16_t a_items[2][200], b_items[2][200];
    static int a_counts[2][200], b_counts[2][200];
    int na[2] = {}, nb[2] = {};
    char s[2048];
    for (int k = 0; k < 2; k++) {
        const ActorSnap* before = SnapOf(owners[k]);
        ParseInv(before ? before->inv : "", b_items[k], b_counts[k], &nb[k], 200);
        if (InvString(world, owners[k], s, sizeof(s))) ParseInv(s, a_items[k], a_counts[k], &na[k], 200);
    }
    int ops = 0;
    auto fresh = [&](uint16_t id) { return id < count && (!g_snap_slots[id].used || g_snap_slots[id].seq != now[id].seq); };
    for (int k = 0; k < 2; k++)  // 'D' first: stacks that were there and are gone (merged, or spent); a slot freed may be reused by a new stack
        for (int j = 0; j < nb[k]; j++) {
            uint16_t id = b_items[k][j];
            uint8_t* ent = id < count && now[id].used ? EntityById(world, id) : nullptr;
            bool gone = !ent || now[id].seq != g_snap_slots[id].seq || ent[0x141];
            bool elsewhere = false;
            for (int m = 0; m < 2 && !elsewhere; m++)
                for (int i = 0; i < na[m] && !elsewhere; i++) elsewhere = a_items[m][i] == id;
            if (!gone || (elsewhere && !fresh(id))) continue;  // the same slot with a new seq is another entity
            w.U8('D');
            w.U16(id);
            ops++;
        }
    for (int k = 0; k < 2; k++)  // 'C': new stacks
        for (int i = 0; i < na[k] && !w.overflow; i++) {
            uint16_t id = a_items[k][i];
            if (!fresh(id)) continue;
            int n = 0;
            uint8_t* bytes = SerializeEntity(world, id, &n);
            if (!bytes || n <= 0) continue;
            w.U8('C');
            w.U16(id);
            w.U16(static_cast<uint16_t>(now[id].seq));
            w.U32(*reinterpret_cast<uint32_t*>(EntityById(world, id) + 0x5d));
            w.U32(static_cast<uint32_t>(n));
            w.Put(bytes, n);
            PutEntityName(w, EntityById(world, id));
            ops++;
            Log("co-op: shop: new stack %u (%s) in the %s's inventory, %d bytes", id, RttiName(EntityById(world, id)),
                k ? "merchant" : "buyer", n);
            if (g_shop_new_n < 16) g_shop_new[g_shop_new_n++] = id;
        }
    for (int k = 0; k < 2; k++)  // 'N': stack counts
        for (int i = 0; i < na[k]; i++) {
            if (fresh(a_items[k][i])) continue;
            int was = -1;
            for (int j = 0; j < nb[k]; j++)
                if (b_items[k][j] == a_items[k][i]) was = b_counts[k][j];
            if (was == a_counts[k][i]) continue;
            w.U8('N');
            w.U16(a_items[k][i]);
            w.U32(static_cast<uint32_t>(a_counts[k][i]));
            ops++;
        }
    for (int k = 0; k < 2; k++) {  // 'I': the lists
        bool same = na[k] == nb[k] && memcmp(a_items[k], b_items[k], na[k] * sizeof(uint16_t)) == 0;
        if (same) continue;
        w.U8('I');
        w.U16(owners[k]);
        PutIdList(w, world, a_items[k], na[k]);
        ops++;
    }
    // 'B': the table's bookkeeping, as the shopper has it now. Its barter screen made and destroyed stacks during the
    // deal (offer-table splits, merges) that nobody else made: their seq numbers and the free list's order differ
    // otherwise (trade_host, shop_no_pause). Written on the other copies before their close, so both run the same
    // close from the same table. Entries the world changed meanwhile change on every copy alike.
    int nbook = 0;
    for (int i = 0; i < count; i++) {
        uint8_t* ent = now[i].used ? *reinterpret_cast<uint8_t**>(Entries(world) + i * 8) : nullptr;
        bool pend = ent && ent[0x141];
        if (now[i].next_free != g_snap_slots[i].next_free || now[i].seq != g_snap_slots[i].seq || pend) nbook++;
    }
    w.U8('B');
    w.U16(static_cast<uint16_t>(count));
    w.U16(*reinterpret_cast<uint16_t*>(world + kWorldFreeHead));
    w.U32(*reinterpret_cast<uint32_t*>(kEntitySerial));
    w.U16(static_cast<uint16_t>(nbook));
    for (int i = 0; i < count; i++) {
        uint8_t* ent = now[i].used ? *reinterpret_cast<uint8_t**>(Entries(world) + i * 8) : nullptr;
        bool pend = ent && ent[0x141];
        if (now[i].next_free != g_snap_slots[i].next_free || now[i].seq != g_snap_slots[i].seq || pend) {
            w.U16(static_cast<uint16_t>(i));
            w.U16(static_cast<uint16_t>(now[i].next_free));
            w.U16(static_cast<uint16_t>(now[i].seq));
        }
    }
    if (nbook) ops++;
    Log("co-op: shop: the deal is %d ops (%d table entries), %d bytes", ops, nbook, w.len);
    return ops;
}
