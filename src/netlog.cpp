// netlog.dll: campaign co-op for Fallout Tactics (GOG 1.27 BOS.exe only; the hash is checked before anything is
// patched). Loaded by FTSE's ftse.lua (package.loadlib); patches the game in memory, never on disk. It started as a
// research logger of the session messages (hence the name and netlog.txt), and still logs them.
// One translation unit: this file holds the hooks, the menus and the lockstep plumbing, and includes
//   save_transfer.inc  the host's file server: saves, deltas, joiner polls (generation, pings)
//   worldmap_sync.inc  the world map: the host's party, travel, pings, fog
//   bot_scenarios.inc  the test bots and snapshots (test mode), and private_sync.inc (conversations, trades,
//                      recruits: snapshots and deltas) with settings_sync.inc and shop_sync.inc
//   dots.inc           selection dots and ping drawing
//   drift.inc          the drift check and its corrections
//   recruit_sync.inc   the recruit screen on every copy
// The key left of "1" writes a numbered marker to the log so actions can be matched to messages.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)  // mstcpip.h defines it only for newer Windows targets
#endif
#include <iphlpapi.h>
#include <windows.h>
#include <intrin.h>
#include <share.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "session_sync.h"  // what every copy takes from the host at a session start (settings_sync.inc)
#include "talk_locks.h"  // the host's say over who talks to an NPC (private_sync.inc)
#include "drift_judge.h"  // the drift check's decisions (drift.inc)

namespace {

// Addresses for BOS.exe SHA-256 C5B91FE5...4E40 (GOG 1.27). Checked before anything is patched.
const uintptr_t kServerVtableRecvSlot = 0x825260 + 0x10;  // Server vtable, slot 4
const uintptr_t kClientVtableRecvSlot = 0x824ec0 + 0x10;  // Client vtable, slot 4
const uintptr_t kServerRecv = 0x63dbb0;
const uintptr_t kClientRecv = 0x6321a0;
const uint8_t kServerRecvPrologue[] = {0x8b, 0x54, 0x24, 0x04, 0x56, 0x8b, 0x02, 0x3d, 0xcc, 0x00, 0x00, 0x00};
const uint8_t kClientRecvPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x56, 0x8b, 0x10, 0x81, 0xfa, 0xcb, 0x00, 0x00, 0x00};

// Host experiment: attach the game's own DirectPlay TCP network to a running (single-player) session.
const uintptr_t kServerVtable = 0x825260;
const size_t kClientServerPtr = 0x248;   // Client+0x248 = Server*
const size_t kServerMaxPlayers = 0x19;   // passed as DPSESSIONDESC2.dwMaxPlayers; single-player sets 1
const uintptr_t kNewDPTCPNetwork = 0x7288d0;  // void* __cdecl(), allocates a DPTCPNetwork
const uintptr_t kServerAddNetwork = 0x724550; // void __thiscall(Server*, Network*), ret 4

// Co-op host: the single-player start shows an invisible lobby, WDummyGameSetup, whose per-frame Update
// (vtable slot 48) is `this->server(+0xfe)->StartGame(force=1)`. In host mode we attach TCP on its first
// frame and hold the start until the host presses Ctrl+marker, so joiners can connect and ready up.
const uintptr_t kDummySetupUpdateSlot = 0x825074 + 48 * 4;  // = 0x825134
const uintptr_t kDummySetupUpdate = 0x63aab0;               // void __thiscall(WDummyGameSetup*)
const size_t kDummySetupServer = 0xfe;
const size_t kServerPlayerList = 0xdb;
const uintptr_t kServerStartGame = 0x63f0b0;  // void __thiscall(Server*, char force), ret 4; unforced throws if a connected player is not ready  // std::list sentinel; record fields follow next/prev at node+8

// Co-op join: the joiner also uses the single-player Load flow (same save), but on the lobby's first
// frame its Client swaps the local loopback for TCP to the host, joins the host's session and never
// starts locally. The host's 0xcb then closes the lobby and the save loads with the host's seed.
const size_t kDummySetupClient = 0xfa;
const uintptr_t kClientSetNetwork = 0x728640;    // void __thiscall(Client*, Network*, FOTString* address), ret 8
const uintptr_t kClientGetSessions = 0x728710;   // void __thiscall(Client*, GameVector*), ret 4
const uintptr_t kClientConnect = 0x728790;       // void __thiscall(Client*, FOTString* password, void* session), ret 8
const uintptr_t kClientSendPlayerInfo = 0x6347a0;  // void __thiscall(Client*): sends 0x64
const uintptr_t kFOTStringFromChar = 0x6c8bf0;   // FOTString* __thiscall(FOTString*, const char*, uint codepage), ret 8
const uintptr_t kCodepage = 0x82ea44;

// Mission spawn 0x637050 spawns a squad for every player in the session. In single-player data the squad
// comes from the campaign (*0x8bdd2c + 0x5d), so with two players the campaign squad would spawn twice.
// The pass-1 call at 0x637202 is redirected: players other than id 1 get an empty spawn list instead,
// the same way the game spawns a player without data. Same rule on every machine, so lockstep holds.
const uintptr_t kSpawnCallSite = 0x637202;     // e8 rel32 -> 0x4f4fb0
const uintptr_t kWorldSpawnPlayer = 0x4f4fb0;  // char __thiscall(World*, rec*, spawn*, int, int), ret 0x10
const uintptr_t kCampaignPtr = 0x8bdd2c;
const size_t kCampaignSquad = 0x5d;
const size_t kPlayerRecordId = 0x2d;
const uintptr_t kGameNew = 0x6c4dd0;           // void* __cdecl(size_t)

// UI "local player" 0x65af20 (fastcall, ECX = UI object): cached at +0xfe, else the World Player slot whose
// id (+0x31) equals the local Client's id (*(+0xfa) + 0x130). A co-op joiner has no slot of its own, so it
// got null and the game screen crashed (0x4df6e0 reads [null+0x4f]). Reimplemented: if no slot matches and
// we are a co-op joiner, use slot 1 (the host's team), so the joiner sees and shares the host's squad.
const uintptr_t kUiLocalPlayer = 0x65af20;
const uint8_t kUiLocalPlayerPrologue[] = {0x8b, 0x81, 0xfe, 0x00, 0x00, 0x00};  // mov eax,[ecx+0xfe]
const uintptr_t kWorldPtr = 0x8bc564;
const size_t kWorldPlayers = 0x733;   // begin; end at +0x737
const size_t kPlayerSize = 0x15d;
const size_t kPlayerId = 0x31;

// Client tick gate 0x635ce0 only lets ticks run once GetPlayerById(own id) 0x50b880 finds a slot. A joiner
// has none, so its world froze after the first frames. Only this call site is redirected (0x635dbe):
// other callers must keep the strict lookup, e.g. "player left" 0x4f4e10 removes the slot it gets.
const uintptr_t kTickGateGetPlayerCall = 0x635dbe;
const uintptr_t kGetPlayerById = 0x50b880;  // uint8_t* __thiscall(World*, int id), ret 4

// Test bot (ftcoop.ini [test] enabled=1). Loads the save from the main menu the way Ctrl+L quick-load does
// (0x4c9710 -> 0x6c2540), the host starts once every expected player is ready, then on exact simulation
// frames it snapshots the squad, moves one member through the real command path (msg 2 via Client send),
// snapshots again and exits. The launcher compares both copies' snapshots.
const uintptr_t kMainMenuUpdateSlot = 0x82e060 + 48 * 4;  // WMainMenu vtable slot 48 (only this class)
const uintptr_t kWindowUpdateStub = 0x401d20;             // shared no-op Update it points to
const uintptr_t kLoadFileAndPlay = 0x6c2540;              // void __cdecl(app, FOTString* file, 0, 0)
const uintptr_t kAppPtr = 0x8bddb8;
const uintptr_t kQuickLoadFlag = 0x8bdda9;    // set by the main menu's Ctrl+L handler 0x6c0ba0 before loading
// "Intro shown this session": set by the intro playlist 0x6c1890. While it is 0, the main menu constructor
// 0x6bff50 schedules a 1 ms timer that plays intro.bik + ss00.bik (0x6c0970 -> 0x6c1890) on every launch.
// Setting it before the menu is built skips the movies ([ui] skip_intro=1).
const uintptr_t kIntroShownFlag = 0x8bddbf;
const uintptr_t kFrameTickCall = 0x4fc0db;                // world frame loop 0x4fbf50: call Client tick processor
const uintptr_t kClientTickProcessor = 0x635e00;          // uint32_t __thiscall(Client*)
const uintptr_t kClientSend = 0x728840;                   // void __thiscall(Client*, void* msg, int len, int reliable), ret 0xc
const size_t kWorldFrame = 0x2eb;                         // incremented once per loop after the world step
const size_t kWorldEntities = 0x743;                      // entries {void* ptr, i16 next_free, i16 seq}
const size_t kPlayerMembers = 0x4b;                       // std::list {next, prev, EntityID {u16 seq, u16 id}}
const size_t kEntityPos = 0x9a;                           // float x, y (height), z
const size_t kActorHp = 0x281;

struct TestConfig {
    bool enabled;
    char save[260];
    char actor[8];  // "host" or "join": which copy issues the move
    int joiners;
    int timeout_s;
    char scenario[16];
    int soak_frames;
    char menu[8];  // "host" or "join": choose the role through the Multiplayer menu instead of ftcoop.ini
    int index;     // joiner number (1-3) when several joiner copies run: simul orders squad member <index>
    int hold;      // dropin: frames to stay in the joined session (at least 400)
    bool menu_cancel;  // after Start Co-op: open the Load Game screen as a person would, cancel it, then load as usual
    bool inv_chaos;    // trade scenarios: the copy that doesn't trade gives and equips at random meanwhile
    bool grow_table;   // talk scenarios: the talker's entity table grows while its conversation is open
    int serial_skew;   // a joiner: its entity serial counter this far ahead at every session start (a long-run copy)
    bool empty_stock;  // a joiner: the quartermasters' stock list emptied at every session start
    bool no_serial_sync;  // mutation, host: the entity serial is not sent at session start (0.10.5)
    bool resync_in_shop;  // host, trades: a re-sync asked for 3 s into its own shop (it must wait for the close)
    int merchant;         // trades: the trader's NPC id (0: the quartermaster, 96)
    bool gamble;          // trades: Gamble instead of Barter, and the screen's Gamble button instead of Offer
};
TestConfig g_test = {};
bool g_test_load_started = false;
bool g_menu_role_chosen = false;  // the Multiplayer menu's Host or Join (co-op) was pressed
bool g_open_load_after = false;       // main menu: open the Load Game screen (after Start Co-op)
bool g_load_screen_up = false;        // that screen is open (a session played from it runs inside it too)
const DWORD kJoinPollMs = 250;        // a joiner's checks with the host (generation, save ready, session up): LAN-cheap
char g_session_name[112] = "[Co-op]";  // host: the DirectPlay session's name, also the discovery reply
void* MakeFOTString(const char* s);

#pragma pack(push, 1)
struct MoveMsg {  // msg 2 with one 27-byte ControllerCommandStruct, as the UI sends a move
    uint32_t type;
    uint16_t count;
    uint8_t cmd;     // 0 = move
    uint16_t actor;  // entity id
    uint32_t e1;
    float x, y, z;
    uint32_t e2;
    int32_t param;   // UI moves always had low byte 4; upper bytes varied
};
#pragma pack(pop)
static_assert(sizeof(MoveMsg) == 33, "move message is 33 bytes in the logs");

struct SpawnList {  // {flag, std::list sentinel, count} as built at 0x63722e
    uint32_t flag;
    void** head;
    uint32_t count;
};

struct GameVector {  // MSVC 6 std::vector as the game uses it: begin at +4, end at +8
    void* allocator;
    void** begin;
    void** end;
    void** capacity;
};

typedef void(__thiscall* RecvFn)(void* self, const uint8_t* msg, int len, int sender);
typedef void(__thiscall* UpdateFn)(void* self);
RecvFn g_server_recv = nullptr;
RecvFn g_client_recv = nullptr;
UpdateFn g_dummy_update = nullptr;
UpdateFn g_mainmenu_update = nullptr;
volatile LONG g_start_requested = 0;
bool g_coop_host = false;     // ftcoop.ini [coop] mode=host
bool g_coop_join = false;     // ftcoop.ini [coop] mode=join, host=<ip>
bool g_share_squad = true;    // ftcoop.ini [coop] share_squad=0 turns the shared-squad fix off (mutation test)
char g_host_address[64] = "127.0.0.1";
void* g_hosted_server = nullptr;
void* g_session_server = nullptr;  // host: the running session's Server (kept after the lobby, unlike the above)
void* g_join_client = nullptr;
bool g_join_network_set = false;
bool g_joined = false;
DWORD g_next_join_try = 0;

CRITICAL_SECTION g_lock;
FILE* g_log = nullptr;
DWORD g_start = 0;
int g_seen[1024];  // per message type, how many were logged in full

void Log(const char* fmt, ...) {
    EnterCriticalSection(&g_lock);
    if (g_log) {
        fprintf(g_log, "%9.3f ", (GetTickCount() - g_start) / 1000.0);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_lock);
}

void Hex(char* out, size_t cap, const uint8_t* p, int n) {
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < cap; i++) {
        o += sprintf_s(out + o, cap - o, "%02x%s", p[i], (i % 4 == 3) ? " " : "");
    }
    out[o] = 0;
}

// Types sent every tick are logged in full only a few times; the rest always.
bool ShouldLogFull(uint32_t type, int len) {
    if (type == 0xcc && len <= 8) return false;  // empty command batch
    if (type == 1 && len > 32) return true;      // tick that carries commands: always
    int limit = (type == 1 || type == 3 || type == 0xcc) ? 20 : 100000;
    int idx = type < 1024 ? (int)type : 1023;
    return g_seen[idx]++ < limit;
}

void LogMessage(const char* dir, const uint8_t* msg, int len, int sender) {
    __try {
        uint32_t type = len >= 4 ? *(const uint32_t*)msg : 0xffffffff;
        if (!ShouldLogFull(type, len)) return;
        char hex[3 * 160 + 64];
        int show = len < 128 ? len : 128;
        Hex(hex, sizeof(hex), msg, show);
        Log("%s type=%u(0x%x) len=%d from=%d | %s%s", dir, type, type, len, sender, hex, len > show ? " ..." : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("%s unreadable message len=%d", dir, len);
    }
}

// Server command check (msg 2, 0x63dce0): a command is accepted only if Player[entity+0x163]+0x2d equals
// the sender's network id. All squad units are in slot 1 (the host's), so co-op players' commands are
// passed on as if sent by slot 1's owner: a shared squad.
const size_t kPlayerNetId = 0x2d;

int SquadOwnerNetId() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return 0;
    uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
    uint8_t* end = *reinterpret_cast<uint8_t**>(world + kWorldPlayers + 4);
    if (!begin || (end - begin) / kPlayerSize < 2) return 0;
    return *reinterpret_cast<int*>(begin + 1 * kPlayerSize + kPlayerNetId);
}

// Server player record node (std::list at Server+0xdb): node+8 present, +0xd in game (set by StartGame
// 0x63f0b0), +0xf connected, +0x31 network id. A player-setup message (0x64, handler 0x63e120) copies the
// client's 0x37-byte record over node+8.., which clears the in-game byte. A joiner resends 0x64 right after
// the start; if that lands before the "loaded" reports (0xcc), the all-loaded check 0x63e090 ignores it and
// the server sends two partial 0xcc messages: each client then runs mission spawn twice and throws XRuntime
// (seen in automated runs). Keep the in-game byte across a 0x64 for a player already in game.
uint8_t* FindServerRecord(uint8_t* server, int net_id) {
    uint8_t* head = *reinterpret_cast<uint8_t**>(server + kServerPlayerList);
    int guard = 0;
    for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && guard < 16;
         node = *reinterpret_cast<uint8_t**>(node), guard++) {
        if (*reinterpret_cast<int*>(node + 0x31) == net_id) return node;
    }
    return nullptr;
}

bool HasTalkCommand(const uint8_t* msg, int len);  // private screens, below
extern volatile DWORD g_remote_talk_tick;

int HostFilterTalks(uint8_t* msg, int len, int sender);  // private_sync.inc
void CoopFromNetwork(const uint8_t* msg, int len, bool server_side);  // private_sync.inc

void __fastcall ServerRecvHook(void* self, void* /*edx*/, const uint8_t* msg, int len, int sender) {
    LogMessage("C->S", msg, len, sender);
    if (g_coop_host && len >= 4 && *reinterpret_cast<const uint32_t*>(msg) == 100) {
        uint8_t* node = FindServerRecord(static_cast<uint8_t*>(self), sender);
        uint8_t in_game = node ? node[0xd] : 0;
        g_server_recv(self, msg, len, sender);
        if (node && in_game && !node[0xd]) {
            node[0xd] = in_game;
            Log("host: kept player %d in game across a player-setup message", sender);
        }
        return;
    }
    if (g_coop_host) len = HostFilterTalks(const_cast<uint8_t*>(msg), len, sender);  // one talker per NPC
    if (g_coop_host) CoopFromNetwork(msg, len, true);  // a talker's delta: applied now (private_sync.inc)
    if (g_coop_host && HasTalkCommand(msg, len) && sender != SquadOwnerNetId()) {
        g_remote_talk_tick = GetTickCount();  // a joiner's conversation: the host closes its copy (private screens)
        Log("host: player %d started a conversation", sender);
    }
    if (g_coop_host && g_share_squad && len >= 4 && *reinterpret_cast<const uint32_t*>(msg) == 2) {
        int owner = SquadOwnerNetId();
        if (owner != 0 && sender != owner) {
            static int logged = 0;
            if (logged++ < 5) Log("host: command from co-op player %d passed on as squad owner %d", sender, owner);
            sender = owner;
        }
    }
    g_server_recv(self, msg, len, sender);
}

// Main thread. Attach the game's DirectPlay TCP network to a Server whose session has started.
bool AttachTcp(uint8_t* server) {
    __try {
        if (!server || *reinterpret_cast<uintptr_t*>(server) != kServerVtable) {
            Log("host: %p is not a Server, not hosting", server);
            return false;
        }
        int old_max = *reinterpret_cast<int*>(server + kServerMaxPlayers);
        *reinterpret_cast<int*>(server + kServerMaxPlayers) = 5;
        // Session name (Server+0x11, passed to the network's host call with max players +0x19, password +0x15):
        // "[Co-op] <name>", so joiners see it in the Multiplayer screen's session list.
        void* session_name = MakeFOTString(g_session_name);
        reinterpret_cast<void*(__thiscall*)(void*, void**)>(0x6c8e80)(server + 0x11, &session_name);  // FOTString assign
        void* net = reinterpret_cast<void*(__cdecl*)()>(kNewDPTCPNetwork)();
        Log("host: server %p max players %d -> 5, session \"%s\", DPTCPNetwork %p, calling AddNetwork", server, old_max,
            g_session_name, net);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kServerAddNetwork)(server, net);
        Log("host: AddNetwork returned, TCP network attached");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("host: exception 0x%08x while attaching TCP network", GetExceptionCode());
        return false;
    }
}

// Logs the server's player records (node+8 onwards) whenever they change, to learn the flag meanings.
void LogPlayerRecords(uint8_t* server) {
    static char last[2048];
    char now[2048];
    size_t o = 0;
    __try {
        uint8_t* head = *reinterpret_cast<uint8_t**>(server + kServerPlayerList);
        int n = 0;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && n < 8;
             node = *reinterpret_cast<uint8_t**>(node), n++) {
            char hex[3 * 0x37 + 16];
            Hex(hex, sizeof(hex), node + 8, 0x37);
            o += sprintf_s(now + o, sizeof(now) - o, "\n      rec%d %s", n, hex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o += sprintf_s(now + o, sizeof(now) - o, " (unreadable)");
    }
    now[o] = 0;
    if (strcmp(now, last) != 0) {
        strcpy_s(last, now);
        Log("host: player records:%s", now);
    }
}

char __fastcall SpawnPlayerHook(void* world, void* /*edx*/, uint8_t* record, void* spawn, int a3, int a4) {
    typedef char(__thiscall * SpawnFn)(void*, uint8_t*, void*, int, int);
    SpawnFn original = reinterpret_cast<SpawnFn>(kWorldSpawnPlayer);
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(kCampaignPtr);
    int id = *reinterpret_cast<int*>(record + kPlayerRecordId);
    if (campaign && spawn == campaign + kCampaignSquad && id != 1) {
        // An empty spawn still fails: SpawnPlayer also assigns a team (0x4f6fc0), and a single-player save
        // has no team slot for a second human. For now the joiner is a spectator: report success, spawn nothing.
        Log("spawn: player %d skipped (spectator, no campaign squad), reporting success", id);
        return 1;
    }
    char ok = original(world, record, spawn, a3, a4);
    Log("spawn: player %d spawned normally -> %d", id, ok);
    return ok;
}

// The original trusts its cache at ui+0xfe. The World's player array is rebuilt during level load, so a
// lookup made before the load finished left a dangling pointer: both copies crashed in 0x49f600 reading the
// member list of freed memory when the bot started fast. Always rescan (8 slots) and refresh the cache.
// "Is this player me" 0x6368a0 (thiscall, ECX = Client, the player slot on the stack, ret 4): the Client's id (+0x130)
// against the slot's (+0x31). Only the game bar's turn buttons ask it (end turn 0x477a80, toggle 0x477bd0, 0x4778f0),
// with the slot whose group has the turn: a co-op joiner plays the shared squad in slot 1 (as UiLocalPlayerHook), so
// its End Turn was dropped before it was sent. Replaced: slot 1 is also the joiner's.
const uintptr_t kIsLocalPlayer = 0x6368a0;
const uint8_t kIsLocalPlayerPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0};  // mov eax,[esp+4]; test eax,eax

bool __fastcall IsLocalPlayerHook(uint8_t* client, void*, uint8_t* player) {
    if (!player) return false;
    if (*reinterpret_cast<int*>(player + kPlayerId) == *reinterpret_cast<int*>(client + 0x130)) return true;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    return g_coop_join && g_share_squad && world && player == *reinterpret_cast<uint8_t**>(world + kWorldPlayers) + kPlayerSize;
}

uint8_t* __fastcall UiLocalPlayerHook(uint8_t* ui) {
    *reinterpret_cast<uint8_t**>(ui + 0xfe) = nullptr;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return nullptr;
    uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
    uint8_t* end = *reinterpret_cast<uint8_t**>(world + kWorldPlayers + 4);
    if (!begin) return nullptr;
    size_t count = (end - begin) / kPlayerSize;
    int my_id = *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(ui + 0xfa) + 0x130);
    // A co-op joiner always plays the shared squad in slot 1, even when its id matches a slot of its own: a save
    // from the host can carry a player record with the joiner's id and no units, and the exit-grid check 0x47a680
    // walks the local player's units, so an empty squad counted as "everyone at the exit" and ended the mission.
    if (g_coop_join && g_share_squad && count > 1) {
        uint8_t* slot = begin + 1 * kPlayerSize;
        *reinterpret_cast<uint8_t**>(ui + 0xfe) = slot;
        return slot;
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t* slot = begin + i * kPlayerSize;
        if (*reinterpret_cast<int*>(slot + kPlayerId) == my_id) {
            *reinterpret_cast<uint8_t**>(ui + 0xfe) = slot;
            return slot;
        }
    }
    if (g_coop_join && count > 1) {
        uint8_t* slot = begin + 1 * kPlayerSize;
        *reinterpret_cast<uint8_t**>(ui + 0xfe) = slot;
        static int logged_id = -1;  // this runs on every UI lookup now (no cache): log once per id
        if (logged_id != my_id) {
            logged_id = my_id;
            Log("ui: local player id %d has no slot, sharing slot 1 (host's team)", my_id);
        }
        return slot;
    }
    return nullptr;
}

uint8_t* __fastcall TickGateGetPlayerHook(uint8_t* world, void* /*edx*/, int id) {
    uint8_t* slot = reinterpret_cast<uint8_t*(__thiscall*)(uint8_t*, int)>(kGetPlayerById)(world, id);
    if (slot || !g_coop_join) return slot;
    uint8_t* begin = *reinterpret_cast<uint8_t**>(world + kWorldPlayers);
    uint8_t* end = *reinterpret_cast<uint8_t**>(world + kWorldPlayers + 4);
    if (!begin || (end - begin) / kPlayerSize < 2) return nullptr;
    return begin + 1 * kPlayerSize;  // co-op joiner: run ticks as part of the host's team
}

bool PatchJmp(uintptr_t site, const uint8_t* expect, size_t n, void* hook) {
    uint8_t* p = reinterpret_cast<uint8_t*>(site);
    if (n < 5 || memcmp(p, expect, n) != 0) return false;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xe9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// Jump from `fn` to `hook`; returns a trampoline that runs the `n` bytes overwritten (whole instructions, none
// relative) and jumps back after them, or nullptr when the bytes there are not `prologue`.
uint8_t* HookWithTrampoline(uintptr_t fn, const uint8_t* prologue, size_t n, void* hook) {
    if (n < 5 || n > 16 || memcmp(reinterpret_cast<void*>(fn), prologue, n) != 0) return nullptr;
    uint8_t* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    memcpy(t, reinterpret_cast<void*>(fn), n);
    t[n] = 0xe9;
    *reinterpret_cast<int32_t*>(t + n + 1) = static_cast<int32_t>((fn + n) - reinterpret_cast<uintptr_t>(t + n + 5));
    return PatchJmp(fn, prologue, n, hook) ? t : nullptr;
}

// The game's network close (0x759960, DPTCPNetwork, vtable 0x7fd098) ends with IDirectPlay->Release() on its
// interface (+0x1c): mov ecx,[eax]; push eax; call [ecx+8]. DirectPlay's TCP provider (dpwsockx) waits there about
// 3 s for one of its own threads (dpwsockx+0x7eca: WaitForSingleObject(thread, INFINITE)), and the game's main thread
// waited with it. Only the Multiplayer screen's network does that (its session search; Start Co-op and Join close
// it); a session's own network releases in 30-160 ms (resync). Sampled 2026-10-28: the main thread in dplayx ->
// dpwsockx -> WaitForSingleObject, from 0x75997c, the whole 3 s. The release now runs on a thread of its own, so the
// game goes on (the waiting message, the save's download); creating the next DirectPlay object still waits for it
// (dplayx serializes the two), so a joiner's session starts no sooner. Stopping the search first (EnumSessions
// DPENUMSESSIONS_STOPASYNC) was refused (E_INVALIDARG) and changed nothing.
const uintptr_t kNetCloseRelease = 0x759976;
const uint8_t kNetCloseReleaseBytes[] = {0x8b, 0x08, 0x50, 0xff, 0x51, 0x08};

DWORD WINAPI ReleaseDirectPlay(LPVOID p) {
    DWORD t0 = GetTickCount();
    __try {
        IUnknown* u = static_cast<IUnknown*>(p);
        u->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("co-op: exception 0x%08x releasing DirectPlay in the background", GetExceptionCode());
        return 0;
    }
    Log("co-op: DirectPlay released in the background (%lu ms the game no longer waits)", GetTickCount() - t0);
    return 0;
}

// The Multiplayer screen's network (its session search) is the one whose release takes 3 s, and even released on a
// thread of its own, the next DirectPlay object waits for it. So when Start Co-op or Join closes that screen, its
// release is skipped: the object is left alive (once per Start Co-op or Join), and the next one starts at once.
DWORD g_forget_release_until = 0;

ULONG __stdcall NetCloseReleaseHook(IUnknown* p) {  // in place of the Release call: same arguments, callee pops
    if (g_forget_release_until && GetTickCount() < g_forget_release_until) {
        g_forget_release_until = 0;
        Log("co-op: the Multiplayer screen's DirectPlay is left alive (its release would hold everything up 3 s)");
        return 0;
    }
    HANDLE t = CreateThread(nullptr, 0, ReleaseDirectPlay, p, 0, nullptr);
    if (t) {
        CloseHandle(t);
        return 0;
    }
    return p->Release();
}

bool PatchNetCloseRelease() {  // push eax; call NetCloseReleaseHook (6 bytes, as the original's)
    uint8_t* p = reinterpret_cast<uint8_t*>(kNetCloseRelease);
    if (memcmp(p, kNetCloseReleaseBytes, sizeof(kNetCloseReleaseBytes)) != 0) return false;
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0x50;
    p[1] = 0xe8;
    *reinterpret_cast<int32_t*>(p + 2) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&NetCloseReleaseHook) - (kNetCloseRelease + 6));
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}

bool PatchCall(uintptr_t site, uintptr_t expect_target, void* hook) {
    uint8_t* p = reinterpret_cast<uint8_t*>(site);
    if (p[0] != 0xe8 || site + 5 + *reinterpret_cast<int32_t*>(p + 1) != expect_target) return false;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void* MakeFOTString(const char* s) {
    void* str = nullptr;
    reinterpret_cast<void*(__thiscall*)(void**, const char*, unsigned)>(kFOTStringFromChar)(
        &str, s, *reinterpret_cast<unsigned*>(kCodepage));
    return str;
}

#include "worldmap_sync.inc"
#include "save_transfer.inc"

// Main thread, from the joiner's single-player lobby. Returns true once connected to the host.
bool JoinHost(uint8_t* client) {
    __try {
        if (!g_join_network_set) {
            void* address = MakeFOTString(g_host_address);
            void* net = reinterpret_cast<void*(__cdecl*)()>(kNewDPTCPNetwork)();
            Log("join: client %p, switching to DPTCPNetwork %p for host %s", client, net, g_host_address);
            reinterpret_cast<void(__thiscall*)(void*, void*, void**)>(kClientSetNetwork)(client, net, &address);
            g_join_network_set = true;
        }
        GameVector sessions = {};
        reinterpret_cast<void(__thiscall*)(void*, GameVector*)>(kClientGetSessions)(client, &sessions);
        int count = sessions.begin ? static_cast<int>(sessions.end - sessions.begin) : 0;
        Log("join: %d session(s) found", count);
        if (count == 0) return false;
        void* password = MakeFOTString("");
        reinterpret_cast<void(__thiscall*)(void*, void**, void*)>(kClientConnect)(client, &password, sessions.begin[0]);
        reinterpret_cast<void(__thiscall*)(void*)>(kClientSendPlayerInfo)(client);
        Log("join: connected to the host's session, waiting for the host to start");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("join: exception 0x%08x", GetExceptionCode());
        return false;
    }
}

// Server player records (node+8): byte 0 present, byte 1 ready, byte 7 connected.
int CountReadyPlayers(uint8_t* server) {
    int n = 0;
    __try {
        uint8_t* head = *reinterpret_cast<uint8_t**>(server + kServerPlayerList);
        int guard = 0;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && guard < 16;
             node = *reinterpret_cast<uint8_t**>(node), guard++) {
            uint8_t* rec = node + 8;
            if (rec[0] && rec[1] && rec[7]) n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

// Loading from inside the menu's Update hung: Update runs inside the window tick (0x6c5130 walks the child
// list) while the load tears the menu down and runs a whole game nested. The real Ctrl+L runs from a key
// event instead. The game's pump 0x784920 never calls DispatchMessage: it peeks its own window's messages
// and hands each to 0x7857f0 (thiscall(app, hwnd, msg, wp, lp, time), ret 0x14). That call (0x7849cc) is
// hooked: Update posts a private message, and the load runs when the pump delivers it, like a key press.
const UINT kBotLoadMessage = WM_APP + 0x51;
const UINT kBotMultiplayerMessage = WM_APP + 0x52;  // test: press the main menu's Multiplayer button
const UINT kOpenLoadMessage = WM_APP + 0x53;        // after Start Co-op: open the Load Game screen
const uintptr_t kSinglePlayerLoad = 0x4ca7b0;       // Single Player > Load: __thiscall(menu, int unused): dialog,
                                                    // then load; it pops its int (ret 4): called without one, our
                                                    // frame was 4 bytes off and leaving a session to the menu
                                                    // failed the stack check (crash 0xc0000409, 2026-10-27)
const uintptr_t kPumpHandlerCall = 0x7849cc;
const uintptr_t kPumpHandler = 0x7857f0;
const uintptr_t kMainMenuMultiplayer = 0x6c0c10;  // the Multiplayer button: __thiscall(menu, 1), ret 4 (via 0x6c1ba0)
volatile HWND g_pump_hwnd = nullptr;
char g_load_name[260] = "";  // the save the posted load opens
void* g_main_menu = nullptr;

// Joiner, after a co-op Join: a message box on the main menu while waiting for the host's lobby. The game's message
// box 0x6602b0 (__cdecl(parent window, FOTString* text), a modal WBOSMessageBox, vtables 0x827ce0 / 0x827ce8) keeps
// the main menu from ticking, so the box's own tick (WaitPopupTick, from the window-tick hook) fetches the host's
// save once the lobby is open and ends the box (vtable +0x34, as the screens' buttons do); the pump then loads the
// save. Clicking OK first stops waiting: co-op join is turned off.
const UINT kWaitPopupMessage = WM_APP + 0x54;
const uintptr_t kMessageBox = 0x6602b0;
bool g_wait_popup_pending = false;
volatile bool g_wait_popup_up = false;
char g_wait_session[128] = "";

void LoadReceivedSave() {  // pump context, main menu: load the host's save as the joiner
    strcpy_s(g_load_name, kReceivedName);
    g_test_load_started = true;
    g_join_pending = true;
    g_join_state = 'L';
    *reinterpret_cast<uint8_t*>(kQuickLoadFlag) = 1;
    Log("co-op: loading the host's save \"%s\"", g_load_name);
    void* file = MakeFOTString(g_load_name);
    reinterpret_cast<void(__cdecl*)(void*, void**, int, int)>(kLoadFileAndPlay)(*reinterpret_cast<void**>(kAppPtr), &file, 0, 0);
    Log("co-op: game session returned to the main menu");
}

const char* RttiName(void* obj);  // bot_scenarios.inc

// ---- Private screens: conversations (WSpeech) and what they lead to: buying/selling, recruiting, squad changes ----
// Talking to someone is a lockstep command (0x17, runtime run 2), so the conversation opens on every copy. What
// follows changes the game only on the copy that shows it and sends nothing (runs 2 and 3; owner 2026-10-21: the
// copies went out of step once party members changed). In co-op the screens belong to the player who started the
// conversation: the copy that sent the 0x17 keeps them, every other copy closes the conversation at once (vtable
// +0x34). A conversation nobody asked for (a script's) stays on the host. When the last private screen has closed,
// that copy's game is the right one: a host re-syncs from it (save, send, everyone reloads); a joiner saves, uploads
// the save to the host, and the host loads it and re-syncs everyone from it.
// Matched by RTTI name: recruiting showed WSingleRecruit / WRecruitItem ticking, never WRecruit (the RTTI vtable
// guessed first), which has a tick of its own; the shared window tick hook only sees the children.
enum PrivateKind { kNotPrivate, kSpeechScreen, kTradeScreen };
volatile DWORD g_local_talk_tick = 0;   // this copy's player sent a talk command (0x17)
volatile DWORD g_remote_talk_tick = 0;  // host: a joiner sent one
bool g_private_open = false;            // a private screen of ours is open
DWORD g_private_seen = 0;               // its last tick
int g_private_frame = 0;                // the world's frame when it opened (no frames run while they are up)
uint8_t* g_private_speech = nullptr;    // the open conversation window (the bot closes it)
uint8_t* g_client_ptr = nullptr;        // the local Client (frame hook)
const DWORD kPrivateClosedMs = 300;     // no private screen ticked for this long: closed
bool g_talker_hold = false;             // this copy holds its own world while its private screens are up
bool g_private_recruit = false;         // the recruit screen was among them (private_sync.inc: a full re-sync)
// [coop] shop_no_pause (experimental): a conversation with anyone but a recruiter pauses nobody. Its talker's world
// keeps running (the conversation's own pause, World+0x2db via 0x4fc800, is swallowed on that copy) and no other copy
// holds.
bool g_shop_no_pause = false;
// The Escape menu (WGameMenu, constructor 0x4817b0, destructor 0x4820f0) and the save screen (WSaveLoad, 0x4bcfa0 /
// 0x4bdc10) raise the pause counter on their own copy only, as in single player. Its world stopped and, with its
// ticks, every other copy's (savemenu: host and joiner both at frame 1995 for the 15 s the host sat in the menu; no
// drift came of it). In co-op they pause nothing: the world runs on under the menu on every copy, as it does under a
// shop (owner: no global pauses for routine actions). [coop] menu_pause=1 keeps the old pause (mutation test). The
// drift after a save came from the write itself (save_sync.inc).
bool g_menu_pause = false;
int g_menu_pauses_swallowed = 0;  // menu pauses not passed on, so their unpauses are not passed on either

bool MenuPauseCaller(uintptr_t from) {
    return (from >= 0x4817b0 && from < 0x4820eb) || (from >= 0x4820f0 && from < 0x482165) ||
           (from >= 0x4bcfa0 && from < 0x4bdc0d) || (from >= 0x4bdc10 && from < 0x4bdda4);
}
// A conversation with a quartermaster who is not a recruiter (shop_no_pause): nobody waits. Kept apart for this
// player's own conversation and the last other player's, which can overlap (one shops while another talks).
bool g_my_conv_light = false;
bool g_other_conv_light = false;
bool g_suppress_pause = false;          // this copy swallows pause-counter changes (its light conversation is up)
int g_suppressed = 0;                   // their balance: +1 per pause swallowed, -1 per unpause
uint8_t* g_pause_trampoline = nullptr;  // the game's pause-counter change, 0x4fc800 (__thiscall(World*, bool), ret 4)

void __fastcall PauseHook(void* world, void* /*edx*/, int on) {
    if (MenuPauseCaller(reinterpret_cast<uintptr_t>(_ReturnAddress()))) {
        bool coop = g_coop_join || (g_coop_host && LiveJoiners() > 0);
        if ((on & 0xff) && coop && !g_menu_pause) {
            g_menu_pauses_swallowed++;
            Log("co-op: the game menu's pause is not passed on: the world runs on under it (%d)", g_menu_pauses_swallowed);
            return;
        }
        if (!(on & 0xff) && g_menu_pauses_swallowed > 0) {
            g_menu_pauses_swallowed--;
            return;
        }
    }
    if (g_suppress_pause) {
        g_suppressed += (on & 0xff) ? 1 : -1;
        Log("co-op: swallowed the game's %s (balance %d)", (on & 0xff) ? "pause" : "resume", g_suppressed);
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(g_pause_trampoline)(world, on);
}

// The mod's own holds: past the hook, never swallowed.
void WorldPause(void* world, bool on) {
    reinterpret_cast<void(__thiscall*)(void*, int)>(g_pause_trampoline ? g_pause_trampoline : reinterpret_cast<uint8_t*>(0x4fc800))(
        world, on ? 1 : 0);
}
const DWORD kTalkWindowMs = 10000;      // a conversation opening this soon after a talk command is that command's

PrivateKind PrivateKindOf(uint8_t* w) {
    static uintptr_t vts[128];
    static PrivateKind kinds[128];
    static int n = 0;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(w);
    for (int i = 0; i < n; i++)
        if (vts[i] == vt) return kinds[i];
    const char* name = RttiName(w);
    PrivateKind k = strstr(name, "WSpeech") ? kSpeechScreen
                    : (strstr(name, "Barter") || strstr(name, "Recruit") || strstr(name, "SquadWindow") ||
                       strstr(name, "SquadSelection") || strstr(name, "WCharacter")) ? kTradeScreen : kNotPrivate;
    if (n < 128) {
        vts[n] = vt;
        kinds[n++] = k;
    }
    return k;
}

bool HasTalkCommand(const uint8_t* msg, int len) {  // msg 2 = {u32 type, u16 count, count x 27-byte command}
    if (len < 6 || *reinterpret_cast<const uint32_t*>(msg) != 2) return false;
    int count = *reinterpret_cast<const uint16_t*>(msg + 4);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len; i++)
        if (msg[6 + i * 27] == 0x17) return true;
    return false;
}

// Client send 0x728840 (char __thiscall(Client*, void* msg, int len, int reliable), ret 0xc): note this copy's own
// talk commands. First 5 bytes (mov ecx, [ecx+4]; test ecx, ecx) go to a trampoline that jumps back after them.
const uintptr_t kClientSendFn = 0x728840;
const uint8_t kClientSendPrologue[] = {0x8b, 0x49, 0x04, 0x85, 0xc9};
uint8_t* g_send_trampoline = nullptr;

bool RefuseLockedTalk(uint8_t* msg, int len);  // private_sync.inc
void SendTalkClaim(void* client, const uint8_t* msg, int len);  // private_sync.inc

int RefuseShopperOrders(uint8_t* msg, int len);  // shop_sync.inc
int RefuseDuringLevelUp(uint8_t* msg, int len);  // recruit_sync.inc
bool LevelLockedScreen(uint8_t* w);              // recruit_sync.inc
void LevelScreenOpened(uint8_t* w);

// The loot screen (WLoot, in WSquadEquip) opens when a character reaches the container its use command (0x04) named, on
// every copy, since every copy runs every command (owner, 2026-10-23: "when two players have the same player selected
// and one of them opens a inventory both screens open the inventory screen"). It belongs to the player who sent that
// command: this copy notes its own (here); a loot screen opening with none of ours pending is closed (PrivateScreenTick).
// A later move, attack or talk order for that character is a change of mind: the use is no longer pending.
const DWORD kUseWindowMs = 120000;  // a walk to a container across the map
DWORD g_my_use_tick = 0;
uint16_t g_my_use_actor = 0;

void NoteMyUses(const uint8_t* msg, int len) {
    if (len < 6 || *reinterpret_cast<const uint32_t*>(msg) != 2) return;
    int count = *reinterpret_cast<const uint16_t*>(msg + 4);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len; i++) {
        const uint8_t* r = msg + 6 + i * 27;
        uint16_t actor;
        memcpy(&actor, r + 1, 2);
        // 0x21 opens the same screen for a character in a vehicle (the owner's driver: every try at their inventory
        // was taken for another player's search and closed, 0.10.5).
        if (r[0] == 0x04 || r[0] == 0x21) {
            g_my_use_tick = GetTickCount();
            g_my_use_actor = actor;
        } else if ((r[0] == 0x00 || r[0] == 0x02 || r[0] == 0x11 || r[0] == 0x17) && actor == g_my_use_actor) {
            g_my_use_tick = 0;
        }
    }
}

char __fastcall ClientSendHook(void* self, void* /*edx*/, const uint8_t* msg, int len, int reliable) {
    if (g_test.enabled && len >= 6 && *reinterpret_cast<const uint32_t*>(msg) == 2) {  // every command this player sends
        int count = *reinterpret_cast<const uint16_t*>(msg + 4);
        for (int i = 0; i < count && 6 + (i + 1) * 27 <= len; i++) {
            const uint8_t* r = msg + 6 + i * 27;
            if (r[0] == 0x30) continue;  // the mod's own records
            uint16_t actor, a2, a3, b2, b3;
            uint32_t param;
            memcpy(&actor, r + 1, 2);
            memcpy(&a2, r + 3, 2);
            memcpy(&a3, r + 5, 2);
            memcpy(&b2, r + 19, 2);
            memcpy(&b3, r + 21, 2);
            memcpy(&param, r + 23, 4);
            Log("cmd: sent type 0x%02x actor %u args %u %u, %u %u, param %08x | %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                r[0], actor, a2, a3, b2, b3, param, r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17], r[18]);
        }
    }
    if (g_coop_host || g_coop_join) {
        len = RefuseShopperOrders(const_cast<uint8_t*>(msg), len);
        if (!len) return 1;  // nothing left: not sent
        len = RefuseDuringLevelUp(const_cast<uint8_t*>(msg), len);
        if (!len) return 1;
        NoteMyUses(msg, len);
    }
    if ((g_coop_host || g_coop_join) && HasTalkCommand(msg, len) && RefuseLockedTalk(const_cast<uint8_t*>(msg), len))
        return 1;  // a talk to an NPC another player is talking to: not sent
    if ((g_coop_host || g_coop_join) && HasTalkCommand(msg, len)) {  // only clients send type 2 (servers: type 1)
        g_local_talk_tick = GetTickCount();
        Log("co-op: this player started a conversation");
        SendTalkClaim(self, msg, len);  // first, so the host relays our claim ahead of the talk
    }
    return reinterpret_cast<char(__fastcall*)(void*, void*, const uint8_t*, int, int)>(g_send_trampoline)(
        self, nullptr, msg, len, reliable);
}

// World map: only the host chooses where the squad goes (owner, 2026-10-21: "on the map lets just have the host
// allowed to move"). A click on a location ends in GoToMap 0x6b6580 (__thiscall(WCampaignWindow*, FOTString* map,
// int mode), ret 8; first 6 bytes mov eax, fs:[0] go to a trampoline). On a joiner it is refused with a notice; the
// joiner follows the host's choice when the host's mission starts (JoinerFollowHost).
const uintptr_t kGoToMapFn = 0x6b6580;
const uint8_t kGoToMapPrologue[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};
uint8_t* g_gotomap_trampoline = nullptr;

void __fastcall GoToMapHook(void* window, void* /*edx*/, void** map, int mode) {
    if (g_coop_join) {
        Log("co-op: refused this player's world map choice: only the host chooses where to go");
        Notice("Co-op: only the host chooses where to go - wait for them");
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*, void*, void**, int)>(g_gotomap_trampoline)(window, nullptr, map, mode);
}

// The conversation is a mode of the mission screen: WGameMain::SetMode 0x471ad0 (__thiscall(WGameMain*, int mode),
// mode at +0x156) builds each mode's windows (12: the conversation, 0x4cf4b0; 6/7: barter, 0x453e00) and closes the
// previous mode's. Closing the conversation window alone (+0x34) left mode 12 set, and quickSave refuses outside modes
// 0, 1 and 10 (talk tests: never written). So a conversation is ended the game's way, SetMode(0), from the mission
// screen's own tick (it ticks after its children, the conversation among them).
const uintptr_t kGameMainSetMode = 0x471ad0;
bool g_end_game_mode = false;
void TakePrivateSnapshot();  // private_sync.inc
void SetFullDiff();          // private_sync.inc
void HostHoldForOther();     // private_sync.inc
void HostHoldTimeout();      // private_sync.inc
extern uint16_t g_private_npc, g_local_talk_npc;
extern bool g_claim_mine;  // private_sync.inc: the last relayed talk claim, and when it came
extern DWORD g_claim_tick;

// A loot screen ticking: kept if this player sent the use that opened it, else closed (the mission screen's mode
// ended, as for another player's conversation). Decided once per screen.
void BotLootTake(uint8_t* w);  // bot_scenarios.inc
DWORD g_chaos_search_until = 0;  // chaos: a forced search waits for its loot screen
int g_loot_mode = 0;              // loot test: what BotLootTake takes (bot_scenarios.inc)

void LootScreenTick(uint8_t* w, DWORD now) {
    static uintptr_t loot_vt = 0;
    static uint8_t* decided = nullptr;
    static bool mine = false;
    static DWORD seen = 0, opened = 0;
    if (!loot_vt) {
        if (!strstr(RttiName(w), "WLoot")) return;
        loot_vt = *reinterpret_cast<uintptr_t*>(w);
    }
    if (*reinterpret_cast<uintptr_t*>(w) != loot_vt) return;
    if (w != decided || now - seen > 1000) {  // a new loot screen (a window can come back at the same address)
        decided = w;
        opened = now;
        mine = g_my_use_tick && now - g_my_use_tick < kUseWindowMs;
        if (mine) {
            g_my_use_tick = 0;
            Log("co-op: loot screen opened for this player (actor %u)", g_my_use_actor);
        } else {
            Log("co-op: loot screen opened for another player's search: closing it here");
            Notice("Co-op: another player is searching that");
        }
    }
    seen = now;
    if (!mine) g_end_game_mode = true;
    static uint8_t* took = nullptr;  // chaos: the bot takes something as the screen's buttons do, then closes
    bool bot_loots = g_test.enabled && (_stricmp(g_test.scenario, "chaos") == 0 || _stricmp(g_test.scenario, "loot") == 0);
    if (mine && bot_loots && opened && now - opened > 700 && took != w) {
        took = w;
        g_chaos_search_until = 0;
        BotLootTake(w);
    }
    if (mine && bot_loots && now - opened > 1500) {
        if (opened) Log("bot: CHAOS closing the loot screen");
        opened = 0;
        g_end_game_mode = true;  // the bot's own search: looked, closed
    }
}

int ConversationOpened(uint8_t* w);  // private_sync.inc

void PrivateScreenTick(uint8_t* w) {  // window-tick hook
    if (!g_coop_host && !g_coop_join) return;
    HostHoldTimeout();
    if (g_end_game_mode && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        g_end_game_mode = false;
        Log("co-op: ending mission-screen mode %d", *reinterpret_cast<int*>(w + 0x156));
        reinterpret_cast<void(__thiscall*)(void*, int)>(kGameMainSetMode)(w, 0);
        return;
    }
    if (g_private_open && *reinterpret_cast<uintptr_t*>(w) == kGameMainVtable) {
        // A mode other than the plain mission screen (0, 1, 10: the modes quickSave accepts) is still part of the
        // conversation (barter 6/7 and the rest): windows like WRecruit tick on their own, not through this hook.
        int mode = *reinterpret_cast<int*>(w + 0x156);
        if (mode != 0 && mode != 1 && mode != 10) g_private_seen = GetTickCount();
        return;
    }
    PrivateKind kind = PrivateKindOf(w);
    DWORD now = GetTickCount();
    // A quest giver opens the Pipboy (its mission briefing) instead of a conversation, on every copy (the talk runs
    // everywhere). Opened right after a relayed talk claim, it is that conversation: the talker's own, everyone
    // else's closed with a notice. The Pipboy opened from its button is left alone.
    static uint8_t* talk_pipboy = nullptr;
    if (kind == kNotPrivate && (w == talk_pipboy ||
                                (!g_private_open && g_claim_tick && now - g_claim_tick < kTalkWindowMs && strstr(RttiName(w), "WPipboy")))) {
        if (w != talk_pipboy) Log("co-op: the Pipboy opened from a talk (a quest giver's briefing)");
        talk_pipboy = w;
        kind = kSpeechScreen;
    }
    if (kind == kNotPrivate) {
        LootScreenTick(w, now);
        return;
    }
    if (!g_private_open && strstr(RttiName(w), "WCharacter") && LevelLockedScreen(w)) {
        g_end_game_mode = true;  // another player is levelling up this character: our screen of them closes
        return;
    }
    if (!g_private_open) {
        bool mine = kind == kTradeScreen || now - g_local_talk_tick < kTalkWindowMs ||
                    (g_coop_host && now - g_remote_talk_tick >= kTalkWindowMs);  // nobody's: a script's, the host's
        // A conversation a talk claim decided (private_sync.inc): the claim's winner, whoever else talked too.
        if (kind == kSpeechScreen && g_claim_tick && now - g_claim_tick < kTalkWindowMs) mine = g_claim_mine;
        // The conversation's own NPC decides when it has a lock: two players' talks at once (two claims, one lock
        // each) left only the last claim to go by (owner, two PCs). Other talks still pending for the same character
        // lost: their locks are freed (ConversationOpened, private_sync.inc).
        if (kind == kSpeechScreen && strstr(RttiName(w), "WSpeech")) {
            int owner = ConversationOpened(w);
            if (owner >= 0) mine = owner == 1;
        }
        if (!mine) {
            static uint8_t* closed = nullptr;
            if (closed != w) {
                closed = w;
                Log("co-op: %s opened for another player's conversation: closing it here", RttiName(w));
                if (w == talk_pipboy) Notice("Co-op: another player got a mission briefing - it is in your Pipboy");
                if (!g_other_conv_light) HostHoldForOther();  // every other copy waits on this frame (private_sync.inc)
            }
            g_end_game_mode = true;
            return;
        }
        g_private_open = true;
        g_private_recruit = false;
        if (strstr(RttiName(w), "WCharacter")) {  // the character screen (a level-up), opened without a conversation
            SetFullDiff();
            g_my_conv_light = false;
            LevelScreenOpened(w);  // the others: not this character's screen, not the recruiter, until it closes
        }
        g_local_talk_tick = 0;
        Log("co-op: %s opened for this player", RttiName(w));
        g_private_npc = g_local_talk_npc;
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        g_private_frame = world ? *reinterpret_cast<int*>(world + kWorldFrame) : 0;
        TakePrivateSnapshot();  // compared when the screens close: what the other copies need
        // This copy's world waits too, as every other copy's does, until the diff is made: it used to run one frame
        // after the screens closed and before the diff, while the others ran that frame only after applying the delta,
        // and NPCs walking in that frame then went their own way on this copy (recruit_host, 3 players).
        if (world && !g_talker_hold && !g_my_conv_light) {
            if (g_test.enabled)
                Log("co-op: pause counter World+0x2db = %d at the conversation (before this copy's hold)",
                    *reinterpret_cast<int*>(world + 0x2db));
            WorldPause(world, true);  // the pause counter, World+0x2db
            g_talker_hold = true;
        }
    }
    g_private_seen = now;
    if (kind == kSpeechScreen) g_private_speech = w;
    if (kind == kTradeScreen && strstr(RttiName(w), "Recruit")) g_private_recruit = true;
}

// Main thread, every game frame: the private screens have closed; true if the other copies must now get our game.
void ReleaseTalkerHold() {  // after the diff: this copy's world goes on with everyone else's
    if (!g_talker_hold) return;
    g_talker_hold = false;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world) WorldPause(world, false);
}

bool PrivateScreensClosed(bool* alone) {
    *alone = false;
    if (!g_private_open) return false;
    // Closed: the mission screen is back to its plain modes and no private screen has ticked for a moment. The world
    // is held (g_talker_hold), so no frame has run since: the talker's changes are exactly what the screens did.
    if (GetTickCount() - g_private_seen < kPrivateClosedMs) return false;
    g_private_open = false;
    g_private_speech = nullptr;
    bool others = g_coop_join || LiveJoiners() > 0;
    Log("co-op: private screens closed%s", others ? ", bringing the other players up to date" : ", nobody else here");
    *alone = !others;
    return true;
}

// A load (a re-sync, the host's save, a quick-load) closes every screen: whatever they did is gone with the old
// world. Without this the next frame took the load for the screens closing, measured the new world against the old
// snapshot, and uploaded it for a second, third reload (shopresync; the owner's night, sessions 6 to 8).
void DropPrivateScreens() {
    if (g_private_open) Log("co-op: a load closed this player's private screens: nothing to send");
    g_private_open = false;
    g_private_speech = nullptr;
    g_private_recruit = false;
    g_talker_hold = false;  // the hold was on the old world
    g_my_conv_light = g_other_conv_light = false;
    g_suppress_pause = false;
    g_suppressed = 0;
}

void WaitPopupTick(uint8_t* w) {
    if (!g_wait_popup_up) return;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(w);
    if (vt != 0x827ce8 && vt != 0x827ce0) return;
    static DWORD next = 0;
    if (GetTickCount() < next) return;
    next = GetTickCount() + kJoinPollMs;
    if (!g_have_received && g_host_lobby_open) g_have_received = FetchSave();
    if (g_have_received) {
        Log("co-op: the host's game is ready: closing the waiting message");
        reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(w))[0x34 / 4])(w, 4);
    }
}

uint32_t __fastcall PumpHandlerHook(void* app, void* /*edx*/, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, DWORD time) {
    g_pump_hwnd = hwnd;
    if (msg == kBotMultiplayerMessage && g_main_menu) {
        Log("bot: pressing Multiplayer on the main menu");
        reinterpret_cast<void(__thiscall*)(void*, int)>(kMainMenuMultiplayer)(g_main_menu, 1);
        Log("bot: back on the main menu after Multiplayer");
        return 0;
    }
    if (msg == kWaitPopupMessage && g_main_menu) {
        char text[256];
        sprintf_s(text, "Joining %s. Waiting for the host's game... (OK stops waiting)", g_wait_session);
        void* str = MakeFOTString(text);
        Log("co-op: showing the waiting message");
        g_wait_popup_up = true;
        reinterpret_cast<void(__cdecl*)(void*, void**)>(kMessageBox)(g_main_menu, &str);
        g_wait_popup_up = false;
        if (g_have_received) {
            LoadReceivedSave();
        } else {
            Log("co-op: the player stopped waiting: co-op join off");
            g_coop_join = false;
        }
        return 0;
    }
    if (msg == kOpenLoadMessage && g_main_menu) {
        Log("co-op: opening the Load Game screen for the co-op host");
        g_load_screen_up = true;  // the menu still ticks under it: no bot load meanwhile
        reinterpret_cast<void(__thiscall*)(void*, int)>(kSinglePlayerLoad)(g_main_menu, 0);
        g_load_screen_up = false;
        Log("co-op: back on the main menu after the Load Game screen");
        return 0;
    }
    if (msg == kBotLoadMessage) {
        *reinterpret_cast<uint8_t*>(kQuickLoadFlag) = 1;  // as the main menu's Ctrl+L handler does
        Log("co-op: loading \"%s\" from the message pump (as Ctrl+L quick-load does)", g_load_name);
        void* file = MakeFOTString(g_load_name);
        reinterpret_cast<void(__cdecl*)(void*, void**, int, int)>(kLoadFileAndPlay)(
            *reinterpret_cast<void**>(kAppPtr), &file, 0, 0);
        Log("bot: game session returned to the main menu");
        return 0;
    }
    return reinterpret_cast<uint32_t(__thiscall*)(void*, HWND, UINT, WPARAM, LPARAM, DWORD)>(kPumpHandler)(
        app, hwnd, msg, wp, lp, time);
}

// A window in the background gets no messages, so the pump hook may not have seen the game window yet:
// fall back to this process's top-level window titled "Fallout: Tactics (TM)" (the pump peeks that window).
BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    char title[64] = "";
    GetWindowTextA(hwnd, title, sizeof(title));
    if (pid == GetCurrentProcessId() && strcmp(title, "Fallout: Tactics (TM)") == 0) {
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
    }
    return TRUE;
}

bool PostToGameWindow(UINT msg) {
    HWND hwnd = g_pump_hwnd;
    if (!hwnd) EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
    if (!hwnd) return false;
    return PostMessageA(hwnd, msg, 0, 0) != 0;
}

// Runs every main-menu frame. A co-op joiner waits here until the host holds a co-op lobby, then fetches the
// host's save and loads it (so it never loads a session that already started). The test bot also loads its
// save from here, and in menu tests first presses Multiplayer.
void __fastcall MainMenuUpdateHook(void* self, void* /*edx*/) {
    g_main_menu = self;
    static DWORD first_seen = 0;
    if (!first_seen) first_seen = GetTickCount();
    bool settled = GetTickCount() - first_seen >= 1500;  // let the menu finish appearing first
    static bool menu_posted = false;
    if (settled && g_open_load_after && PostToGameWindow(kOpenLoadMessage)) {
        g_open_load_after = false;
    } else if (settled && g_wait_popup_pending && PostToGameWindow(kWaitPopupMessage)) {
        g_wait_popup_pending = false;
        g_wait_popup_up = true;  // from now until the box closes: the box, not this menu, fetches and loads
        Log("co-op: main menu: the waiting message goes up");
    } else if (settled && g_test.enabled && g_test.menu[0] && !g_menu_role_chosen && !menu_posted) {
        menu_posted = PostToGameWindow(kBotMultiplayerMessage);
    } else if (settled && !g_test_load_started && !g_load_screen_up) {
        const char* load = nullptr;
        if (g_coop_join && g_fetch_save) {  // a joiner only ever loads the host's save
            static DWORD next_fetch = 0;
            if (g_wait_popup_up) {
                // the waiting message fetches and loads
            } else if (!g_have_received && g_host_lobby_open && GetTickCount() >= next_fetch) {
                next_fetch = GetTickCount() + kJoinPollMs;
                g_have_received = FetchSave();
            }
            if (g_have_received && !g_wait_popup_up) load = kReceivedName;  // the host's save, not our own choice
        } else if (g_test.enabled && _stricmp(g_test.scenario, "watch") != 0 && (!g_test.menu[0] || g_menu_role_chosen)) {
            load = g_test.save;
        }
        if (load) {
            strcpy_s(g_load_name, load);
            if (PostToGameWindow(kBotLoadMessage)) {
                g_test_load_started = true;
                if (g_coop_join) {
                    g_join_pending = true;
                    g_join_state = 'L';
                }
                Log("co-op: main menu is up, posting the load of \"%s\"", g_load_name);
            }
        }
    }
    g_mainmenu_update(self);
}

DWORD g_lobby_opened_tick = 0;  // host: when the current co-op lobby opened
void* g_join_window = nullptr;   // joiner: the lobby window being joined (cleared at each session start)
volatile LONG g_sessions_started = 0;  // S->C 0xce ("go") count: one per session (map, world map, load)
volatile LONG g_snap_tag = 0;  // save generation of the current co-op session (0 = solo): tags SNAP lines
bool g_session_coop = false;  // host: the current session came from a co-op lobby
DWORD g_main_thread_id = 0;
#include "bot_scenarios.inc"
#include "dots.inc"
#include "drift.inc"
#include "recruit_sync.inc"
#include "save_sync.inc"


// On a hang, log where the game's main thread is: EIP and BOS.exe return addresses on its stack.
void LogMainThreadLocation() {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_thread_id);
    if (!t) return;
    SuspendThread(t);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(t, &c)) {
        char chain[512];
        size_t o = 0;
        uint32_t* sp = reinterpret_cast<uint32_t*>(c.Esp);
        for (int i = 0, found = 0; i < 2000 && found < 16; i++) {
            uint32_t v;
            __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (v >= 0x401000 && v < 0x7fc000) {
                o += sprintf_s(chain + o, sizeof(chain) - o, " %08x", v);
                found++;
            }
        }
        chain[o] = 0;
        Log("bot: main thread at eip=%08x, stack:%s", c.Eip, chain);
    }
    ResumeThread(t);
    CloseHandle(t);
}

// On a hang, an error dialog is the usual cause: log the text of every window this process owns.
BOOL CALLBACK LogChildText(HWND hwnd, LPARAM) {
    char text[512] = "";
    if (GetWindowTextA(hwnd, text, sizeof(text)) > 0) Log("bot:   control text: %s", text);
    return TRUE;
}

BOOL CALLBACK LogOwnWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    char title[256] = "", cls[64] = "";
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    Log("bot: window \"%s\" class %s", title, cls);
    EnumChildWindows(hwnd, LogChildText, 0);
    return TRUE;
}

DWORD WINAPI TestWatchdog(LPVOID) {
    Sleep(g_test.timeout_s * 1000);
    Log("bot: TIMEOUT after %d s", g_test.timeout_s);
    LogMainThreadLocation();
    EnumWindows(LogOwnWindow, 0);
    if (g_log) fflush(g_log);
    ExitProcess(3);
}

// ---- Multiplayer menu (WMultiSetup, built by 0x4a84a0): Start Co-op, and Join that understands co-op sessions ----
// The original Host (0x4a9a20: Create Game box, DirectPlay server, deathmatch lobby) and Join (0x4a9820: join the
// session picked in the list) stay for normal multiplayer. The mod adds a "Start Co-op" button: it turns hosting on,
// closes the screen (any result but 4 returns from the multiplayer loop 0x637720 to the main menu) and opens the
// Load Game screen there. The host's DirectPlay session is named "[Co-op] <name>", so it shows in every joiner's
// session list. Join on a co-op session does the co-op join instead of the original; on any other session it runs
// the original Join. DirectPlay does not tell the joiner the host's address, so the joiner asks over UDP (port
// [coop] file_port): to the IP field's address, loopback and the LAN broadcast; the host's reply carries its session
// name, and its sender address is the host. Fields: IP text FOTString at +0x135, name at +0x139, client at +0x100,
// session list at +0x125 (selected item at list+0x10f, its session record at item+0x134). Session records
// (DPTCPNetwork enumeration callback 0x759c70): 0x85 bytes each, in [network+0x90, network+0x94), network =
// client+4: +0 vtable (0x833768), +4 type name ("DPNetwork"), +8 session name FOTString, +0xc password flag,
// +0xd current players, +0x11 max players, +0x15 dwUser1 (game state).
const uintptr_t kMultiJoinButton = 0x4a9820;
const uint8_t kMultiButtonPrologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
const uintptr_t kMultiSetupVtable = 0x805228;
const uintptr_t kMultiSetupCtor = 0x4a84a0;      // __thiscall(this, parent, client), ret 8
const uintptr_t kMultiSetupCtorCall = 0x637767;  // in the multiplayer loop 0x637720
const uintptr_t kOperatorNew = 0x6c4dd0;         // void* __cdecl(size)
const uintptr_t kMakeCallback = 0x4ac1d0;        // void* __thiscall(owner, storage*, handler, 0), ret 0xc
const uintptr_t kUnregisterCallback = 0x6c7d40;  // void __thiscall(owner, storage*): drop a callback from the owner list
const uintptr_t kButtonCtor = 0x65bcb0;          // __thiscall(button, parent, int pos[2], callback, FOTString*, style), ret 0x14
const uintptr_t kLoadGameButton = 0x4ca7b0;      // Single Player > Load: __thiscall(menu, int unused), ret 4: dialog, then load
const size_t kMultiSetupIp = 0x135, kMultiSetupName = 0x139, kMultiSetupClient = 0x100, kMultiSetupList = 0x125;
const char kCoopPrefix[] = "[Co-op]";
const size_t kSessionRecordName = 8;

uint8_t* g_join_trampoline = nullptr;  // the original Join's first 5 bytes, then a jmp back

void FOTStringToAnsi(uint8_t* field, char* out, size_t cap) {
    out[0] = 0;
    __try {
        const wchar_t* w = *reinterpret_cast<const wchar_t**>(field);
        if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, out, static_cast<int>(cap), nullptr, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
    out[cap - 1] = 0;
}

void CloseMultiSetup(uint8_t* w) {
    reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(w))[0x34 / 4])(w, 2);
}

void ResolveAddress(const char* in, char* out, size_t cap) {  // IPv4 text or a host name (e.g. a VPN machine name)
    out[0] = 0;
    if (!in[0]) return;
    in_addr a;
    if (inet_pton(AF_INET, in, &a) == 1) {
        strcpy_s(out, cap, in);
        return;
    }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(in, nullptr, &hints, &res) == 0 && res) {
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, out, cap);
        freeaddrinfo(res);
    } else {
        Log("menu: could not resolve \"%s\"", in);
    }
}

// Every IPv4 network this PC is on, as its directed broadcast address (192.168.1.255 for 192.168.1.x/24). The limited
// broadcast 255.255.255.255 leaves through one adapter only, which Windows picks: with a virtual one (Hyper-V/WSL,
// VirtualBox, a VPN) it often never reaches the LAN, and two PCs with no firewall still did not find each other
// (owner, 2026-10-27: "when they hit join there was nothing that happened"; one PC worked through 127.0.0.1).
int LanBroadcasts(char (*out)[16], int max) {
    ULONG size = 16 * 1024;
    static uint8_t buf[64 * 1024];
    IP_ADAPTER_ADDRESSES* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf);
    size = sizeof(buf);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             list, &size) != NO_ERROR)
        return 0;
    int n = 0;
    for (IP_ADAPTER_ADDRESSES* a = list; a && n < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength > 30) continue;
            uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            uint32_t mask = u->OnLinkPrefixLength ? 0xffffffffu << (32 - u->OnLinkPrefixLength) : 0;
            in_addr b;
            b.s_addr = htonl(ip | ~mask);
            inet_ntop(AF_INET, &b, out[n++], 16);
        }
    }
    return n;
}

// This PC's IPv4 addresses for others to reach it: adapters that are up, with a default gateway first; no loopback, no
// link-local (169.254).
int LocalAddresses(char (*out)[16], int max) {
    static uint8_t buf[64 * 1024];
    IP_ADAPTER_ADDRESSES* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf);
    ULONG size = sizeof(buf);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR)
        return 0;
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (IP_ADAPTER_ADDRESSES* a = list; a && n < max; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if ((a->FirstGatewayAddress != nullptr) != (pass == 0)) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                in_addr ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
                if ((ntohl(ip.s_addr) >> 16) == 0xa9fe) continue;  // 169.254: no address from the network
                inet_ntop(AF_INET, &ip, out[n++], 16);
            }
        }
    return n;
}

// Joiner: ask co-op hosts for themselves over UDP: the address typed in, the addresses in the session's name (the
// host's, see BecomeHost), this PC, every network's broadcast. want:
// the session name picked; a host that answers with another name is taken if it is the only one that answered (the
// session list and the host's own name can differ).
bool DiscoverCoopHost(const char* address, const char* want, char* out_ip, size_t cap) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    DWORD timeout = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    // A query to an address where nothing listens (127.0.0.1 on any PC but the host's) comes back as ICMP "port
    // unreachable", and Windows then fails the socket's next recvfrom at once with WSAECONNRESET. The old loop took
    // that as "no more answers": every round ended in no time, before the host's answer came (owner, two PCs: "no
    // co-op host answered" in the same millisecond as the search). Off, and each round listens for its full time.
    BOOL connreset = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &connreset, sizeof(connreset), nullptr, 0, &got, nullptr, nullptr);
    char bcast[16][16], named[4][16];
    int nb = LanBroadcasts(bcast, 16), nn = 0;
    if (const char* at = strstr(want, " @")) {  // "[Co-op] <name> @a,b,c"
        for (const char* p = at + 2; *p && nn < 4;) {
            int k = 0;
            while (*p && *p != ',' && k < 15) named[nn][k++] = *p++;
            named[nn][k] = 0;
            in_addr test;
            if (k && inet_pton(AF_INET, named[nn], &test) == 1) nn++;
            if (*p == ',') p++;
        }
    }
    const char* targets[28] = {address};
    int nt = 1;
    for (int i = 0; i < nn; i++) targets[nt++] = named[i];
    targets[nt++] = "127.0.0.1";
    targets[nt++] = "255.255.255.255";
    for (int i = 0; i < nb && nt < 28; i++) targets[nt++] = bcast[i];
    char sent_to[400] = "";
    for (int i = 0; i < nt; i++)
        if (targets[i] && targets[i][0]) {
            strcat_s(sent_to, " ");
            strcat_s(sent_to, targets[i]);
        }
    Log("join: looking for co-op hosts at%s (port %d)", sent_to, g_file_port);
    bool found = false, recv_error_logged = false;
    char other_ip[64] = "", other_name[96] = "";
    int others = 0;
    for (int round = 0; round < 5 && !found; round++) {
        for (int ti = 0; ti < nt; ti++) {
            const char* t = targets[ti];
            if (!t || !t[0]) continue;
            sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(static_cast<u_short>(g_file_port));
            inet_pton(AF_INET, t, &to.sin_addr);
            sendto(s, "FTCOOP?\n", 8, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        if (g_test.enabled) {  // as on a PC without the host: one query where nothing listens (an ICMP "unreachable")
            sockaddr_in dead = {};
            dead.sin_family = AF_INET;
            dead.sin_port = htons(static_cast<u_short>(g_file_port + 98));
            inet_pton(AF_INET, "127.0.0.1", &dead.sin_addr);
            sendto(s, "FTCOOP?\n", 8, 0, reinterpret_cast<sockaddr*>(&dead), sizeof(dead));
        }
        char buf[160];
        DWORD round_end = GetTickCount() + 300;
        while (!found && static_cast<LONG>(round_end - GetTickCount()) > 0) {
            sockaddr_in from = {};
            int fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0) {
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT && !recv_error_logged) {
                    recv_error_logged = true;
                    Log("join: discovery receive error %d (going on listening)", err);
                }
                if (err != WSAETIMEDOUT) Sleep(10);
                continue;
            }
            buf[n] = 0;
            char* nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            if (strncmp(buf, "FTCOOP! ", 8) != 0) continue;
            if (want[0] && strcmp(buf + 8, want) != 0) {  // another name: kept in case it is the only host
                char ip[64];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                if (strcmp(ip, other_ip) != 0) {
                    others += other_ip[0] ? 1 : 0;
                    if (!other_ip[0]) others = 1;
                    strcpy_s(other_ip, ip);
                    strcpy_s(other_name, buf + 8);
                }
                continue;
            }
            inet_ntop(AF_INET, &from.sin_addr, out_ip, cap);
            Log("join: co-op host \"%s\" answered from %s", buf + 8, out_ip);
            found = true;
            break;
        }
    }
    closesocket(s);
    if (!found && others == 1) {
        strcpy_s(out_ip, cap, other_ip);
        Log("join: no host named \"%s\", but one co-op host answered: \"%s\" at %s", want, other_name, other_ip);
        found = true;
    }
    return found;
}

// Host: answer joiners' "FTCOOP?" with "FTCOOP! <session name>".
DWORD WINAPI DiscoveryResponder(LPVOID) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(g_file_port));
    a.sin_addr.s_addr = INADDR_ANY;
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        Log("host: discovery could not listen on UDP %d (error %d)", g_file_port, WSAGetLastError());
        return 0;
    }
    Log("host: answering co-op discovery on UDP %d as \"%s\"", g_file_port, g_session_name);
    for (;;) {
        char buf[64];
        sockaddr_in from = {};
        int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0 || strncmp(buf, "FTCOOP?", 7) != 0) continue;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        static char last_ip[64] = "";
        static DWORD last_at = 0;
        if (strcmp(ip, last_ip) != 0 || GetTickCount() - last_at > 5000)  // one line per joiner's search, not per probe
            Log("host: a joiner at %s is looking for the host; answering", ip);
        strcpy_s(last_ip, ip);
        last_at = GetTickCount();
        char reply[160];
        int rl = sprintf_s(reply, "FTCOOP! %s\n", g_session_name);
        sendto(s, reply, rl, 0, reinterpret_cast<sockaddr*>(&from), fl);
    }
}

uint8_t* SelectedSession(uint8_t* w) {  // the session record picked in the list, or nullptr
    __try {
        uint8_t* list = *reinterpret_cast<uint8_t**>(w + kMultiSetupList);
        uint8_t* item = list ? *reinterpret_cast<uint8_t**>(list + 0x10f) : nullptr;
        return item ? *reinterpret_cast<uint8_t**>(item + 0x134) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Co-op join: find the host's address (UDP), set the role, back to the main menu, which loads the host's save.
// session: the co-op session's name to match, or "" (no session picked: the first host that answers).
bool JoinCoop(uint8_t* w, const char* session) {
    char field[64], address[64], name[32], ip[64] = "";
    FOTStringToAnsi(w + kMultiSetupIp, field, sizeof(field));
    FOTStringToAnsi(w + kMultiSetupName, name, sizeof(name));
    ResolveAddress(field, address, sizeof(address));
    if (!DiscoverCoopHost(address, session, ip, sizeof(ip))) {
        Log("menu: Join: no co-op host answered (session \"%s\", address field \"%s\")", session, field);
        Notice("Co-op: the host did not answer - type the host's address in the IP box and press Join again");
        return false;
    }
    Log("menu: Multiplayer > Join: co-op session \"%s\" at %s, name \"%s\"", session, ip, name);
    BecomeJoiner(ip, name);
    strcpy_s(g_wait_session, session[0] ? session : ip);
    if (char* at = strstr(g_wait_session, " @")) *at = 0;  // the name, without the host's addresses
    g_wait_popup_pending = true;  // the main menu shows "waiting for the host" until the save arrives
    g_menu_role_chosen = true;
    g_forget_release_until = GetTickCount() + 5000;  // NetCloseReleaseHook: this screen's release is skipped
    CloseMultiSetup(w);
    return true;
}

// Button handlers are __thiscall(owner, int arg), ret 4: the button pushes one argument (0x7053ea: ecx = owner +
// adjust, push [callback+0x14], call handler). As __fastcall the arg is the third parameter, popped on return.
void __fastcall MultiJoinButtonHook(uint8_t* w, void* /*edx*/, int arg) {
    uint8_t* rec = SelectedSession(w);
    char session[128] = "";
    if (rec) FOTStringToAnsi(rec + kSessionRecordName, session, sizeof(session));
    if (rec && strncmp(session, kCoopPrefix, sizeof(kCoopPrefix) - 1) == 0) {
        JoinCoop(w, session);
        return;
    }
    if (!rec) {  // nothing picked: try the address field as a co-op host (the list may not show a VPN host)
        char field[64];
        FOTStringToAnsi(w + kMultiSetupIp, field, sizeof(field));
        if (field[0] && JoinCoop(w, "")) return;
    }
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(g_join_trampoline)(w, nullptr, arg);  // normal join
}

void __fastcall StartCoopButton(uint8_t* w, void* /*edx*/, int /*arg*/) {
    char name[32];
    FOTStringToAnsi(w + kMultiSetupName, name, sizeof(name));
    if (name[0]) strncpy_s(g_player_name, name, _TRUNCATE);
    Log("menu: Multiplayer > Start Co-op (name \"%s\")", g_player_name);
    BecomeHost();
    g_menu_role_chosen = true;
    // The test bot loads its own save from the main menu instead (not in watch mode, where a person plays).
    g_open_load_after = !g_test.enabled || _stricmp(g_test.scenario, "watch") == 0 || g_test.menu_cancel;
    g_forget_release_until = GetTickCount() + 5000;  // NetCloseReleaseHook: this screen's release is skipped
    CloseMultiSetup(w);
}

void AddStartCoopButton(uint8_t* w) {
    __try {
        // As the screen's constructor does for its own buttons: a temporary callback {owner, handler, 0, 0, 0}
        // (0x4ac1d0 registers it in the owner's callback list via 0x6c7cf0), the button keeps its own copy, then
        // the temporary is unregistered (0x6c7d40(owner, &temporary)). Leaving it registered crashed on click.
        uint32_t storage[8] = {};
        void* button = reinterpret_cast<void*(__cdecl*)(size_t)>(kOperatorNew)(0x15b);
        void* callback = reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kMakeCallback)(
            w, storage, (void*)&StartCoopButton, 0);
        void* text = MakeFOTString("Start Co-op");
        int pos[2] = {280, 390};  // above Host (280, 436)
        reinterpret_cast<void*(__thiscall*)(void*, void*, int*, void*, void**, int)>(kButtonCtor)(
            button, w, pos, callback, &text, 0);
        if (storage[0]) reinterpret_cast<void(__thiscall*)(void*, void*)>(kUnregisterCallback)(
            reinterpret_cast<void*>(storage[0]), storage);
        Log("menu: Start Co-op button added to the Multiplayer screen");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("menu: exception 0x%08x adding the Start Co-op button", GetExceptionCode());
    }
}

void* __fastcall MultiSetupCtorHook(uint8_t* self, void* /*edx*/, void* parent, void* client) {
    void* r = reinterpret_cast<void*(__thiscall*)(void*, void*, void*)>(kMultiSetupCtor)(self, parent, client);
    AddStartCoopButton(self);
    return r;
}

// The screen enables Join only while a listed session is selected (0x4a9cd0: list at +0x125, Join button at
// +0x131, enable 0x6d1350(button, on); flag at button+0x102). Join is kept enabled: with nothing picked it tries the
// address field as a co-op host.
const size_t kMultiSetupJoinButton = 0x131;
const uintptr_t kWidgetEnable = 0x6d1350;  // void __thiscall(widget, char enabled)
bool g_mp_menu = true;  // [coop] mp_menu

// Test: the joiner's enumerated sessions (names), to see the host's co-op session the way the list does.
bool FindListedCoopSession(uint8_t* w, char* out, size_t cap) {
    out[0] = 0;
    __try {
        uint8_t* client = *reinterpret_cast<uint8_t**>(w + kMultiSetupClient);
        uint8_t* net = client ? *reinterpret_cast<uint8_t**>(client + 4) : nullptr;
        if (!net) return false;
        uint8_t* b = *reinterpret_cast<uint8_t**>(net + 0x90);
        uint8_t* e = *reinterpret_cast<uint8_t**>(net + 0x94);
        static char seen[1024] = "";
        char all[1024] = "";
        size_t o = 0;
        for (uint8_t* r = b; b && r + 0x85 <= e && o < sizeof(all) - 140; r += 0x85) {
            char name[128];
            FOTStringToAnsi(r + kSessionRecordName, name, sizeof(name));
            o += sprintf_s(all + o, sizeof(all) - o, "\"%s\" ", name);
            if (!out[0] && strncmp(name, kCoopPrefix, sizeof(kCoopPrefix) - 1) == 0) {
                strcpy_s(out, cap, name);
            }
        }
        if (strcmp(all, seen) != 0) {
            strcpy_s(seen, all);
            Log("bot: session list: %s", all[0] ? all : "(empty)");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("bot: exception 0x%08x reading the session list", GetExceptionCode());
        return false;
    }
    return out[0] != 0;
}

void BotDriveMultiSetup(uint8_t* w) {  // called for every window tick
    if (*reinterpret_cast<uintptr_t*>(w) != kMultiSetupVtable) return;
    uint8_t* join = nullptr;
    __try {
        join = *reinterpret_cast<uint8_t**>(w + kMultiSetupJoinButton);
        if (g_mp_menu && join && !join[0x102]) reinterpret_cast<void(__thiscall*)(void*, char)>(kWidgetEnable)(join, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        join = nullptr;
    }
    // Test: press Start Co-op, or join the co-op session once the session list shows it.
    if (!g_test.enabled || !g_test.menu[0] || g_menu_role_chosen) return;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("bot: Multiplayer screen: Join button %s", join && join[0x102] ? "enabled" : "DISABLED");
    }
    static int ticks = 0;
    if (++ticks < 30) return;
    if (_stricmp(g_test.menu, "host") == 0) {
        Log("bot: pressing Start Co-op on the Multiplayer screen");
        // Called the way the button's dispatcher (0x7053ea) calls handlers: __thiscall(owner, arg), callee pops.
        reinterpret_cast<void(__thiscall*)(void*, int)>(reinterpret_cast<void*>(&StartCoopButton))(w, 0);
        return;
    }
    char session[128];
    static DWORD next_try = 0;
    if (FindListedCoopSession(w, session, sizeof(session)) && GetTickCount() >= next_try) {
        next_try = GetTickCount() + 2000;
        Log("bot: the session list shows \"%s\": joining it", session);
        JoinCoop(w, session);
    }
}

void __fastcall DummyUpdateHook(void* self, void* /*edx*/) {
    if (g_coop_join) {
        // Never start the local game: the host's 0xcb closes this lobby.
        if (g_fetch_save && !g_join_pending) return;  // a stale lobby, not one we just loaded into
        uint8_t* client = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + kDummySetupClient);
        // Per lobby window, not per client: a campaign keeps its Client across map changes, and every new
        // session needs a fresh join (the SP flow gives the client a new loopback network each time).
        // g_join_window is also cleared when a session starts: a new lobby can reuse the old one's address.
        if (self != g_join_window) {
            g_join_window = self;
            g_join_client = client;
            g_session_server = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + kDummySetupServer);  // our own, idle
            g_join_network_set = false;
            g_joined = false;
            g_next_join_try = 0;
            Log("join: single-player lobby is up, joining %s", g_host_address);
        }
        if (!g_joined && GetTickCount() >= g_next_join_try) {
            g_joined = JoinHost(client);
            if (g_joined) g_join_pending = false;
            g_next_join_try = GetTickCount() + kJoinPollMs;  // "0 session(s) found": the host's session shows a moment later
        }
        return;
    }
    if (!g_coop_host) {
        g_dummy_update(self);
        return;
    }
    uint8_t* server = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + kDummySetupServer);
    static bool solo = false;
    if (server != g_hosted_server) {
        g_hosted_server = server;
        g_session_server = server;
        InterlockedExchange(&g_start_requested, 0);
        // Only a session that comes from loading a save (whose file the joiners can fetch) is a co-op lobby.
        // Any other (e.g. world map -> mission) starts alone and re-syncs by quick-save + quick-load.
        solo = !g_load_pending;
        g_load_pending = false;
        g_session_coop = !solo;
        if (solo) {
            Log("host: session without a load (world map -> mission?): starting alone, will re-sync");
        } else {
            Log("host: co-op lobby is up: starting when every connected player is ready (Ctrl+marker starts now)");
            AttachTcp(server);
            g_lobby_opened_tick = GetTickCount();
            InterlockedExchange(&g_lobby_open, 1);
        }
    }
    if (solo) {
        // World map -> mission restarts the game on the same Server, whose records still list connected joiners:
        // the original forced StartGame waited for their "loaded" (0xcc) forever while they sat on the world map.
        // Mark every other player not connected (node+0xf), then run the original forced start: the all-loaded
        // check 0x63e090 and the start message 0x63e5c0 only count connected players. (An unforced start threw
        // net_playersReady every frame: after a mission the ready flags are reset, the host's own too.)
        // The joiners follow after the re-sync (the quick-load replaces this Server).
        __try {
            uint8_t* head = *reinterpret_cast<uint8_t**>(server + kServerPlayerList);
            int guard = 0;
            for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && guard < 16;
                 node = *reinterpret_cast<uint8_t**>(node), guard++) {
                if (*reinterpret_cast<int*>(node + 0x35) != 1 && node[0xf]) {
                    node[0xf] = 0;
                    Log("host: solo start: player %d set not connected for this start", *reinterpret_cast<int*>(node + 0x35));
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("host: solo start: exception 0x%08x", GetExceptionCode());
        }
        g_dummy_update(self);
        return;
    }
    LogPlayerRecords(server);
    bool bot_lobby = g_test.enabled && _stricmp(g_test.scenario, "watch") != 0;  // watch: a person plays, normal rules
    if (bot_lobby && !g_start_requested) {
        // The test's fixed joiner count, or more if more are polling (drop-in: a joiner registered mid-session).
        int expected = max(g_test.joiners, LiveJoiners());
        int ready = CountReadyPlayers(server);
        if (ready >= 1 + expected) {
            Log("bot: %d players ready (host + %d), starting", ready, expected);
            InterlockedExchange(&g_start_requested, 1);
        }
    } else if (!bot_lobby && !g_start_requested) {
        // Play: everyone polling the host (live joiners) is expected. Give up waiting on a joiner after 30 s.
        int live = LiveJoiners();
        int ready = CountReadyPlayers(server);
        DWORD waited = GetTickCount() - g_lobby_opened_tick;
        if (live == 0 || ready >= 1 + live || waited > 30000) {
            Log("host: %d ready, %d joiner(s) expected, %lu ms: starting", ready, live, waited);
            InterlockedExchange(&g_start_requested, 1);
        }
    }
    if (InterlockedExchange(&g_start_requested, 0)) {
        Log("host: starting the game for everyone (StartGame force)");
        g_dummy_update(self);
    }
}

void __fastcall ClientRecvHook(void* self, void* /*edx*/, const uint8_t* msg, int len, int sender) {
    LogMessage("S->C", msg, len, sender);
    // Joiner on the world map: the host restarts the game on the old Server for its solo mission, and the stray
    // start/go messages (seen as a phantom "session 2" on the joiner) left the joiner's client with go-flag
    // +0x24d = 0 in the next session, so its tick gate never opened. Ignore the old session until we follow.
    if (g_coop_join && g_join_ignore_server) return;
    if (g_coop_join) CoopFromNetwork(msg, len, false);  // a talker's delta: applied now (private_sync.inc)
    g_client_recv(self, msg, len, sender);
    if (len >= 4 && *reinterpret_cast<const uint32_t*>(msg) == 206) {
        LONG n = InterlockedIncrement(&g_sessions_started);
        // SNAP lines carry the save generation both copies loaded, so the test compares like with like.
        g_snap_tag = g_coop_host ? (g_session_coop ? g_serve_gen : 0) : g_loaded_gen;
        g_resync_state = 0;  // a quick-load ends the old session: never carry a re-sync step into the new one
        InterlockedExchange(&g_lobby_open, 0);
        g_session_start_tick = GetTickCount();
        if (g_coop_host) RestoreQuickSave();  // the re-sync load has read QuickSave.sav: put the player's back
        if (g_coop_host && g_session_coop) Notice("Co-op: %d player(s) in this game", 1 + LiveJoiners());
        if (g_coop_join) {
            g_join_state = 'P';
            Notice("Co-op: in the host's game");
        }
        g_world_closing = false;
        if (g_coop_join && g_test.enabled && g_test.empty_stock) EmptyStockForTest();  // shop_sync.inc
        LogStockSummary("at the session start");
        if (g_coop_join && g_test.enabled && g_test.serial_skew) {
            *reinterpret_cast<uint32_t*>(0x8be148) += g_test.serial_skew;  // kEntitySerial (private_sync.inc)
            Log("bot: entity serial put %d ahead, as on a copy that ran other sessions", g_test.serial_skew);
        }
        ClearNpcLocks();
        ClearHeldCommands();  // shop_sync.inc
        // A drift seen in the old session (held off by a shop's lock) is gone with it: acted on in the new one, it was
        // a second re-sync straight after the first (shopresync).
        if (g_judge.reason) Log("co-op: new session: the old session's drift report is dropped");
        g_judge.NewSession();
        if (g_coop_host) {
            Log("session %ld started (%s)", n, g_session_coop ? "co-op lobby" : "alone");
            // World map -> mission starts without a load, so no joiner could come along: re-sync by save.
            if (!g_session_coop) RequestResync("session started without a co-op lobby");
        } else {
            Log("session %ld started", n);
        }
        // A save that restarts the server's ticks behind the world (see LocalServerTicksToFrame) makes every copy wait
        // until they catch up: report it.
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        uint8_t* server = static_cast<uint8_t*>(g_session_server);
        if (g_coop_host && world && server && *reinterpret_cast<uintptr_t*>(server) == kServerVtable &&
            *reinterpret_cast<int*>(server + 0x99) < *reinterpret_cast<int*>(world + kWorldFrame))
            Log("host: server ticks start at %d, behind the world's frame %d: the copies will wait",
                *reinterpret_cast<int*>(server + 0x99), *reinterpret_cast<int*>(world + kWorldFrame));
        g_session_coop = false;
        g_hosted_server = nullptr;  // the next lobby can reuse this Server's address
        g_join_window = nullptr;
    }
}

bool BytesMatch(uintptr_t addr, const uint8_t* expect, size_t n) {
    return memcmp(reinterpret_cast<const void*>(addr), expect, n) == 0;
}

template <typename Fn>
bool PatchSlot(uintptr_t slot, uintptr_t expect, void* hook, Fn* original) {
    uintptr_t* p = reinterpret_cast<uintptr_t*>(slot);
    if (*p != expect) return false;
    DWORD old;
    if (!VirtualProtect(p, sizeof(*p), PAGE_READWRITE, &old)) return false;
    *original = reinterpret_cast<Fn>(*p);
    *p = reinterpret_cast<uintptr_t>(hook);
    VirtualProtect(p, sizeof(*p), old, &old);
    return true;
}

DWORD WINAPI MarkerThread(LPVOID) {
    // The key left of "1" (backtick on US/UK layouts) is unbound in keys.cfg. Find it by scan code
    // so it works on any keyboard layout.
    UINT marker_vk = MapVirtualKeyA(0x29, MAPVK_VSC_TO_VK);
    Log("marker key: scan code 0x29 -> vk 0x%x", marker_vk);
    int mark = 0;
    bool was_down = false;
    for (;;) {
        // GetAsyncKeyState is global: only react when this game window has focus, so two copies on
        // one PC don't both act on one key press.
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
        bool focused = fg_pid == GetCurrentProcessId();
        bool down = focused && marker_vk && (GetAsyncKeyState(marker_vk) & 0x8000) != 0;
        if (down && !was_down) {
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                InterlockedExchange(&g_start_requested, 1);
                Log("===== START REQUESTED (Ctrl+marker) =====");
                Beep(600, 200);
            } else {
                Log("===== MARK %d =====", ++mark);
                Beep(1200, 80);  // MessageBeep is silent while the game owns the sound device
            }
        }
        was_down = down;
        Sleep(30);
    }
}

// Logs access violations and other hard faults (first-chance) with EIP and a short stack scan, so a crash
// in either copy names the faulting function. C++ throws (0xe06d7363) are logged once each by EIP.
LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    static volatile LONG logged = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool hard = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW;
    if ((hard || code == 0xe06d7363) && InterlockedIncrement(&logged) <= 20) {
        CONTEXT* c = ep->ContextRecord;
        ULONG_PTR info1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        Log("EXCEPTION 0x%08x at eip=%08x addr=%08x eax=%08x ecx=%08x edx=%08x esi=%08x edi=%08x",
            code, c->Eip, (unsigned)info1, c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi);
        if (code == 0xe06d7363 && ep->ExceptionRecord->NumberParameters >= 3) {
            // MSVC throw: [1] = thrown object, [2] = ThrowInfo {attr, unwind, forward, CatchableTypeArray*};
            // CatchableTypeArray {count, CatchableType* [..]}; CatchableType {props, TypeDescriptor*, ...};
            // TypeDescriptor {vftable, spare, char name[]}.
            __try {
                const uint32_t* ti = reinterpret_cast<const uint32_t*>(ep->ExceptionRecord->ExceptionInformation[2]);
                const uint32_t* cta = reinterpret_cast<const uint32_t*>(ti[3]);
                const uint32_t* ct = reinterpret_cast<const uint32_t*>(cta[1]);
                const char* name = reinterpret_cast<const char*>(ct[1] + 8);
                Log("  C++ throw of %s", name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (hard || code == 0xe06d7363) {
            // Return addresses into BOS.exe .text found on the stack (rough call chain).
            char chain[512];
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
            chain[o] = 0;
            Log("  stack:%s", chain);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void Init() {
    InitializeCriticalSection(&g_lock);
    g_start = GetTickCount();
    g_log = _fsopen("netlog.txt", "w", _SH_DENYNO);  // readable while the game runs
    Log("netlog loaded");
    g_main_thread_id = GetCurrentThreadId();  // loaded from FTSE's Lua start-up on the game's main thread
    AddVectoredExceptionHandler(1, CrashLogger);
    if (!BytesMatch(kServerRecv, kServerRecvPrologue, sizeof(kServerRecvPrologue)) ||
        !BytesMatch(kClientRecv, kClientRecvPrologue, sizeof(kClientRecvPrologue))) {
        Log("BOS.exe does not match GOG 1.27, nothing patched");
        return;
    }
    bool ok = PatchSlot(kServerVtableRecvSlot, kServerRecv, (void*)&ServerRecvHook, &g_server_recv) &&
              PatchSlot(kClientVtableRecvSlot, kClientRecv, (void*)&ClientRecvHook, &g_client_recv) &&
              PatchSlot(kDummySetupUpdateSlot, kDummySetupUpdate, (void*)&DummyUpdateHook, &g_dummy_update);
    Log(ok ? "hooks installed (Server/Client receive, single-player lobby update)"
           : "vtable slots unexpected, hooks not installed");
    char mode[16] = "";
    GetPrivateProfileStringA("coop", "mode", "off", mode, sizeof(mode), ".\\ftcoop.ini");
    g_share_squad = GetPrivateProfileIntA("coop", "share_squad", 1, ".\\ftcoop.ini") != 0;
    if (!g_share_squad) Log("co-op: share_squad=0, co-op players' commands are NOT passed on (mutation test)");
    g_file_port = GetPrivateProfileIntA("coop", "file_port", 47701, ".\\ftcoop.ini");
    g_fetch_save = GetPrivateProfileIntA("coop", "fetch_save", 1, ".\\ftcoop.ini") != 0;
    g_follow = GetPrivateProfileIntA("coop", "follow", 1, ".\\ftcoop.ini") != 0;
    GetPrivateProfileStringA("coop", "host", "127.0.0.1", g_host_address, sizeof(g_host_address), ".\\ftcoop.ini");
    GetPrivateProfileStringA("coop", "name", "", g_player_name, sizeof(g_player_name), ".\\ftcoop.ini");
    if (!g_player_name[0]) strcpy_s(g_player_name, "Player");
    InitializeCriticalSection(&g_serve_lock);
    InitializeCriticalSection(&g_notice_lock);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // Every hook is installed; each acts only once a role is set (ftcoop.ini mode= or the Multiplayer menu).
    Log(PatchCall(kLoadCheckCall, kLoadCheck, (void*)&LoadCheckHook) ? "load-check hook installed"
                                                                     : "load-check call site unexpected, hook not installed");
    Log(PatchSlot(kClientLostSessionSlot, kClientLostSession, (void*)&LostSessionHook, &g_client_lost_session)
            ? "lost-session hook installed (a joiner follows the host instead of showing the error)"
            : "lost-session slot unexpected, hook not installed");
    bool frame = PatchCall(kFrameTickCall, kClientTickProcessor, (void*)&FrameTickHook);
    bool gate = PatchCall(kStepGateCalls[0], kStepGate, (void*)&StepGateHook) &&
                PatchCall(kStepGateCalls[1], kStepGate, (void*)&StepGateHook);
    Log("co-op: step gate hooks %s (the hold for a drift correction)", gate ? "ok" : "FAILED");
    bool tick = PatchJmp(kWindowTick, kWindowTickPrologue, sizeof(kWindowTickPrologue), (void*)&WindowTickHook);
    Log("co-op: frame hook %s, window-tick hook %s", frame ? "ok" : "FAILED", tick ? "ok" : "FAILED");
    Log("co-op: network-close hook %s (DirectPlay released in the background)", PatchNetCloseRelease() ? "ok" : "FAILED");
    Log(PatchCall(kSpawnCallSite, kWorldSpawnPlayer, (void*)&SpawnPlayerHook)
            ? "spawn hook installed (campaign squad only for player 1)"
            : "spawn call site unexpected, spawn hook not installed");
    Log(PatchJmp(kUiLocalPlayer, kUiLocalPlayerPrologue, sizeof(kUiLocalPlayerPrologue), (void*)&UiLocalPlayerHook)
            ? "ui local-player hook installed"
            : "ui local-player prologue unexpected, hook not installed");
    Log(PatchJmp(kIsLocalPlayer, kIsLocalPlayerPrologue, sizeof(kIsLocalPlayerPrologue), (void*)&IsLocalPlayerHook)
            ? "turn-owner hook installed (a joiner's End Turn on the squad's turn)"
            : "turn-owner prologue unexpected, hook not installed");
    Log(PatchCall(kTickGateGetPlayerCall, kGetPlayerById, (void*)&TickGateGetPlayerHook)
            ? "tick-gate player hook installed"
            : "tick-gate call site unexpected, hook not installed");
    bool send = false;
    if (BytesMatch(kClientSendFn, kClientSendPrologue, sizeof(kClientSendPrologue))) {
        g_send_trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_send_trampoline) {
            memcpy(g_send_trampoline, reinterpret_cast<void*>(kClientSendFn), 5);
            g_send_trampoline[5] = 0xe9;
            *reinterpret_cast<int32_t*>(g_send_trampoline + 6) =
                static_cast<int32_t>((kClientSendFn + 5) - reinterpret_cast<uintptr_t>(g_send_trampoline + 10));
            send = PatchJmp(kClientSendFn, kClientSendPrologue, sizeof(kClientSendPrologue), (void*)&ClientSendHook);
        }
    }
    Log("co-op: client-send hook %s (who started a conversation)", send ? "ok" : "FAILED");
    bool gotomap = false;
    if (BytesMatch(kGoToMapFn, kGoToMapPrologue, sizeof(kGoToMapPrologue))) {
        g_gotomap_trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_gotomap_trampoline) {
            memcpy(g_gotomap_trampoline, reinterpret_cast<void*>(kGoToMapFn), 6);
            g_gotomap_trampoline[6] = 0xe9;
            *reinterpret_cast<int32_t*>(g_gotomap_trampoline + 7) =
                static_cast<int32_t>((kGoToMapFn + 6) - reinterpret_cast<uintptr_t>(g_gotomap_trampoline + 11));
            gotomap = PatchJmp(kGoToMapFn, kGoToMapPrologue, sizeof(kGoToMapPrologue), (void*)&GoToMapHook);
        }
    }
    Log("co-op: world-map hook %s (only the host chooses where to go)", gotomap ? "ok" : "FAILED");
    Log("co-op: world-map travel hooks %s (the host moves the party, the others watch)", InstallWorldMapHooks() ? "ok" : "FAILED");
    Log("co-op: actor-command hook %s (co-op commands, NPC locks)",
        PatchSlot(kActorActSlot, kActorAct, (void*)&ActorActHook, &g_actor_act) ? "ok" : "FAILED");
    InstallDots();
    InstallRecruitHooks();
    InstallLevelUpHooks();
    InstallSaveSyncHooks();
    Log("co-op: joiner load guard %s, %s",
        PatchSlot(kGameMainVtable + 0x38, 0x6c6260, (void*)&GameMainCloseHook, &g_gamemain_close) ? "ok" : "FAILED",
        PatchSlot(kCampaignWindowVtable + 0x34, 0x6c61f0, (void*)&CampaignCloseHook, &g_campaign_close) ? "ok" : "FAILED");
    bool menu = PatchSlot(kMainMenuUpdateSlot, kWindowUpdateStub, (void*)&MainMenuUpdateHook, &g_mainmenu_update);
    bool pump = PatchCall(kPumpHandlerCall, kPumpHandler, (void*)&PumpHandlerHook);
    Log("co-op: main-menu hook %s, pump hook %s", menu ? "ok" : "FAILED", pump ? "ok" : "FAILED");
    g_mp_menu = GetPrivateProfileIntA("coop", "mp_menu", 1, ".\\ftcoop.ini") != 0;
    g_lockstep_chunks = GetPrivateProfileIntA("coop", "lockstep_chunks", 300, ".\\ftcoop.ini");
    g_recreate_recruits = GetPrivateProfileIntA("coop", "recreate_recruits", 0, ".\\ftcoop.ini");
    g_shop_no_pause = GetPrivateProfileIntA("coop", "shop_no_pause", 1, ".\\ftcoop.ini") != 0;
    g_menu_pause = GetPrivateProfileIntA("coop", "menu_pause", 0, ".\\ftcoop.ini") != 0;
    {
        static const uint8_t kPausePrologue[] = {0x8a, 0x44, 0x24, 0x04, 0x84, 0xc0};  // mov al, [esp+4]; test al, al
        g_pause_trampoline = HookWithTrampoline(0x4fc800, kPausePrologue, sizeof(kPausePrologue), (void*)&PauseHook);
        Log("co-op: pause-counter hook %s", g_pause_trampoline ? "ok" : "FAILED");
        static const uint8_t kBarterPrologue[] = {0x66, 0x8b, 0x81, 0x00, 0x01, 0x00, 0x00};  // mov ax, [ecx+0x100]
        g_barter_trampoline = HookWithTrampoline(0x4d0000, kBarterPrologue, sizeof(kBarterPrologue), (void*)&BarterButtonHook);
        static const uint8_t kGamblePrologue[] = {0x56, 0x8b, 0xf1, 0x6a, 0x07};  // push esi; mov esi, ecx; push 7
        g_gamble_trampoline = HookWithTrampoline(0x4d0090, kGamblePrologue, sizeof(kGamblePrologue), (void*)&GambleButtonHook);
        Log("co-op: gamble hook %s", g_gamble_trampoline ? "ok" : "FAILED");
        static const uint8_t kQmDtorPrologue[] = {0x6a, 0xff, 0x68, 0x90, 0xfd, 0x7b, 0x00};  // push -1; push 0x7bfd90
        g_qm_dtor_trampoline = HookWithTrampoline(0x454f40, kQmDtorPrologue, sizeof(kQmDtorPrologue), (void*)&QmDtorHook);
        Log("co-op: barter hook %s, shop close hook %s", g_barter_trampoline ? "ok" : "FAILED",
            g_qm_dtor_trampoline ? "ok" : "FAILED");
    }
    if (g_mp_menu) {
        // Join keeps its original behaviour for normal sessions: copy its first 5 bytes (push ebp; mov ebp, esp;
        // push -1) into a trampoline that jumps back after them.
        bool join_btn = false;
        if (BytesMatch(kMultiJoinButton, kMultiButtonPrologue, sizeof(kMultiButtonPrologue))) {
            g_join_trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (g_join_trampoline) {
                memcpy(g_join_trampoline, reinterpret_cast<void*>(kMultiJoinButton), 5);
                g_join_trampoline[5] = 0xe9;
                *reinterpret_cast<int32_t*>(g_join_trampoline + 6) =
                    static_cast<int32_t>((kMultiJoinButton + 5) - reinterpret_cast<uintptr_t>(g_join_trampoline + 10));
                join_btn = PatchJmp(kMultiJoinButton, kMultiButtonPrologue, sizeof(kMultiButtonPrologue), (void*)&MultiJoinButtonHook);
            }
        }
        bool ctor = PatchCall(kMultiSetupCtorCall, kMultiSetupCtor, (void*)&MultiSetupCtorHook);
        Log("menu: Multiplayer screen: Start Co-op button %s, co-op-aware Join %s", ctor ? "ok" : "FAILED",
            join_btn ? "ok" : "FAILED");
    }
    g_test.enabled = GetPrivateProfileIntA("test", "enabled", 0, ".\\ftcoop.ini") != 0;
    if (g_test.enabled) StartShotThread();
    if (g_test.enabled && GetPrivateProfileIntA("test", "entity_log", 0, ".\\ftcoop.ini")) InstallEntityLogHooks();
    if (GetPrivateProfileIntA("ui", "skip_intro", g_test.enabled ? 1 : 0, ".\\ftcoop.ini") != 0) {
        *reinterpret_cast<uint8_t*>(kIntroShownFlag) = 1;
        Log("ui: skipping the startup movies ([ui] skip_intro)");
    }
    if (g_test.enabled) {
        GetPrivateProfileStringA("test", "save", "user/save/QuickSave.sav", g_test.save, sizeof(g_test.save), ".\\ftcoop.ini");
        GetPrivateProfileStringA("test", "actor", "host", g_test.actor, sizeof(g_test.actor), ".\\ftcoop.ini");
        g_test.joiners = GetPrivateProfileIntA("test", "joiners", 1, ".\\ftcoop.ini");
        g_test.timeout_s = GetPrivateProfileIntA("test", "timeout", 180, ".\\ftcoop.ini");
        GetPrivateProfileStringA("test", "scenario", "move", g_test.scenario, sizeof(g_test.scenario), ".\\ftcoop.ini");
        g_test.soak_frames = GetPrivateProfileIntA("test", "soak_frames", 4000, ".\\ftcoop.ini");
        GetPrivateProfileStringA("test", "menu", "", g_test.menu, sizeof(g_test.menu), ".\\ftcoop.ini");
        g_test.index = GetPrivateProfileIntA("test", "index", 1, ".\\ftcoop.ini");
        g_test.hold = GetPrivateProfileIntA("test", "hold", 0, ".\\ftcoop.ini");
        g_test.menu_cancel = GetPrivateProfileIntA("test", "menu_cancel", 0, ".\\ftcoop.ini") != 0;
        g_test.inv_chaos = GetPrivateProfileIntA("test", "inv_chaos", 0, ".\\ftcoop.ini") != 0;
        g_test.grow_table = GetPrivateProfileIntA("test", "grow_table", 0, ".\\ftcoop.ini") != 0;
        g_test.serial_skew = GetPrivateProfileIntA("test", "serial_skew", 0, ".\\ftcoop.ini");
        g_test.empty_stock = GetPrivateProfileIntA("test", "empty_stock", 0, ".\\ftcoop.ini") != 0;
        g_test.no_serial_sync = GetPrivateProfileIntA("test", "no_serial_sync", 0, ".\\ftcoop.ini") != 0;
        g_test.resync_in_shop = GetPrivateProfileIntA("test", "resync_in_shop", 0, ".\\ftcoop.ini") != 0;
        g_test.merchant = GetPrivateProfileIntA("test", "merchant", 0, ".\\ftcoop.ini");
        g_test.gamble = GetPrivateProfileIntA("test", "gamble", 0, ".\\ftcoop.ini") != 0;
        Log("bot: TEST MODE save=\"%s\" actor=%s joiners=%d timeout=%ds scenario=%s menu=%s", g_test.save, g_test.actor,
            g_test.joiners, g_test.timeout_s, g_test.scenario, g_test.menu[0] ? g_test.menu : "-");
        CreateThread(nullptr, 0, TestWatchdog, nullptr, 0, nullptr);
    }
    RestoreQuickSaveAfterCrash();
    if (_stricmp(mode, "host") == 0) BecomeHost();
    if (_stricmp(mode, "join") == 0) BecomeJoiner(nullptr, nullptr);
    Log("co-op mode at start: %s (ftcoop.ini [coop] mode=; the Multiplayer menu can set it)",
        g_coop_host ? "host" : g_coop_join ? "join" : "off");
    CreateThread(nullptr, 0, MarkerThread, nullptr, 0, nullptr);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
