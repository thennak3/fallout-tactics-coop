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

// ---- Level-ups (QuestBarter.sav): the Level effect button opens mission-screen mode 3, the character screen
// (WCharacter, vtable 0x7ffa84). With a perk to choose, its setup (0x45ab80) makes the perk dialog WPickPerk (ctor
// 0x4b5d60, vtable 0x8063a8), keeps it at WCharacter+0x159a and runs its modal loop, vtable slot 11 (+0x2c, the base
// window's 0x6c6190): window ticks, the message-pump hook and the dialog's own update and draw slots are not reached
// from that loop. Its Done (0x4b64b0, __fastcall(dialog, edx, button), ret 4) gives the list's highlighted perk to the
// dialog (vt+0x188) and ends the loop with 4 (EndModal, vt+0x34); Cancel (0x6c62c0) ends it with 0x10. The test bot
// replaces the loop: Done pressed at once, 4 returned.
const uintptr_t kPickPerkModalSlot = 0x8063a8 + 0x2c;
const uintptr_t kBaseModal = 0x6c6190;
typedef int(__fastcall* ModalFn)(void*, void*);
ModalFn g_pickperk_modal = nullptr;

int __fastcall PickPerkModalHook(uint8_t* self, void* edx) {
    if (g_test.enabled && _stricmp(g_test.scenario, "levelup") == 0) {
        Log("bot: LEVEL perk dialog: pressing Done (the highlighted perk)");
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(0x4b64b0)(self, nullptr, nullptr);
        return 4;
    }
    return g_pickperk_modal(self, edx);
}


// ---- Level-up lock (owner, 2026-10-22: "block the level up screen on that character and block the recruitment
// screen if a player is levelling up ... for other players"). The levelling player's copy sends record 8 (kLevelOpen,
// the character's slot at +5) when its character screen opens and record 9 (kLevelClose) once its changes are out;
// every other copy then closes a character screen of that character and refuses talks to a recruiter.
// Two players opening the same character's screen at once (a race, as at a merchant): every record carries a claim, and
// every copy sees the records in the host's relay order, so the first open for a character wins everywhere. A copy
// whose own open comes back second closes its screen (its changes never reach the character: the screen keeps its own
// copy until Done) and says so. A close carries its claim too, so a loser's close leaves the winner's lock alone.
struct LevelLock { bool open; uint16_t who; uint32_t claim; };
LevelLock g_level_locks[4] = {};  // other players' open character screens
bool g_level_mine_sent = false;
bool g_level_mine_won = false;  // our open came back first for its character
uint32_t g_level_my_claim = 0;
uint16_t g_level_my_who = 0;

LevelLock* LevelLockOf(uint16_t who) {
    for (LevelLock& l : g_level_locks)
        if (l.open && l.who == who) return &l;
    return nullptr;
}
bool AnyLevelLock() {
    for (const LevelLock& l : g_level_locks)
        if (l.open) return true;
    return false;
}

// The squad member a character screen shows: the first squad EntityID inside the window (test mode logs every one).
uint16_t CharacterOfScreen(uint8_t* w) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint16_t squad[16];
    int n = world ? GetSquad(world, squad, 16) : 0;
    uint16_t first = 0;
    char hits[300] = "";
    int hl = 0;
    for (int off = 0; off + 4 <= 0x1600; off++)
        for (int k = 0; k < n; k++)
            if (*reinterpret_cast<uint32_t*>(w + off) == FullId(world, squad[k])) {
                if (!first) first = squad[k];
                if (hl < 260) hl += sprintf_s(hits + hl, sizeof(hits) - hl, " +%x=%u", off, squad[k]);
            }
    static bool logged = false;
    if (g_test.enabled && !logged) {
        logged = true;
        Log("co-op: character screen: squad ids inside at%s", hits[0] ? hits : " none");
    }
    return first;
}

void SendLevelRecord(uint8_t kind, uint16_t who, uint32_t claim) {
    uint8_t r[27];
    CoopRecord(r, kind, 0);
    memcpy(r + 5, &who, 2);
    memcpy(r + 7, &claim, 4);
    SendCoopRecords(reinterpret_cast<const uint8_t(*)[27]>(r), 1);
}

// This copy's character screen opened (netlog.cpp) and, once its changes are out, closed (PrivateCloseStep).
void LevelScreenOpened(uint8_t* w) {
    uint16_t who = CharacterOfScreen(w);
    LARGE_INTEGER qpc;  // unique per player and per open, as a talk claim
    QueryPerformanceCounter(&qpc);
    uint32_t seed[3] = {qpc.LowPart, static_cast<uint32_t>(qpc.HighPart), GetCurrentProcessId()};
    uint32_t h = Fnv(2166136261u, seed, sizeof(seed));
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    g_level_my_claim = h ? h : 1;
    g_level_my_who = who;
    g_level_mine_sent = true;
    g_level_mine_won = false;
    SendLevelRecord(kLevelOpen, who, g_level_my_claim);
    Log("co-op: character screen of %u open here (claim %08x): the others wait for it", who, g_level_my_claim);
}
void LevelScreenClosed() {
    if (!g_level_mine_sent) return;
    g_level_mine_sent = false;
    SendLevelRecord(kLevelClose, g_level_my_who, g_level_my_claim);
}

void HandleLevelRecord(const uint8_t* r) {
    uint16_t who;
    uint32_t claim;
    memcpy(&who, r + 5, 2);
    memcpy(&claim, r + 7, 4);
    bool mine = g_level_mine_sent && claim == g_level_my_claim;
    if (r[3] == kLevelOpen && mine) {
        if (LevelLockOf(who)) {  // another player's open for this character came first: theirs
            Log("co-op: character screen of %u: another player's came first (claim %08x lost)", who, claim);
            Notice("Co-op: another player is levelling up that character - wait for them");
            g_end_game_mode = true;  // our screen closes without its Done: nothing changed
        } else {
            g_level_mine_won = true;
            Log("co-op: character screen of %u is ours (claim %08x)", who, claim);
        }
    } else if (r[3] == kLevelOpen && !LevelLockOf(who)) {
        // First for this character (an open of ours that comes back later loses). Ours, if it went first, wins.
        bool ours_first = g_level_mine_sent && g_level_my_who == who && g_level_mine_won;
        if (!ours_first)
            for (LevelLock& l : g_level_locks)
                if (!l.open) {
                    l = {true, who, claim};
                    Log("co-op: another player's character screen of %u is open (claim %08x)", who, claim);
                    break;
                }
    } else if (r[3] == kLevelClose && !mine) {
        for (LevelLock& l : g_level_locks)
            if (l.open && l.claim == claim) {
                l = {};
                Log("co-op: the other player's character screen of %u closed", who);
            }
    }
}

// Another player's character screen of this character is open: ours closes (netlog.cpp, as it opens).
bool LevelLockedScreen(uint8_t* w) {
    if (!AnyLevelLock()) return false;
    uint16_t who = CharacterOfScreen(w);
    if (!LevelLockOf(who)) return false;
    Log("co-op: refused this player's character screen of %u: another player is levelling them up", who);
    Notice("Co-op: another player is levelling up that character - wait for them");
    return true;
}

// Client send: a talk to a recruiter while another player levels up is taken out (recruiting changes their squad).
int RefuseDuringLevelUp(uint8_t* msg, int len) {
    if (!AnyLevelLock() || len < 6 || *reinterpret_cast<uint32_t*>(msg) != 2) return len;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    int count = *reinterpret_cast<uint16_t*>(msg + 4);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len;) {
        uint8_t* r = msg + 6 + i * 27;
        uint16_t npc;
        memcpy(&npc, r + 5, 2);
        if (r[0] == 0x17 && world && npc < EntityCount(world) && IsActor(world, npc) &&
            reinterpret_cast<bool(__thiscall*)(void*)>((*reinterpret_cast<void***>(EntityById(world, npc)))[0x3f0 / 4])(EntityById(world, npc))) {
            Log("co-op: refused this player's talk to recruiter %u: a player is levelling up", npc);
            Notice("Co-op: a player is levelling up - recruit when they are done");
            memmove(r, r + 27, len - (r + 27 - msg));
            len -= 27;
            count--;
            *reinterpret_cast<uint16_t*>(msg + 4) = static_cast<uint16_t>(count);
            continue;
        }
        i++;
    }
    return count ? len : 0;
}

// The character screen's Done (0x4678b0, __fastcall(WCharacter*, edx, button), ret 4) keeps the changes. It waits until
// this copy knows its open won: the records flow through the host's ticks, and with the host levelling up (its world
// held) a joiner heard it had lost only after it had pressed Done and kept a perk the host also chose (levelrace).
const uint8_t kCharDonePrologue[] = {0x56, 0x8b, 0xf1, 0x8a, 0x86, 0x22, 0x01, 0x00, 0x00};
uint8_t* g_chardone_trampoline = nullptr;

void __fastcall CharDoneHook(uint8_t* self, void* edx, void* button) {
    if (g_level_mine_sent && !g_level_mine_won) {
        Log("co-op: the character screen's Done waits: not yet known whether another player has this character");
        Notice("Co-op: checking whether another player has this character - press Done again in a moment");
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(g_chardone_trampoline)(self, edx, button);
}

void InstallLevelUpHooks() {
    g_chardone_trampoline = HookWithTrampoline(0x4678b0, kCharDonePrologue, sizeof(kCharDonePrologue), (void*)&CharDoneHook);
    Log("co-op: perk dialog hook %s, character screen Done hook %s",
        PatchSlot(kPickPerkModalSlot, kBaseModal, (void*)&PickPerkModalHook, &g_pickperk_modal) ? "ok" : "FAILED",
        g_chardone_trampoline ? "ok" : "FAILED");
}
