// ---- The world map: the host travels, everyone else watches (included from netlog.cpp before save_transfer.inc) ----
// Owner, 2026-10-28: on the world map anyone could move, and nobody saw the host's party move. There is no lockstep
// session on the world map (the mission's ended), so the host's party goes out with the answer to the joiners'
// generation poll (every kJoinPollMs, save_transfer.inc), and a joiner's own clicks do nothing.
// WWorldWindow (the world map, inside WCampaignWindow): its per-frame update 0x6ba780 moves the party in real time
// towards the destination ww+0x10a/+0x10e (floats) while ww+0x101 (moving) is set, and writes the position to the
// campaign object (*0x8bdd2c) +0xa5/+0xa9 (floats); it rolls random encounters in 0x6bad60 (__thiscall(ww, float
// dt), ret 4). A click on open ground or a location calls 0x6ba410 (__thiscall(ww, float dest[2]), ret 4): the
// destination, moving = 1. A click while standing on a location enters it (0x6ba580 -> GoToMap 0x6b6580, refused on a
// joiner by GoToMapHook).
const uintptr_t kWorldMapTravel = 0x6ba410;
const uint8_t kWorldMapTravelPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x10};  // mov eax,[esp+4]; mov edx,[eax]
const uintptr_t kWorldMapEncounterRoll = 0x6bad60;
const uint8_t kWorldMapEncounterPrologue[] = {0x6a, 0xff, 0x68, 0xa2, 0x26, 0x7f, 0x00};  // push -1; push SEH handler
const uintptr_t kCampaignPtr2 = 0x8bdd2c;
const size_t kCampPartyX = 0xa5, kCampPartyY = 0xa9, kWwDestX = 0x10a, kWwDestY = 0x10e, kWwMoving = 0x101;

struct WorldMapState {
    float x, y, dx, dy;
    int moving;
};
SRWLOCK g_wm_lock = SRWLOCK_INIT;
WorldMapState g_wm_host = {};  // host: its party now; joiner: the host's, as last heard
DWORD g_wm_host_tick = 0;      // host: when its world map last ticked; joiner: when the host's last arrived
bool g_wm_applying = false;    // joiner: our own call of the travel setter, with the host's destination
uint8_t* g_travel_trampoline = nullptr;
uint8_t* g_roll_trampoline = nullptr;
uintptr_t g_ww_vtable = 0;
const char* RttiName(void* obj);  // bot_scenarios.inc
void Notice(const char* fmt, ...);  // save_transfer.inc

void __fastcall WorldMapTravelHook(uint8_t* ww, void* edx, float* dest) {
    if (g_coop_join && !g_wm_applying) {
        static DWORD said = 0;
        if (GetTickCount() - said > 3000) {
            said = GetTickCount();
            Log("co-op: refused this player's world map travel: only the host moves the party");
            Notice("Co-op: only the host moves the party on the world map");
        }
        return;
    }
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float*)>(g_travel_trampoline)(ww, edx, dest);
}

void __fastcall WorldMapEncounterHook(uint8_t* ww, void* edx, float dt) {
    if (g_coop_join) return;  // the host's copy rolls; the joiners follow it into the encounter's map
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float)>(g_roll_trampoline)(ww, edx, dt);
}

// Window-tick hook, every window: the world map's own tick ran just before.
void WorldMapTick(uint8_t* w) {
    if (!g_coop_host && !g_coop_join) return;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(w);
    if (!g_ww_vtable) {
        if (!strstr(RttiName(w), "WWorldWindow@@")) return;
        g_ww_vtable = vt;
    }
    if (vt != g_ww_vtable) return;
    uint8_t* camp = *reinterpret_cast<uint8_t**>(kCampaignPtr2);
    if (!camp) return;
    if (g_coop_host) {
        WorldMapState s = {*reinterpret_cast<float*>(camp + kCampPartyX), *reinterpret_cast<float*>(camp + kCampPartyY),
                           *reinterpret_cast<float*>(w + kWwDestX), *reinterpret_cast<float*>(w + kWwDestY), w[kWwMoving]};
        AcquireSRWLockExclusive(&g_wm_lock);
        g_wm_host = s;
        g_wm_host_tick = GetTickCount();
        ReleaseSRWLockExclusive(&g_wm_lock);
        return;
    }
    AcquireSRWLockShared(&g_wm_lock);
    WorldMapState s = g_wm_host;
    DWORD heard = g_wm_host_tick;
    ReleaseSRWLockShared(&g_wm_lock);
    static DWORD applied = 0;
    if (!heard || heard == applied) return;
    applied = heard;
    // The host's party where it is now, and on its way the same: this copy's own update moves it between the
    // host's reports (the same campaign speed and terrain), and every report puts it back where the host has it.
    *reinterpret_cast<float*>(camp + kCampPartyX) = s.x;
    *reinterpret_cast<float*>(camp + kCampPartyY) = s.y;
    bool same_way = w[kWwMoving] && *reinterpret_cast<float*>(w + kWwDestX) == s.dx &&
                    *reinterpret_cast<float*>(w + kWwDestY) == s.dy;
    if (s.moving && !same_way) {  // the game's own setter: destination, moving, the view kept on the party
        float dest[2] = {s.dx, s.dy};
        g_wm_applying = true;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float*)>(g_travel_trampoline)(w, nullptr, dest);
        g_wm_applying = false;
    } else if (!s.moving) {
        w[kWwMoving] = 0;
    }
    static int last_moving = -1;
    if (s.moving != last_moving) {
        last_moving = s.moving;
        Log("co-op: world map: the host's party %s (%.1f, %.1f)", s.moving ? "travels from" : "stopped at", s.x, s.y);
    }
}

// ---- Pings: "I want to go there" (owner, 2026-10-28) ----
// Any player marks a spot on the world map (a right click; a joiner's left click too, since it can't travel); every
// copy draws a marker there in that player's colour (the selection dots' colours: host red, then blue, yellow,
// green) for kPingMs. One ping per player: a new one replaces it. A joiner's goes to the host ("PING <id> x y", the
// host knows its colour); the host's GEN answer carries every live ping with its age, so all copies draw the same.
const DWORD kPingMs = 10000;
struct MapPing {
    float x, y;
    DWORD born;  // GetTickCount() here; 0: none
};
MapPing g_pings[4] = {};  // by colour, under g_wm_lock

void AddPing(int colour, float x, float y) {  // host
    if (colour < 0 || colour > 3) return;
    AcquireSRWLockExclusive(&g_wm_lock);
    g_pings[colour] = {x, y, GetTickCount() | 1};
    ReleaseSRWLockExclusive(&g_wm_lock);
    static const char* const names[4] = {"red", "blue", "yellow", "green"};
    Log("co-op: world map: %s pinged (%.1f, %.1f)", names[colour], x, y);
}

int LivePings(MapPing* out) {  // every copy: the pings to draw now, by colour (born 0 where none)
    AcquireSRWLockShared(&g_wm_lock);
    memcpy(out, g_pings, sizeof(g_pings));
    ReleaseSRWLockShared(&g_wm_lock);
    int n = 0;
    for (int k = 0; k < 4; k++) {
        if (out[k].born && GetTickCount() - out[k].born < kPingMs)
            n++;
        else
            out[k].born = 0;
    }
    return n;
}

void SendPingToHost(float x, float y);  // save_transfer.inc

// WWorldWindow mouse-down (vtable slot 51, 0x82dbd0 -> 0x6ba2a0): __thiscall(ww, int pt[2] window-local, int button,
// int), ret 0xc. Button 1 travels (or enters a location under the party); others did nothing. The world point is pt
// plus the view's scroll (campaign +0x9d/+0xa1), as the game computes a destination.
typedef char(__fastcall* MapMouseFn)(uint8_t*, void*, int*, int, int);
MapMouseFn g_map_mouse = nullptr;
const uintptr_t kMapMouseSlot = 0x82dbd0, kMapMouse = 0x6ba2a0;

char __fastcall MapMouseHook(uint8_t* ww, void* edx, int* pt, int button, int extra) {
    if (g_test.enabled && pt) Log("bot: world map mouse-down, button %d at (%d, %d)", button, pt[0], pt[1]);
    if ((g_coop_host || g_coop_join) && pt && (button == 2 || (button == 1 && g_coop_join))) {
        uint8_t* camp = *reinterpret_cast<uint8_t**>(kCampaignPtr2);
        if (camp) {
            float x = static_cast<float>(pt[0]) + *reinterpret_cast<float*>(camp + 0x9d);
            float y = static_cast<float>(pt[1]) + *reinterpret_cast<float*>(camp + 0xa1);
            if (g_coop_host) {
                AddPing(0, x, y);
            } else {
                Log("co-op: world map: this player pings (%.1f, %.1f)", x, y);
                SendPingToHost(x, y);
            }
            return 1;
        }
    }
    return g_map_mouse(ww, edx, pt, button, extra);
}

// Host, the file server's GEN answer: " WMAP x y dx dy moving" while its world map ticked in the last second, and
// " PINGS colour:x:y:age_ms ..." while any ping is live.
void WorldMapLine(char* out, size_t cap) {
    out[0] = 0;
    AcquireSRWLockShared(&g_wm_lock);
    WorldMapState s = g_wm_host;
    DWORD t = g_wm_host_tick;
    ReleaseSRWLockShared(&g_wm_lock);
    int n = 0;
    if (t && GetTickCount() - t < 1000)
        n = sprintf_s(out, cap, " WMAP %.4f %.4f %.4f %.4f %d", s.x, s.y, s.dx, s.dy, s.moving);
    MapPing p[4];
    if (!LivePings(p)) return;
    n += sprintf_s(out + n, cap - n, " PINGS");
    for (int k = 0; k < 4; k++)
        if (p[k].born)
            n += sprintf_s(out + n, cap - n, " %d:%.1f:%.1f:%lu", k, p[k].x, p[k].y, GetTickCount() - p[k].born);
}

void PingsFromHost(const char* line) {  // joiner, the poller thread
    const char* p = strstr(line, " PINGS");
    MapPing got[4] = {};
    if (p) {
        p += 6;
        int k, used;
        float x, y;
        unsigned long age;
        while (sscanf_s(p, " %d:%f:%f:%lu%n", &k, &x, &y, &age, &used) == 4) {
            p += used;
            if (k >= 0 && k < 4 && age < kPingMs) got[k] = {x, y, (GetTickCount() - age) | 1};
        }
    }
    AcquireSRWLockExclusive(&g_wm_lock);
    for (int k = 0; k < 4; k++) {
        if (got[k].born && (!g_pings[k].born || g_pings[k].x != got[k].x || g_pings[k].y != got[k].y)) {
            static const char* const names[4] = {"red", "blue", "yellow", "green"};
            Log("co-op: world map: %s pinged (%.1f, %.1f)", names[k], got[k].x, got[k].y);
        }
        if (!got[k].born || !g_pings[k].born || g_pings[k].x != got[k].x || g_pings[k].y != got[k].y)
            g_pings[k] = got[k];  // the same ping keeps the time it was first heard here
    }
    ReleaseSRWLockExclusive(&g_wm_lock);
}

// Joiner, the poller thread: the host's party, from that answer.
void WorldMapFromHost(const char* line) {
    PingsFromHost(line);
    const char* p = strstr(line, " WMAP ");
    if (!p) return;
    WorldMapState s = {};
    if (sscanf_s(p, " WMAP %f %f %f %f %d", &s.x, &s.y, &s.dx, &s.dy, &s.moving) != 5) return;
    AcquireSRWLockExclusive(&g_wm_lock);
    g_wm_host = s;
    g_wm_host_tick = GetTickCount() | 1;
    ReleaseSRWLockExclusive(&g_wm_lock);
}

template <typename Fn>
bool PatchSlot(uintptr_t slot, uintptr_t expect, void* hook, Fn* original);  // netlog.cpp

bool InstallWorldMapHooks() {
    if (!PatchSlot(kMapMouseSlot, kMapMouse, (void*)&MapMouseHook, &g_map_mouse)) return false;
    g_travel_trampoline = HookWithTrampoline(kWorldMapTravel, kWorldMapTravelPrologue, sizeof(kWorldMapTravelPrologue),
                                             (void*)&WorldMapTravelHook);
    g_roll_trampoline = HookWithTrampoline(kWorldMapEncounterRoll, kWorldMapEncounterPrologue,
                                           sizeof(kWorldMapEncounterPrologue), (void*)&WorldMapEncounterHook);
    return g_travel_trampoline && g_roll_trampoline;
}
