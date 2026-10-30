// ---- save transfer and re-sync: the joiner always plays from the host's save ----
// Included from netlog.cpp.
// A save is not loaded by name at session start: Client+0x13a (level file) stays the shared empty string
// (0x8af7e4) up to the level load, and the 0xc9 roster's file name is empty; loading a save fills user/$$current$$
// from the file and the level loads from there (FTSE notes). So the transfer happens before the joiner loads:
// the host records the save file each load checks (0x6c2540 calls the save check 0x4c1290(file) at 0x6c260e, from
// the Load menu and quick-load alike) and serves it on a side TCP port with a generation number that grows with
// every load. The joiner downloads it to user/save/ftcoop_received.sav and loads that file.
// Re-sync (map changes, mid-match join): the host quick-saves and quick-loads (in a mission: quickSave 0x478340,
// quickLoad 0x478860 on WGameMain), so its new session goes through the normal load path with a new generation.
// A joiner that sees a newer generation fetches it and loads it the way the game's own quick-loads do: assign the
// global "file to load" FOTString 0x8bddb4 (0x6c8e80) and close the current screen with result 0x10 (world map
// WCampaignWindow vtable+0x34, mission WGameMain vtable+0x38).
// Protocol: "GET\n" -> "FTCOOP1 <gen> <size> <name>\n" + <size> bytes;
// "GEN [<joiner id> <player name>]\n" -> "GEN <gen> <lobby 0|1> <name>\n". A joiner's GEN every 2 s registers it
// (drop-in: a new joiner during a running session makes the host re-sync) and is its heartbeat; lobby=1 while the
// host holds a co-op lobby, the only time a joiner at the main menu may load (else it would load a session that
// already started without it).
const uintptr_t kLoadCheckCall = 0x6c260e;  // e8 -> 0x4c1290
const uintptr_t kLoadCheck = 0x4c1290;      // bool __thiscall(void*, FOTString* file), ret 4
const uintptr_t kFileToLoad = 0x8bddb4;     // FOTString global assigned by the quick-loads
const uintptr_t kStringAssign = 0x6c8e80;   // FOTString* __thiscall(FOTString* dst, FOTString* src)
const uintptr_t kGameMainPtr = 0x8bc384;    // WGameMain* (the in-mission screen), 0 outside missions
const uintptr_t kGameMainVtable = 0x801ac8;
const uintptr_t kCampaignWindowVtable = 0x82d2a8;
const uintptr_t kQuickSave = 0x478340;      // void __fastcall(WGameMain*), writes user/save/QuickSave.sav
const uintptr_t kQuickLoad = 0x478860;      // void __fastcall(WGameMain*), loads it (assigns 0x8bddb4, closes 0x10)
const char kQuickSavePath[] = "core\\user\\save\\QuickSave.sav";
const char kReceivedName[] = "user/save/ftcoop_received.sav";  // as the game names files (relative to core/)
const char kReceivedPath[] = "core\\user\\save\\ftcoop_received.sav";
// The re-sync goes through the game's quick-save slot. The host's own QuickSave.sav is kept: copied aside before the
// re-sync quick-save and put back once the new session has started (the load has read the file by then); the
// re-sync save itself is served to joiners as ftcoop_resync.sav. A backup left by a crash is restored at start.
const char kQuickSaveName[] = "user/save/QuickSave.sav";
const char kQuickSaveBackupPath[] = "core\\user\\save\\ftcoop_quicksave_backup.sav";
const char kResyncName[] = "user/save/ftcoop_resync.sav";
const char kResyncPath[] = "core\\user\\save\\ftcoop_resync.sav";

int g_file_port = 47701;      // [coop] file_port
bool g_fetch_save = true;     // [coop] fetch_save=0 turns the transfer off (mutation test)
bool g_have_received = false;
CRITICAL_SECTION g_serve_lock;
char g_serve_name[260] = "";  // game name, e.g. user/save/New Save Game.sav
volatile LONG g_serve_gen = 0;      // host: grows with every load
volatile LONG g_host_gen = 0;       // joiner: latest generation the host announced
LONG g_loaded_gen = 0;              // joiner: generation of the save it is playing from
bool g_load_pending = false;        // host: a load is under way (its session lobby is a co-op lobby)
bool g_file_server_started = false;
volatile LONG g_lobby_open = 0;     // host: a co-op lobby is holding the start (answered in GEN)
DWORD g_session_start_tick = 0;     // host: GetTickCount() at the current session's start (0xce)
// The host's game settings as " SET ..." (settings_sync.inc): the host's GEN replies carry them, so a joiner has them
// from its first frame, before the in-game record acts. Joiner: the host's last reply.
SRWLOCK g_settings_line_lock = SRWLOCK_INIT;
char g_settings_line[96] = "";
void HostSettingsLine(char* out, size_t size);  // settings_sync.inc

// Host: joiners known from their GEN polls. Live = polled in the last kJoinerTimeoutMs. Each poll reports what
// the joiner is doing: 'W' waiting at the main menu for a lobby, 'L' loading or in its lobby, 'P' playing; and the
// generation it loaded. The host re-syncs on what joiners report, not on gaps between polls: under load the
// polls stalled for 20 s, the host took the joiner for gone and back again, and re-synced in a loop.
const DWORD kJoinerTimeoutMs = 15000;
const DWORD kResyncSpacingMs = 20000;  // at most one re-sync per joiner this often
struct Joiner {
    unsigned id;
    char name[32];
    char state;     // W, L or P (see above)
    long gen;       // generation the joiner loaded
    DWORD last_seen, last_resync;
    int colour;     // selection dots: 1..3 (blue, yellow, green), 0 none yet (the host is red)
};
Joiner g_joiners[8] = {};
int g_joiner_count = 0;

// Joiner: who we are to the host.
unsigned g_joiner_id = 0;
char g_player_name[32] = "Player";
volatile LONG g_host_lobby_open = 0;  // joiner: the host's latest GEN said a co-op lobby is open
volatile char g_join_state = 'W';     // joiner: W, L or P, reported in every poll

void NoteJoiner(unsigned id, const char* name, char state, long gen) {  // file server thread, under g_serve_lock
    DWORD now = GetTickCount();
    for (int i = 0; i < g_joiner_count; i++) {
        Joiner& j = g_joiners[i];
        if (j.id == id) {
            if (j.state != state) Log("host: %s is now %c (generation %ld)", j.name, state, gen);
            j.state = state;
            j.gen = gen;
            j.last_seen = now;
            return;
        }
    }
    if (g_joiner_count >= 8) return;
    Joiner& j = g_joiners[g_joiner_count++];
    j.id = id;
    strncpy_s(j.name, name, _TRUNCATE);
    j.state = state;
    j.gen = gen;
    j.last_seen = now;
    j.last_resync = now - kResyncSpacingMs;
    Log("host: %s joined (id %08x, state %c)", j.name, id, state);
}

int LiveJoiners() {
    EnterCriticalSection(&g_serve_lock);
    int n = 0;
    DWORD now = GetTickCount();
    for (int i = 0; i < g_joiner_count; i++)
        if (now - g_joiners[i].last_seen <= kJoinerTimeoutMs) n++;
    LeaveCriticalSection(&g_serve_lock);
    return n;
}

// Host, main thread, every game frame: a live joiner left out of the running session needs a re-sync
// (quick-save + quick-load) so everyone, the newcomer included, loads the same save in a new lobby:
// - it reports W (waiting at the menu) in a poll made after this session started, or
// - it reports L (loaded, in its lobby) 15 s into the session: the lobby started without it.
const char* NewJoinerNeedingResync() {
    static char reason[64];
    const char* r = nullptr;
    if (g_lobby_open) return nullptr;  // a lobby is open: waiting joiners load into that one
    EnterCriticalSection(&g_serve_lock);
    DWORD now = GetTickCount();
    for (int i = 0; i < g_joiner_count; i++) {
        Joiner& j = g_joiners[i];
        if (now - j.last_seen > kJoinerTimeoutMs || now - j.last_resync < kResyncSpacingMs) continue;
        bool polled_in_session = static_cast<LONG>(j.last_seen - g_session_start_tick) > 2500;
        if (polled_in_session && j.state == 'W') sprintf_s(reason, "%s joined mid-session", j.name);
        else if (polled_in_session && j.state == 'L' && now - g_session_start_tick > 15000)
            sprintf_s(reason, "%s missed the start", j.name);
        else continue;
        j.last_resync = now;
        r = reason;
        break;
    }
    LeaveCriticalSection(&g_serve_lock);
    return r;
}

bool SendAll(SOCKET s, const char* p, int n) {
    while (n > 0) {
        int k = send(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

bool RecvAll(SOCKET s, char* p, int n) {
    while (n > 0) {
        int k = recv(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

bool RecvLine(SOCKET s, char* line, int cap) {
    int n = 0;
    while (n < cap - 1) {
        char c;
        if (recv(s, &c, 1, 0) != 1) return false;
        if (c == '\n') break;
        line[n++] = c;
    }
    line[n] = 0;
    return true;
}

void GamePathToFile(const char* name, char* out, size_t cap) {  // "user/save/x.sav" -> "core\user\save\x.sav"
    sprintf_s(out, cap, "core\\%s", name);
    for (char* p = out; *p; p++)
        if (*p == '/') *p = '\\';
}

// A joiner's game after its private screens closed (trading, recruiting, squad changes: see netlog.cpp): it
// quick-saves and uploads the save ("PUT <id> <size> <player name>\n" + bytes, answered "OK\n"); the host loads it
// and re-syncs everyone from it.
const char kUploadName[] = "user/save/ftcoop_upload.sav";
const char kUploadPath[] = "core\\user\\save\\ftcoop_upload.sav";
const char kUploadPartPath[] = "core\\user\\save\\ftcoop_upload.part";
volatile LONG g_upload_ready = 0;  // host: a joiner's save has arrived at kUploadPath
char g_upload_from[32] = "";

void ReceiveUpload(SOCKET c, const char* req) {  // host, file server thread
    unsigned id = 0;
    long size = 0;
    char who[32] = "a player";
    if (sscanf_s(req, "PUT %x %ld %31[^\n]", &id, &size, who, static_cast<unsigned>(sizeof(who))) < 2 || size <= 0 ||
        size >= 64 * 1024 * 1024)
        return;
    char* data = static_cast<char*>(malloc(size));
    FILE* f = nullptr;
    bool ok = RecvAll(c, data, size) && fopen_s(&f, kUploadPartPath, "wb") == 0 && f;
    if (f) {
        ok = ok && fwrite(data, 1, size, f) == static_cast<size_t>(size);
        fclose(f);
    }
    free(data);
    ok = ok && MoveFileExA(kUploadPartPath, kUploadPath, MOVEFILE_REPLACE_EXISTING);
    Log("host: %s %s's save (%ld bytes)", ok ? "received" : "FAILED to receive", who, size);
    if (!ok) return;
    SendAll(c, "OK\n", 3);
    EnterCriticalSection(&g_serve_lock);
    strcpy_s(g_upload_from, who);
    LeaveCriticalSection(&g_serve_lock);
    InterlockedExchange(&g_upload_ready, 1);
}

// A private-screen delta too big for lockstep (private_sync.inc: a new recruit with full gear, several at once): the
// host keeps the last one in memory. "DPUT <hash> <size>\n" + bytes (a joiner talker uploads it, answered "OK\n");
// "DGET <hash>\n" -> "DELTA <size>\n" + bytes, or "NONE\n" when the host has no delta with that hash.
uint8_t* g_big_delta = nullptr;
uint32_t g_big_size = 0, g_big_hash = 0;

void StoreBigDelta(const uint8_t* p, uint32_t n, uint32_t hash) {  // under g_serve_lock
    free(g_big_delta);
    g_big_delta = static_cast<uint8_t*>(malloc(n ? n : 1));
    memcpy(g_big_delta, p, n);
    g_big_size = n;
    g_big_hash = hash;
}

void ServeDelta(SOCKET c, const char* req) {  // host, file server thread
    unsigned hash = 0;
    long size = 0;
    if (strncmp(req, "DPUT ", 5) == 0) {
        if (sscanf_s(req, "DPUT %x %ld", &hash, &size) != 2 || size <= 0 || size > 16 * 1024 * 1024) return;
        uint8_t* data = static_cast<uint8_t*>(malloc(size));
        bool ok = RecvAll(c, reinterpret_cast<char*>(data), size);
        if (ok) {
            EnterCriticalSection(&g_serve_lock);
            StoreBigDelta(data, static_cast<uint32_t>(size), hash);
            LeaveCriticalSection(&g_serve_lock);
            SendAll(c, "OK\n", 3);
        }
        free(data);
        Log("host: %s a delta from a joiner (%ld bytes, hash %08x)", ok ? "received" : "FAILED to receive", size, hash);
        return;
    }
    if (sscanf_s(req, "DGET %x", &hash) != 1) return;
    EnterCriticalSection(&g_serve_lock);
    bool have = g_big_delta && g_big_hash == hash;
    uint32_t n = have ? g_big_size : 0;
    uint8_t* copy = have ? static_cast<uint8_t*>(malloc(n)) : nullptr;
    if (copy) memcpy(copy, g_big_delta, n);
    LeaveCriticalSection(&g_serve_lock);
    char line[40];
    int hl = have ? sprintf_s(line, "DELTA %u\n", n) : sprintf_s(line, "NONE\n");
    bool ok = SendAll(c, line, hl) && (!have || SendAll(c, reinterpret_cast<char*>(copy), n));
    free(copy);
    Log("host: %s the delta %08x to a joiner (%u bytes)", have ? (ok ? "sent" : "FAILED to send") : "does not have", hash, n);
}

void ServeSelection(SOCKET c, const char* req);  // dots.inc

void ServeOne(SOCKET c) {
    char req[1024];
    if (!RecvLine(c, req, sizeof(req))) return;
    if (strncmp(req, "SEL ", 4) == 0) {
        ServeSelection(c, req);
        return;
    }
    if (strncmp(req, "PING ", 5) == 0) {  // "PING <joiner id> <where> x y": a joiner's ping, in its colour
        unsigned id = 0;
        float x = 0, y = 0;
        int colour = -1, where = 0;
        if (sscanf_s(req, "PING %x %d %f %f", &id, &where, &x, &y) == 4) {
            EnterCriticalSection(&g_serve_lock);
            for (int i = 0; i < g_joiner_count; i++)
                if (g_joiners[i].id == id && g_joiners[i].colour) colour = g_joiners[i].colour;
            LeaveCriticalSection(&g_serve_lock);
        }
        if (colour > 0) AddPing(colour, where, x, y);
        else Log("co-op: a ping from joiner %08x, who has no colour yet: dropped", id);
        SendAll(c, "OK\n", 3);
        return;
    }
    if (strncmp(req, "DPUT ", 5) == 0 || strncmp(req, "DGET ", 5) == 0) {
        ServeDelta(c, req);
        return;
    }
    char name[260];
    unsigned id = 0;
    char who[32] = "", state = 'W';
    long their_gen = 0;
    bool gen_req = strncmp(req, "GEN", 3) == 0 && (req[3] == 0 || req[3] == ' ');
    // "GEN <id> <state> <gen> <name>" (the name last: it may contain spaces)
    if (gen_req)
        sscanf_s(req, "GEN %x %c %ld %31[^\n]", &id, &state, 1, &their_gen, who, static_cast<unsigned>(sizeof(who)));
    EnterCriticalSection(&g_serve_lock);
    strcpy_s(name, g_serve_name);
    LONG gen = g_serve_gen;
    if (gen_req && id) NoteJoiner(id, who[0] ? who : "a player", state, their_gen);
    LeaveCriticalSection(&g_serve_lock);
    if (gen_req) {
        char line[700];
        char wmap[300];
        WorldMapLine(wmap, sizeof(wmap));  // the host's party on the world map, if it is there
        char set[96];
        HostSettingsLine(set, sizeof(set));
        int n = sprintf_s(line, "GEN %ld %d %s%s%s\n", gen, g_lobby_open ? 1 : 0, name, wmap, set);
        SendAll(c, line, n);
        return;
    }
    if (strncmp(req, "PUT ", 4) == 0) {
        ReceiveUpload(c, req);
        return;
    }
    if (strcmp(req, "GET") != 0) return;
    char path[300];
    GamePathToFile(name, path, sizeof(path));
    FILE* f = nullptr;
    if (!name[0] || fopen_s(&f, path, "rb") != 0 || !f) {
        Log("host: file server has nothing to send (\"%s\")", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = static_cast<char*>(malloc(size > 0 ? size : 1));
    size_t got = fread(data, 1, size, f);
    fclose(f);
    char header[400];
    int hl = sprintf_s(header, "FTCOOP1 %ld %ld %s\n", gen, size, name);
    bool ok = static_cast<long>(got) == size && SendAll(c, header, hl) && SendAll(c, data, size);
    free(data);
    Log("host: %s \"%s\" (%ld bytes, generation %ld) to a joiner", ok ? "sent" : "FAILED to send", name, size, gen);
}

DWORD WINAPI ServeConnection(LPVOID p) {
    SOCKET c = reinterpret_cast<SOCKET>(p);
    ServeOne(c);
    closesocket(c);
    return 0;
}

DWORD WINAPI FileServerThread(LPVOID) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(g_file_port));
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls, 4) != 0) {
        Log("host: file server could not listen on port %d (error %d)", g_file_port, WSAGetLastError());
        return 0;
    }
    Log("host: file server listening on port %d", g_file_port);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD timeout = 10000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        // One thread per connection: a slow transfer must not hold up other joiners' polls.
        HANDLE t = CreateThread(nullptr, 0, ServeConnection, reinterpret_cast<LPVOID>(c), 0, nullptr);
        if (t) CloseHandle(t);
        else {
            ServeOne(c);
            closesocket(c);
        }
    }
}

void OurQuickSave(void* game_main);  // save_sync.inc: the game's quick-save, never turned into a co-op request

bool g_quicksave_guard = false;  // host: a re-sync is using the quick-save slot; the owner's file is set aside
bool g_quicksave_had = false;    // the owner had a QuickSave.sav before the re-sync

void GuardQuickSave() {  // before the re-sync quick-save
    if (g_quicksave_guard) return;
    g_quicksave_had = GetFileAttributesA(kQuickSavePath) != INVALID_FILE_ATTRIBUTES;
    if (g_quicksave_had && !CopyFileA(kQuickSavePath, kQuickSaveBackupPath, FALSE)) {
        Log("co-op: could not back up QuickSave.sav (error %lu): re-sync not started", GetLastError());
        return;
    }
    g_quicksave_guard = true;
    Log("co-op: %s", g_quicksave_had ? "backed up the player's QuickSave.sav for the re-sync" : "no QuickSave.sav to keep");
}

void RestoreQuickSave() {  // after the re-sync session started
    if (!g_quicksave_guard) return;
    g_quicksave_guard = false;
    bool ok = g_quicksave_had ? (CopyFileA(kQuickSaveBackupPath, kQuickSavePath, FALSE) && DeleteFileA(kQuickSaveBackupPath))
                              : (DeleteFileA(kQuickSavePath) || GetLastError() == ERROR_FILE_NOT_FOUND);
    Log("co-op: %s the player's QuickSave.sav%s", g_quicksave_had ? "restored" : "removed the re-sync's",
        ok ? "" : " - FAILED");
}

void RestoreQuickSaveAfterCrash() {  // at start: a backup left behind means a re-sync never finished
    if (GetFileAttributesA(kQuickSaveBackupPath) == INVALID_FILE_ATTRIBUTES) return;
    bool ok = CopyFileA(kQuickSaveBackupPath, kQuickSavePath, FALSE) && DeleteFileA(kQuickSaveBackupPath);
    Log("co-op: restored QuickSave.sav from an unfinished re-sync%s", ok ? "" : " - FAILED");
}

void DropPrivateScreens();  // netlog.cpp
// The mission is being torn down for a load (a joiner following the host, the host's re-sync, any load) until the
// next session starts. A shop closed by the teardown measured its deal against a half-gone world and sent it, and the
// others applied it in the new session (shopresync: the free lists apart from then on).
bool g_world_closing = false;

// Host: every load passes its save file through the save check; serve it as a new generation.
char __fastcall LoadCheckHook(void* self, void* /*edx*/, void** file) {
    char name[260] = "";
    const wchar_t* w = file ? reinterpret_cast<const wchar_t*>(*file) : nullptr;
    if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, name, sizeof(name), nullptr, nullptr);
    if (g_coop_host || g_coop_join) {
        g_world_closing = true;
        DropPrivateScreens();
    }
    if (g_coop_host && g_quicksave_guard && _stricmp(name, kQuickSaveName) == 0) {
        // Serve a copy: the player's own QuickSave.sav goes back in place once this session starts.
        if (CopyFileA(kQuickSavePath, kResyncPath, FALSE)) strcpy_s(name, kResyncName);
        else Log("host: could not copy the re-sync save (error %lu), serving QuickSave.sav", GetLastError());
    }
    if (g_coop_host) {
        EnterCriticalSection(&g_serve_lock);
        strcpy_s(g_serve_name, name);
        LONG gen = ++g_serve_gen;
        LeaveCriticalSection(&g_serve_lock);
        g_load_pending = true;
        Log("host: loading save \"%s\", serving it to joiners as generation %ld", name, gen);
    }
    return reinterpret_cast<char(__thiscall*)(void*, void**)>(kLoadCheck)(self, file);
}

void StartFileServer() {
    if (g_file_server_started) return;
    g_file_server_started = true;
    CreateThread(nullptr, 0, FileServerThread, nullptr, 0, nullptr);
}

SOCKET ConnectHost();

// Joiner, from the world map's mouse-down (main thread): the ping goes to the host on a thread of its own, so a slow
// host never stalls the game.
DWORD WINAPI PingThread(LPVOID p) {
    float* xy = static_cast<float*>(p);  // where, x, y
    char req[96];
    int n = sprintf_s(req, "PING %08x %d %.1f %.1f\n", g_joiner_id, static_cast<int>(xy[0]), xy[1], xy[2]);
    delete[] xy;
    SOCKET s = ConnectHost();
    char reply[16];
    if (s != INVALID_SOCKET) {
        if (!SendAll(s, req, n) || !RecvLine(s, reply, sizeof(reply))) Log("co-op: the ping did not reach the host");
        closesocket(s);
    }
    return 0;
}

void SendPingToHost(int where, float x, float y) {
    float* xy = new float[3]{static_cast<float>(where), x, y};
    HANDLE t = CreateThread(nullptr, 0, PingThread, xy, 0, nullptr);
    if (t) CloseHandle(t);
    else delete[] xy;
}

SOCKET ConnectHost() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD timeout = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(g_file_port));
    inet_pton(AF_INET, g_host_address, &a.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// Joiner, main thread. Blocking with 10 s timeouts (loopback / LAN for now). On success g_loaded_gen is the
// generation of the file now at kReceivedPath.
bool FetchSave() {
    bool ok = false;
    SOCKET s = ConnectHost();
    char header[400];
    if (s != INVALID_SOCKET && SendAll(s, "GET\n", 4) && RecvLine(s, header, sizeof(header))) {
        long gen = 0, size = -1;
        char name[260] = "";
        if (sscanf_s(header, "FTCOOP1 %ld %ld %259[^\n]", &gen, &size, name, static_cast<unsigned>(sizeof(name))) == 3 &&
            size > 0 && size < 64 * 1024 * 1024) {
            char* data = static_cast<char*>(malloc(size));
            FILE* f = nullptr;
            if (RecvAll(s, data, size) && fopen_s(&f, kReceivedPath, "wb") == 0 && f) {
                ok = fwrite(data, 1, size, f) == static_cast<size_t>(size);
                fclose(f);
                g_loaded_gen = gen;
                Log("join: received host's save \"%s\" (%ld bytes, generation %ld) as %s", name, size, gen, kReceivedName);
            }
            free(data);
        }
    }
    if (!ok) Log("join: could not get the host's save yet (error %d), will retry", WSAGetLastError());
    if (s != INVALID_SOCKET) closesocket(s);
    return ok;
}

// Joiner: a big delta to the host (DPUT) or from it (DGET). Blocking, 10 s timeouts; false on failure.
bool PutBigDelta(const uint8_t* p, uint32_t n, uint32_t hash) {
    SOCKET s = ConnectHost();
    char line[64];
    int hl = sprintf_s(line, "DPUT %08x %u\n", hash, n);
    char reply[16] = "";
    bool ok = s != INVALID_SOCKET && SendAll(s, line, hl) && SendAll(s, reinterpret_cast<const char*>(p), n) &&
              RecvLine(s, reply, sizeof(reply)) && strcmp(reply, "OK") == 0;
    if (s != INVALID_SOCKET) closesocket(s);
    return ok;
}

uint32_t GetBigDelta(uint32_t hash, uint8_t* out, uint32_t cap) {
    SOCKET s = ConnectHost();
    char line[64];
    int hl = sprintf_s(line, "DGET %08x\n", hash);
    char reply[40] = "";
    unsigned n = 0;
    bool ok = s != INVALID_SOCKET && SendAll(s, line, hl) && RecvLine(s, reply, sizeof(reply)) &&
              sscanf_s(reply, "DELTA %u", &n) == 1 && n <= cap && RecvAll(s, reinterpret_cast<char*>(out), n);
    if (s != INVALID_SOCKET) closesocket(s);
    return ok ? n : 0;
}

// Joiner: ask the host for its current generation every kJoinPollMs (background thread, never touches game state).
// It was 2 s: every join and every re-sync waited up to that long before the joiner even heard of the new save.
DWORD WINAPI GenerationPoller(LPVOID) {
    for (;;) {
        Sleep(kJoinPollMs);
        if (!g_coop_join) continue;  // stopped waiting (or never joined): don't register with the host
        SOCKET s = ConnectHost();
        char line[700];
        char req[64];
        int rl = sprintf_s(req, "GEN %08x %c %ld %s\n", g_joiner_id, g_join_state, g_loaded_gen, g_player_name);
        if (s != INVALID_SOCKET && SendAll(s, req, rl) && RecvLine(s, line, sizeof(line))) {
            long gen = 0;
            int lobby = 0;
            int got = sscanf_s(line, "GEN %ld %d", &gen, &lobby);
            WorldMapFromHost(line);
            const char* set = nullptr;  // the last one: the save's name comes before it and may hold anything
            for (const char* q = line; (q = strstr(q, " SET ")) != nullptr; q++) set = q;
            if (set) {
                AcquireSRWLockExclusive(&g_settings_line_lock);
                strncpy_s(g_settings_line, set, strcspn(set, "\r\n"));
                ReleaseSRWLockExclusive(&g_settings_line_lock);
            }
            if (got >= 1 && gen > g_host_gen) {
                InterlockedExchange(&g_host_gen, gen);
                Log("join: host announced generation %ld (%s)", gen, line);
            }
            if (got == 2 && lobby != g_host_lobby_open) {
                InterlockedExchange(&g_host_lobby_open, lobby);
                Log("join: host's co-op lobby is %s", lobby ? "open" : "closed");
            }
        }
        if (s != INVALID_SOCKET) closesocket(s);
    }
}

// The host tears its session down the moment it quick-loads, before the joiner's poller (2 s) sees the new
// generation; the joiner then got "connection to the server was lost" and, with that dialog up, its mission screen
// stopped ticking, so it never followed. Client vtable slot 3 (lost session, 0x632080: clears the network via
// 0x7286e0, then shows net_lostSession) is replaced for co-op joiners: clear the network only, keep the screen.
const uintptr_t kClientLostSessionSlot = 0x824ec0 + 3 * 4;
const uintptr_t kClientLostSession = 0x632080;
const uintptr_t kClientClearNetwork = 0x7286e0;  // void __fastcall(Client*)
typedef void(__fastcall* LostSessionFn)(void* client);
LostSessionFn g_client_lost_session = nullptr;

void __fastcall LostSessionHook(void* client) {
    if (!g_coop_join || !g_fetch_save) {
        g_client_lost_session(client);
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(kClientClearNetwork)(client);
    Log("join: the host's session ended (host generation %ld, ours %ld): waiting to follow, no error shown",
        g_host_gen, g_loaded_gen);
}

void AssignFileToLoad(const char* name) {
    void* str = MakeFOTString(name);
    reinterpret_cast<void*(__thiscall*)(void*, void**)>(kStringAssign)(reinterpret_cast<void*>(kFileToLoad), &str);
}

// Joiner, main thread, from the window tick: if the host has moved to a newer save, follow it from the world map
// or a mission by loading it the way the game's quick-load does.
// Joiner: set when it starts loading the host's save (first load or a follow), cleared once that lobby has joined.
// Only such a lobby may join: in the transition test an old lobby window from session 1 ticked again when the
// world map opened, looked new, and joined the host's next lobby without loading the new save.
bool g_join_pending = false;
bool g_join_ignore_server = false;  // joiner: on the world map, ignore the old session until it follows
bool g_follow = true;  // [coop] follow=0: never follow the host to a new save (mutation test)

// A joiner plays the host's game: its own load (the Load menu, the quick-load key) would put it in another game, so
// only the host loads. In a mission or on the world map a load sets the file to load (0x8bddb4) and closes the screen
// with 0x10 (WGameMain vtable +0x38, WCampaignWindow +0x34, the base window's 0x6c6260 / 0x6c61f0); the load check
// runs only after the screen is gone, too late. So that close is refused on a joiner unless the file is the host's
// save (following the host closes the same way).
void Notice(const char* fmt, ...);  // below
typedef void(__fastcall* ScreenCloseFn)(void*, void*, int);
ScreenCloseFn g_gamemain_close = nullptr, g_campaign_close = nullptr;

bool RefuseJoinerLoad(int result) {
    if (result != 0x10 || !g_coop_join || g_loaded_gen <= 0) return false;
    const wchar_t* w = *reinterpret_cast<const wchar_t**>(kFileToLoad);
    char name[260] = "";
    if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, name, sizeof(name), nullptr, nullptr);
    if (!name[0] || _stricmp(name, kReceivedName) == 0) return false;
    Log("join: refused this player's load of \"%s\": only the host loads a game in co-op", name);
    Notice("Co-op: only the host can load a game - ask them to load it");
    return true;
}
void __fastcall GameMainCloseHook(void* self, void* edx, int result) {
    if (!RefuseJoinerLoad(result)) g_gamemain_close(self, edx, result);
}
void __fastcall CampaignCloseHook(void* self, void* edx, int result) {
    if (!RefuseJoinerLoad(result)) g_campaign_close(self, edx, result);
}

void JoinerFollowHost(uint8_t* window) {
    // The old world map window keeps ticking during the next mission: only count it while no mission screen exists.
    if (g_coop_join && g_fetch_save && *reinterpret_cast<uintptr_t*>(window) == kCampaignWindowVtable &&
        !g_join_ignore_server && !g_join_pending && *reinterpret_cast<void**>(kGameMainPtr) == nullptr) {
        g_join_ignore_server = true;
        Log("join: on the world map: ignoring the old session until the host's next save");
    }
    if (!g_coop_join || !g_fetch_save || !g_follow || g_host_gen <= g_loaded_gen || g_loaded_gen == 0) return;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(window);
    int close_slot = vt == kCampaignWindowVtable ? 0x34 : vt == kGameMainVtable ? 0x38 : 0;
    if (!close_slot) return;
    static DWORD next_try = 0;
    if (GetTickCount() < next_try) return;
    next_try = GetTickCount() + kJoinPollMs;
    if (!FetchSave()) return;
    AssignFileToLoad(kReceivedName);
    g_join_pending = true;
    g_join_state = 'L';
    g_join_ignore_server = false;
    Log("join: following the host to generation %ld from %s (load \"%s\", close with 0x10)", g_loaded_gen,
        vt == kCampaignWindowVtable ? "the world map" : "the mission", kReceivedName);
    g_world_closing = true;
    reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(window))[close_slot / 4])(window, 0x10);
}

// Host, main thread, once per frame in a mission: re-sync by quick-save then quick-load, when requested.
// State 1: quick-save; state 2: wait until QuickSave.sav is written (async), then quick-load (a new load: new
// generation, co-op lobby, joiners follow).
int g_resync_state = 0;
const char* g_resync_reason = "";
FILETIME g_resync_mark = {};

bool QuickSaveWrittenSince(const FILETIME& mark) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(kQuickSavePath, GetFileExInfoStandard, &a)) return false;
    return CompareFileTime(&a.ftLastWriteTime, &mark) > 0;
}

// quickSave 0x478340 returns without saving while WGameMain+0x15a (a save under way) is set, while the mode +0x156 is
// not 0, 1 or 10, while +0x15c (the saved-notice countdown) runs, or when 0x4782a0 says no: World+0x7c9 set, or no
// squad member passes its vtable+0x480 check. After a conversation the first quick-save was refused (talk tests): ask
// again every 100 frames and log the state that refused it.
void QuickSaveRetry(uint8_t* game_main, int waited, const char* who) {
    if (waited % 100 != 50 || QuickSaveWrittenSince(g_resync_mark)) return;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    Log("%s: quick-save not written after %d frames (saving %d, mode %d, countdown %d, world+0x7c9 %d): asking again", who,
        waited, game_main[0x15a], *reinterpret_cast<int*>(game_main + 0x156), *reinterpret_cast<int*>(game_main + 0x15c),
        world ? world[0x7c9] : -1);
    // What 0x4782a0 checks per squad member (Actor vtable+0x480, 0x56df50): the World+0x513 actor passes; else HP
    // (+0x281) > 0, +0xf96 clear and the FOTString at +0xf8a not the empty one (0x8bc71c).
    uint8_t* begin = world ? *reinterpret_cast<uint8_t**>(world + kWorldPlayers) : nullptr;
    uint8_t* end = world ? *reinterpret_cast<uint8_t**>(world + kWorldPlayers + 4) : nullptr;
    for (uint8_t* pl = begin; pl && pl < end; pl += kPlayerSize) {
        uint8_t* head = *reinterpret_cast<uint8_t**>(pl + 0x4b);
        for (uint8_t* n = head ? *reinterpret_cast<uint8_t**>(head) : nullptr; n && n != head; n = *reinterpret_cast<uint8_t**>(n)) {
            uint16_t id = *reinterpret_cast<uint16_t*>(n + 10);
            uint8_t* e = *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(world + 0x743) + id * 8);
            void* str = *reinterpret_cast<void**>(e + 0xf8a);
            Log("%s:   player %d squad %u: hp %d, +0xf96 %d, +0xf8a %s, World+0x513 %u", who, *reinterpret_cast<int*>(pl + kPlayerId),
                id, *reinterpret_cast<int*>(e + 0x281), e[0xf96],
                str == *reinterpret_cast<void**>(0x8bc71c) ? "empty" : "set", *reinterpret_cast<uint16_t*>(world + 0x513));
        }
    }
    OurQuickSave(game_main);  // save_sync.inc: never a request
}

// Every copy runs its own Server, also a joiner (its single-player lobby's, idle once it joined the host), and a save
// stores that Server's tick numbers (+0x95 last sent, +0x99 next; frame+1 and frame+5 while running). A joiner's
// stayed at its load (1957 for a world at frame 2549), so the host, loading the joiner's save, sent ticks from there
// and every copy waited ~40 s for them to reach the world's frame (talk_join). Before a joiner saves for the host, its
// Server's numbers are set as a running one's would be. (Setting them on the host after the load instead stopped the
// ticks altogether: the clients' state came from the save too.)
void LocalServerTicksToFrame() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    uint8_t* server = static_cast<uint8_t*>(g_session_server);
    if (!world || !server || *reinterpret_cast<uintptr_t*>(server) != kServerVtable) {
        Log("join: no local Server to bring up to date (%p)", server);
        return;
    }
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    Log("join: local Server ticks %d/%d -> %d/%d (world frame %d)", *reinterpret_cast<int*>(server + 0x95),
        *reinterpret_cast<int*>(server + 0x99), frame + 1, frame + 5, frame);
    *reinterpret_cast<int*>(server + 0x95) = frame + 1;
    *reinterpret_cast<int*>(server + 0x99) = frame + 5;
}

bool ShopBusy();  // shop_sync.inc
const DWORD kResyncShopWaitMs = 120000;
bool g_resync_now = false;  // test (shopresync): this re-sync goes ahead with a shop open, as one after the wait does

void HostResyncStep() {
    if (!g_coop_host || g_resync_state == 0) return;
    uint8_t* game_main = *reinterpret_cast<uint8_t**>(kGameMainPtr);
    if (!game_main || *reinterpret_cast<uintptr_t*>(game_main) != kGameMainVtable) return;
    static int waited = 0;
    static DWORD shop_wait = 0;
    if (g_resync_state == 1 && ShopBusy() && !g_resync_now) {
        // The save waits for the shop to close: saved meanwhile, the merchant's own inventory is put aside for the
        // shop's stock (the owner's night: a re-sync quick-saved while the host's shop closed, and the merchant had
        // nothing left after). A player's shop may stay open for a long time, so not for ever.
        if (!shop_wait) {
            shop_wait = GetTickCount();
            Log("host: re-sync (%s): waiting for the shop to close", g_resync_reason);
        }
        if (GetTickCount() - shop_wait < kResyncShopWaitMs) return;
        Log("host: re-sync: the shop is still open after %lu s: saving anyway", kResyncShopWaitMs / 1000);
    }
    if (g_resync_state == 1) {
        shop_wait = 0;
        g_resync_now = false;
        GuardQuickSave();
        GetSystemTimeAsFileTime(&g_resync_mark);
        Log("host: re-sync (%s): quick-save", g_resync_reason);
        OurQuickSave(game_main);  // save_sync.inc: never a request
        g_resync_state = 2;
        waited = 0;
    } else if (g_resync_state == 2) {
        // quickLoad does nothing while WGameMain+0x15c is non-zero (quickSave refuses then too). It counts down after
        // a save (seen at 100). First try called quickLoad at once and nothing happened. Wait for file and countdown.
        int popup = *reinterpret_cast<int*>(game_main + 0x15c);
        static bool popup_logged = false;
        if (popup && !popup_logged) {
            popup_logged = true;
            Log("host: re-sync waiting: WGameMain+0x15c = %d (a countdown, e.g. the saved notice)", popup);
        }
        if (QuickSaveWrittenSince(g_resync_mark) && popup == 0) {
            Log("host: re-sync: quick-save written, pop-up gone after %d frames, quick-load", waited);
            g_resync_state = 3;
            waited = 0;
            g_world_closing = true;
            reinterpret_cast<void(__fastcall*)(void*)>(kQuickLoad)(game_main);
        } else if (QuickSaveRetry(game_main, waited, "host"), ++waited > 1200) {
            Log("host: re-sync FAILED: after %d frames save written=%d, pop-up=%08x", waited,
                QuickSaveWrittenSince(g_resync_mark) ? 1 : 0, popup);
            g_resync_state = 0;
            RestoreQuickSave();
        }
    } else if (g_resync_state == 3) {
        // Still in the same mission long after quick-load was asked for: it was refused, try again.
        if (++waited > 100) {
            Log("host: re-sync: still in the mission 100 frames after quick-load, retrying");
            g_world_closing = false;
            g_resync_state = 2;
            waited = 0;
        }
    } else if (g_resync_state == 5) {
        // A joiner's uploaded game: load it the way a quick-load does (a new load: new generation, joiners follow).
        Log("host: re-sync (%s): loading \"%s\"", g_resync_reason, kUploadName);
        AssignFileToLoad(kUploadName);
        g_resync_state = 6;
        waited = 0;
        g_world_closing = true;
        reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(game_main))[0x38 / 4])(game_main, 0x10);
    } else if (g_resync_state == 6) {
        if (++waited > 100) {
            Log("host: re-sync: still in the mission 100 frames after loading the joiner's save, retrying");
            g_world_closing = false;
            g_resync_state = 5;
        }
    }
}

// Joiner, main thread, once per frame in a mission: after its private screens closed, quick-save (state 1), wait
// for the file (2), then upload it on a thread; the player's own QuickSave.sav is put back once it has been read.
int g_push_state = 0;
void Notice(const char* fmt, ...);  // below

struct Upload {
    char* data;
    long size;
};

DWORD WINAPI UploadThread(LPVOID p) {
    Upload* u = static_cast<Upload*>(p);
    SOCKET s = ConnectHost();
    char req[80], reply[16] = "";
    int rl = sprintf_s(req, "PUT %08x %ld %s\n", g_joiner_id, u->size, g_player_name);
    bool ok = s != INVALID_SOCKET && SendAll(s, req, rl) && SendAll(s, u->data, u->size) &&
              RecvLine(s, reply, sizeof(reply)) && strcmp(reply, "OK") == 0;
    Log("join: %s our save to the host (%ld bytes)", ok ? "uploaded" : "FAILED to upload", u->size);
    if (!ok) Notice("Co-op: could not send your changes to the host");
    if (s != INVALID_SOCKET) closesocket(s);
    free(u->data);
    delete u;
    return 0;
}

void JoinerPushStep() {
    if (!g_coop_join || g_push_state == 0) return;
    uint8_t* game_main = *reinterpret_cast<uint8_t**>(kGameMainPtr);
    if (!game_main || *reinterpret_cast<uintptr_t*>(game_main) != kGameMainVtable) return;
    static int waited = 0;
    if (g_push_state == 1) {
        GuardQuickSave();
        GetSystemTimeAsFileTime(&g_resync_mark);
        Log("join: quick-save for the host");
        LocalServerTicksToFrame();
        OurQuickSave(game_main);  // save_sync.inc: never a request
        g_push_state = 2;
        waited = 0;
        return;
    }
    int popup = *reinterpret_cast<int*>(game_main + 0x15c);
    if (!QuickSaveWrittenSince(g_resync_mark) || popup != 0) {
        QuickSaveRetry(game_main, waited, "join");
        if (++waited > 1200) {
            Log("join: quick-save for the host FAILED: not written after %d frames (pop-up %08x)", waited, popup);
            Notice("Co-op: could not save your changes for the host");
            g_push_state = 0;
            RestoreQuickSave();
        }
        return;
    }
    g_push_state = 0;
    Upload* u = new Upload{nullptr, 0};
    FILE* f = nullptr;
    if (fopen_s(&f, kQuickSavePath, "rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        u->size = ftell(f);
        fseek(f, 0, SEEK_SET);
        u->data = static_cast<char*>(malloc(u->size > 0 ? u->size : 1));
        if (static_cast<long>(fread(u->data, 1, u->size, f)) != u->size) u->size = 0;
        fclose(f);
    }
    RestoreQuickSave();
    if (u->size <= 0) {
        Log("join: could not read the quick-save for the host");
        free(u->data);
        delete u;
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, UploadThread, u, 0, nullptr);
    if (t) CloseHandle(t);
}

// ---- In-mission notices: a line in the game's chat/message display ----
// The game shows "<name> left" through Client::ShowChat 0x6330a0 (__thiscall(Client*, int player id, FOTString* text,
// char team), ret 0xc; called from the player-left handler 0x6361f0). Any thread queues a notice; the main thread
// shows queued notices from the frame hook, as player 1 (the host's colour).
const uintptr_t kClientShowChat = 0x6330a0;
CRITICAL_SECTION g_notice_lock;
char g_notices[8][96];
int g_notice_count = 0;

void Notice(const char* fmt, ...) {
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("notice: %s", text);
    EnterCriticalSection(&g_notice_lock);
    if (g_notice_count < 8) strcpy_s(g_notices[g_notice_count++], text);
    LeaveCriticalSection(&g_notice_lock);
}

void ShowQueuedNotices(uint8_t* client) {  // main thread, in a mission
    char batch[8][96];
    EnterCriticalSection(&g_notice_lock);
    int n = g_notice_count;
    memcpy(batch, g_notices, sizeof(batch));
    g_notice_count = 0;
    LeaveCriticalSection(&g_notice_lock);
    for (int i = 0; i < n; i++) {
        __try {
            void* text = MakeFOTString(batch[i]);
            reinterpret_cast<void(__thiscall*)(void*, int, void**, char)>(kClientShowChat)(client, 1, &text, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("notice: exception 0x%08x showing \"%s\"", GetExceptionCode(), batch[i]);
        }
    }
}

// Host, main thread: notice joiners whose polls stopped (left or lost), once each.
void NoticeLeftJoiners() {
    static bool was_live[8] = {};
    EnterCriticalSection(&g_serve_lock);
    DWORD now = GetTickCount();
    for (int i = 0; i < g_joiner_count; i++) {
        bool live = now - g_joiners[i].last_seen <= kJoinerTimeoutMs;
        if (was_live[i] && !live) Notice("Co-op: %s left", g_joiners[i].name);
        was_live[i] = live;
    }
    LeaveCriticalSection(&g_serve_lock);
}

// Runtime roles, chosen in the Multiplayer menu (or by ftcoop.ini [coop] mode= for the tests).
bool g_poller_started = false;
DWORD WINAPI DiscoveryResponder(LPVOID);  // netlog.cpp
int LocalAddresses(char (*out)[16], int max);  // netlog.cpp

void BecomeHost() {
    g_coop_host = true;
    g_coop_join = false;
    // "[Co-op] <name> @<this PC's addresses>": the session list (DirectPlay's, which reaches the joiners) carries where
    // to find the host; the session record itself holds no address (DirectPlay keeps it). A joiner asks those
    // addresses first (two PCs: "when they hit join there was nothing that happened", owner, 2026-10-27).
    char ips[3][16];
    int n = LocalAddresses(ips, 3);
    int len = sprintf_s(g_session_name, "[Co-op] %s", g_player_name);
    for (int i = 0; i < n; i++) len += sprintf_s(g_session_name + len, sizeof(g_session_name) - len, "%s%s", i ? "," : " @", ips[i]);
    StartFileServer();
    static bool responder = false;
    if (!responder) {
        responder = true;
        CreateThread(nullptr, 0, DiscoveryResponder, nullptr, 0, nullptr);
    }
    Log("co-op: hosting (file port %d). Load a save or start a campaign; players can join at any time.", g_file_port);
}

void BecomeJoiner(const char* address, const char* name) {
    if (address && address[0]) strncpy_s(g_host_address, address, _TRUNCATE);
    if (name && name[0]) strncpy_s(g_player_name, name, _TRUNCATE);
    g_coop_join = true;
    g_coop_host = false;
    g_have_received = false;
    if (!g_joiner_id) g_joiner_id = ((GetTickCount() * 2654435761u) ^ GetCurrentProcessId()) | 1;
    if (g_fetch_save && !g_poller_started) {
        g_poller_started = true;
        CreateThread(nullptr, 0, GenerationPoller, nullptr, 0, nullptr);
    }
    Log("co-op: joining %s as \"%s\" (id %08x)", g_host_address, g_player_name, g_joiner_id);
}

void RequestResync(const char* reason) {
    if (g_coop_host && g_resync_state == 0) {
        g_resync_reason = reason;
        g_resync_state = 1;
    }
}
