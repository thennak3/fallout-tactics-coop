// ---- in-game bot: runs once per simulation frame, before the frame's ticks are applied ----
// Included from netlog.cpp. Scenario from ftcoop.ini [test] scenario= (move, simul, attack, soak).
// Every 50 frames each copy logs every Actor on the map (id, position, HP); the launcher compares all
// snapshot frames the two copies share, so enemy AI, NPCs and combat results are part of the check.
int g_bot_t0 = -1;
int g_bot_last_snap = -1;
bool g_bot_done_step[16] = {};
const uintptr_t kActorVtable = 0x80c1d0;
const float kMoveMin = 10.0f, kMoveMax = 40.0f;

int GetSquad(uint8_t* world, uint16_t* ids, int max) {
    uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
    uint8_t* end = *reinterpret_cast<uint8_t**>(world + kWorldPlayers + 4);
    if (!begin || (end - begin) / kPlayerSize < 2) return 0;
    uint8_t* head = *reinterpret_cast<uint8_t**>(begin + 1 * kPlayerSize + kPlayerMembers);
    int n = 0;
    for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && n < max;
         node = *reinterpret_cast<uint8_t**>(node)) {
        ids[n++] = *reinterpret_cast<uint16_t*>(node + 10);  // EntityID.id
    }
    return n;
}

uint8_t* Entries(uint8_t* world) { return *reinterpret_cast<uint8_t**>(world + kWorldEntities); }
int EntityCount(uint8_t* world) {
    return static_cast<int>((*reinterpret_cast<uint8_t**>(world + kWorldEntities + 4) - Entries(world)) / 8);
}
uint8_t* EntityById(uint8_t* world, uint16_t id) {  // an id outside the table gives the dummy entity (slot 0)
    return *reinterpret_cast<uint8_t**>(Entries(world) + (id < EntityCount(world) ? id : 0) * 8);
}
float* EntityPos(uint8_t* world, uint16_t id) { return reinterpret_cast<float*>(EntityById(world, id) + kEntityPos); }
int ActorHp(uint8_t* world, uint16_t id) { return *reinterpret_cast<int*>(EntityById(world, id) + kActorHp); }
uint32_t FullId(uint8_t* world, uint16_t id) {  // EntityID {u16 seq, u16 id}; seq from the table entry +6
    return *reinterpret_cast<uint16_t*>(Entries(world) + id * 8 + 6) | (static_cast<uint32_t>(id) << 16);
}
bool IsActor(uint8_t* world, int i) {
    uint8_t* e = EntityById(world, static_cast<uint16_t>(i));
    return e && e != *reinterpret_cast<uint8_t**>(Entries(world)) && *reinterpret_cast<uintptr_t*>(e) == kActorVtable;
}
// recruit: dismiss two, add one back (the list selects the last dismissed); recruitnew: add someone never recruited
bool IsRecruitScenario() { return _stricmp(g_test.scenario, "recruit") == 0 || _stricmp(g_test.scenario, "recruitnew") == 0; }
// trade: one player trades; trade2: then the other player trades with the same merchant and also sells an item
bool IsTradeScenario() {
    return _stricmp(g_test.scenario, "trade") == 0 || _stricmp(g_test.scenario, "trade2") == 0 ||
           _stricmp(g_test.scenario, "shopresync") == 0;
}
DWORD g_bot_level_tick = 0;

void DumpWindowTree(uint8_t* w, int depth);  // below

bool g_bot_level_request = false;
bool g_bot_inv_request = false;  // invscreen: press INV from the mission screen's tick
bool g_bot_menu_request = false;  // savemenu: open the Escape menu and its save screen from the mission screen's tick
bool g_bot_menu_done = false;

// savemenu: the host does what a player does to save: opens the Escape menu (SetMode 2) and stays in it 3 s, presses its
// Save button (0x482180), sits in the save screen for [test] hold seconds (9 if 0), leaves it, stays in the menu 3 s
// more, closes it, and quick-saves (as the quick-save key does), while the joiner keeps ordering its member. Presses
// come from the application's input pump (save_sync.inc), as clicks do: Save pressed from a window tick, inside the
// world's step, hung the save screen's loop on a lock (2026-10-28). In that loop no window tick of ours runs (as for
// WPickPerk, recruit_sync.inc), so the pump ends it: Cancel's result 0x10 through EndModal (vtable +0x34).
extern uint8_t* g_saveload_window;  // save_sync.inc
extern DWORD g_saveload_opened;
uint8_t* g_bot_press_save = nullptr;  // the game menu whose Save the pump presses
bool g_bot_save_closed = false;
bool g_bot_quick_save = false;
DWORD g_bot_clicks_from = 0;  // joiner: orders its member from the pump every 2 s from here (the host is in its menu)
void BotMoveMember(uint8_t* client, uint8_t* world, int member, unsigned variant);

// With its window in the background the save screen's loop sleeps until a window message comes: wake it while it is up.
DWORD WINAPI SaveScreenWaker(LPVOID) {
    while (g_saveload_window) {
        if (g_pump_hwnd) PostMessageA(g_pump_hwnd, WM_NULL, 0, 0);
        Sleep(250);
    }
    return 0;
}

void BotPumpTick() {  // save_sync.inc's input-pump hook, test mode
    if (_stricmp(g_test.scenario, "savemenu") != 0) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    DWORD now = GetTickCount();
    static DWORD last_click = 0;
    static unsigned clicks = 0;
    if (g_bot_clicks_from && now - g_bot_clicks_from < 22000 && now - last_click > 2000 && g_client_ptr) {
        last_click = now;
        Log("bot: SAVEMENU the joiner orders its member (click %u) at frame %d", ++clicks, frame);
        BotMoveMember(g_client_ptr, world, g_test.index, 1 + clicks);
    }
    static uint8_t* screen = nullptr;
    if (g_saveload_window && g_saveload_window != screen) {
        screen = g_saveload_window;
        Log("bot: SAVEMENU the save screen is up at frame %d", frame);
        CloseHandle(CreateThread(nullptr, 0, SaveScreenWaker, nullptr, 0, nullptr));
    }
    if (screen && g_saveload_window &&
        now - g_saveload_opened > static_cast<DWORD>(g_test.hold > 0 ? g_test.hold : 9) * 1000) {
        uint8_t* w = g_saveload_window;
        Log("bot: SAVEMENU leaving the save screen at frame %d (pause counter %d)", frame, world[0x2db]);
        reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(w))[0x34 / 4])(w, 0x10);
        return;
    }
    if (screen && !g_saveload_window && !g_bot_save_closed) {
        g_bot_save_closed = true;
        Log("bot: SAVEMENU save screen closed at frame %d (pause counter %d)", frame, world[0x2db]);
    }
    if (uint8_t* menu = g_bot_press_save) {
        g_bot_press_save = nullptr;
        Log("bot: SAVEMENU pressing Save at frame %d (pause counter %d)", frame, world[0x2db]);
        reinterpret_cast<void(__thiscall*)(void*, int)>(0x482180)(menu, 0);  // as the button: in co-op it waits for a hold
    }
    if (g_bot_quick_save) {
        g_bot_quick_save = false;
        uint8_t* gm = *reinterpret_cast<uint8_t**>(kGameMainPtr);
        Log("bot: SAVEMENU quick-saving at frame %d", frame);
        if (gm) reinterpret_cast<void(__fastcall*)(void*)>(kQuickSave)(gm);  // as the key: in co-op it waits for a hold
    }
}

void BotSaveMenuTick(uint8_t* w) {
    static int step = 0;  // 1 menu asked for, 2 in the menu, 3 Save pressed, 4 menu closing
    static DWORD at = 0;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    bool on_main = *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable;
    bool on_menu = strstr(RttiName(w), "WGameMenu@@") != nullptr;
    if (step == 0 && g_bot_menu_request && on_main) {
        step = 1;
        Log("bot: SAVEMENU opening the game menu at frame %d (pause counter %d)", frame, world[0x2db]);
        reinterpret_cast<void(__thiscall*)(void*, int)>(kGameMainSetMode)(w, 2);
    } else if (step == 1 && on_menu) {
        step = 2;
        at = GetTickCount();
    } else if (step == 2 && on_menu && GetTickCount() - at > 3000) {
        step = 3;
        at = 0;
        g_bot_press_save = w;  // BotPumpTick presses it
        Log("bot: SAVEMENU Save to be pressed from the pump (frame %d)", frame);
    } else if (step == 3 && g_bot_save_closed && on_main && !at) {
        at = GetTickCount();  // back in the menu
    } else if (step == 3 && at && on_main && GetTickCount() - at > 3000) {
        step = 4;
        Log("bot: SAVEMENU closing the game menu at frame %d (pause counter %d)", frame, world[0x2db]);
        g_end_game_mode = true;  // SetMode(0), from the mission screen's tick (PrivateScreenTick)
    } else if (step == 4 && on_main && !g_bot_menu_done && *reinterpret_cast<int*>(w + 0x156) == 0) {
        g_bot_menu_done = true;
        Log("bot: SAVEMENU game menu closed at frame %d (pause counter %d)", frame, world[0x2db]);
        g_bot_quick_save = true;
    }
}
DWORD g_bot_inv_tick = 0;
DWORD g_bot_inv_hold_ms = 0;  // chaos: how long the INV screen stays up (0: [test] hold seconds)

// levelup: one skill point spent on squad member 0, as the character screen's spinner does: its first skill
// (attribute table 0, Skills at +0x363, values from +0x367) one higher.
void BotLevelSkill() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint16_t squad[16];
    if (!world || GetSquad(world, squad, 16) < 1) return;
    int* skill = reinterpret_cast<int*>(EntityById(world, squad[0]) + 0x367);
    Log("bot: LEVEL SKILL member %u +0x367 %d -> %d", squad[0], *skill, *skill + 1);
    ++*skill;
}  // levelup: when the bot pressed the Level button (the screen may hold the world)
uint32_t Fnv(uint32_t h, const void* p, size_t n);  // private_sync.inc
bool IsShopTalk() { return _stricmp(g_test.scenario, "shoptalk") == 0; }  // QuestBarter.sav: host shops, joiner talks
int g_bot_deals = 0;  // deals this copy's bot made

bool InList(const uint16_t* ids, int n, int id) {
    for (int i = 0; i < n; i++)
        if (ids[i] == id) return true;
    return false;
}

// Inventories (FTSE Actor.h / Inventory.h / Collectable.h, whose offset comments are from the entity's start): an
// Actor's inventory EntityID {u16 seq, u16 id} at +0x1006; the Inventory entity (vtable 0x818af8) has a circular list at +0x1a9
// (sentinel node; node: next, prev, EntityID); an item's count at +0x1ad (Collectable struct +8).
const size_t kActorInventory = 0x1006;
const uintptr_t kContainerVtable = 0x817a2c;  // FTSE Container.h: struct at +0x1a6, inventory EntityID at struct +4
const size_t kContainerInventory = 0x1aa;
const uintptr_t kInventoryVtable = 0x818af8;
const size_t kInventoryList = 0x1a9;
const size_t kItemCount = 0x1ad;

// An Actor's equipment: three handle slots {seq +0x100e+4i, slot +0x1010+4i}, i = 0, 1 the hands, 2 the armour
// (getter: Actor vfunc +0x54c, 0x5581a0), and the active hand, byte +0x1029 (0/1, set by command 0x0b). The game
// changes them with commands 5 drop, 6 equip, 7 unequip, 0x0b hand: "slot.seq,slot.seq,slot.seq/hand", "" if not
// an Actor. Not part of InvString: the inventory list and the equipment are kept apart (the private-screen delta).
const size_t kActorEquip = 0x100e, kActorHand = 0x1029;
void EquipString(uint8_t* world, uint16_t actor, char* out, size_t size) {
    out[0] = 0;
    __try {
        uint8_t* a = EntityById(world, actor);
        if (*reinterpret_cast<uintptr_t*>(a) != kActorVtable) return;
        const uint16_t* s = reinterpret_cast<const uint16_t*>(a + kActorEquip);
        sprintf_s(out, size, "%u.%u,%u.%u,%u.%u/%u", s[1], s[0], s[3], s[2], s[5], s[4], a[kActorHand]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, size, "?");
    }
}

// "id:count,id:count" (list order), or "-" when empty; false if the actor has no readable inventory.
bool InvString(uint8_t* world, uint16_t actor, char* out, size_t size) {
    out[0] = 0;
    __try {
        uint8_t* owner = EntityById(world, actor);
        uintptr_t ovt = *reinterpret_cast<uintptr_t*>(owner);
        size_t field = ovt == kActorVtable ? kActorInventory : ovt == kContainerVtable ? kContainerInventory : 0;
        if (!field && ovt != kInventoryVtable) return false;
        uint16_t inv_id = field ? *reinterpret_cast<uint16_t*>(owner + field + 2) : actor;  // or an Inventory itself
        if (inv_id == 0 || inv_id >= EntityCount(world)) return false;
        uint8_t* inv = EntityById(world, inv_id);
        if (!inv || *reinterpret_cast<uintptr_t*>(inv) != kInventoryVtable) return false;
        uint8_t* head = *reinterpret_cast<uint8_t**>(inv + kInventoryList);
        size_t len = 0;
        int n = 0;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node && node != head && n < 200;
             node = *reinterpret_cast<uint8_t**>(node), n++) {
            uint16_t item = *reinterpret_cast<uint16_t*>(node + 10);
            if (item == 0 || len + 16 >= size) continue;
            uint8_t* e = EntityById(world, item);
            len += sprintf_s(out + len, size - len, "%s%u:%d", len ? "," : "", item,
                             e ? *reinterpret_cast<int*>(e + kItemCount) : -1);
        }
        if (len == 0) strcpy_s(out, size, "-");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, size, "?");
        return false;
    }
}

uint16_t FirstItem(uint8_t* world, uint16_t actor) {
    char s[2048];
    if (!InvString(world, actor, s, sizeof(s)) || s[0] == '-') return 0;
    return static_cast<uint16_t>(atoi(s));
}

uint32_t HashVars(uint8_t* world);  // private_sync.inc
uint32_t HashWide(uint32_t h, const wchar_t* s);  // private_sync.inc

void SnapshotAll(uint8_t* world, int frame) {
    int count = EntityCount(world), actors = 0;
    for (int i = 1; i < count; i++) actors += IsActor(world, i);
    Log("SNAP s=%ld frame=%d actors=%d entities=%d", g_snap_tag, frame, actors, count);
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    char ids[128] = "";
    size_t len = 0;
    for (int i = 0; i < n; i++) len += sprintf_s(ids + len, sizeof(ids) - len, "%s%u", i ? "," : "", squad[i]);
    Log("SQUAD s=%ld f=%d n=%d ids=%s", g_snap_tag, frame, n, ids);
    // Entity table bookkeeping (used, next_free, seq per entry; free-list head World+0x74f): the copies must hand out
    // the same slots to new entities.
    uint32_t h = 2166136261u;
    uint8_t* e = Entries(world);
    void* dummy = *reinterpret_cast<void**>(e);
    for (int i = 0; i < count; i++) {
        void* p = *reinterpret_cast<void**>(e + i * 8);
        uint32_t v = (p && p != dummy ? 1u : 0u) | (static_cast<uint32_t>(*reinterpret_cast<uint16_t*>(e + i * 8 + 6)) << 1);
        h = (h ^ v) * 16777619u;
        h = (h ^ *reinterpret_cast<uint16_t*>(e + i * 8 + 4)) * 16777619u;
    }
    h = (h ^ *reinterpret_cast<uint16_t*>(world + 0x74f)) * 16777619u;
    Log("ETAB s=%ld f=%d n=%d hash=%08x", g_snap_tag, frame, count, h);
    {  // the free list: unused slots, and how many of them its chain reaches from the head (a gap: slots lost to it,
       // so the table grows sooner; the owner's grew 2000 -> 16000 in an evening, 2026-10-27)
        int unused = 0, reach = 0, bad = 0;
        static uint8_t seen[65536];
        memset(seen, 0, count < 65536 ? count : 65536);
        for (int i = 1; i < count; i++) {
            void* p = *reinterpret_cast<void**>(e + i * 8);
            if (!p || p == dummy) unused++;
        }
        for (uint16_t s = *reinterpret_cast<uint16_t*>(world + 0x74f); s && s < count && !seen[s];
             s = *reinterpret_cast<uint16_t*>(e + s * 8 + 4)) {
            seen[s] = 1;
            void* p = *reinterpret_cast<void**>(e + s * 8);
            if (p && p != dummy) bad++;  // a used slot on the free list
            reach++;
        }
        char lost[256] = "";
        size_t ll = 0;
        for (int i = 1; i < count && ll + 8 < sizeof(lost); i++) {
            void* p = *reinterpret_cast<void**>(e + i * 8);
            if ((!p || p == dummy) && !seen[i]) ll += sprintf_s(lost + ll, sizeof(lost) - ll, " %d", i);
        }
        Log("FREE s=%ld f=%d unused=%d reach=%d lost=%d used_on_list=%d%s%s", g_snap_tag, frame, unused, reach,
            unused - (reach - bad), bad, ll ? " e.g." : "", lost);
    }
    {  // every entry, to diff copies offline: used (0/1), next_free, seq as hex, 4096 entries at most
        static char line[4096 * 12 + 64];
        size_t len = 0;
        for (int i = 0; i < count && i < 4096; i++) {
            void* p = *reinterpret_cast<void**>(e + i * 8);
            len += sprintf_s(line + len, sizeof(line) - len, "%d%04x%04x ", p && p != dummy ? 1 : 0,
                             *reinterpret_cast<uint16_t*>(e + i * 8 + 4), *reinterpret_cast<uint16_t*>(e + i * 8 + 6));
        }
        Log("ETABX s=%ld f=%d %s", g_snap_tag, frame, line);
    }
    Log("VARS s=%ld f=%d hash=%08x", g_snap_tag, frame, HashVars(world));  // mission and campaign variables
    {  // the squad's attribute tables (stats, skills, perks: a level-up)
        uint16_t sq[16];
        int nsq = GetSquad(world, sq, 16);
        uint32_t h = 2166136261u;
        for (int k = 0; k < nsq; k++) h = Fnv(h, EntityById(world, sq[k]) + 0x2a7, 4 * 0x339 - 1);  // private_sync.inc
        Log("ATTR s=%ld f=%d hash=%08x", g_snap_tag, frame, h);
    }
    {  // the campaign's stock list (what a quartermaster sells): templates and counts
        uint32_t sh = 2166136261u;
        __try {
            uint8_t* campaign = *reinterpret_cast<uint8_t**>(0x8bdd2c);
            wchar_t** tb = campaign ? *reinterpret_cast<wchar_t***>(campaign + 0x151) : nullptr;
            wchar_t** te = campaign ? *reinterpret_cast<wchar_t***>(campaign + 0x155) : nullptr;
            int* cnt = campaign ? *reinterpret_cast<int**>(campaign + 0x161) : nullptr;
            for (int i = 0; tb && tb + i < te && i < 4096; i++) {
                sh = HashWide(sh, tb[i]);
                sh = (sh ^ static_cast<uint32_t>(cnt ? cnt[i] : 0)) * 16777619u;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sh ^= 0xdeadbeef;
        }
        Log("STOCK s=%ld f=%d hash=%08x", g_snap_tag, frame, sh);
    }
    for (int i = 1; i < count; i++) {
        if (!IsActor(world, i)) continue;
        float* p = EntityPos(world, static_cast<uint16_t>(i));
        Log("SNAP s=%ld f=%d id=%d pos=(%.3f,%.3f,%.3f) hp=%d", g_snap_tag, frame, i, p[0], p[1], p[2],
            ActorHp(world, static_cast<uint16_t>(i)));
        char inv[2048], eq[64];
        EquipString(world, static_cast<uint16_t>(i), eq, sizeof(eq));
        if (InvString(world, static_cast<uint16_t>(i), inv, sizeof(inv)))
            Log("INV s=%ld f=%d id=%d items=%s|eq=%s", g_snap_tag, frame, i, inv, eq);
    }
}

// Every frame: log each actor's HP change, so copies can be compared hit by hit (snapshots are only every 50
// frames). The first frame of a session records without logging.
// Note: an earlier test hooked the World's random accessor 0x505b00 (&World+0x729, or a per-frame seed when the flag
// 0x8bdd8b is set) to log draws outside the client tick processor. The World's own update (around 0x4fc2xx) runs
// next to that call, not inside it, so those draws are normal; the per-frame RNG line below is the real check.
volatile bool g_in_tick = false;

void WatchHp(uint8_t* world, int frame, bool first) {
    static int last[4096];
    int count = EntityCount(world);
    if (count > 4096) count = 4096;
    for (int i = 1; i < count; i++) {
        if (!IsActor(world, i)) continue;
        int hp = ActorHp(world, static_cast<uint16_t>(i));
        if (!first && hp != last[i]) Log("HPCHG s=%ld f=%d id=%d %d->%d", g_snap_tag, frame, i, last[i], hp);
        last[i] = hp;
    }
}

void SendCommand(uint8_t* client, uint8_t cmd, uint16_t actor, uint32_t e1, const float* loc, int32_t param) {
    MoveMsg m = {};
    m.type = 2;
    m.count = 1;
    m.cmd = cmd;
    m.actor = actor;
    m.e1 = e1;
    if (loc) { m.x = loc[0]; m.y = loc[1]; m.z = loc[2]; }
    m.param = param;
    reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(kClientSend)(client, &m, sizeof(m), 1);
}

// Walk targets are waypoints: every Actor's position on the first in-game frame. Somebody was placed there by
// the game, so it is walkable, and unlike live characters the waypoints do not drift towards the squad (the
// first soak went idle: after a few walks no live character was 10-30 units away). A target is a waypoint
// 10-40 units from the walker on the same floor height; `variant` picks among them (0 = farthest).
float g_waypoints[128][3];
int g_waypoint_count = 0;

void RecordWaypoints(uint8_t* world) {
    for (int i = 1; i < EntityCount(world) && g_waypoint_count < 128; i++) {
        if (!IsActor(world, i)) continue;
        memcpy(g_waypoints[g_waypoint_count++], EntityPos(world, static_cast<uint16_t>(i)), sizeof(float) * 3);
    }
    Log("bot: %d waypoints recorded", g_waypoint_count);
}

int PickWalkTarget(uint8_t* world, uint16_t walker, unsigned variant) {
    float* a = EntityPos(world, walker);
    int cand[128], nc = 0, far_i = -1, near_i = -1;
    float far_d = 0, near_d = 1e9f;
    for (int i = 0; i < g_waypoint_count; i++) {
        float* p = g_waypoints[i];
        float dy = p[1] - a[1];
        float d = sqrtf((p[0] - a[0]) * (p[0] - a[0]) + (p[2] - a[2]) * (p[2] - a[2]));
        if (dy <= -1.0f || dy >= 1.0f) continue;
        if (d >= 5.0f && d < near_d) { near_d = d; near_i = i; }
        if (d < kMoveMin || d > kMoveMax) continue;
        cand[nc++] = i;
        if (d > far_d) { far_d = d; far_i = i; }
    }
    // A member the other player's bot already walked off may have none in range (a drop-in arrives later, dropin
    // 2026-10-28): the nearest one on its floor at least 5 units away then.
    if (nc == 0) return near_i;
    return variant == 0 ? far_i : cand[variant % nc];
}

void BotMoveMember(uint8_t* client, uint8_t* world, int member, unsigned variant) {
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    if (n <= member) {
        Log("bot: FAIL squad has %d members, need %d", n, member + 1);
        return;
    }
    uint16_t walker = squad[member];
    int t = PickWalkTarget(world, walker, variant);
    if (t < 0) {
        Log("bot: FAIL no waypoint %.0f-%.0f units from squad member %u to walk to", kMoveMin, kMoveMax, walker);
        return;
    }
    float* a = EntityPos(world, walker);
    float* p = g_waypoints[t];
    Log("bot: MOVE id=%u from (%.3f,%.3f,%.3f) towards waypoint %d at (%.3f,%.3f,%.3f)", walker, a[0], a[1], a[2], t,
        p[0], p[1], p[2]);
    SendCommand(client, 0, walker, 0, p, 4);
}

// The whole squad to a point, one command per member in one message, as the UI sends a squad move (a small grid of
// targets around the point, as the owner's x6 squad moves had).
void BotMoveSquad(uint8_t* client, uint8_t* world, const float* target) {
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    uint8_t m[6 + 27 * 16] = {};
    *reinterpret_cast<uint32_t*>(m) = 2;
    *reinterpret_cast<uint16_t*>(m + 4) = static_cast<uint16_t>(n);
    for (int i = 0; i < n; i++) {
        uint8_t* r = m + 6 + 27 * i;
        float loc[3] = {target[0] + (i % 3 - 1) * 1.25f, target[1], target[2] + (i / 3) * 1.25f};
        r[0] = 0;
        memcpy(r + 1, &squad[i], 2);
        memcpy(r + 7, loc, 12);
        int32_t param = 4;
        memcpy(r + 23, &param, 4);
    }
    Log("bot: SQUAD MOVE %d members to (%.1f,%.1f,%.1f)", n, target[0], target[1], target[2]);
    reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(kClientSend)(client, m, 6 + 27 * n, 1);
}

// Attack (command 0x02, target in e1) the nearest living character outside the squad, as the UI does.
void BotAttack(uint8_t* client, uint8_t* world) {
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    if (n < 1) {
        Log("bot: FAIL squad is empty");
        return;
    }
    float* a = EntityPos(world, squad[0]);
    int best = -1;
    float best_d = 1e9f;
    for (int i = 1; i < EntityCount(world); i++) {
        if (!IsActor(world, i) || InList(squad, n, i) || ActorHp(world, static_cast<uint16_t>(i)) <= 0) continue;
        float* p = EntityPos(world, static_cast<uint16_t>(i));
        float d = sqrtf((p[0] - a[0]) * (p[0] - a[0]) + (p[2] - a[2]) * (p[2] - a[2]));
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) {
        Log("bot: FAIL no living character to attack");
        return;
    }
    Log("bot: ATTACK id=%u target=%d hp=%d distance %.2f", squad[0], best, ActorHp(world, static_cast<uint16_t>(best)), best_d);
    SendCommand(client, 2, squad[0], FullId(world, static_cast<uint16_t>(best)), nullptr, 0);
}

// Map transition as a mission script does it: ActionEndMission's execute 0x680b60 (fastcall, ECX = action) reads
// only its target name (FOTString at +0xb); "World Map" schedules the game screen's exit to the world map
// (WGameMain+0x19d.. timer). Both bots call it on the same frame, like a trigger firing in the lockstep simulation.
const uintptr_t kActionEndMissionExecute = 0x680b60;

void BotEndMission(const char* target) {
    static uint8_t action[0x40];
    memset(action, 0, sizeof(action));
    *reinterpret_cast<void**>(action + 0xb) = MakeFOTString(target);
    Log("bot: END MISSION -> \"%s\" (as an ActionEndMission trigger does)", target);
    reinterpret_cast<void(__fastcall*)(void*)>(kActionEndMissionExecute)(action);
}

// Talk: the owner's own click on the bunker's recruiting NPC (2026-10-21 live session, host log at 40.0 s): command
// 0x17, squad member 0x03bc, NPC EntityID 0x0051000a; the other fields as the UI sent them.
const uint8_t kOwnerTalkCommand[27] = {0x17, 0xbc, 0x03, 0x0a, 0x00, 0x51, 0x00, 0x00, 0x00, 0x00, 0x49, 0x01, 0x00, 0x00,
                                       0x00, 0x00, 0x58, 0x6b, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
uint16_t g_bot_changed_id = 0;

// The talk command does nothing from across the room, and no direct move towards the NPC moved anyone (the owner's
// spot next to it, a waypoint near it; squad members 0x03bc and 0 alike). The owner walked round first: their squad
// moves from 16.3 s to 36.3 s (decoded from the live log) go down to z 320, across, and up to the NPC's open side.
// Squad member 0 follows that route, one leg every 80 frames, then talks (the talk command's actor field patched).
uint16_t g_bot_talker = 0;
const float kOwnerRoute[][3] = {{357.7f, 32.0f, 326.9f}, {355.85f, 32.15f, 322.5f}, {346.7f, 32.0f, 319.3f},
                                {340.7f, 32.0f, 318.3f}, {334.2f, 31.75f, 321.6f}, {332.73f, 32.09f, 335.16f}};
const int kOwnerRouteLegs = sizeof(kOwnerRoute) / sizeof(kOwnerRoute[0]);

void BotRouteLeg(uint8_t* client, uint8_t* world, int leg) {
    if (leg == 0) {
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        if (n < 1) {
            Log("bot: FAIL squad is empty");
            return;
        }
        g_bot_talker = squad[0];
    }
    float* a = EntityPos(world, g_bot_talker);
    Log("bot: ROUTE leg %d: member %u at (%.3f,%.3f,%.3f) to (%.3f,%.3f,%.3f)", leg, g_bot_talker, a[0], a[1], a[2],
        kOwnerRoute[leg][0], kOwnerRoute[leg][1], kOwnerRoute[leg][2]);
    SendCommand(client, 0, g_bot_talker, 0, kOwnerRoute[leg], 4);
}

// target 0: the owner's NPC as sent; else the talk goes to that entity (EntityID patched in e1).
void BotTalk(uint8_t* client, uint8_t* world, uint16_t target, int frame) {
    uint8_t m[6 + 27] = {2, 0, 0, 0, 1, 0};
    memcpy(m + 6, kOwnerTalkCommand, sizeof(kOwnerTalkCommand));
    memcpy(m + 7, &g_bot_talker, 2);
    if (target) {
        uint32_t e1 = FullId(world, target);
        memcpy(m + 9, &e1, 4);
    }
    Log("bot: TALK (the owner's talk command, to NPC %u, from member %u) at frame %d", target ? target : 81,
        g_bot_talker, frame);
    reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(kClientSend)(client, m, sizeof(m), 1);
}

// Trade: the bunker's merchant, NPC 96 (runtime run 2: the owner talked to entity 0x60, then bought and sold).
// The talker walks the owner's first two route legs, then to the merchant's counter side, talks, and presses the
// conversation's Barter button: 0x4d0000 (__thiscall(WSpeech*, button), ret 4; FTSE notes: barter 0x4d0000, gamble
// 0x4d0090, recruit 0x4d0110). It sets the trading pair (0x8bc218 buyer, 0x8bc208 merchant, from WSpeech +0x100..
// +0x106) and asks World+0x2b9 (vtable +0x28) to open it; the barter screen is mission-screen mode 6
// (WBarter 0x453e00, which pauses the world while it is up).
// The deal (BotDealOffer, BotDealPress): every Actor has a barter offer, a second Inventory entity whose EntityID is at
// +0x100a (vtable slot 406, FTSE's GetUnk22EntityID). Dragging an item onto the table puts it there. The barter table
// (WGameMain+0x11e, WBarterTable) has Offer 0x455830 (thiscall: the deal 0x4558a0 if table +0x104's value (+0x147) is
// at least table +0x108's) and Done 0x455880 (SetMode(0)). The deal moves the buyer's offer to the merchant and the
// merchant's to the buyer with RemoveInventory/AddToInventory (Entity vtable +0x508/+0x504). The bot fills both
// offers with those functions (caps from the buyer, one item from the merchant) and presses Offer.
// Observed (trade_host): the merchant holds 2 items of its own; opening the barter screen fills its inventory with the
// shop's stock (30 stacks) and closing it takes the stock away again. What the other players must get is the trader's
// game once the screens have closed (logged as INV SYNCED).
const uint16_t kMerchant = 96;
const uint16_t kQuestGiver = 266;  // QuestBarter.sav: General Barnaky (B01_GenBarnaky.ent)
const float kMerchantApproach[3] = {359.5f, 32.0f, 320.5f};
const uintptr_t kSpeechBarterButton = 0x4d0000;
const uintptr_t kBarterOffer = 0x455830;
const size_t kVtAddInventory = 0x504, kVtRemoveInventory = 0x508;
const size_t kActorOffer = 0x100a, kEntitySprite = 0x105, kItemValue = 0x1b6;
uint8_t* g_bot_barter_speech = nullptr;  // press Barter on this conversation from the mission screen's tick
bool g_bot_deal_offered = false;         // both offers filled: press Offer

struct InvResult { int32_t status; uint32_t e1; int32_t u2, u3; wchar_t* err; uint32_t e4; int32_t u5; };

// Move `count` of `item` from one actor's inventory to another's; returns the moved item's id, 0 on failure.
uint16_t MoveItem(uint8_t* world, uint16_t from, uint16_t to, uint16_t item, int count) {
    typedef void*(__thiscall * InvFn)(void*, InvResult*, uint32_t, uint32_t*, int32_t, int32_t);
    uint8_t* src = EntityById(world, from);
    uint8_t* dst = EntityById(world, to);
    InvResult r = {};
    uint32_t moved = FullId(world, item);
    reinterpret_cast<InvFn>((*reinterpret_cast<void***>(src))[kVtRemoveInventory / 4])(src, &r, moved, &moved, count, 0);
    if (r.status != 0) {
        Log("bot: remove %u from %u failed (status %d)", item, from, r.status);
        return 0;
    }
    InvResult a = {};
    uint32_t added = moved;
    reinterpret_cast<InvFn>((*reinterpret_cast<void***>(dst))[kVtAddInventory / 4])(dst, &a, moved, &added, count, 0);
    if (a.status != 0) {
        Log("bot: add %u to %u failed (status %d)", moved >> 16, to, a.status);
        return 0;
    }
    return static_cast<uint16_t>(moved >> 16);
}

void LogInv(uint8_t* world, const char* tag, uint16_t actor) {
    char s[2048];
    InvString(world, actor, s, sizeof(s));
    Log("bot: INV %s id=%u items=%s", tag, actor, s);
}

void ItemName(uint8_t* world, uint16_t item, char* out, size_t size) {
    out[0] = 0;
    __try {
        const wchar_t* w = *reinterpret_cast<wchar_t**>(EntityById(world, item) + kEntitySprite);
        if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, out, static_cast<int>(size), nullptr, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, size, "?");
    }
}

// An inventory as (item, count) pairs, from InvString.
int InvItems(uint8_t* world, uint16_t owner, uint16_t* items, int* counts, int max) {
    char s[2048];
    if (!InvString(world, owner, s, sizeof(s)) || s[0] == '-') return 0;
    int n = 0;
    for (char* p = s; *p && n < max;) {
        items[n] = static_cast<uint16_t>(strtoul(p, &p, 10));
        counts[n++] = *p == ':' ? strtol(p + 1, &p, 10) : 0;
        if (*p == ',') p++;
    }
    return n;
}

bool IsCaps(uint8_t* world, uint16_t item) {  // money: "sprites/items/Money/Ringpulls.spr" (value 1 each)
    char name[128];
    ItemName(world, item, name, sizeof(name));
    _strlwr_s(name);
    return strstr(name, "/money/") != nullptr;
}

int CapsOf(uint8_t* world, uint16_t owner, uint16_t* stack) {
    uint16_t items[200];
    int counts[200];
    int n = InvItems(world, owner, items, counts, 200), total = 0;
    for (int i = 0; i < n; i++)
        if (IsCaps(world, items[i])) {
            total += counts[i];
            if (stack) *stack = items[i];
        }
    return total;
}

uint16_t OfferOf(uint8_t* world, uint16_t actor) {
    return *reinterpret_cast<uint16_t*>(EntityById(world, actor) + kActorOffer + 2);
}

// Entity to memory, as the level editor's undo does (world_undo.cpp, 0x42ec00): a memory streambuf (0x14 bytes: base
// 0x426830, vtable 0x7fde04, FOTString buffer at +0xc, -1 at +8, 0 at +0x10), an archive on it (0x704060
// thiscall(archive, mode, streambuf, 1)), then the entity saver 0x5eabd0 cdecl(archive*, EntityID), which the game's
// own save-entity-to-file 0x5ea630 calls the same way. Exploration: log what comes out.
void ExploreSerialize(uint8_t* world, uint16_t id, int mode) {
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
        reinterpret_cast<void(__thiscall*)(void*, int, void*, int)>(0x704060)(ar, mode, sb, 1);
        char ok = reinterpret_cast<char(__cdecl*)(void*, uint32_t)>(0x5eabd0)(ar, FullId(world, id));
        uint8_t* buf = reinterpret_cast<uint8_t*>(sb[3]);
        int h[3] = {};
        if (buf) memcpy(h, buf - 0xc, sizeof(h));
        Log("bot: SERIALIZE entity %u mode %d: ok %d, sb %08x %08x %08x %08x %08x, buffer %p header %d %d %d", id, mode,
            ok, sb[0], sb[1], sb[2], sb[3], sb[4], buf, h[0], h[1], h[2]);
        if (buf) {
            char hex[3 * 96 + 1] = "", txt[200] = "";
            int n = 96;
            for (int i = 0; i < n; i++) sprintf_s(hex + 3 * i, 4, "%02x ", buf[i]);
            for (int i = 0; i < 190; i++) txt[i] = buf[i] >= 32 && buf[i] < 127 ? static_cast<char>(buf[i]) : '.';
            Log("bot: SERIALIZE bytes: %s", hex);
            Log("bot: SERIALIZE text: %s", txt);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("bot: SERIALIZE exception 0x%08x", GetExceptionCode());
    }
}

int g_bot_caps_before = 0;
void PressHandler(uintptr_t handler, uint8_t* owner, const char* what);  // below, with the recruit screen
void BotChangeQuestVar(uint8_t* world);  // private_sync.inc
bool ShopOpenForOther(uint16_t* buyer);  // shop_sync.inc: another player's shop is open (its buyer's slot)

// Fill the offers: the merchant's first item that is not caps, and caps from the buyer (three times its value, at
// least 20, at most what the buyer has).
void BotDealOffer(uint8_t* world) {
    uint16_t buyer = *reinterpret_cast<uint16_t*>(0x8bc21a), merchant = *reinterpret_cast<uint16_t*>(0x8bc20a);
    uint16_t ob = OfferOf(world, buyer), om = OfferOf(world, merchant);
    Log("bot: barter pair: buyer %u (offer inventory %u), merchant %u (offer inventory %u)", buyer, ob, merchant, om);
    uint16_t items[200];
    int counts[200];
    int n = InvItems(world, merchant, items, counts, 200);
    // The cheapest item that is not money, preferring a kind the buyer does not carry: it cannot merge into one of the
    // buyer's stacks, so the merchant's entity itself (made when the barter screen opened, on this copy only) ends up
    // in the buyer's inventory, and the delta must create it on the other copies.
    uint16_t own[200];
    int own_counts[200];
    int nown = InvItems(world, buyer, own, own_counts, 200);
    uint16_t buy = 0;
    int cheapest = 0x7fffffff;
    bool buy_new = false;
    for (int i = 0; i < n; i++) {
        int v = *reinterpret_cast<int*>(EntityById(world, items[i]) + kItemValue);
        if (IsCaps(world, items[i]) || v <= 0) continue;
        char nm[64], mine[64];
        ItemName(world, items[i], nm, sizeof(nm));
        bool fresh = true;
        for (int j = 0; j < nown && fresh; j++) {
            ItemName(world, own[j], mine, sizeof(mine));
            fresh = strcmp(nm, mine) != 0;
        }
        if ((fresh && !buy_new) || (fresh == buy_new && v < cheapest)) {
            cheapest = v;
            buy = items[i];
            buy_new = fresh;
        }
    }
    Log("bot: buying %s item kind", buy_new ? "a NEW" : "an already carried");
    uint16_t caps = 0;
    g_bot_caps_before = CapsOf(world, buyer, &caps);
    char names[2048] = "";
    size_t len = 0;
    int nb = InvItems(world, buyer, items, counts, 200);
    for (int i = 0; i < nb && len + 80 < sizeof(names); i++) {
        char nm[64];
        ItemName(world, items[i], nm, sizeof(nm));
        len += sprintf_s(names + len, sizeof(names) - len, " %u=%s(%d,v%d)", items[i], nm, counts[i],
                         *reinterpret_cast<int*>(EntityById(world, items[i]) + kItemValue));
    }
    Log("bot: buyer's items:%s", names);
    if (!buy || !caps || !ob || !om) {
        Log("bot: FAIL no deal possible (item %u, caps stack %u, offers %u/%u)", buy, caps, ob, om);
        return;
    }
    char name[128];
    ItemName(world, buy, name, sizeof(name));
    int value = *reinterpret_cast<int*>(EntityById(world, buy) + kItemValue);
    int pay = value * 3 < 20 ? 20 : value * 3;
    if (pay > g_bot_caps_before) pay = g_bot_caps_before;
    LogInv(world, "BEFORE", buyer);
    LogInv(world, "BEFORE", merchant);
    uint16_t a = MoveItem(world, merchant, om, buy, 1);
    uint16_t b = MoveItem(world, buyer, ob, caps, pay);
    if (_stricmp(g_test.scenario, "trade2") == 0 && g_coop_join) {  // the second shopper (the joiner) sells too
        uint16_t sell = 0;
        for (int i = 0; i < nb && !sell; i++)
            if (!IsCaps(world, items[i]) && items[i] != buy) sell = items[i];
        if (sell) {
            char sn[128];
            ItemName(world, sell, sn, sizeof(sn));
            uint16_t c = MoveItem(world, buyer, ob, sell, 1);
            Log("bot: SELL the buyer's %u \"%s\" too: offered %u", sell, sn, c);
        }
    }
    g_bot_deals++;
    Log("bot: OFFER the merchant's %u \"%s\" (value %d) for %d of the buyer's %d caps: offered %u and %u", buy, name,
        value, pay, g_bot_caps_before, a, b);
    g_bot_deal_offered = a && b;
}

void BotDealPress(uint8_t* gm, uint8_t* world) {
    uint8_t* table = *reinterpret_cast<uint8_t**>(gm + 0x11e);
    uint16_t buyer = *reinterpret_cast<uint16_t*>(0x8bc21a), merchant = *reinterpret_cast<uint16_t*>(0x8bc20a);
    if (!table) {
        Log("bot: FAIL no barter table");
        return;
    }
    uint8_t* t104 = *reinterpret_cast<uint8_t**>(table + 0x104);
    uint8_t* t108 = *reinterpret_cast<uint8_t**>(table + 0x108);
    Log("bot: table values: +104 %d, +108 %d", t104 ? *reinterpret_cast<int*>(t104 + 0x147) : -1,
        t108 ? *reinterpret_cast<int*>(t108 + 0x147) : -1);
    LogInv(world, "OFFER", OfferOf(world, buyer));
    LogInv(world, "OFFER", OfferOf(world, merchant));
    PressHandler(kBarterOffer, table, "the barter table's Offer button");
    int caps_after = CapsOf(world, buyer, nullptr);
    LogInv(world, "AFTER", buyer);
    LogInv(world, "AFTER", merchant);
    LogInv(world, "OFFER AFTER", OfferOf(world, buyer));
    LogInv(world, "OFFER AFTER", OfferOf(world, merchant));
    Log("bot: DEAL buyer %u caps %d -> %d", buyer, g_bot_caps_before, caps_after);
    BotChangeQuestVar(world);
}

// Recruit: the conversation's Recruit button 0x4d0110 (FTSE notes; __thiscall(WSpeech*, button), ret 4) on the
// recruiting NPC (81, the owner's talk). Exploration: log the screen's window tree, and in every button each dword
// that points into the code section (a button keeps its own copy of its callback {owner, handler}; handler
// __fastcall(owner, edx, arg)), to find the recruit screen's own buttons.
const uintptr_t kSpeechRecruitButton = 0x4d0110;
uint8_t* g_bot_recruit_speech = nullptr;

void DumpWindowTree(uint8_t* w, int depth) {
    if (!w || depth > 8) return;
    __try {
        const char* name = RttiName(w);
        char extra[512] = "";
        size_t len = 0;
        if (strstr(name, "Button") || strstr(name, "Btn")) {
            for (int off = 0xf0; off < 0x160 && len + 24 < sizeof(extra); off++) {
                uint32_t v = *reinterpret_cast<uint32_t*>(w + off);
                if (v >= 0x401000 && v < 0x7bf000)
                    len += sprintf_s(extra + len, sizeof(extra) - len, " +%x=%08x(after %08x)", off, v,
                                     *reinterpret_cast<uint32_t*>(w + off - 4));
            }
        }
        const int* rc = reinterpret_cast<const int*>(w + 0xc3);
        Log("ui: tree %*s%s %p vt %08x rect %d,%d,%d,%d%s", depth * 2, "", name, w, *reinterpret_cast<uint32_t*>(w),
            rc[0], rc[1], rc[2], rc[3], extra);
        for (uint8_t* c = *reinterpret_cast<uint8_t**>(w + 0x18); c; c = *reinterpret_cast<uint8_t**>(c + 0x24))
            DumpWindowTree(c, depth + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ui: tree exception at %p", w);
    }
}

// Recruit screen (mode 8, WSingleRecruit; first recruit run's window tree): a button keeps its handler at +0x112 and
// the handler's owner at +0x10e (the conversation's: Done 0x4cffd0, Gamble 0x4d0090, Barter 0x4d0000, Recruit
// 0x4d0110). WSingleRecruit's buttons, decompiled: 0x4d4fe0 (image button) adds the selected recruit (vtable +0x1a8
// the selection, +0x1b4 add); 0x4d5010 (image button) removes a flagged squad member (+0x1b8); 0x4d4e70 opens the
// character sheet; 0x4d4e60 (16 bytes before it): Done, presumably. Handlers are __fastcall(owner, edx, arg).
const uintptr_t kRecruitAdd = 0x4d4fe0, kRecruitRemove = 0x4d5010, kRecruitDone = 0x4d4e60;
uint8_t* g_bot_single_recruit = nullptr;

void LogSquad(uint8_t* world, const char* tag) {
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    char s[128] = "";
    size_t len = 0;
    for (int i = 0; i < n; i++) len += sprintf_s(s + len, sizeof(s) - len, "%s%u", i ? "," : "", squad[i]);
    Log("bot: SQUAD %s n=%d ids=%s", tag, n, s);
}

void PressHandler(uintptr_t handler, uint8_t* owner, const char* what) {
    Log("bot: pressing %s (%08x on %s)", what, handler, RttiName(owner));
    reinterpret_cast<void(__fastcall*)(void*, void*, int)>(handler)(owner, nullptr, 0);
}

void BotRecruitSpeechTick(uint8_t* w) {
    static int ticks = 0;
    if (++ticks == 10) {
        g_bot_done_step[1] = true;
        g_bot_recruit_speech = w;
    }
}

void BotRecruitGameMainTick(uint8_t* w) {
    int mode = *reinterpret_cast<int*>(w + 0x156);
    static DWORD pressed = 0;
    static bool dumped = false, ended = false;
    if (g_bot_recruit_speech) {
        uint8_t* speech = g_bot_recruit_speech;
        g_bot_recruit_speech = nullptr;
        Log("bot: pressing the conversation's Recruit button (mode %d)", mode);
        DumpWindowTree(speech, 0);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kSpeechRecruitButton)(speech, nullptr);
        Log("bot: Recruit button returned, mode %d", *reinterpret_cast<int*>(w + 0x156));
        pressed = GetTickCount();
        return;
    }
    if (!pressed) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    static bool added = false, done = false;
    DWORD t = GetTickCount() - pressed;
    if (!dumped && t >= 1000) {
        dumped = true;
        Log("bot: RECRUIT screen: mode %d, recruit window %p", mode, g_bot_single_recruit);
        LogSquad(world, "BEFORE");
    }
    static bool removed = false;
    if (!removed && t >= 1200 && mode == 8 && g_bot_single_recruit) {
        // The squad is full (6) in bunker.sav: dismiss the last member first, as a player would (select it, press
        // the remove button). 0x4d5010 removes the squad member whose entity flags +0xb2 have bit 0x40.
        removed = true;
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        char flags[128] = "";
        size_t len = 0;
        for (int i = 0; i < n; i++)
            len += sprintf_s(flags + len, sizeof(flags) - len, " %u:%02x", squad[i], EntityById(world, squad[i])[0xb2]);
        Log("bot: squad entity flags +0xb2:%s", flags);
        // Two dismissals, then one recruit: the recruit list's selection is the last dismissed member, so a single
        // dismiss-and-add gives back the squad it started with and the other copies would have nothing to follow.
        for (int k = 1; k <= 2 && n > 2; k++) {
            for (int i = 0; i < n; i++) EntityById(world, squad[i])[0xb2] &= ~0x40;
            EntityById(world, squad[n - k])[0xb2] |= 0x40;
            Log("bot: selecting squad member %u to dismiss", squad[n - k]);
            PressHandler(kRecruitRemove, g_bot_single_recruit, "the recruit screen's remove button");
        }
        EntityById(world, squad[0])[0xb2] |= 0x40;  // the talker is selected again, as it was
        LogSquad(world, "AFTER REMOVE");
        // Recruit someone who was not just dismissed. The add button (0x4d4fe0: vtable +0x1a8 picks, +0x1b4 adds)
        // picks from the recruit pool, a player in World+0x733 (stride 0x15d) whose members list is at +0x4b; the pick
        // is the pool member flagged +0xb2 bit 0x40, as for remove. The dismissed members went to that pool flagged.
        // The recruit list box (+0xfa): 0x6cde50 gives {u8, item* begin @+4, end @+8}; an item's +0x134 -> EntityID
        // at +0x153 (seq) / +0x155 (id); 0x6cded0(listbox, index, flag) selects one (remove selects the member it
        // dismissed that way). Select an entry that was not just dismissed.
        __try {
            uint8_t* lb = *reinterpret_cast<uint8_t**>(g_bot_single_recruit + 0xfa);
            uint8_t* v = reinterpret_cast<uint8_t*(__thiscall*)(void*)>(0x6cde50)(lb);
            uint8_t** it = *reinterpret_cast<uint8_t***>(v + 4);
            uint8_t** end = *reinterpret_cast<uint8_t***>(v + 8);
            char ents[256] = "";
            size_t el = 0;
            int pick_index = -1;
            uint16_t pick_id = 0;
            for (int i = 0; it && it + i < end && i < 32; i++) {
                uint16_t id = *reinterpret_cast<uint16_t*>(*reinterpret_cast<uint8_t**>(it[i] + 0x134) + 0x155);
                el += sprintf_s(ents + el, sizeof(ents) - el, "%s%u", i ? "," : "", id);
                if (pick_index < 0 && id != squad[n - 1] && id != squad[n - 2] && !InList(squad, n, id)) {
                    pick_index = i;
                    pick_id = id;
                }
            }
            Log("bot: recruit list entries: %s", ents);
            if (pick_index >= 0 && _stricmp(g_test.scenario, "recruitnew") == 0) {  // recruit: re-add, no new character
                reinterpret_cast<void(__thiscall*)(void*, int, int)>(0x6cded0)(lb, pick_index, 1);
                Log("bot: RECRUIT choosing pool member %u (list entry %d, not one just dismissed)", pick_id, pick_index);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("bot: exception 0x%08x reading the recruit list", GetExceptionCode());
        }
        uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
        int players = static_cast<int>((*reinterpret_cast<uint8_t**>(world + kWorldPlayers + 4) - begin) / kPlayerSize);
        for (int p = 0; p < players && n > 2; p++) {
            if (p == 1) continue;
            uint8_t* head = *reinterpret_cast<uint8_t**>(begin + p * kPlayerSize + kPlayerMembers);
            uint16_t pool[64];
            int np = 0;
            for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node && node != head && np < 64;
                 node = *reinterpret_cast<uint8_t**>(node))
                pool[np++] = *reinterpret_cast<uint16_t*>(node + 10);
            char ids[256] = "";
            size_t il = 0;
            for (int i = 0; i < np && il + 8 < sizeof(ids); i++) il += sprintf_s(ids + il, sizeof(ids) - il, "%s%u", i ? "," : "", pool[i]);
            Log("bot: player %d members (%d): %s", p, np, ids);
            if (!InList(pool, np, squad[n - 1]) && !InList(pool, np, squad[n - 2])) continue;
            uint16_t pick = 0;
            for (int i = 0; i < np && !pick; i++)
                if (pool[i] != squad[n - 1] && pool[i] != squad[n - 2] && IsActor(world, pool[i])) pick = pool[i];
            Log("bot: player %d holds a dismissed member; a member that was not: %u", p, pick);
            break;
        }
    }
    if (!added && t >= 1700 && mode == 8 && g_bot_single_recruit) {
        added = true;
        PressHandler(kRecruitAdd, g_bot_single_recruit, "the recruit screen's add button");
        LogSquad(world, "AFTER ADD");
    }
    if (!done && t >= 2500 && mode == 8 && g_bot_single_recruit) {
        done = true;
        uint8_t* r = g_bot_single_recruit;
        g_bot_single_recruit = nullptr;
        PressHandler(kRecruitDone, r, "the recruit screen's Done button");
        Log("bot: Done returned, mode %d", *reinterpret_cast<int*>(w + 0x156));
        LogSquad(world, "AFTER DONE");
    }
    if (!ended && t >= 4500) {
        ended = true;
        if (mode != 0) {
            Log("bot: recruit screen still up (mode %d): leaving it with SetMode(0)", mode);
            g_end_game_mode = true;
        }
        Log("bot: recruiting finished");
    }
}

void BotTradeSpeechTick(uint8_t* w) {  // the open conversation's tick (no game frames run meanwhile)
    static int ticks = 0;
    if (++ticks == 10) {
        g_bot_done_step[1] = true;
        if (_stricmp(g_test.scenario, "shopresync") == 0 && g_coop_host) {  // after the re-sync: talking is the test
            Log("bot: the host's conversation with the merchant is up: closing it");
            g_end_game_mode = true;
            return;
        }
        g_bot_barter_speech = w;
    }
}

// Test mode: a hardware write watch (debug register 0) on one byte, set on the main thread from a helper thread (a
// thread cannot set its own debug registers). Each hit logs EIP and the BOS.exe return addresses on the stack: used to
// find what allocates entities when the barter screen opens (the free list's head, World+0x74f).
volatile LONG g_watch_hits = 0;
LONG CALLBACK WatchHit(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    c->Dr6 = 0;
    if (InterlockedIncrement(&g_watch_hits) <= 80) {
        char chain[400];
        size_t o = 0;
        uint32_t* sp = reinterpret_cast<uint32_t*>(c->Esp);
        for (int i = 0, found = 0; i < 400 && found < 12; i++) {
            uint32_t v;
            __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (v >= 0x401000 && v < 0x7fc000) {
                o += sprintf_s(chain + o, sizeof(chain) - o, " %08x", v);
                found++;
            }
        }
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        Log("WATCH hit %ld eip=%08x head=%u stack:%s", g_watch_hits, c->Eip,
            world ? *reinterpret_cast<uint16_t*>(world + 0x74f) : 0, chain);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct WatchSet { uintptr_t addr; bool on; };
DWORD WINAPI WatchSetter(LPVOID p) {
    WatchSet* ws = static_cast<WatchSet*>(p);
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_main_thread_id);
    if (!t) return 0;
    SuspendThread(t);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c)) {
        c.Dr0 = ws->on ? ws->addr : 0;
        c.Dr7 = ws->on ? (c.Dr7 & ~0xf0003u) | 0x1u | (0x1u << 16) : (c.Dr7 & ~0xf0003u);  // L0, write, 1 byte
        c.Dr6 = 0;
        SetThreadContext(t, &c);
    }
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}

void SetWriteWatch(uintptr_t addr, bool on) {
    static bool handler = false;
    if (!handler) {
        AddVectoredExceptionHandler(1, WatchHit);
        handler = true;
    }
    static WatchSet ws;
    ws = {addr, on};
    HANDLE h = CreateThread(nullptr, 0, WatchSetter, &ws, 0, nullptr);
    if (h) {
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
    }
    Log("bot: write watch %s at %08x", on ? "on" : "off", static_cast<unsigned>(addr));
}

void BotTradeGameMainTick(uint8_t* w) {  // the mission screen's tick, after its children's
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    int mode = *reinterpret_cast<int*>(w + 0x156);
    static int barter_ticks = 0, wait = 0;
    static DWORD barter_up = 0;  // the screen ticks many times per game frame (the world is paused): time it
    if (g_bot_barter_speech) {
        uint8_t* speech = g_bot_barter_speech;
        g_bot_barter_speech = nullptr;
        Log("bot: pressing the conversation's Barter button (mode %d)", mode);
        LogInv(world, "BEFORE", g_bot_talker);
        LogInv(world, "BEFORE", kMerchant);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kSpeechBarterButton)(speech, nullptr);
        Log("bot: Barter button returned, mode %d", *reinterpret_cast<int*>(w + 0x156));
        wait = 1;
        return;
    }
    if (mode == 6 || mode == 7) {
        if (++barter_ticks == 1) {
            barter_up = GetTickCount();
            Log("bot: BARTER screen up (mode %d)", mode);
            if (_stricmp(g_test.scenario, "browse") == 0) SetWriteWatch(0, false);
        }
        if (barter_ticks == 2)  // what stops the world here: the pause counter, or the mode alone
            Log("bot: during barter: pause counter World+0x2db = %d, frame %d", *reinterpret_cast<int*>(world + 0x2db),
                *reinterpret_cast<int*>(world + kWorldFrame));
        if (barter_ticks == 2 && _stricmp(g_test.scenario, "browse") != 0) BotDealOffer(world);  // browse: look only
        if (g_bot_deal_offered && GetTickCount() - barter_up >= 700) {
            g_bot_deal_offered = false;
            BotDealPress(w, world);
        }
        // shopresync: the shop stays up until the host's re-sync reloads everyone
        DWORD barter_hold = _stricmp(g_test.scenario, "shopresync") == 0 ? 60000 : 2000;
        if (barter_up && GetTickCount() - barter_up >= barter_hold) {
            barter_up = 0;
            Log("bot: leaving the barter screen");
            g_end_game_mode = true;
        }
        return;
    }
    static DWORD pressed_at = 0;  // the screen may open a few frames after the press (shop_no_pause: at its frame)
    if (wait == 1 && !pressed_at) pressed_at = GetTickCount();
    if (wait > 0 && barter_ticks == 0 && pressed_at && GetTickCount() - pressed_at > 5000 && wait++ == 1) {
        Log("bot: FAIL the barter screen did not open (mode %d)", mode);
        g_end_game_mode = true;
    }
}

void BotLocalChange(uint8_t* world) {  // stands in for a trade: a change only this copy makes
    uint16_t squad[16];
    if (GetSquad(world, squad, 16) < 1) {
        Log("bot: FAIL squad is empty");
        return;
    }
    g_bot_changed_id = squad[0];
    int* hp = reinterpret_cast<int*>(EntityById(world, squad[0]) + kActorHp);
    Log("bot: LOCAL CHANGE id=%u hp %d->%d", squad[0], *hp, *hp - 1);
    *hp -= 1;
}

// Talk scenario, from the open conversation's window tick (no game frames run meanwhile): change this copy's game
// once, then end the conversation 60 ticks later.
bool GrowTable(uint8_t* world, int to);  // private_sync.inc
int NextTableSize(int n);

void BotSpeechTick(uint8_t* w) {
    static int ticks = 0;
    if (ticks == 0 && g_test.grow_table) {
        // [test] grow_table: the entity table grows while this conversation is open, the way it does once the free
        // list runs out (the game's allocation, 0x4e50c0), and the free list goes on from the new slots as it would
        // then; the others must grow theirs from the delta ('T'), not reload.
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        int n = EntityCount(world), m = NextTableSize(n);
        uint16_t* head = reinterpret_cast<uint16_t*>(world + 0x74f);
        uint16_t old_head = *head;
        if (GrowTable(world, m)) {
            *reinterpret_cast<int16_t*>(Entries(world) + (m - 1) * 8 + 4) = static_cast<int16_t>(old_head);
            *head = static_cast<uint16_t>(n);
            Log("bot: GROW the entity table %d -> %d during the conversation; free list from %d, then the old %u", n, m, n,
                old_head);
        }
    }
    if (ticks == 0) {
        g_bot_done_step[1] = true;
        extern bool g_my_conv_light;
        if (!g_my_conv_light) {  // a shop's light conversation (the world runs on) changes inventories: trade tests
            BotLocalChange(*reinterpret_cast<uint8_t**>(kWorldPtr));
            BotChangeQuestVar(*reinterpret_cast<uint8_t**>(kWorldPtr));  // what a story conversation changes
        }
    }
    if (++ticks == 60) {
        Log("bot: ending the conversation");
        g_end_game_mode = true;  // as leaving the conversation does: the mission screen back to mode 0
    }
}

DWORD WINAPI FinishSoon(LPVOID) {
    Sleep(10000);
    Log("bot: DONE (10 s into session %ld)", g_sessions_started);
    if (g_log) fflush(g_log);
    ExitProcess(0);
}

// Which screen is up: the shared window tick 0x6c5130 (slot 70 of most windows: own Update at vtable+0xc0,
// then each child's tick at +0x118; children from +0x18, next sibling +0x24 read after the call) is
// reimplemented byte-for-byte in behaviour, and logs each window class (MSVC RTTI behind the vtable) the first
// time it ticks. Test mode only.
const uintptr_t kWindowTick = 0x6c5130;
const uint8_t kWindowTickPrologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x06, 0xff, 0x90, 0xc0, 0x00, 0x00, 0x00};

const char* RttiName(void* obj) {
    __try {
        const uint32_t* vt = *reinterpret_cast<uint32_t**>(obj);
        const uint32_t* col = reinterpret_cast<const uint32_t*>(vt[-1]);
        return reinterpret_cast<const char*>(col[3] + 8);  // TypeDescriptor name, e.g. ".?AVWGameMain@@"
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

void NoteWindowClass(void* w) {
    static uint32_t seen[96];
    static int n = 0;
    uint32_t vt = *reinterpret_cast<uint32_t*>(w);
    for (int i = 0; i < n; i++)
        if (seen[i] == vt) return;
    if (n < 96) seen[n++] = vt;
    Log("ui: window %s first ticks (session %ld)", RttiName(w), g_sessions_started);
}

// World map -> mission as a click on a location does (WWorldWindow mouse-down 0x6ba2a0 -> 0x6ba580): campaign
// locations at *0x8bdd2c + 0x1cd (begin) / + 0x1d1 (end), 0x2f bytes each: +0x1c FOTString mission id,
// +0x20 FOTString map, +0x24 state (0 = not available), +0x2d/+0x2e flags. Mode: 3 if the mission is done
// (0x6a85d0), else 4 if +0x2d, else 1 (and, if !+0x2e, +0x24 = 2 and 0x6a87d0 marks it). Then
// 0x6b6580(WCampaignWindow*, FOTString* map, mode): logs "entering mission", closes the world map with 4.
const uintptr_t kGoToMap = 0x6b6580;       // void __thiscall(WCampaignWindow*, FOTString*, int), ret 8
const uintptr_t kMissionDone = 0x6a85d0;   // char __thiscall(campaign*, FOTString*), ret 4
const uintptr_t kMissionMark = 0x6a87d0;   // void __thiscall(campaign*, FOTString*), ret 4

extern HANDLE g_shot_event;  // dots.inc: test mode, capture the window now

void BotWorldMapStartMission(uint8_t* campaign_window) {
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(kCampaignPtr);
    uint8_t* begin = *reinterpret_cast<uint8_t**>(campaign + 0x1cd);
    uint8_t* end = *reinterpret_cast<uint8_t**>(campaign + 0x1d1);
    for (uint8_t* loc = begin; loc && loc < end; loc += 0x2f) {
        if (*reinterpret_cast<int*>(loc + 0x24) == 0) continue;
        char map[128] = "";
        const wchar_t* w = *reinterpret_cast<wchar_t**>(loc + 0x20);
        if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, map, sizeof(map), nullptr, nullptr);
        int mode = 3;
        void** id = reinterpret_cast<void**>(loc + 0x1c);
        if (!reinterpret_cast<char(__thiscall*)(void*, void**)>(kMissionDone)(campaign, id)) {
            if (!loc[0x2d]) {
                if (!loc[0x2e]) {
                    *reinterpret_cast<int*>(loc + 0x24) = 2;
                    reinterpret_cast<void(__thiscall*)(void*, void**)>(kMissionMark)(campaign, id);
                }
                mode = 1;
            } else {
                mode = 4;
            }
        }
        Log("bot: WORLD MAP -> \"%s\" mode %d (as a click on its location does)", map, mode);
        reinterpret_cast<void(__thiscall*)(void*, void**, int)>(kGoToMap)(campaign_window, reinterpret_cast<void**>(loc + 0x20), mode);
        return;
    }
    Log("bot: FAIL no available location on the world map");
}

// World map -> random encounter through the game's own path. Travel (WWorldWindow 0x6bad60, a private LCG at +0x112,
// only on the copy that travels: the host) queues a hit from its encounter table (campaign + 0x1ed, 0x45 bytes each,
// +0 FOTString name, +0x10 enabled) as a copy of the entry's name in the vector at campaign + 4 (begin) / + 8 (end) /
// + 0xc (capacity), then calls the dispatcher 0x6bcaa0. That one waits for +0x11a > 7, asks "encounter_confirm" while
// its roll is below campaign + 0x1c, then builds the map (0x6bab20) and enters it (0x6b6580, mode 2). The bot queues
// the first enabled entry and calls the dispatcher with the confirm threshold at 0 (as if the player accepted).
const uintptr_t kEncounterDispatch = 0x6bcaa0;  // void __thiscall(WWorldWindow*, int), ret 4
const uintptr_t kGameFree = 0x6c4e50;           // void __cdecl(void*)

void BotWorldMapEncounter(uint8_t* world_window) {
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(kCampaignPtr);
    uint8_t* table = *reinterpret_cast<uint8_t**>(campaign + 0x1ed);
    uint8_t* table_end = *reinterpret_cast<uint8_t**>(campaign + 0x1f1);
    uint8_t* entry = nullptr;
    for (uint8_t* e = table; e && e < table_end; e += 0x45)
        if (e[0x10]) { entry = e; break; }
    if (!entry) { Log("bot: FAIL no enabled random encounter in the campaign's table"); return; }
    void*** q = reinterpret_cast<void***>(campaign + 4);  // begin, end, capacity
    size_t n = q[0] ? q[1] - q[0] : 0;
    if (!q[0] || q[1] == q[2]) {
        void** grown = static_cast<void**>(reinterpret_cast<void*(__cdecl*)(size_t)>(kGameNew)((n + 1) * 4));
        for (size_t i = 0; i < n; ++i) grown[i] = q[0][i];
        if (q[0]) reinterpret_cast<void(__cdecl*)(void*)>(kGameFree)(q[0]);
        q[0] = grown; q[1] = grown + n; q[2] = grown + n + 1;
    }
    void* name = *reinterpret_cast<void**>(entry);  // a copy of the FOTString: one more reference (count at -0xc)
    if (name) ++*reinterpret_cast<int*>(static_cast<uint8_t*>(name) - 0xc);
    *q[1]++ = name;
    Log("bot: ENCOUNTER queued table entry %d of %d (%zu already queued, flag 0x8aeb20 = %d); dispatching",
        static_cast<int>((entry - table) / 0x45), static_cast<int>((table_end - table) / 0x45), n, *reinterpret_cast<uint8_t*>(0x8aeb20));
    int confirm = *reinterpret_cast<int*>(campaign + 0x1c);
    *reinterpret_cast<int*>(campaign + 0x1c) = 0;
    *reinterpret_cast<void**>(world_window + 0x116) = nullptr;
    *reinterpret_cast<int*>(world_window + 0x11a) = 8;
    reinterpret_cast<void(__thiscall*)(void*, int)>(kEncounterDispatch)(world_window, 0);
    *reinterpret_cast<int*>(campaign + 0x1c) = confirm;
    Log("bot: ENCOUNTER dispatched (mission mode now %d)", *reinterpret_cast<int*>(campaign + 0x14));
}

void BotDriveMultiSetup(uint8_t* w);  // netlog.cpp
void WaitPopupTick(uint8_t* w);  // netlog.cpp
void PrivateScreenTick(uint8_t* w);  // netlog.cpp
void PrivateCloseStep();  // below
void FixHoldTick();       // drift.inc
void LevelScreenClosed();  // recruit_sync.inc
bool AnyLevelLock();
bool PrivateScreensClosed(bool* alone);  // netlog.cpp
void ReleaseTalkerHold();     // netlog.cpp
extern uint8_t* g_private_speech;
extern bool g_end_game_mode;
extern uint8_t* g_client_ptr;
void BotTurnBasedTick(uint8_t* gm);  // turnbased, below

void __fastcall WindowTickHook(uint8_t* w) {
    NoteWindowClass(w);
    typedef void(__thiscall * Fn)(void*);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(w))[0xc0 / 4])(w);
    for (uint8_t* c = *reinterpret_cast<uint8_t**>(w + 0x18); c; c = *reinterpret_cast<uint8_t**>(c + 0x24)) {
        reinterpret_cast<Fn>((*reinterpret_cast<void***>(c))[0x118 / 4])(c);
    }
    JoinerFollowHost(w);  // after the tick, so the window is not closed mid-update
    BotDriveMultiSetup(w);
    WorldMapTick(w);  // worldmap_sync.inc
    if (g_test.enabled && g_test.menu_cancel && strstr(RttiName(w), "WSaveLoad@@")) {
        // The Load Game screen opened by Start Co-op: cancel it. The call that opened it returns to the menu, the
        // same way as leaving a session played inside it (0x4ca7b0 returned that way crashed, 2026-10-27).
        static DWORD opened = 0;
        static bool cancelled = false;
        if (!opened) opened = GetTickCount();
        if (!cancelled && GetTickCount() - opened > 1500) {
            cancelled = true;
            Log("bot: cancelling the Load Game screen");
            reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(w))[0x34 / 4])(w, 0);
        }
    }
    WaitPopupTick(w);
    PrivateScreenTick(w);
    if ((g_coop_host || g_coop_join) && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        __try {
            PrivateCloseStep();  // also while the world is held (no frames run)
            FixHoldTick();       // drift.inc: the host's correction while every copy holds
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("co-op: exception 0x%08x closing the private screens", GetExceptionCode());
        }
    }
    if (g_test.enabled && w == g_private_speech &&
        (_stricmp(g_test.scenario, "talk") == 0 || _stricmp(g_test.scenario, "talk2") == 0 || _stricmp(g_test.scenario, "talk2same") == 0 ||
         _stricmp(g_test.scenario, "quest") == 0 ||
         (IsShopTalk() && g_coop_join)))
        BotSpeechTick(w);
    if (g_test.enabled && IsRecruitScenario()) {
        if (w == g_private_speech) BotRecruitSpeechTick(w);
        if (*reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) BotRecruitGameMainTick(w);
        static uintptr_t dumped[8];
        static int nd = 0;
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(w);
        bool seen = false;
        for (int i = 0; i < nd; i++) seen |= dumped[i] == vt;
        if (strstr(RttiName(w), "WSingleRecruit")) g_bot_single_recruit = w;
        if (!seen && nd < 8 && strstr(RttiName(w), "Recruit")) {  // WRecruit ticks on its own: dump its children's trees
            dumped[nd++] = vt;
            Log("bot: recruit window %s ticks: its tree", RttiName(w));
            DumpWindowTree(w, 0);
        }
    }
    if (g_test.enabled && (IsTradeScenario() || _stricmp(g_test.scenario, "browse") == 0 || (IsShopTalk() && g_coop_host))) {
        if (w == g_private_speech) BotTradeSpeechTick(w);
        if (*reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) BotTradeGameMainTick(w);
    }
    // levelup: the other player, while the levelling player's screen is open (from the mission screen's tick: with
    // the host levelling up, no frames run anywhere): the same character's screen (closed again, with a notice) and
    // the recruiter (refused).
    if (g_test.enabled && _stricmp(g_test.scenario, "levelup") == 0 && _stricmp(g_test.actor, "all") != 0 &&
        (_stricmp(g_test.actor, "host") == 0) != g_coop_host && AnyLevelLock() &&
        *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        static bool screen_tried = false, recruit_tried = false;
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        if (!screen_tried) {
            screen_tried = true;
            Log("bot: the other player opens the same character's screen while it levels up");
            g_bot_level_request = true;
        } else if (!recruit_tried && *reinterpret_cast<int*>(w + 0x156) == 0 && world && g_client_ptr) {
            recruit_tried = true;
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 0) g_bot_talker = squad[0];
            Log("bot: the other player tries the recruiter while a character levels up");
            BotTalk(g_client_ptr, world, 0, *reinterpret_cast<int*>(world + kWorldFrame));
        }
    }
    // chaos: a conversation a script opened (a mission message: kept on the host's copy, everyone waits), read, then
    // closed. From the mission screen's tick: no game frames run while it is up.
    if (g_test.enabled && _stricmp(g_test.scenario, "chaos") == 0 && strstr(RttiName(w), "WTutor")) {
        static uint8_t* tutor = nullptr;  // a tutorial popup a trigger opened (on every copy): read, then closed
        static DWORD tutor_seen = 0;
        if (tutor != w) { tutor = w; tutor_seen = GetTickCount(); }
        if (GetTickCount() - tutor_seen > 2000 && !g_end_game_mode) {
            Log("bot: CHAOS closing a tutorial popup");
            g_end_game_mode = true;
            tutor_seen = GetTickCount();
        }
    }
    if (g_test.enabled && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable && _stricmp(g_test.scenario, "chaos") == 0) {
        static DWORD speech_seen = 0;
        if (g_private_open && g_private_speech) {
            if (!speech_seen) speech_seen = GetTickCount();
            if (GetTickCount() - speech_seen > 2000 && !g_end_game_mode) {
                Log("bot: CHAOS closing a conversation (%s)", RttiName(g_private_speech));
                g_end_game_mode = true;
                speech_seen = GetTickCount();
            }
        } else {
            speech_seen = 0;
        }
    }
    if (g_test.enabled && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable &&
        (_stricmp(g_test.scenario, "turnbased") == 0 || _stricmp(g_test.scenario, "tbauto") == 0))
        BotTurnBasedTick(w);
    if (g_test.enabled && _stricmp(g_test.scenario, "savemenu") == 0) BotSaveMenuTick(w);
    if (g_test.enabled && (g_bot_inv_request || g_bot_inv_tick) && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        if (g_bot_inv_request) {  // the game bar's INV button: handler 0x47fab0 (owner +0x10e, the WGameBar)
            g_bot_inv_request = false;
            for (uint8_t* c = *reinterpret_cast<uint8_t**>(w + 0x18); c; c = *reinterpret_cast<uint8_t**>(c + 0x24))
                if (*reinterpret_cast<uintptr_t*>(c) == 0x80299c)
                    for (uint8_t* b = *reinterpret_cast<uint8_t**>(c + 0x18); b; b = *reinterpret_cast<uint8_t**>(b + 0x24))
                        if (*reinterpret_cast<uintptr_t*>(b + 0x112) == 0x47fab0) {
                            reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(0x47fab0)(*reinterpret_cast<void**>(b + 0x10e), nullptr, b);
                            g_bot_inv_tick = GetTickCount();
                            Log("bot: INV opened: mode %d", *reinterpret_cast<int*>(w + 0x156));
                        }
        } else if (GetTickCount() - g_bot_inv_tick > (g_bot_inv_hold_ms ? g_bot_inv_hold_ms : static_cast<DWORD>(g_test.hold) * 1000)) {
            g_bot_inv_tick = 0;
            Log("bot: INV closing (mode %d)", *reinterpret_cast<int*>(w + 0x156));
            g_end_game_mode = true;
        }
    }
    if (g_test.enabled && g_bot_level_request && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        g_bot_level_request = false;
        g_bot_level_tick = GetTickCount();  // before: the perk dialog's own loop runs inside this call
        reinterpret_cast<void(__thiscall*)(void*, int)>(0x471aa0)(w, 3);
        Log("bot: LEVEL pressed: mode %d", *reinterpret_cast<int*>(w + 0x156));
        g_bot_level_tick = GetTickCount();
    }
    if (g_test.enabled && g_bot_level_tick && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        static bool dumped = false, closed = false;
        DWORD t = GetTickCount() - g_bot_level_tick;
        (void)dumped;  // (a direct skill poke was tried here: the screen's Done writes its own copy over it)
        static DWORD last_press = 0;
        if (t > 3000 && !closed && GetTickCount() - last_press > 1000) {  // the screen's Done (0x4678b0), until it closes
            last_press = GetTickCount();
            uint8_t* done = nullptr;
            for (uint8_t* c = *reinterpret_cast<uint8_t**>(w + 0x18); c; c = *reinterpret_cast<uint8_t**>(c + 0x24))
                if (strstr(RttiName(c), "WCharacter"))
                    for (uint8_t* b = *reinterpret_cast<uint8_t**>(c + 0x18); b; b = *reinterpret_cast<uint8_t**>(b + 0x24))
                        if (*reinterpret_cast<uintptr_t*>(b + 0x112) == 0x4678b0) done = b;
            if (done) {
                Log("bot: LEVEL pressing the character screen's Done");
                reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(0x4678b0)(*reinterpret_cast<void**>(done + 0x10e), nullptr, done);
            } else if (*reinterpret_cast<int*>(w + 0x156) != 3) {
                closed = true;
                Log("bot: LEVEL the character screen is closed");
            } else {
                closed = true;
                Log("bot: FAIL no Done button on the character screen; leaving it");
                g_end_game_mode = true;
            }
        }
    }
    if (g_test.enabled && g_coop_host && _stricmp(g_test.scenario, "transition") == 0 &&
        *reinterpret_cast<uintptr_t*>(w) == kCampaignWindowVtable) {
        static int ticks = 0;  // let the world map settle (about 2 s) before choosing
        if (++ticks == 60) BotWorldMapStartMission(w);
    }
    if (g_test.enabled && g_coop_host && _stricmp(g_test.scenario, "encounter") == 0 && strstr(RttiName(w), "WWorldWindow")) {
        static int ticks = 0;  // let the world map settle (about 2 s), then meet a random encounter
        if (++ticks == 60) BotWorldMapEncounter(w);
    }
    if (g_test.enabled && _stricmp(g_test.scenario, "worldmap") == 0) {
        // The world map: the joiner tries to travel (refused), the host travels to open ground; both log their party
        // (WMPOS) so the runner can see the joiner's follow the host's; once the host's has stopped, it picks a mission.
        static uint8_t* campaign_window = nullptr;
        if (*reinterpret_cast<uintptr_t*>(w) == kCampaignWindowVtable) campaign_window = w;
        if (strstr(RttiName(w), "WWorldWindow@@")) {
            static int ticks = 0, arrived = -1;
            static DWORD arrived_at = 0;
            ++ticks;
            uint8_t* camp = *reinterpret_cast<uint8_t**>(kCampaignPtr);
            float* party = reinterpret_cast<float*>(camp + 0xa5);
            int* wr = reinterpret_cast<int*>(w + 0xc3);
            if (ticks == 1) {  // the cursor starts in the window's corner and the map edge-scrolls: park it mid-map
                PostMessageA(g_pump_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM((wr[0] + wr[2]) / 2, (wr[1] + wr[3]) / 2));
                Log("bot: cursor parked mid-map at (%d, %d)", (wr[0] + wr[2]) / 2, (wr[1] + wr[3]) / 2);
            }
            if (ticks % 10 == 0) {
                int seen = 0;
                uint32_t fog = FogHash(camp, &seen);
                Log("WMPOS t=%d x=%.3f y=%.3f moving=%d fog=%08x seen=%d", ticks, party[0], party[1], w[0x101], fog, seen);
            }
            if (ticks == 30 && g_coop_join) {
                float dest[2] = {party[0] - 40.0f, party[1] + 30.0f};
                Log("bot: the joiner tries to travel on the world map to (%.1f, %.1f)", dest[0], dest[1]);
                reinterpret_cast<void(__thiscall*)(void*, float*)>(0x6ba410)(w, dest);
            }
            if (ticks == 40 && g_coop_host) {
                float dest[2] = {party[0] + 250.0f, party[1] + 120.0f};  // far enough to uncover new ground
                Log("bot: WMAP the host travels to (%.1f, %.1f)", dest[0], dest[1]);
                reinterpret_cast<void(__thiscall*)(void*, float*)>(0x6ba410)(w, dest);
            }
            // Pings: the joiner's left click and the host's right click, through the window's own mouse-down slot (as
            // the game's input calls it).
            typedef char(__thiscall * MouseFn)(void*, int*, int, int);
            MouseFn mouse = reinterpret_cast<MouseFn>((*reinterpret_cast<void***>(w))[51]);
            if (ticks == 50 && g_coop_join) {
                int pt[2] = {150, 120};
                Log("bot: PING the joiner clicks its map at (150, 120)");
                mouse(w, pt, 0x00a1b201, 0);  // the button in the low byte only, as the game's input passes it
            }
            if (ticks == 55 && g_coop_host) {
                int pt[2] = {300, 200};
                Log("bot: PING the host right-clicks its map at (300, 200)");
                mouse(w, pt, 0x7f3a5c02, 0);
            }
            static DWORD pinged_at = 0;
            if (ticks == 50) pinged_at = GetTickCount();
            if (pinged_at && g_coop_join) {  // the test windows edge-scroll (the cursor): keep both pings in view
                MapPing pp[4];
                float cx = 0, cy = 0;
                int np = LivePings(pp);
                for (int k = 0; k < 4 && np; k++)
                    if (pp[k].born) cx += pp[k].x / np, cy += pp[k].y / np;
                if (np) {
                    *reinterpret_cast<float*>(camp + 0x9d) = cx - (wr[2] - wr[0]) / 2.0f;
                    *reinterpret_cast<float*>(camp + 0xa1) = cy - (wr[3] - wr[1]) / 2.0f;
                }
            }
            if (pinged_at && GetTickCount() - pinged_at > 2000 && g_coop_join) {  // both pings drawn by now
                pinged_at = 0;
                Log("bot: PING screenshot of the joiner's map");
                if (g_shot_event) SetEvent(g_shot_event);
            }
            if (g_coop_host && ticks > 45 && !w[0x101] && arrived < 0) {
                arrived = ticks;
                arrived_at = GetTickCount();
                Log("bot: WMAP the host's party stopped at (%.3f, %.3f)", party[0], party[1]);
            }
            if (g_coop_host && arrived_at && GetTickCount() - arrived_at > 5000 && campaign_window) {  // the pings seen first
                arrived_at = 0;
                BotWorldMapStartMission(campaign_window);
            }
        }
    }
    if (g_test.enabled && _stricmp(g_test.scenario, "exit") == 0 && *reinterpret_cast<uintptr_t*>(w) == kCampaignWindowVtable) {
        static int ticks = 0;  // the joiner tries first (refused), the host chooses a little later
        ++ticks;
        if (ticks == 30 && g_coop_join) {
            Log("bot: the joiner tries to choose a location on the world map");
            BotWorldMapStartMission(w);
        }
        if (ticks == 90 && g_coop_host) BotWorldMapStartMission(w);
    }
}

// ---- chaos: every copy plays at once, as fast as people would and faster (owner, 2026-10-23: "just call stuff
// constantly, swap characters, issue move commands, etc etc"). Each copy draws from its own random stream (seeded per
// copy and logged, never the game's), so the copies act differently and on the same characters at overlapping times:
// moves of one character or the whole squad, gives of a random stack between two characters (0x13), its own inventory
// screen opened and closed, the nearest container used (0x04: its loot screen, then closed), its selection swapped.
// The snapshots every 50 frames compare the copies afterwards; the drift check corrects or re-syncs on the way.
uint32_t g_chaos_rng = 0;
uint32_t ChaosRand() {
    g_chaos_rng = g_chaos_rng * 1664525u + 1013904223u;
    return g_chaos_rng >> 8;
}

// msg 2 with one command and both entity fields: give (0x13) takes the recipient in e1, the item in e2, count << 3 in param.
void SendCommand2(uint8_t* client, uint8_t cmd, uint16_t actor, uint32_t e1, uint32_t e2, int32_t param) {
    MoveMsg m = {};
    m.type = 2;
    m.count = 1;
    m.cmd = cmd;
    m.actor = actor;
    m.e1 = e1;
    m.e2 = e2;
    m.param = param;
    reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(kClientSend)(client, &m, sizeof(m), 1);
}

// The loot screen's own moves (WLoot 0x803f00): Take All (button callback 0x495fa0) sends command 0x15 through
// 0x631700(Client, actor handle*, container handle*); a stack dropped on the character sends 0x14 through
// 0x631690(Client, actor*, container*, item*, count). The Client is WLoot+0x110, the container's handle +0x100, the
// character the one that searched it (the button takes the screen's selection). Owner, two PCs: the copies split after
// the other player took everything from a body.
void BotLootTake(uint8_t* w) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    void* client = *reinterpret_cast<void**>(w + 0x110);
    if (!world || !client) return;
    extern uint16_t g_my_use_actor;  // the character that searched it (netlog.cpp), the one taking here
    uint32_t actor = g_my_use_actor ? FullId(world, g_my_use_actor) : 0;
    uint16_t container = *reinterpret_cast<uint16_t*>(w + 0x102);
    uint16_t items[200];
    int counts[200];
    int k = InvItems(world, container, items, counts, 200);
    if (!actor || k == 0) {
        Log("bot: CHAOS loot screen of %u: %s", container, actor ? "empty" : "no character selected");
        return;
    }
    extern int g_loot_mode;  // loot test: 1 take all, 2 one stack; 0 chaos: either
    bool take_all = g_loot_mode ? g_loot_mode == 1 : (ChaosRand() % 2) != 0;
    if (take_all) {
        Log("bot: CHAOS TAKE ALL from %u (%d stacks) by %u", container, k, actor >> 16);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t*, void*)>(0x631700)(client, &actor, w + 0x100);
    } else {
        int i = static_cast<int>(ChaosRand() % static_cast<uint32_t>(k));
        uint32_t item = FullId(world, items[i]);
        Log("bot: CHAOS TAKE item %u x%d from %u by %u", items[i], counts[i], container, actor >> 16);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t*, void*, uint32_t*, int)>(0x631690)(client, &actor, w + 0x100, &item,
                                                                                              counts[i] > 0 ? counts[i] : 1);
    }
}

// The inventory screen's equipment moves, as its drags and clicks send them (WSquadEquip 0x46f940 / 0x46f520 /
// 0x470290): equip 0x631320(Client, actor*, item*, slot) cmd 6, unequip 0x631300(Client, actor*, item*) cmd 7, active
// hand 0x631a80(Client, actor*, hand) cmd 0x0b. A random one for character a.
void BotEquipMove(uint8_t* client, uint8_t* world, uint16_t a) {
    uint32_t actor = FullId(world, a);
    uint8_t* e = EntityById(world, a);
    unsigned what = ChaosRand() % 3;
    int slot = static_cast<int>(ChaosRand() % 3);
    uint32_t worn = *reinterpret_cast<uint32_t*>(e + kActorEquip + 4 * slot);
    uint16_t items[64];
    int counts[64];
    int k = InvItems(world, a, items, counts, 64);
    if (what == 0 && k > 0) {
        uint32_t item = FullId(world, items[ChaosRand() % k]);
        Log("bot: CHAOS equip item %u on %u, slot %d", item >> 16, a, slot);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t*, uint32_t*, int)>(0x631320)(client, &actor, &item, slot);
    } else if (what == 1 && (worn >> 16)) {
        Log("bot: CHAOS unequip item %u from %u, slot %d", worn >> 16, a, slot);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t*, uint32_t*)>(0x631300)(client, &actor, &worn);
    } else {
        int hand = e[kActorHand] ? 0 : 1;
        Log("bot: CHAOS active hand of %u -> %d", a, hand);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t*, int)>(0x631a80)(client, &actor, hand);
    }
}

// [test] inv_chaos (trade scenarios, the copy that doesn't trade): while the other player walks to the merchant and
// shops (a shop pauses nobody), this one gives stacks and equips, unequips and swaps hands among its squad, never
// member 0 (the trader's). The owner's bunker night: one shopped, the other re-equipped the squad, and they split.
void BotInventoryChaos(uint8_t* client, uint8_t* world, int rel_s) {
    static int next = 0;
    if (!g_chaos_rng) g_chaos_rng = GetTickCount() ^ 0x85ebca6bu;
    if (rel_s < next || g_private_open) return;
    next = rel_s + 5 + static_cast<int>(ChaosRand() % 15);
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    if (n < 3) return;
    int ia = 1 + static_cast<int>(ChaosRand() % (n - 1));
    uint16_t a = squad[ia];
    if (ChaosRand() % 2) {
        BotEquipMove(client, world, a);
        return;
    }
    int ib = 1 + static_cast<int>(ChaosRand() % (n - 2));
    uint16_t b = squad[ib >= ia ? ib + 1 : ib];
    uint16_t items[64];
    int counts[64];
    int k = InvItems(world, a, items, counts, 64);
    if (k <= 0) return;
    int i = static_cast<int>(ChaosRand() % k);
    int c = counts[i] > 1 ? 1 + static_cast<int>(ChaosRand() % counts[i]) : 1;
    Log("bot: CHAOS give %d of item %u (stack of %d) from %u to %u", c, items[i], counts[i], a, b);
    SendCommand2(client, 0x13, a, FullId(world, b), FullId(world, items[i]), c << 3);
}

void BotChaos(uint8_t* client, uint8_t* world, int rel_s) {
    static int next = 0;
    if (!g_chaos_rng) {
        g_chaos_rng = GetTickCount() ^ (g_coop_host ? 0x9e3779b9u : 0x85ebca6bu * static_cast<uint32_t>(g_test.index));
        Log("bot: CHAOS seed %08x", g_chaos_rng);
        next = 20 + static_cast<int>(ChaosRand() % 20);
    }
    extern DWORD g_chaos_search_until;  // a forced search: no other orders until its loot screen opens (netlog.cpp)
    if (rel_s < next || g_private_open || GetTickCount() < g_chaos_search_until) return;
    next = rel_s + 6 + static_cast<int>(ChaosRand() % 25);
    uint16_t squad[16];
    int n = GetSquad(world, squad, 16);
    if (n < 1) return;
    int ia = static_cast<int>(ChaosRand() % n);
    uint16_t a = squad[ia];
    unsigned roll = ChaosRand() % 100;
    static int searches = 0;
    if (searches < 2 && rel_s >= 60 * (searches + 1)) roll = 90;  // two searches early in every run: the loot path
    if (roll < 35) {  // one character somewhere
        int t = PickWalkTarget(world, a, ChaosRand());
        if (t >= 0) {
            Log("bot: CHAOS move %u to waypoint %d", a, t);
            SendCommand(client, 0, a, 0, g_waypoints[t], 4);
        }
    } else if (roll < 50) {  // the whole squad
        int t = PickWalkTarget(world, squad[0], ChaosRand());
        if (t >= 0) {
            Log("bot: CHAOS squad move to waypoint %d", t);
            BotMoveSquad(client, world, g_waypoints[t]);
        }
    } else if (roll < 75 && ChaosRand() % 2) {
        BotEquipMove(client, world, a);
    } else if (roll < 75 && n > 1) {  // a stack from one character to another, all of it or part
        int ib = static_cast<int>(ChaosRand() % (n - 1));
        uint16_t b = squad[ib >= ia ? ib + 1 : ib];
        uint16_t items[64];
        int counts[64];
        int k = InvItems(world, a, items, counts, 64);
        if (k <= 0) return;
        int i = static_cast<int>(ChaosRand() % k);
        int c = counts[i] > 1 ? 1 + static_cast<int>(ChaosRand() % counts[i]) : 1;
        Log("bot: CHAOS give %d of item %u (stack of %d) from %u to %u", c, items[i], counts[i], a, b);
        SendCommand2(client, 0x13, a, FullId(world, b), FullId(world, items[i]), c << 3);
    } else if (roll < 85) {  // this player's inventory screen, for a moment
        if (!g_bot_inv_tick) {
            g_bot_inv_hold_ms = 500 + ChaosRand() % 2500;
            Log("bot: CHAOS inventory screen for %u ms", g_bot_inv_hold_ms);
            g_bot_inv_request = true;
        }
    } else if (roll < 93) {  // the nearest container (its loot screen opens for this player when the character gets there)
        float* p = EntityPos(world, a);
        int best = -1;
        float best_d = 30.0f;
        for (int i = 1; i < EntityCount(world); i++) {
            uint8_t* e = EntityById(world, static_cast<uint16_t>(i));
            if (*reinterpret_cast<uintptr_t*>(e) != kContainerVtable) continue;
            float* q = EntityPos(world, static_cast<uint16_t>(i));
            float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2]));
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best > 0) {
            Log("bot: CHAOS %u uses container %d (distance %.1f)", a, best, best_d);
            SendCommand2(client, 0x04, a, FullId(world, static_cast<uint16_t>(best)), 0, 6);  // param 6, as the UI sends a use
            if (roll == 90 && searches < 2) {
                extern DWORD g_chaos_search_until;
                g_chaos_search_until = GetTickCount() + 20000;
            }
            searches++;
        }
    } else if (GetPrivateProfileIntA("test", "chaos_select", 1, ".\\ftcoop.ini")) {
        // a new selection: what the dots show elsewhere (the selection is this copy's own, +0xb2 bit 0x40)
        for (int i = 0; i < n; i++) {
            uint8_t* e = EntityById(world, squad[i]);
            if (ChaosRand() % 2) e[0xb2] |= 0x40; else e[0xb2] &= ~0x40;
        }
        Log("bot: CHAOS selection swapped");
    }
}

// Exploration ([test] entity_log): every actor's raw memory every 10 frames, to ftcoop_actors.bin as {u32 frame, u16 id,
// kActorDump bytes}, for an offline diff of two copies: the first field that differs before a one-copy attack.
const size_t kActorDump = 0xfa0;  // past the attributes (+0x2a7 + 4 * 0x339)
int g_dump_actors = -1;           // [test] entity_log, read on the first frame
void DumpActors(uint8_t* world, int frame) {
    static FILE* f = nullptr;
    if (!f && fopen_s(&f, "ftcoop_actors.bin", "wb") != 0) f = nullptr;
    if (!f) return;
    static uint8_t buf[kActorDump];
    for (int i = 1; i < EntityCount(world); i++) {
        if (!IsActor(world, i)) continue;
        __try {
            memcpy(buf, EntityById(world, static_cast<uint16_t>(i)), kActorDump);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(buf, 0xee, sizeof(buf));
        }
        uint16_t id = static_cast<uint16_t>(i);
        fwrite(&frame, 4, 1, f);
        fwrite(&id, 2, 1, f);
        fwrite(buf, 1, kActorDump, f);
    }
    fflush(f);
}

bool BotMissionPings(int rel);  // dots.inc

bool Once(int step) {
    if (g_bot_done_step[step]) return false;
    g_bot_done_step[step] = true;
    return true;
}

bool g_tb_done = false;  // turnbased: enough squad turns seen
// ---- turnbased: Individual / Squad turn-based combat with shared control of one squad (owner, 2026-10-27).
// World+0x427: 0 = real time, 1 or 2 = a turn-based combat runs (started by record 0x1f: the game bar's toggle
// 0x477bd0 or a hit, from the preference World+0x78d, single player, or +0x791 when World+0x7c9). Turn groups: vector
// World+0x430/+0x434 (0x18 bytes each), the current one's index at World+0x43c; 0x507bc0 gives the player slot that
// owns the current group. End turn: 0x477a80(WGameMain*, squad) checks 0x6368a0(client, that player) (the client's id
// +0x130 against the player's +0x31) and sends record 0x22 (squad) or 0x0d per character. Every copy's bot orders its
// own squad member on each squad turn; the players take turns pressing the real end-turn button.
const uintptr_t kToggleTurnBased = 0x477bd0;   // void __thiscall(WGameMain*)
const uintptr_t kEndTurn = 0x477a80;           // void __thiscall(WGameMain*, char squad), ret 4
const uintptr_t kCurrentTurnPlayer = 0x507bc0; // uint8_t* __thiscall(World*): the player slot whose group is current
const size_t kWorldCombat = 0x427;
const size_t kWorldTurnGroups = 0x430;
const size_t kWorldTurnGroup = 0x43c;

uint8_t* TurnPlayer(uint8_t* world) {
    if (!world || *reinterpret_cast<int*>(world + kWorldCombat) == 0) return nullptr;
    __try {
        return reinterpret_cast<uint8_t*(__thiscall*)(void*)>(kCurrentTurnPlayer)(world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint8_t* SquadOwner(uint8_t* world) {  // the player slot that owns the shared squad (the host's)
    uint16_t squad[16];
    if (GetSquad(world, squad, 16) < 1) return nullptr;
    int slot = *reinterpret_cast<uint8_t*>(EntityById(world, squad[0]) + 0x163);
    return *reinterpret_cast<uint8_t**>(world + kWorldPlayers) + slot * kPlayerSize;
}

// Frame hook, every co-op session: one line whenever the combat mode, the current group or its player changes, so
// the copies' turn sequences can be compared.
void LogTurns(uint8_t* world, int frame) {
    static int last_mode = -1, last_group = -1, last_player = -2;
    static long pref_logged = 0;
    if (pref_logged != g_snap_tag) {
        pref_logged = g_snap_tag;
        Log("TBPREF s=%ld single=%d multi=%d", g_snap_tag, *reinterpret_cast<int*>(world + 0x78d), *reinterpret_cast<int*>(world + 0x791));
    }
    int mode = *reinterpret_cast<int*>(world + kWorldCombat);
    int group = mode ? *reinterpret_cast<uint16_t*>(world + kWorldTurnGroup) : -1;
    uint8_t* p = TurnPlayer(world);
    int player = p ? *reinterpret_cast<int*>(p + kPlayerId) : -1;
    if (mode == last_mode && group == last_group && player == last_player) return;
    last_mode = mode, last_group = group, last_player = player;
    uint8_t* b = *reinterpret_cast<uint8_t**>(world + kWorldTurnGroups);
    uint8_t* e = *reinterpret_cast<uint8_t**>(world + kWorldTurnGroups + 4);
    Log("TURN s=%ld f=%d mode=%d group=%d/%d player=%d squad=%d", g_snap_tag, frame, mode, group,
        b ? static_cast<int>((e - b) / 0x18) : 0, player, p && p == SquadOwner(world) ? 1 : 0);
}

uint16_t CurrentTurnActor(uint8_t* world) {  // the current group's first member (an entity handle: index in the high half)
    uint8_t* b = *reinterpret_cast<uint8_t**>(world + kWorldTurnGroups);
    uint8_t* e = *reinterpret_cast<uint8_t**>(world + kWorldTurnGroups + 4);
    int idx = *reinterpret_cast<uint16_t*>(world + kWorldTurnGroup);
    if (!b || idx >= (e - b) / 0x18) return 0;
    uint8_t* g = b + idx * 0x18;
    uint8_t* mb = *reinterpret_cast<uint8_t**>(g + 4);
    return mb && mb != *reinterpret_cast<uint8_t**>(g + 8) ? *reinterpret_cast<uint16_t*>(mb + 2) : 0;
}

// The mission screen's tick (UI context: the buttons' own calls). Wall-clock pacing, as people press buttons.
void BotTurnBasedTick(uint8_t* gm) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world || !g_client_ptr || g_snap_tag < 1) return;
    static DWORD t0 = 0, turn_start = 0, pressed_at = 0;
    static bool toggled = false, was_squad = false, acted = false, pressed = false, fallback = false;
    static int turn = 0;
    DWORD now = GetTickCount();
    if (!t0) t0 = now;
    int mode = *reinterpret_cast<int*>(world + kWorldCombat);
    // tbauto: nobody touches the toggle; the host's squad fires at the nearest enemy and the game starts the combat
    // itself (fire 0x611730 -> 0x506b30), turn-based from the host's option (on every copy: settings_sync.inc).
    bool auto_start = _stricmp(g_test.scenario, "tbauto") == 0;
    static DWORD last_attack = 0;
    static int attacks = 0;
    if (auto_start && g_coop_host && mode == 0 && attacks < 6 && now - t0 > 3000 && now - last_attack > 5000) {
        last_attack = now;
        ++attacks;
        Log("bot: TB attacking to start a combat (try %d, turn-based option %d)", attacks, *reinterpret_cast<int*>(world + 0x78d));
        BotAttack(g_client_ptr, world);
    }
    if (!auto_start && !toggled && g_coop_host && now - t0 > 3000) {
        toggled = true;
        Log("bot: TB toggling turn-based (preference single %d, multi %d, world+0x7c9 %d)", *reinterpret_cast<int*>(world + 0x78d),
            *reinterpret_cast<int*>(world + 0x791), world[0x7c9]);
        reinterpret_cast<void(__thiscall*)(void*)>(kToggleTurnBased)(gm);
    }
    static bool was_tb = false;
    if (!mode) {
        if (was_tb && !g_tb_done) {
            g_tb_done = true;
            Log("bot: TB back in real time after %d squad turns", turn);
        }
        return;
    }
    was_tb = true;
    uint8_t* cur = TurnPlayer(world);
    bool squad = cur && cur == SquadOwner(world);
    if (squad && !was_squad) {
        ++turn;
        turn_start = now, acted = false, pressed = false, fallback = false;
        Log("bot: TB squad turn %d", turn);
    } else if (!squad && was_squad) {
        Log("bot: TB squad turn %d over", turn);
    }
    was_squad = squad;
    if (!squad) return;
    int players = 1 + g_test.joiners;
    int me = g_coop_host ? 0 : g_test.index;
    // Squad turn-based (mode 2): the whole squad's turn, every player orders its own member. Individual (mode 1): one
    // character's turn, which only that character can use: the players take it in turns (the presser orders it).
    if (!acted && now - turn_start > 1500) {
        acted = true;
        int member = me;
        if (mode == 1) {
            uint16_t squad_ids[16];
            int n = GetSquad(world, squad_ids, 16);
            uint16_t cur_id = CurrentTurnActor(world);
            member = -1;
            for (int i = 0; i < n; i++)
                if (squad_ids[i] == cur_id) member = i;
            if (turn % players != me) member = -1;
        }
        if (member >= 0) {
            if (mode == 2 && me == 0 && turn % 2 == 0) BotAttack(g_client_ptr, world);
            else BotMoveMember(g_client_ptr, world, member, static_cast<unsigned>(turn));
        }
    }
    if (!pressed && now - turn_start > 5000 && turn % players == me) {
        pressed = true, pressed_at = now;
        Log("bot: TB END TURN pressed (squad turn %d, player %d)", turn, me);
        reinterpret_cast<void(__thiscall*)(void*, char)>(kEndTurn)(gm, 1);
    }
    if (pressed && !fallback && now - pressed_at > 8000) {
        fallback = true;
        Log("bot: TB end turn had no effect after 8 s (squad turn %d, player %d)", turn, me);
    }
    if (!fallback && me == 0 && turn % players != 0 && now - turn_start > 15000) {
        fallback = true;
        Log("bot: TB the host ends squad turn %d instead", turn);
        reinterpret_cast<void(__thiscall*)(void*, char)>(kEndTurn)(gm, 1);
    }
    // Every player pressed End Turn twice: the last player leaves turn-based through the toggle (record 0x1e, after
    // the same "my turn" check), and the run ends once the combat is over on this copy.
    static bool leaving = false;
    if (turn > 2 * players + 1 && !leaving && me == players - 1) {
        leaving = true;
        Log("bot: TB leaving turn-based (player %d)", me);
        reinterpret_cast<void(__thiscall*)(void*)>(kToggleTurnBased)(gm);
    }
}

void BotFinish() {
    Log("bot: DONE");
    if (g_log) fflush(g_log);
    ExitProcess(0);
}

// A host alone (test joiners=0): once its conversation or shop has closed, its squad must still move. In 0.10.4 its
// world stayed held after the screens closed and no frame ran again (the owner, alone in the bunker).
void BotSoloAfterClose(uint8_t* client, uint8_t* world, int rel_s) {
    static int closed_at = -1;
    static uint16_t walker = 0;
    static float from[3];
    if (g_test.joiners != 0 || !g_coop_host || g_snap_tag != 1) return;
    if (closed_at < 0) {
        if (!g_bot_done_step[1] || g_private_open) return;
        uint16_t squad[16];
        if (GetSquad(world, squad, 16) < 1) return;
        closed_at = rel_s;
        walker = squad[0];
        memcpy(from, EntityPos(world, walker), sizeof(from));
        Log("bot: solo: the screens are closed at frame %d; moves squad member %u", rel_s, walker);
        BotMoveMember(client, world, 0, 3);
    } else if (rel_s >= closed_at + 150 && Once(15)) {
        float* p = EntityPos(world, walker);
        float d = sqrtf((p[0] - from[0]) * (p[0] - from[0]) + (p[2] - from[2]) * (p[2] - from[2]));
        Log("bot: solo: 150 frames after the close, squad member %u moved %.1f units", walker, d);
    }
}

void BotStep(uint8_t* client, uint8_t* world, int frame) {
    if (g_bot_t0 < 0) {
        g_bot_t0 = frame;
        Log("bot: in game, first frame %d, mode %s, scenario %s", frame, g_coop_host ? "host" : g_coop_join ? "join" : "off",
            g_test.scenario);
        RecordWaypoints(world);
    }
    int rel = frame - g_bot_t0;
    // Per session: frames since the session's first in-game frame; snapshots every 50 of them, tagged s=<session>.
    static LONG session = 0;
    static int session_t0 = 0;
    if (g_sessions_started != session) {
        session = g_sessions_started;
        session_t0 = frame;
        g_bot_last_snap = -1;
        Log("bot: session %ld first frame %d", session, frame);
    }
    int rel_s = frame - session_t0;
    if (g_snap_tag > 0) WatchHp(world, frame, rel_s == 0);
    if (g_snap_tag > 0) Log("RNG s=%ld f=%d v=%08x", g_snap_tag, frame, *reinterpret_cast<uint32_t*>(world + 0x729));
    if (g_snap_tag > 0) LogTurns(world, frame);
    // Test god mode (lootbody): the squad's HP back to its maximum (Actor +0xca6) every frame, on every copy alike.
    if (g_snap_tag > 0 && (_stricmp(g_test.scenario, "lootbody") == 0 || _stricmp(g_test.scenario, "loot") == 0)) {
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        for (int i = 0; i < n; i++) {
            uint8_t* e = EntityById(world, squad[i]);
            int hp = *reinterpret_cast<int*>(e + kActorHp), max_hp = *reinterpret_cast<int*>(e + 0xca6);
            if (hp > 0 && hp < max_hp) *reinterpret_cast<int*>(e + kActorHp) = max_hp;
        }
    }
    if (g_dump_actors < 0) g_dump_actors = GetPrivateProfileIntA("test", "entity_log", 0, ".\\ftcoop.ini");
    if (g_dump_actors && g_snap_tag == 1 && rel_s % 10 == 0) DumpActors(world, frame);
    if (g_snap_tag > 0 && rel_s > 0 && rel_s % 50 == 0 && rel_s != g_bot_last_snap) {  // co-op sessions only
        g_bot_last_snap = rel_s;
        SnapshotAll(world, frame);
    }
    bool host_acts = _stricmp(g_test.actor, "host") == 0;
    // [test] actor: host, join (the first joiner only) or all (every joiner: the race for one merchant).
    bool i_act = _stricmp(g_test.actor, "all") == 0 ? g_coop_join
                                                     : host_acts == g_coop_host && (g_coop_host || g_test.index == 1);
    const char* s = g_test.scenario;
    if (_stricmp(s, "survey") == 0) {
        // Exploration: who stands next to the first squad members (a new save: which NPCs to talk to).
        if (rel >= 30 && Once(0) && g_coop_host) {
            uint16_t squad[16];
            int n = GetSquad(world, squad, 16);
            for (int m = 0; m < n && m < 2; m++) {
                float* a = EntityPos(world, squad[m]);
                struct Near { float d; int id; } best[4] = {{1e9f, 0}, {1e9f, 0}, {1e9f, 0}, {1e9f, 0}};
                for (int i = 1; i < EntityCount(world); i++) {
                    bool ours = false;
                    for (int k = 0; k < n; k++) ours |= squad[k] == i;
                    if (!IsActor(world, i) || ours) continue;
                    float* b = EntityPos(world, static_cast<uint16_t>(i));
                    float d = (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]);
                    for (int k = 0; k < 4; k++)
                        if (d < best[k].d) {
                            for (int j = 3; j > k; j--) best[j] = best[j - 1];
                            best[k] = {d, i};
                            break;
                        }
                }
                Log("bot: SURVEY member %d (%u) at (%.2f,%.2f,%.2f)", m, squad[m], a[0], a[1], a[2]);
                for (const Near& nb : best) {
                    if (!nb.id) continue;
                    uint8_t* e = EntityById(world, static_cast<uint16_t>(nb.id));
                    char name[80] = "";
                    __try {
                        const wchar_t* nm = *reinterpret_cast<wchar_t**>(e + 0x199);
                        for (int c = 0; nm && nm[c] && c < 79; c++) name[c] = nm[c] < 0x80 ? static_cast<char>(nm[c]) : '?';
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                    bool qm = reinterpret_cast<bool(__thiscall*)(void*)>((*reinterpret_cast<void***>(e))[0x3f4 / 4])(e);
                    bool rm = reinterpret_cast<bool(__thiscall*)(void*)>((*reinterpret_cast<void***>(e))[0x3f0 / 4])(e);
                    Log("bot: SURVEY   NPC %d %s \"%s\" at distance %.2f%s%s", nb.id, RttiName(e), name, sqrtf(nb.d),
                        qm ? " quartermaster" : "", rm ? " recruiter" : "");
                }
            }
        }
        if (rel >= 35 && Once(2) && g_coop_host) {  // where a character keeps its numbers
            static const struct { uint32_t vt; const char* name; } kinds[] = {
                {0x809434, "Stats"}, {0x824bc4, "Skills"}, {0x824c04, "Perks"}, {0x824be4, "Traits"},
                {0x824bf4, "OTraits"}, {0x809594, "Status"}, {0x824c24, "ActorStatus"}};
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 0) {
                uint8_t* e = EntityById(world, squad[0]);
                char line[600] = "";
                int n = 0;
                for (int off = 0; off < 0x3000 && n < 560; off++)
                    for (const auto& k : kinds)
                        if (*reinterpret_cast<uint32_t*>(e + off) == k.vt) n += sprintf_s(line + n, sizeof(line) - n, " %s@+%x", k.name, off);
                Log("bot: SURVEY member %u's stat objects:%s", squad[0], line[0] ? line : " none inside");
            }
        }
        if (rel >= 40 && Once(1) && g_coop_host) {  // the mission screen's windows: which button is "Level"
            uint8_t* gm = *reinterpret_cast<uint8_t**>(kGameMainPtr);
            if (gm) DumpWindowTree(gm, 0);
        }
        if (rel >= 60) BotFinish();
    } else if (_stricmp(s, "move") == 0) {
        if (rel >= 30 && Once(0) && i_act) BotMoveMember(client, world, 0, 0);
        if (rel >= 410) BotFinish();
    } else if (_stricmp(s, "savemenu") == 0) {
        // Both copies order a member (they walk), then the host sits in the Escape menu and its save screen
        // (BotSaveMenuTick) while the joiner keeps clicking orders for its member, on wall-clock time from its pump.
        if (rel >= 30 && Once(0)) BotMoveMember(client, world, g_coop_host ? 0 : g_test.index, 0);
        if (rel >= 40 && g_coop_host && Once(1)) g_bot_menu_request = true;
        if (rel >= 40 && g_coop_join && Once(1)) g_bot_clicks_from = GetTickCount();
        if (rel >= 450 && (g_coop_join || g_bot_menu_done)) BotFinish();
    } else if (_stricmp(s, "ping") == 0) {
        if (BotMissionPings(rel)) BotFinish();  // dots.inc
    } else if (_stricmp(s, "simul") == 0) {
        // Same frame: the host orders member 0, the joiner orders member 1.
        if (rel >= 30 && Once(0)) BotMoveMember(client, world, g_coop_host ? 0 : g_test.index, 0);
        if (rel >= 410) BotFinish();
    } else if (_stricmp(s, "attack") == 0) {
        if (rel >= 30 && Once(0) && i_act) BotAttack(client, world);
        if (rel >= 200 && Once(1) && i_act) BotAttack(client, world);
        if (rel >= 510) BotFinish();
    } else if (_stricmp(s, "soak") == 0) {
        // Every 120 frames one side orders a squad member to a varying target, alternating host and joiner.
        if (rel >= 30 && rel < g_test.soak_frames && (rel - 30) % 120 == 0) {
            int round = (rel - 30) / 120;
            bool hosts_turn = (round % 2) == 0;
            if (hosts_turn == g_coop_host) BotMoveMember(client, world, round % 3, 1 + round * 7);
        }
        if (rel >= g_test.soak_frames + 20) BotFinish();
    } else if (_stricmp(s, "resync") == 0) {
        // Mid-mission re-sync (the mechanism behind map changes and mid-match join): the host quick-saves and
        // quick-loads, the joiner follows from its mission screen; both keep going in session 2.
        if (session == 1 && rel_s >= 60 && Once(0) && g_coop_host) RequestResync("bot: mid-mission re-sync");
        if (session >= 2 && rel_s >= 300) BotFinish();
    } else if (_stricmp(s, "invscreen") == 0) {
        // Does a full-screen window on one copy (the inventory: the world runs on under it) change what NPCs do there?
        // The host opens its inventory (the game bar's INV button, 0x47fab0) at 30 frames for [test] hold wall-clock
        // seconds (0: never opened, the control run); both play on to 900 frames and compare.
        if (rel_s >= 30 && Once(0) && g_coop_host && g_test.hold > 0) g_bot_inv_request = true;
        if (rel_s >= 900) BotFinish();
    } else if (_stricmp(s, "joinload") == 0) {
        // A joiner's own load is refused (only the host loads): the joiner presses quick-load mid-mission, as its F9 key
        // would; both copies must play on in the same session, in step.
        if (session == 1 && rel_s >= 60 && Once(0) && g_coop_join) {
            uint8_t* gm = *reinterpret_cast<uint8_t**>(kGameMainPtr);
            Log("bot: the joiner presses quick-load");
            if (gm) reinterpret_cast<void(__fastcall*)(void*)>(kQuickLoad)(gm);
        }
        if (rel_s >= 300) BotFinish();
    } else if (_stricmp(s, "drift") == 0) {
        // The safety net: the joiner moves an NPC on its copy only (a desync on purpose); the host must notice from
        // the joiner's position hashes, re-sync, and both play on in step in session 2.
        if (session == 1 && rel_s >= 100 && Once(0) && g_coop_join) {
            uint16_t squad[16];
            int n = GetSquad(world, squad, 16);
            for (int i = 1; i < EntityCount(world); i++) {
                bool ours = false;
                for (int k = 0; k < n; k++) ours |= squad[k] == i;
                if (!IsActor(world, i) || ours) continue;
                float* p = EntityPos(world, static_cast<uint16_t>(i));
                p[0] += 3.0f;
                Log("bot: DRIFT moved NPC %d on this copy only, at frame %d", i, frame);
                break;
            }
        }
        if ((session >= 2 && rel_s >= 200) || (session == 1 && rel_s >= 500)) BotFinish();  // corrected: no reload
    } else if (_stricmp(s, "driftequip") == 0) {
        // The same with equipment: the joiner empties a squad member's hand and swaps its active hand on its copy
        // only; the host must see the inventories differ and correct the hands and armour ('G'), with no reload.
        if (session == 1 && rel_s >= 100 && Once(0) && g_coop_join) {
            uint16_t squad[16];
            int n = GetSquad(world, squad, 16);
            for (int k = 0; k < n; k++) {
                uint8_t* e = EntityById(world, squad[k]);
                for (int slot = 0; slot < 3; slot++) {
                    uint32_t* h = reinterpret_cast<uint32_t*>(e + kActorEquip + 4 * slot);
                    if (!(*h >> 16)) continue;
                    Log("bot: DRIFT emptied slot %d (item %u) of %u and swapped its hand on this copy only, at frame %d",
                        slot, *h >> 16, squad[k], frame);
                    *h = 0;
                    e[kActorHand] ^= 1;
                    k = n;
                    break;
                }
            }
        }
        if ((session >= 2 && rel_s >= 200) || (session == 1 && rel_s >= 500)) BotFinish();
    } else if (_stricmp(s, "transition") == 0 || _stricmp(s, "encounter") == 0 || _stricmp(s, "worldmap") == 0) {
        if (g_snap_tag == 1 && rel_s >= 60 && Once(0)) BotEndMission("World Map");
        if (g_snap_tag >= 2 && rel_s >= 300) BotFinish();
    } else if (_stricmp(s, "dropin") == 0) {
        // Drop-in: the host plays alone (generation 1) until a joiner arrives through the Multiplayer menu; the
        // host re-syncs and both play generation 2. There the host moves member 0 and the joiner member 1.
        if (g_snap_tag >= 2 && rel_s >= 30 && Once(0) && g_coop_host) BotMoveMember(client, world, 0, 0);
        if (g_snap_tag >= 2 && rel_s >= 60 && Once(1) && g_coop_join) BotMoveMember(client, world, 1, 0);
        if (g_snap_tag >= 2 && rel_s >= max(400, g_test.hold)) BotFinish();  // [test] hold: stay longer
    } else if (_stricmp(s, "talk") == 0) {
        // Bunker save: [test] actor starts the conversation the owner had with the recruiting NPC (the same command,
        // byte for byte). Its copy keeps the conversation, the other copy must close it. While it is open the actor's
        // copy changes one HP locally, as a trade would change an inventory; then it ends the conversation. The
        // other copy must end up with that change: session 2 starts from the actor's game.
        for (int leg = 0; leg < kOwnerRouteLegs; leg++)
            if (g_snap_tag == 1 && rel_s >= 30 + 80 * leg && Once(5 + leg) && i_act) BotRouteLeg(client, world, leg);
        const int talk_at = 30 + 80 * kOwnerRouteLegs + 40;
        if (g_snap_tag == 1 && rel_s >= talk_at && Once(0) && i_act) {
            float* a = EntityPos(world, g_bot_talker);
            Log("bot: route done, member %u at (%.3f,%.3f,%.3f)", g_bot_talker, a[0], a[1], a[2]);
            BotTalk(client, world, 0, frame);
        }
        if (g_snap_tag == 1 && rel_s >= talk_at + 500 && !g_bot_done_step[1] && i_act) {
            Log("bot: FAIL no conversation opened 500 frames after the talk command");
            BotFinish();
        }
        // While the conversation is open the talker's window tick drives the bot (BotSpeechTick). The changes reach
        // the others as a delta at the conversation's end, no reload: both copies play on in session 1.
        BotSoloAfterClose(client, world, rel_s);
        if (g_snap_tag == 1 && rel_s >= talk_at + 450) BotFinish();
    } else if (IsRecruitScenario()) {
        // Bunker save: [test] actor's member 0 walks the owner's route to the recruiting NPC, talks, presses Recruit
        // (BotRecruitGameMainTick). First run: exploration of the recruit screen's windows and buttons.
        for (int leg = 0; leg < kOwnerRouteLegs; leg++)
            if (g_snap_tag == 1 && rel_s >= 30 + 80 * leg && Once(5 + leg) && i_act) BotRouteLeg(client, world, leg);
        const int talk_at = 30 + 80 * kOwnerRouteLegs + 40;
        if (g_snap_tag == 1 && rel_s >= talk_at && Once(0) && i_act) BotTalk(client, world, 0, frame);
        if (g_snap_tag == 1 && rel_s >= talk_at + 500 && !g_bot_done_step[1] && i_act) {
            Log("bot: FAIL no conversation opened 500 frames after the talk command");
            BotFinish();
        }
        if ((g_snap_tag >= 2 && rel_s >= 200) || (g_snap_tag == 1 && rel_s >= talk_at + 450)) BotFinish();  // no reload: session 1 goes on
    } else if (_stricmp(s, "exit") == 0) {
        // Bunker save: the whole squad walks onto the bunker's exit grid, where the owner left it in runtime run 3
        // (squad moves to (369.3, 32, 345.5) and (370.0, 32, 343.2), then the world map). The mission's own exit
        // trigger ends it. On the world map the joiner tries to choose a location (refused: only the host chooses),
        // then the host chooses; everyone plays the new mission (WindowTickHook).
        static const float legs[2][3] = {{369.3f, 32.0f, 345.5f}, {370.0f, 32.0f, 343.2f}};
        for (int leg = 0; leg < 2; leg++)
            if (g_snap_tag == 1 && rel_s >= 30 + 120 * leg && Once(5 + leg) && i_act) BotMoveSquad(client, world, legs[leg]);
        if (g_snap_tag == 1 && rel_s >= 900 && Once(0)) Log("bot: FAIL the squad is not off the map 900 frames in");
        if (g_snap_tag >= 2 && rel_s >= 300) BotFinish();
    } else if (_stricmp(s, "loot") == 0) {
        // Containers.sav, god mode: (1) the joiner's member 1 searches the nearest container and takes everything,
        // (2) the host's member 0 searches another and takes one stack. Each
        // search as the UI sends it (0x04, param 6); the loot screen's buttons do the taking (BotLootTake), then the
        // screen closes as a player closes it. Owner, two PCs: the games split after the other player looted.
        extern int g_loot_mode;
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        // Containers 291 and 411 open a loot screen in this save (570 does not: locked, or not a loot container).
        static const struct { int at; bool host; int member; int mode; uint16_t box; } steps[] = {
            {40, false, 1, 1, 291}, {350, true, 0, 2, 411}};
        for (int i = 0; i < 2; i++) {
            const auto& st = steps[i];
            if (rel_s < st.at || !Once(10 + i) || st.host != g_coop_host || (!g_coop_host && g_test.index != 1) || n <= st.member) continue;
            uint16_t a = squad[st.member];
            float* p = EntityPos(world, a);
            int best = st.box;
            float* q = EntityPos(world, st.box);
            float best_d = sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[2] - p[2]) * (q[2] - p[2]));
            g_loot_mode = st.mode;
            for (int k = 0; k < n; k++) {  // only the searcher selected on this copy, as a player clicking with it
                uint8_t* e = EntityById(world, squad[k]);
                if (k == st.member) e[0xb2] |= 0x40; else e[0xb2] &= ~0x40;
            }
            Log("bot: LOOT step %d: member %u searches container %d (distance %.1f), then %s", i + 1, a, best, best_d,
                st.mode == 1 ? "takes all" : "takes one stack");
            SendCommand2(client, 0x04, a, FullId(world, static_cast<uint16_t>(best)), 0, 6);
        }
        if (rel_s >= 750) BotFinish();
    } else if (_stricmp(s, "lootbody") == 0) {
        // Exploration (owner, two PCs: inventories split after the other player looted a corpse and pressed Done):
        // the host's squad kills the nearest enemy if no body lies near; the joiner's member 1 uses the nearest body
        // (command 0x04, as for a container). Which screens open where is logged (ui: window ... first ticks).
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        auto nearest_body = [&](uint16_t from) {
            float* a = EntityPos(world, from);
            int best = -1;
            float best_d = 1e9f;
            for (int i = 1; i < EntityCount(world); i++) {
                if (!IsActor(world, i) || InList(squad, n, i) || ActorHp(world, static_cast<uint16_t>(i)) > 0) continue;
                float* p = EntityPos(world, static_cast<uint16_t>(i));
                float d = sqrtf((p[0] - a[0]) * (p[0] - a[0]) + (p[2] - a[2]) * (p[2] - a[2]));
                if (d < best_d) { best_d = d; best = i; }
            }
            return best;
        };
        static int body = -1;
        if (g_coop_host && n > 0 && rel_s >= 30 && rel_s % 120 == 30 && nearest_body(squad[0]) < 0 && rel_s < 1500)
            BotAttack(client, world);
        if (g_coop_join && g_test.index == 1 && n > 1 && body < 0 && rel_s >= 60 && rel_s % 60 == 0) {
            body = nearest_body(squad[1]);
            if (body >= 0) {
                Log("bot: LOOTBODY member %u uses body %d (%s)", squad[1], body, RttiName(EntityById(world, static_cast<uint16_t>(body))));
                SendCommand2(client, 0x04, squad[1], FullId(world, static_cast<uint16_t>(body)), 0, 0);
            }
        }
        if (g_test.hold > 0 && rel_s >= g_test.hold) BotFinish();
    } else if (_stricmp(s, "talk2") == 0 || _stricmp(s, "talk2same") == 0) {
        // Bunker save: two conversations at once with two NPCs (owner, two PCs, 2026-10-27: the host talked to one NPC
        // while the other player talked to another, and no NPC was ever freed again; the host's re-sync waits for
        // free NPCs, so the games stayed apart). The host's member 0 walks the owner's route to the recruiter, the
        // joiner's member 1 walks to the merchant; both talk on the same frame, each changes its game while its
        // conversation is up (BotSpeechTick), then closes it.
        const int talk_at = 30 + 80 * kOwnerRouteLegs + 40;
        // talk2same: both players send the same character (the host's talk reaches it first; the joiner's is lost).
        bool same = _stricmp(s, "talk2same") == 0;
        if (g_coop_host) {
            for (int leg = 0; leg < kOwnerRouteLegs; leg++)
                if (g_snap_tag == 1 && rel_s >= 30 + 80 * leg && Once(5 + leg)) BotRouteLeg(client, world, leg);
            if (g_snap_tag == 1 && rel_s >= talk_at && Once(0)) BotTalk(client, world, 0, frame);
        } else if (g_test.index == 1) {
            for (int leg = 0; leg < 2; leg++)
                if (g_snap_tag == 1 && rel_s >= 30 + 80 * leg && Once(5 + leg)) {
                    BotRouteLeg(client, world, leg);  // leg 0 picks member 0
                    uint16_t squad[16];
                    if (!same && leg == 0 && GetSquad(world, squad, 16) > 1) {
                        g_bot_talker = squad[1];
                        SendCommand(client, 0, g_bot_talker, 0, kOwnerRoute[0], 4);
                    }
                }
            if (!same && g_snap_tag == 1 && rel_s >= 190 && Once(7)) SendCommand(client, 0, g_bot_talker, 0, kMerchantApproach, 4);
            if (g_snap_tag == 1 && rel_s >= talk_at && Once(0)) BotTalk(client, world, kMerchant, frame);
        }
        if (rel_s >= talk_at + 700) BotFinish();
    } else if (_stricmp(s, "browse") == 0) {
        // Bunker save: [test] actor walks to the merchant, talks, opens barter, buys nothing, leaves. No reload: only
        // the entity bookkeeping (the shop's stock came and went) goes to the other copy as co-op commands. Meanwhile
        // the other player tries to talk to the same merchant and must be refused (the NPC lock).
        for (int leg = 0; leg < 2; leg++)
            if (g_snap_tag == 1 && rel_s >= 30 + 80 * leg && Once(5 + leg) && i_act) BotRouteLeg(client, world, leg);
        if (g_snap_tag == 1 && rel_s >= 190 && Once(7) && i_act) SendCommand(client, 0, g_bot_talker, 0, kMerchantApproach, 4);
        const int talk_at = 310;
        if (g_snap_tag == 1 && rel_s >= talk_at && Once(0) && i_act) {
            SetWriteWatch(reinterpret_cast<uintptr_t>(world + 0x74f), true);  // what the conversation and barter allocate
            BotTalk(client, world, kMerchant, frame);
        }
        if (g_snap_tag == 1 && rel_s >= talk_at + 8 && Once(2) && !i_act) {
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 1) {
                g_bot_talker = squad[1];
                Log("bot: the other player tries to talk to the merchant too");
                BotTalk(client, world, kMerchant, frame);
            }
        }
        if (rel_s >= talk_at + 500) BotFinish();
    } else if (_stricmp(s, "levelup") == 0) {
        // QuestBarter.sav: a squad member can level up; the bar shows a "Level" effect button (WEffectButton, handler
        // 0x479e60 __fastcall(WGameMain*, edx, button): by the button's effect name, "objectiveComplete" opens the Pipboy
        // (mode 11), "sneak" a command, anything else SetMode(3)). [test] actor presses it as a click does.
        bool race = _stricmp(g_test.actor, "all") == 0;  // levelrace: both press on the same frame
        if (rel_s >= 30 && Once(0) && (i_act || race)) {
            uint8_t* gm = *reinterpret_cast<uint8_t**>(kGameMainPtr);
            uint8_t* level = nullptr;
            for (uint8_t* c = gm ? *reinterpret_cast<uint8_t**>(gm + 0x18) : nullptr; c; c = *reinterpret_cast<uint8_t**>(c + 0x24))
                if (strstr(RttiName(c), "WEffectButton") && *reinterpret_cast<int*>(c + 0xc3) >= 0) level = c;
            if (!level) {
                Log("bot: FAIL no visible effect button (the Level button)");
            } else {
                // The handler takes the effect's data (arg +4 -> an object whose +0x2c is its name), not the button;
                // for a level-up it ends in 0x471aa0(3) on the mission screen, called here directly.
                // SetMode runs from the mission screen's own tick (called from the frame step it never returned).
                Log("bot: LEVEL pressing the Level button %p (0x471aa0(3), from the mission screen's tick) at frame %d", level, frame);
                g_bot_level_request = true;
            }
        }
        if (rel_s >= 300) BotFinish();
    } else if (_stricmp(s, "quest") == 0) {
        // QuestBarter.sav: [test] actor's member 0 stands next to General Barnaky (NPC 266, the bunker's quest giver)
        // and talks to him; the conversation's changes (the game's own, and a quest variable and an HP the bot
        // changes while it is open) reach the others as a delta.
        if (rel_s >= 30 && Once(0) && i_act) {
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 0) g_bot_talker = squad[0];
            BotTalk(client, world, kQuestGiver, frame);
        }
        if (rel_s >= 530 && !g_bot_done_step[1] && i_act && Once(1)) Log("bot: FAIL no conversation opened 500 frames after the talk command");
        if (rel_s >= 480) BotFinish();
    } else if (IsShopTalk()) {
        // QuestBarter.sav: one player shops while another talks. The host's member 1 trades with the quartermaster
        // (NPC 96, no pause); while that shop is open the joiner's member 0 talks to General Barnaky (NPC 266), which
        // holds everyone, the shopper's world too. Both finish; everything must match, with no reload.
        if (rel_s >= 30 && Once(0) && g_coop_host) {
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 1) g_bot_talker = squad[1];
            BotTalk(client, world, kMerchant, frame);
        }
        // The joiner tries the quest giver while the host shops (refused: a conversation would hold the shopper too),
        // then again once the shop has closed.
        uint16_t shop_buyer = 0;
        static bool tried = false;
        if (g_coop_join && g_test.hold != 1 && ShopOpenForOther(&shop_buyer) && Once(1)) {  // [test] hold=1: no talk
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 0) g_bot_talker = squad[0];
            Log("bot: the joiner talks to the quest giver while the host shops");
            BotTalk(client, world, kQuestGiver, frame);
            tried = true;
        }
        if (g_coop_join && tried && !ShopOpenForOther(&shop_buyer) && Once(2)) {
            Log("bot: the shop is done: the joiner talks to the quest giver again");
            BotTalk(client, world, kQuestGiver, frame);
        }
        if (rel_s >= 700) BotFinish();
    } else if (IsTradeScenario()) {
        // Bunker save. Both players move squad members around first (the other side member 1, the actor member 2),
        // then [test] actor's member 0 walks to the merchant, talks, barters (BotTradeGameMainTick) and trades.
        // Session 1 must match on both copies until the talk; session 2 (from the trader's game) everywhere.
        if (g_snap_tag == 1 && rel_s >= 30 && Once(2) && !i_act) BotMoveMember(client, world, 1, 0);
        if (g_snap_tag == 1 && rel_s >= 60 && Once(3) && i_act) BotMoveMember(client, world, 2, 0);
        if (g_test.inv_chaos && !i_act && rel_s >= 100) BotInventoryChaos(client, world, rel_s);
        for (int leg = 0; leg < 2; leg++)
            if (g_snap_tag == 1 && rel_s >= 30 + 80 * leg && Once(5 + leg) && i_act) BotRouteLeg(client, world, leg);
        if (g_snap_tag == 1 && rel_s >= 190 && Once(7) && i_act) {
            Log("bot: ROUTE to the merchant: member %u to (%.3f,%.3f,%.3f)", g_bot_talker, kMerchantApproach[0],
                kMerchantApproach[1], kMerchantApproach[2]);
            SendCommand(client, 0, g_bot_talker, 0, kMerchantApproach, 4);
        }
        const int talk_at = 310;
        if (g_snap_tag == 1 && rel_s >= talk_at && Once(0) && i_act) {
            float* a = EntityPos(world, g_bot_talker);
            float* b = EntityPos(world, kMerchant);
            Log("bot: at the merchant: member %u at (%.3f,%.3f,%.3f), merchant at (%.3f,%.3f,%.3f)", g_bot_talker,
                a[0], a[1], a[2], b[0], b[1], b[2]);
            BotTalk(client, world, kMerchant, frame);
        }
        if (g_snap_tag == 1 && rel_s >= talk_at + 500 && !g_bot_done_step[1] && i_act) {
            Log("bot: FAIL no conversation opened 500 frames after the talk command");
            BotFinish();
        }
        // While someone else's shop is open (shop_no_pause): try to order the trading character and to talk to the
        // recruiter; both must be refused on this copy.
        if (_stricmp(s, "trade2") == 0 && g_snap_tag == 1 && rel_s >= talk_at + 300 && Once(11) && !i_act) {
            uint16_t squad[16];
            if (GetSquad(world, squad, 16) > 0) g_bot_talker = squad[0];  // the member the first trade left at the merchant
            Log("bot: the second player trades with the merchant too");
            BotTalk(client, world, kMerchant, frame);
        }
        uint16_t shop_buyer = 0;
        if (ShopOpenForOther(&shop_buyer) && Once(9) && _stricmp(s, "trade2") != 0) {
            uint16_t squad[16];
            int n = GetSquad(world, squad, 16);
            for (int i = 0; i < n; i++)
                if (squad[i] == shop_buyer) {
                    Log("bot: tries to move the trading squad member %u", squad[i]);
                    BotMoveMember(client, world, i, 3);
                }
            Log("bot: tries to talk to the recruiter while the shop is open");
            uint16_t keep = g_bot_talker;
            g_bot_talker = n > 1 ? squad[n - 1] : squad[0];
            BotTalk(client, world, 0, frame);
            g_bot_talker = keep;
        }
        // shopresync: a re-sync while the other player's shop is open (the owner's night: the shop never closed, and
        // in every later session the merchant and the shopper's character stayed refused). The host forces it 3 s after
        // the shop opened; in the reloaded session the host moves the shopper's character, then talks to the merchant.
        bool shopresync = _stricmp(s, "shopresync") == 0;
        static uint16_t resync_buyer = 0;
        static DWORD shop_seen = 0;
        if (shopresync && g_coop_host && g_snap_tag == 1 && ShopOpenForOther(&shop_buyer)) {
            if (!shop_seen) shop_seen = GetTickCount();
            if (GetTickCount() - shop_seen > 3000 && Once(12)) {
                resync_buyer = shop_buyer;
                Log("bot: re-sync while the shop of squad member %u is open", shop_buyer);
                RequestResync("bot: re-sync while a shop is open");
            }
        }
        if (shopresync && g_coop_host && g_snap_tag >= 2 && resync_buyer) {
            uint16_t squad[16];
            int n = GetSquad(world, squad, 16);
            if (rel_s >= 60 && Once(13))
                for (int i = 0; i < n; i++)
                    if (squad[i] == resync_buyer) {
                        Log("bot: after the re-sync, moves the squad member %u that was trading", resync_buyer);
                        BotMoveMember(client, world, i, 3);
                    }
            if (rel_s >= 120 && Once(14)) {
                g_bot_talker = resync_buyer;
                Log("bot: after the re-sync, talks to the merchant");
                BotTalk(client, world, kMerchant, frame);
            }
        }
        BotSoloAfterClose(client, world, rel_s);
        int end_at = _stricmp(s, "trade2") == 0 ? talk_at + 750 : shopresync ? talk_at + 1500 : talk_at + 450;
        if ((g_snap_tag >= 2 && rel_s >= 300) || (g_snap_tag == 1 && rel_s >= end_at)) BotFinish();  // no reload: session 1 goes on
    } else if (_stricmp(s, "turnbased") == 0 || _stricmp(s, "tbauto") == 0) {
        static int done_at = -1;
        if (g_tb_done && done_at < 0) done_at = rel_s;
        if ((done_at >= 0 && rel_s >= done_at + 100) || (g_test.hold > 0 && rel_s >= g_test.hold)) BotFinish();
    } else if (_stricmp(s, "chaos") == 0) {
        BotChaos(client, world, rel_s);
        if (g_test.hold > 0 && rel_s >= g_test.hold) BotFinish();
    } else if (_stricmp(s, "watch") == 0 || _stricmp(s, "play") == 0) {
        // A person plays; the bot only logs (snapshots, RNG, HP changes) so copies can be compared afterwards.
        // play: unlike watch, the copies load [test] save themselves and the host waits for [test] joiners.
        // [test] hold: frames to play before both copies finish (0: until the runner's timeout).
        if (g_test.hold > 0 && rel_s >= g_test.hold) BotFinish();
    } else {
        Log("bot: FAIL unknown scenario \"%s\"", s);
        BotFinish();
    }
}

#include "private_sync.inc"

void PrivateCloseStep() {  // frame hook, and the mission screen's tick (the world may be held: no frames)
    bool alone = false;
    if (!PrivateScreensClosed(&alone)) return;
    if (alone) {
        // A host on its own sends nothing, but its world was held for the screens like anyone's: it stayed held, and
        // nobody could move after a conversation or a shop (the owner, alone in the bunker, 0.10.4).
        LevelScreenClosed();
        ReleaseTalkerHold();
        extern bool g_suppress_pause;
        g_suppress_pause = false;
        return;
    }
    if (g_test.enabled && (IsTradeScenario() || IsRecruitScenario() || IsShopTalk())) {
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);  // what the others will get
        LogInv(world, "SYNCED", g_bot_talker);
        LogInv(world, "SYNCED", kMerchant);
        LogSquad(world, "SYNCED");
    }
    int diff = PrivateScreensDiff();
    LevelScreenClosed();  // a character screen's lock goes once its changes are out
    ReleaseTalkerHold();  // the delta is out: this world goes on (a re-sync needs frames too)
    extern bool g_suppress_pause;
    extern int g_suppressed;
    if (g_suppress_pause) {
        g_suppress_pause = false;
        Log("co-op: the light conversation is over: pause changes go through again (balance %d%s)", g_suppressed,
            g_suppressed ? ", NOT ZERO" : "");
    }
    if (diff != kDiffFull) {
        // nothing to reload: the co-op commands carried what changed
    } else if (g_coop_host) {
        RequestResync("the host's private screens closed");
        Notice("Co-op: the host traded or changed the squad - saving and reloading for everyone");
    } else if (g_coop_join) {
        g_push_state = 1;
        Notice("Co-op: sending your trades and squad changes to the host");
    }
}

void DotsFrame();  // dots.inc
void DriftFrame(uint8_t* world);  // drift.inc
void DriftCheckHost();

// World+0x2ef counts the world's update calls (0x4fbf50 adds 1 after its fixed-step loop, once per redraw), not its
// frames: a slower copy runs several frames per call, so the copies' counters part (3-4 apart within a minute,
// 2026-10-23 actor dumps: every actor's +0x13c, stamped from it by the entity "moved" update 0x5e00f0). About 40
// places in the simulation read it (cached results "fresh this update"), and NPCs then fired at someone on one copy
// only (chaos runs: same random draws up to that shot, three times). So at the start of every frame it is set from
// the frame number, the same everywhere; x 256 keeps it rising across the update calls between two frames.
void StampUpdateCounter() {
    uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (w) *reinterpret_cast<int*>(w + 0x2ef) = *reinterpret_cast<int*>(w + kWorldFrame) * 256;
}

// Exploration: the world tick 0x4fc230 (__thiscall(World*)), called from the fixed-step loop at 0x4fc0e2; while it
// runs, the code is the lockstep frame's. Random draws outside it (drift.inc) are another path's.
const uintptr_t kWorldTickCall = 0x4fc0e2;
const uintptr_t kWorldTick = 0x4fc230;
bool g_in_world_tick = false;
void __fastcall WorldTickHook(uint8_t* world) {
    g_in_world_tick = true;
    reinterpret_cast<void(__thiscall*)(void*)>(kWorldTick)(world);
    g_in_world_tick = false;
}

uint32_t __fastcall FrameTickHook(uint8_t* client) {
    g_client_ptr = client;
    __try {
        if (g_coop_host || g_coop_join) StampUpdateCounter();  // first, before the frame's world step
        {  // the x87 control word the frame runs under (precision, rounding): logged when it changes
            static unsigned last_cw = 0xffffffff;
            unsigned short cw;
            __asm fnstcw cw
            if (cw != last_cw) {
                uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr);
                Log("co-op: FPU control word %04x (was %04x) at frame %d", cw, last_cw & 0xffff,
                    w ? *reinterpret_cast<int*>(w + kWorldFrame) : -1);
                last_cw = cw;
            }
        }
        if (g_test.enabled && g_dump_actors > 0)
            if (uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr))  // exploration: the world clock and its step
                Log("TIME f=%d t=%08x step=%08x flags 7d=%02x 81=%02x 83=%02x 85=%02x 8b=%02x", *reinterpret_cast<int*>(w + kWorldFrame),
                    *reinterpret_cast<uint32_t*>(w + 0x2f7), *reinterpret_cast<uint32_t*>(w + 0x2c1),
                    *reinterpret_cast<uint8_t*>(0x8bdd7d), *reinterpret_cast<uint8_t*>(0x8bdd81),
                    *reinterpret_cast<uint8_t*>(0x8bdd83), *reinterpret_cast<uint8_t*>(0x8bdd85),
                    *reinterpret_cast<uint8_t*>(0x8bdd8b));
        ReapplyBookkeeping();
        RunDeferredCreates();  // shop_sync.inc
        RunHeldCommands();     // shop_sync.inc: item moves made while a shop was open
        DotsFrame();           // dots.inc
        if (uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr))
            if (g_coop_host || g_coop_join) {
                SettingsFrame(w);
                RunDuePrunes(w);
            }
        if (uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr)) {
            DriftFrame(w);
            if (g_coop_host) DriftCheckHost();
        }
        if (g_test.enabled) {  // exploration: a hash of every actor's position, every frame (+0x117 differs per copy)
            static int last = -1;
            uint8_t* w = *reinterpret_cast<uint8_t**>(kWorldPtr);
            int f = w ? *reinterpret_cast<int*>(w + kWorldFrame) : -1;
            if (w && f != last && f >= 2200 && f <= 2700) {
                uint32_t h = 2166136261u;
                for (int i = 1; i < EntityCount(w); i++) {
                    if (!IsActor(w, i)) continue;
                    h = Fnv(h, EntityPos(w, static_cast<uint16_t>(i)), 12);
                    h = Fnv(h, reinterpret_cast<uint8_t*>(&i), 4);
                }
                Log("POSH f=%d %08x", f, h);
            }
            last = f;
        }
        if (g_test.enabled) DebugDumpActors();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("co-op: exception 0x%08x re-applying delta bookkeeping", GetExceptionCode());
    }
    __try {
        if (g_coop_host) {
            if (const char* reason = NewJoinerNeedingResync()) {
                RequestResync(reason);
                Notice("Co-op: %s - saving and reloading to bring them in", reason);
            }
            NoticeLeftJoiners();
            if (InterlockedExchange(&g_upload_ready, 0)) {
                if (g_resync_state == 0) {
                    Notice("Co-op: %s traded or changed the squad - reloading everyone from their game", g_upload_from);
                    g_resync_reason = "a joiner's private screens closed";
                    g_resync_state = 5;
                } else {
                    Log("host: a joiner's save arrived during a re-sync: dropped");
                }
            }
        }
        PrivateCloseStep();
        JoinerPushStep();
        ShowQueuedNotices(client);
        HostResyncStep();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("host: exception 0x%08x in re-sync step", GetExceptionCode());
    }
    if (g_test.enabled) {
        __try {
            uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
            BotStep(client, world, *reinterpret_cast<int*>(world + kWorldFrame));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("bot: exception 0x%08x in frame step", GetExceptionCode());
        }
    }
    g_in_tick = true;
    uint32_t r = reinterpret_cast<uint32_t(__thiscall*)(uint8_t*)>(kClientTickProcessor)(client);
    g_in_tick = false;
    return r;
}
