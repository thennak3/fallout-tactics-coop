// ---- Selection dots: a coloured dot above a squad member's name tab for every player who has them selected ----
// Included from netlog.cpp after bot_scenarios.inc. Not lockstep: each copy reads its own player's selection every
// frame; joiners send theirs to the host on the file port ("SEL", about 5 times a second) and get every player's
// back. Colours go by join order: the host red, then blue, yellow, green; a joiner that drops (no GEN poll for
// kJoinerTimeoutMs) frees its colour for the next one.
// The squad bar's portraits (WPortrait, vtable 0x806d44) draw in vtable slot 4 (0x4bc7d0, __thiscall(this, renderer),
// ret 4) and hold their character's EntityID at +0x153. The renderer's vt+0x24 draws a rectangle outline
// {x0, y0, x1, y1} (ints, the window's own coordinates) in an ARGB colour (the portrait's selection frame). A portrait
// draws only when invalidated: base slot 3 (0x6c7260, __thiscall(this)) invalidates the whole window. Slot 0
// (0x4bcdf0, ret 4) is the destructor. A character is selected while its +0xb2 has bit 0x40.
// The dots go above the name tabs, so they are drawn by the game view (vtable 0x804570, 800x472 above the bottom
// panel; slot 4 0x4a0100 draws it, same signature; slot 0 0x4a34b0 the destructor). The game redraws only the view's
// changed parts, so a change of dots repaints the whole view (window base slot 3). The bottom panel (vtable 0x80299c, a portrait's
// +0x10e) holds the six name tabs (vtable 0x80805c, 80x30, panel coordinates) at +0xfe..+0x112 and the six portraits
// in the same order at +0x147..+0x15b. Window rect {x0, y0, x1, y1} at +0xc3, relative to the parent.
const uintptr_t kPortraitDrawSlot = 0x806d44 + 4 * 4;
const uintptr_t kPortraitDraw = 0x4bc7d0;
const uintptr_t kPortraitDtorSlot = 0x806d44;
const uintptr_t kPortraitDtor = 0x4bcdf0;
const size_t kPortraitEntity = 0x153;
typedef void(__fastcall* PortraitDrawFn)(void*, void*, void*);
typedef void*(__fastcall* PortraitDtorFn)(void*, void*, unsigned);
const uintptr_t kViewDrawSlot = 0x804570 + 4 * 4;
const uintptr_t kViewDraw = 0x4a0100;
const uintptr_t kPanelVtable = 0x80299c;
const uintptr_t kTabVtable = 0x80805c;
PortraitDrawFn g_portrait_draw = nullptr;
PortraitDrawFn g_view_draw = nullptr;
PortraitDtorFn g_view_dtor = nullptr;
uint8_t* g_view = nullptr;  // main thread: the game view last seen drawing, until destroyed
const uintptr_t kViewDtorSlot = 0x804570;
const uintptr_t kViewDtor = 0x4a34b0;
PortraitDtorFn g_portrait_dtor = nullptr;

const uint32_t kDotColour[4] = {0xffe02828, 0xff3070ff, 0xffffd820, 0xff28d050};
const char* const kDotName[4] = {"red", "blue", "yellow", "green"};
const int kDotSize = 7;

struct PlayerSel {
    bool live;
    int n;
    uint16_t slots[16];
};
SRWLOCK g_dots_lock = SRWLOCK_INIT;
PlayerSel g_dots[4] = {};    // what is drawn, by colour
PlayerSel g_my_sel = {};     // this copy's player, read every frame
volatile LONG g_my_colour = -1;
volatile DWORD g_dots_heard = 0;  // joiner: the host's last answer
uint8_t* g_portraits[16] = {};    // main thread: portraits seen drawing, until destroyed
HANDLE g_shot_event = nullptr;    // test mode: capture the window now

void FillRect(uint8_t* renderer, int x0, int y0, int x1, int y1, uint32_t argb) {
    int rect[4] = {x0, y0, x1, y1};
    reinterpret_cast<void(__thiscall*)(void*, int*, uint32_t*, int, int)>(
        (*reinterpret_cast<void***>(renderer))[0x24 / 4])(renderer, rect, &argb, 0, 0);
}

int FormatSlots(char* out, size_t cap, const PlayerSel& s) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < s.n; i++) n += sprintf_s(out + n, cap - n, i ? ",%u" : "%u", s.slots[i]);
    return n;
}

const char* ParseSlots(const char* p, PlayerSel& s) {  // "922,959,..." up to a space or the end
    s.n = 0;
    while (*p >= '0' && *p <= '9') {
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 10);
        p = end;
        if (s.n < 16) s.slots[s.n++] = static_cast<uint16_t>(v);
        if (*p == ',') p++;
    }
    return p;
}

void DriftFromJoiner(unsigned id, const char* name, const char* p);  // drift.inc
int FormatDrift(char* out, size_t cap);

// Host, file server thread: "SEL <joiner id> <slots>[ H<drift hashes>]" -> "SELS <your colour> <colour>:<slots> ...".
void ServeSelection(SOCKET c, const char* req) {
    unsigned id = 0;
    int used = 0;
    if (sscanf_s(req, "SEL %x %n", &id, &used) < 1 || !used) return;
    PlayerSel sel = {};
    ParseSlots(req + used, sel);
    int colour = -1;
    char who[32] = "a player";
    EnterCriticalSection(&g_serve_lock);
    DWORD now = GetTickCount();
    for (int i = 0; i < g_joiner_count; i++) {
        Joiner& j = g_joiners[i];
        if (now - j.last_seen > kJoinerTimeoutMs) j.colour = 0;  // dropped: its colour is free again
    }
    for (int i = 0; i < g_joiner_count && colour < 0; i++) {
        Joiner& j = g_joiners[i];
        if (j.id != id || now - j.last_seen > kJoinerTimeoutMs) continue;
        for (int k = 1; k < 4 && !j.colour; k++) {
            bool taken = false;
            for (int o = 0; o < g_joiner_count; o++) taken |= g_joiners[o].colour == k;
            if (!taken) {
                j.colour = k;
                Log("host: %s's colour is %s", j.name, kDotName[k]);
            }
        }
        if (j.colour) colour = j.colour;
        strcpy_s(who, j.name);
    }
    LeaveCriticalSection(&g_serve_lock);
    DriftFromJoiner(id, who, req + used);
    char line[400];
    int n = sprintf_s(line, "SELS %d", colour);
    AcquireSRWLockExclusive(&g_dots_lock);
    if (colour > 0) {
        g_dots[colour] = sel;
        g_dots[colour].live = true;
    }
    for (int k = 0; k < 4; k++) {
        if (!g_dots[k].live) continue;
        n += sprintf_s(line + n, sizeof(line) - n, " %d:", k);
        n += FormatSlots(line + n, sizeof(line) - n, g_dots[k]);
    }
    ReleaseSRWLockExclusive(&g_dots_lock);
    n += sprintf_s(line + n, sizeof(line) - n, "\n");
    SendAll(c, line, n);
}

// Joiner: send this player's selection and take everyone's, about 5 times a second.
DWORD WINAPI SelectionPoller(LPVOID) {
    for (;;) {
        Sleep(200);
        if (!g_coop_join || g_join_state != 'P') continue;
        char req[1024];
        int rl = sprintf_s(req, "SEL %08x ", g_joiner_id);
        AcquireSRWLockShared(&g_dots_lock);
        rl += FormatSlots(req + rl, sizeof(req) - rl, g_my_sel);
        ReleaseSRWLockShared(&g_dots_lock);
        rl += FormatDrift(req + rl, sizeof(req) - rl);
        rl += sprintf_s(req + rl, sizeof(req) - rl, "\n");
        SOCKET s = ConnectHost();
        char line[400];
        if (s != INVALID_SOCKET && SendAll(s, req, rl) && RecvLine(s, line, sizeof(line))) {
            int colour = -1, used = 0;
            if (sscanf_s(line, "SELS %d%n", &colour, &used) == 1) {
                PlayerSel got[4] = {};
                for (const char* p = line + used; *p;) {
                    while (*p == ' ') p++;
                    int k = *p - '0';
                    if (k < 0 || k > 3 || p[1] != ':') break;
                    got[k].live = true;
                    p = ParseSlots(p + 2, got[k]);
                }
                AcquireSRWLockExclusive(&g_dots_lock);
                memcpy(g_dots, got, sizeof(g_dots));
                ReleaseSRWLockExclusive(&g_dots_lock);
                InterlockedExchange(&g_my_colour, colour);
                g_dots_heard = GetTickCount();
            }
        }
        if (s != INVALID_SOCKET) closesocket(s);
    }
}

void StartSelectionPoller() {
    static bool started = false;
    if (started) return;
    started = true;
    CloseHandle(CreateThread(nullptr, 0, SelectionPoller, nullptr, 0, nullptr));
}

// Main thread, every client tick: read this player's selection; the host keeps the table; repaint on a change.
void DotsFrame() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    PlayerSel mine = {};
    if (world) {
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        for (int k = 0; k < n; k++)
            if (EntityById(world, squad[k])[0xb2] & 0x40) mine.slots[mine.n++] = squad[k];
    }
    if (g_coop_host) {
        bool others = LiveJoiners() > 0;
        bool held[4] = {};
        EnterCriticalSection(&g_serve_lock);
        DWORD now = GetTickCount();
        for (int i = 0; i < g_joiner_count; i++)
            if (g_joiners[i].colour && now - g_joiners[i].last_seen <= kJoinerTimeoutMs) held[g_joiners[i].colour] = true;
        LeaveCriticalSection(&g_serve_lock);
        InterlockedExchange(&g_my_colour, 0);
        AcquireSRWLockExclusive(&g_dots_lock);
        g_dots[0] = mine;
        g_dots[0].live = others;
        for (int k = 1; k < 4; k++) g_dots[k].live &= held[k];
        g_my_sel = mine;
        ReleaseSRWLockExclusive(&g_dots_lock);
    } else {
        AcquireSRWLockExclusive(&g_dots_lock);
        g_my_sel = mine;
        if (!g_coop_join || GetTickCount() - g_dots_heard > 3000) memset(g_dots, 0, sizeof(g_dots));
        ReleaseSRWLockExclusive(&g_dots_lock);
        if (g_coop_join) StartSelectionPoller();
    }
    // What the bar shows now; repaint when it changed.
    char table[400];
    int tl = 0;
    table[0] = 0;
    AcquireSRWLockShared(&g_dots_lock);
    for (int k = 0; k < 4; k++) {
        if (!g_dots[k].live) continue;
        tl += sprintf_s(table + tl, sizeof(table) - tl, "%s%s ", tl ? "| " : "", kDotName[k]);
        tl += FormatSlots(table + tl, sizeof(table) - tl, g_dots[k]);
        tl += sprintf_s(table + tl, sizeof(table) - tl, " ");
    }
    ReleaseSRWLockShared(&g_dots_lock);
    static char shown[400] = "";
    // Also a new view (after a load) and every 2 s while there are dots: a repaint asked while the view was not up
    // yet was lost, and the view repaints only what changed (seen: dots on one copy, none on the other).
    static uint8_t* last_view = nullptr;
    static DWORD last_repaint = 0;
    if (g_view && table[0] && (g_view != last_view || GetTickCount() - last_repaint > 2000)) {
        last_view = g_view;
        last_repaint = GetTickCount();
        reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(g_view))[3])(g_view);
    }
    if (strcmp(table, shown) != 0) {
        strcpy_s(shown, table);
        if (g_view) reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(g_view))[3])(g_view);
        if (g_test.enabled) Log("dots: %s", table[0] ? table : "none");
    }
    if (!g_test.enabled || !world) return;
    // Test (host_move): the joiner keeps only its second and third characters selected from frame 120; both copies
    // capture their window at frame 220. The bit is set the way the game's own select-all (0x49f6b0) and deselect
    // (0x4cfc10) set it, but without the selection screen's own refresh: on the joiner's copy the deselected
    // portraits then draw black (seen 2026-10-22); a player's click goes through the game's selection code instead.
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    static bool narrowed = false, shot = false;
    if (g_coop_join && !narrowed && frame >= 120 && strcmp(g_test.scenario, "move") == 0) {
        narrowed = true;
        uint16_t squad[16];
        int n = GetSquad(world, squad, 16);
        for (int k = 0; k < n; k++) {
            uint8_t* e = EntityById(world, squad[k]);
            e[0xb2] = (k == 1 || k == 2) ? (e[0xb2] | 0x40) : (e[0xb2] & ~0x40);
            *reinterpret_cast<int*>(e + 0x13c) = *reinterpret_cast<int*>(world + 0x2ef) + 1;
        }
        Log("bot: the joiner selects only its 2nd and 3rd characters");
    }
    static int first_frame = -1;
    if (first_frame < 0) first_frame = frame;
    if (!shot && frame >= first_frame + 200 && g_shot_event) {
        shot = true;
        SetEvent(g_shot_event);
    }
}

void DrawDot(uint8_t* renderer, int x, int y, uint32_t argb) {
    FillRect(renderer, x - 1, y - 1, x + kDotSize, y + kDotSize, 0xff000000);
    for (int i = 0; 2 * i < kDotSize; i++) FillRect(renderer, x + i, y + i, x + kDotSize - 1 - i, y + kDotSize - 1 - i, argb);
}

void __fastcall PortraitDrawHook(uint8_t* self, void* edx, uint8_t* renderer) {
    g_portrait_draw(self, edx, renderer);
    bool known = false;
    for (uint8_t* p : g_portraits) known |= p == self;
    for (uint8_t*& p : g_portraits)
        if (!known && !p) {
            p = self;
            known = true;
        }
}

int* Rect(uint8_t* w) { return reinterpret_cast<int*>(w + 0xc3); }

void __fastcall ViewDrawHook(uint8_t* self, void* edx, uint8_t* renderer) {
    g_view_draw(self, edx, renderer);
    g_view = self;
    // The bottom panel is a child of the mission screen (children from +0x18, next sibling +0x24). Found there, not
    // through the small portraits: with one character selected the bar shows a single large portrait instead, and
    // the small ones never draw (QuestBarter.sav: no dots at all).
    uint8_t* panel = nullptr;
    uint8_t* game_main = *reinterpret_cast<uint8_t**>(kGameMainPtr);
    for (uint8_t* c = game_main ? *reinterpret_cast<uint8_t**>(game_main + 0x18) : nullptr; c && !panel;
         c = *reinterpret_cast<uint8_t**>(c + 0x24))
        if (*reinterpret_cast<uintptr_t*>(c) == kPanelVtable) panel = c;
    if (!panel) return;
    // Tab k shows squad member k (the bar fills its tabs in squad order; a tab holds only the name and HP it shows).
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint16_t squad[16];
    int nsquad = world ? GetSquad(world, squad, 16) : 0;
    bool drew = false;
    AcquireSRWLockShared(&g_dots_lock);
    for (int k = 0; k < 6 && k < nsquad; k++) {
        uint8_t* tab = *reinterpret_cast<uint8_t**>(panel + 0xfe + 4 * k);
        if (!tab || *reinterpret_cast<uintptr_t*>(tab) != kTabVtable) continue;
        uint16_t slot = squad[k];
        int colours[4], n = 0;
        for (int c = 0; c < 4; c++) {
            if (!g_dots[c].live) continue;
            for (int i = 0; i < g_dots[c].n; i++)
                if (g_dots[c].slots[i] == slot) {
                    colours[n++] = c;
                    break;
                }
        }
        if (!n) continue;
        // The tab's place in the view's coordinates (panel and view share a parent), the dots centred above it.
        int tx0 = Rect(panel)[0] + Rect(tab)[0] - Rect(self)[0], tx1 = Rect(panel)[0] + Rect(tab)[2] - Rect(self)[0];
        int ty = Rect(panel)[1] + Rect(tab)[1] - Rect(self)[1];
        int width = n * kDotSize + (n - 1) * 3;
        int x = (tx0 + tx1 - width) / 2, y = ty - kDotSize - 3;
        for (int i = 0; i < n; i++, x += kDotSize + 3) DrawDot(renderer, x, y, kDotColour[colours[i]]);
        drew = true;
    }
    ReleaseSRWLockShared(&g_dots_lock);
    // The view scrolls by moving what it drew and redrawing only the strip that came into sight: the dots moved with
    // the world and left copies behind (owner: "during screen drag the control dots don't update and move with the
    // world"). While there are dots, the whole view is drawn again next frame.
    if (drew) reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(self))[3])(self);
}

// World map pings (worldmap_sync.inc): WWorldWindow's draw (vtable 0x82db04 slot 4, 0x6bbb00), then a marker at each
// live ping in the window's own coordinates, the world point less the view's scroll (campaign +0x9d/+0xa1): a black
// ring, the player's colour inside, a white centre, and a stalk above so it reads as a flag on the map.
const uintptr_t kWorldMapDrawSlot = 0x82db04 + 4 * 4, kWorldMapDraw = 0x6bbb00;
PortraitDrawFn g_worldmap_draw = nullptr;

// The renderer's rectangle (+0x24) is an outline, one pixel wide: a filled box is outlines, each one pixel in (as
// DrawDot does). Corners inclusive.
void FillBox(uint8_t* renderer, int x0, int y0, int x1, int y1, uint32_t argb) {
    for (int i = 0; x0 + i <= x1 - i && y0 + i <= y1 - i; i++) FillRect(renderer, x0 + i, y0 + i, x1 - i, y1 - i, argb);
}

void DrawPing(uint8_t* renderer, int x, int y, uint32_t argb) {
    FillBox(renderer, x - 2, y - 26, x + 2, y - 7, 0xff000000);  // a flag: the pole and its pennant, then the
    FillBox(renderer, x + 1, y - 26, x + 13, y - 18, 0xff000000);  // marker at its foot
    FillBox(renderer, x + 2, y - 25, x + 12, y - 19, argb);
    FillBox(renderer, x - 1, y - 25, x + 1, y - 8, argb);
    FillBox(renderer, x - 8, y - 8, x + 8, y + 8, 0xff000000);
    FillBox(renderer, x - 6, y - 6, x + 6, y + 6, argb);
    FillBox(renderer, x - 2, y - 2, x + 2, y + 2, 0xffffffff);
}

void __fastcall WorldMapDrawHook(uint8_t* self, void* edx, uint8_t* renderer) {
    g_worldmap_draw(self, edx, renderer);
    static bool drew_last = false;
    MapPing p[4];
    uint8_t* camp = *reinterpret_cast<uint8_t**>(kCampaignPtr2);
    int n = camp ? LivePings(p) : 0;
    static DWORD said = 0;
    bool say = g_test.enabled && n && GetTickCount() - said > 1000;
    if (say) said = GetTickCount();
    for (int k = 0; k < 4 && n; k++) {
        if (!p[k].born) continue;
        int x = static_cast<int>(p[k].x - *reinterpret_cast<float*>(camp + 0x9d));
        int y = static_cast<int>(p[k].y - *reinterpret_cast<float*>(camp + 0xa1));
        DrawPing(renderer, x, y, kDotColour[k]);
        if (say) Log("bot: world map drawn with ping %s at (%d, %d), window (%d, %d)-(%d, %d)", kDotName[k], x, y,
                     Rect(self)[0], Rect(self)[1], Rect(self)[2], Rect(self)[3]);
    }
    // Drawn again next frame while any ping shows (the map scrolls and redraws only what changed), and once more
    // after the last one is gone so it leaves no trace.
    if (n || drew_last) reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(self))[3])(self);
    drew_last = n > 0;
}

void* __fastcall ViewDtorHook(uint8_t* self, void* edx, unsigned flags) {
    if (g_view == self) g_view = nullptr;
    return g_view_dtor(self, edx, flags);
}

void* __fastcall PortraitDtorHook(uint8_t* self, void* edx, unsigned flags) {
    for (uint8_t*& p : g_portraits)
        if (p == self) p = nullptr;
    return g_portrait_dtor(self, edx, flags);
}

// Test mode: the game window as ftcoop_shot.bmp in the game folder, when DotsFrame asks for it.
DWORD WINAPI ShotThread(LPVOID) {
  for (int shot = 1;; shot++) {
    WaitForSingleObject(g_shot_event, INFINITE);
    ResetEvent(g_shot_event);
    Sleep(500);  // the repaint
    HWND hwnd = g_pump_hwnd;
    if (!hwnd) EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
    if (!hwnd) continue;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC wdc = GetDC(hwnd), mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    BOOL ok = PrintWindow(hwnd, mdc, 3);  // PW_CLIENTONLY | PW_RENDERFULLCONTENT
    SelectObject(mdc, old);
    BITMAPINFOHEADER bi = {sizeof(bi), w, h, 1, 24, BI_RGB};
    int stride = (w * 3 + 3) & ~3;
    uint8_t* px = static_cast<uint8_t*>(malloc(stride * h));
    GetDIBits(mdc, bmp, 0, h, px, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    BITMAPFILEHEADER bf = {0x4d42, static_cast<DWORD>(sizeof(bf) + sizeof(bi) + stride * h), 0, 0, sizeof(bf) + sizeof(bi)};
    FILE* f = nullptr;
    char file[32];
    sprintf_s(file, shot == 1 ? "ftcoop_shot.bmp" : "ftcoop_shot%d.bmp", shot);
    if (fopen_s(&f, file, "wb") == 0 && f) {
        fwrite(&bf, sizeof(bf), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        fwrite(px, stride * h, 1, f);
        fclose(f);
    }
    Log("dots: window captured as %s (%dx%d, PrintWindow %s)", file, w, h, ok ? "ok" : "failed");
    free(px);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
  }
}

template <typename Fn>
bool PatchSlot(uintptr_t slot, uintptr_t expect, void* hook, Fn* original);  // netlog.cpp

void InstallDots() {
    Log("co-op: portrait hooks %s, %s (selection dots)",
        PatchSlot(kPortraitDrawSlot, kPortraitDraw, (void*)&PortraitDrawHook, &g_portrait_draw) ? "ok" : "FAILED",
        PatchSlot(kPortraitDtorSlot, kPortraitDtor, (void*)&PortraitDtorHook, &g_portrait_dtor) ? "ok" : "FAILED");
    Log("co-op: view hook %s (selection dots above the name tabs)",
        PatchSlot(kViewDrawSlot, kViewDraw, (void*)&ViewDrawHook, &g_view_draw) &&
                PatchSlot(kViewDtorSlot, kViewDtor, (void*)&ViewDtorHook, &g_view_dtor) ? "ok" : "FAILED");
    Log("co-op: world map draw hook %s (pings)",
        PatchSlot(kWorldMapDrawSlot, kWorldMapDraw, (void*)&WorldMapDrawHook, &g_worldmap_draw) ? "ok" : "FAILED");
}

void StartShotThread() {  // after the test config is read
    g_shot_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, ShotThread, nullptr, 0, nullptr));
}
