// ---- Recruiter visits without a reload (issue #1): the recruit screen's candidates on every copy ----
// Included from netlog.cpp after drift.inc.
// The recruit screen (WSingleRecruit, vtable 0x808bc0, 0x153 bytes from the game's new 0x6c4dd0) is made by SetMode
// for mode 8 at 0x4722eb: ctor 0x4d6060 __thiscall(this, WGameMain* parent, [WGameMain+0x102]), stored at
// WGameMain+0x122. Its deleting destructor (vtable slot 0, 0x4d9280, ret 4) runs 0x4d6830 and frees it. The constructor
// walks the campaign's recruit pool (*0x8bdd2c +0x181) and makes a character for each candidate.
const uintptr_t kRecruitCtorCall = 0x4722eb;
const uintptr_t kRecruitCtor = 0x4d6060;
const uintptr_t kRecruitVtable = 0x808bc0;
const uintptr_t kRecruitDtorSlot = kRecruitVtable;
const uintptr_t kRecruitDtor = 0x4d9280;
typedef void*(__fastcall* RecruitDtorFn)(void*, void*, unsigned);
RecruitDtorFn g_recruit_dtor = nullptr;

// Exploration (test mode): which entities a step made or removed, by comparing the entity table before and after.
SlotState g_rx_before[kMaxSlots];
int g_rx_before_n = 0;
uint16_t g_rx_made[256];  // what the constructor made
int g_rx_nmade = 0;

void LogTableChange(uint8_t* world, const char* what) {
    static SlotState now[kMaxSlots];
    int n = 0;
    ReadSlots(world, now, &n);
    char made[1200] = "", gone[1200] = "";
    int ml = 0, gl = 0, nm = 0, ng = 0;
    for (int i = 1; i < n && i < g_rx_before_n; i++) {
        bool was = g_rx_before[i].used, is = now[i].used && (!was || g_rx_before[i].seq == now[i].seq);
        if (now[i].used && (!was || g_rx_before[i].seq != now[i].seq)) {
            nm++;
            if (g_rx_nmade < 256) g_rx_made[g_rx_nmade++] = static_cast<uint16_t>(i);
            if (ml < 1100) ml += sprintf_s(made + ml, sizeof(made) - ml, " %d:%s", i, RttiName(EntityById(world, static_cast<uint16_t>(i))) + 4);
        } else if (was && !now[i].used) {
            ng++;
            if (gl < 1100) gl += sprintf_s(gone + gl, sizeof(gone) - gl, " %d", i);
        }
        (void)is;
    }
    Log("recruit: %s: %d made%s; %d gone%s; free head %u", what, nm, made, ng, gone,
        *reinterpret_cast<uint16_t*>(world + kWorldFreeHead));
}

void RetakeSnapshotKeepVars();
void SendRecruitOpen();

void* __fastcall RecruitCtorHook(void* self, void* /*edx*/, uint8_t* parent, void* p2) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world && g_test.enabled) ReadSlots(world, g_rx_before, &g_rx_before_n);
    g_rx_nmade = 0;
    void* r = reinterpret_cast<void*(__thiscall*)(void*, void*, void*)>(kRecruitCtor)(self, parent, p2);
    if (world && (g_coop_host || g_coop_join)) {  // the candidates exist; every other copy makes them now too
        RetakeSnapshotKeepVars();
        SendRecruitOpen();
        Log("co-op: recruit screen open here at frame %d: every copy makes its candidates", *reinterpret_cast<int*>(world + kWorldFrame));
    }
    if (world && g_test.enabled) {
        uint32_t v = reinterpret_cast<uint32_t>(p2);
        int as_entity = 0;
        for (int i = 1; i < EntityCount(world) && !as_entity; i++)
            if (EntityById(world, static_cast<uint16_t>(i)) == p2) as_entity = i;
        Log("recruit: screen made %p, parent %p (%s), arg %08x (%s, entity %d)", self, parent, RttiName(parent), v,
            RttiName(p2), as_entity);
        LogTableChange(world, "the screen's constructor");
    }
    return r;
}

void* __fastcall RecruitDtorHook(void* self, void* edx, unsigned flags) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world && g_test.enabled) ReadSlots(world, g_rx_before, &g_rx_before_n);
    void* r = g_recruit_dtor(self, edx, flags);
    if (world && g_test.enabled) {
        LogTableChange(world, "the screen's destructor");
        char line[1200] = "";
        int n = 0, pending = 0, alive = 0;
        for (int k = 0; k < g_rx_nmade; k++) {
            uint8_t* e = EntityById(world, g_rx_made[k]);
            if (e[0x141]) pending++;
            else {
                alive++;
                if (n < 1100) n += sprintf_s(line + n, sizeof(line) - n, " %u:%s", g_rx_made[k], RttiName(e) + 4);
            }
        }
        Log("recruit: of the %d the constructor made: %d being destroyed, %d alive:%s", g_rx_nmade, pending, alive, line);
    }
    return r;
}

void InstallRecruitHooks() {
    Log("co-op: recruit screen hooks %s, %s",
        PatchCall(kRecruitCtorCall, kRecruitCtor, (void*)&RecruitCtorHook) ? "ok" : "FAILED",
        PatchSlot(kRecruitDtorSlot, kRecruitDtor, (void*)&RecruitDtorHook, &g_recruit_dtor) ? "ok" : "FAILED");
}

// ---- The sync. A recruiter visit holds every copy (the talker hold and the host's hold), so every copy is on the same
// frame while the screen is up, and the screen's allocations can run on every copy in turn: the recruiter's when it
// opens the screen, every other copy's when the record arrives, with no frame in between anywhere. The recruiter's
// snapshot is taken again after its screen made the candidates, so its diff at close sees only what the player did
// (who joined, who left, where they stand), and every copy runs the screen's destructor before applying it.
bool g_recruit_mine_sent = false;   // this copy's screen is up and its record went out
uint8_t* g_headless_recruit = nullptr;  // another player's screen, made here without a window

void RetakeSnapshotKeepVars() {
    static uint32_t var_h[2][kMaxVars];
    int nvars[2] = {g_snap_nvars[0], g_snap_nvars[1]};
    uint32_t vars = g_snap_vars;
    memcpy(var_h, g_snap_var_h, sizeof(var_h));
    TakePrivateSnapshot();
    memcpy(g_snap_var_h, var_h, sizeof(var_h));
    g_snap_nvars[0] = nvars[0];
    g_snap_nvars[1] = nvars[1];
    g_snap_vars = vars;
}

void SendRecruitOpen() {
    uint8_t r[27];
    CoopRecord(r, kRecruitOpen, 0);
    SendCoopRecords(reinterpret_cast<const uint8_t(*)[27]>(r), 1);
    g_recruit_mine_sent = true;
    g_recruit_synced = true;
}

// Another player's recruit screen: the same constructor, the same arguments as SetMode passes, never shown.
void HandleRecruitOpen(const uint8_t* /*r*/) {
    if (g_recruit_mine_sent) {
        Log("co-op: our recruit screen's record went round");
        return;
    }
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint8_t* game_main = *reinterpret_cast<uint8_t**>(kGameMainPtr);
    if (!world || !game_main || g_headless_recruit) {
        Log("co-op: a recruit screen opened elsewhere, but %s: not made here", !world || !game_main ? "no mission screen" : "one is already up");
        return;
    }
    // The constructor deselects this player's squad (the screen's own selection); this player keeps theirs.
    uint16_t squad[16];
    uint8_t flags[16];
    int n = GetSquad(world, squad, 16);
    for (int k = 0; k < n; k++) flags[k] = EntityById(world, squad[k])[0xb2];
    if (g_test.enabled) ReadSlots(world, g_rx_before, &g_rx_before_n);
    g_rx_nmade = 0;
    void* obj = reinterpret_cast<void*(__cdecl*)(size_t)>(0x6c4dd0)(0x153);
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*)>(kRecruitCtor)(obj, game_main, *reinterpret_cast<void**>(game_main + 0x102));
    for (int k = 0; k < n; k++) EntityById(world, squad[k])[0xb2] = flags[k];
    // The constructor makes it a child of the mission screen, so it would tick and draw there (seen: the recruit
    // screen over the host's game). Unlinked from the parent's child list (window: +0x14 parent, +0x18 / +0x1c first
    // and last child, +0x20 / +0x24 previous and next sibling), it is never drawn or ticked.
    uint8_t* w = static_cast<uint8_t*>(obj);
    auto link = [](uint8_t* p, int off) -> uint8_t*& { return *reinterpret_cast<uint8_t**>(p + off); };
    bool found = false;
    for (uint8_t* c = link(game_main, 0x18); c && !found; c = link(c, 0x24)) found = c == w;
    if (link(w, 0x14) == game_main && found) {
        uint8_t *prev = link(w, 0x20), *next = link(w, 0x24);
        if (prev) link(prev, 0x24) = next;
        else link(game_main, 0x18) = next;
        if (next) link(next, 0x20) = prev;
        else link(game_main, 0x1c) = prev;
        link(w, 0x14) = link(w, 0x20) = link(w, 0x24) = nullptr;
        Log("co-op: the recruit screen made here is not shown (unlinked from the mission screen)");
    } else {
        Log("co-op: the recruit screen made here is not in the mission screen's children (parent %p, found %d)", link(w, 0x14), found);
    }
    g_headless_recruit = static_cast<uint8_t*>(obj);
    g_recruit_synced = true;
    if (g_test.enabled) LogTableChange(world, "another player's recruit screen, made here");
    Log("co-op: another player's recruit screen made here without a window at frame %d", *reinterpret_cast<int*>(world + kWorldFrame));
    if (g_test.enabled && g_shot_event) SetEvent(g_shot_event);
}

void CloseHeadlessRecruit() {
    if (g_headless_recruit) {
        uint8_t* obj = g_headless_recruit;
        g_headless_recruit = nullptr;
        reinterpret_cast<void*(__thiscall*)(void*, unsigned)>((*reinterpret_cast<void***>(obj))[0])(obj, 1);
        Log("co-op: the other player's recruit screen closed here too");
    }
    g_recruit_mine_sent = false;
    g_recruit_synced = false;
}
