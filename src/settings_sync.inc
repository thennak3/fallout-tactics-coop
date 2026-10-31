// ---- the host's game settings on every copy (included from private_sync.inc) ----
// The World keeps a copy of the game-settings record at World+0x749 (fields as 0x650640 reads and writes bos.cfg):
// +0x30 game.difficulty, +0x34 game.opponentLevelOffset, +0x3c game.ctbSpeed (float), +0x40 game.friendlyFire
// (float), +0x44 game.turnBasedSingle, +0x48 game.turnBasedMulti, +0x68 game.violenceLevel, +0x7f game.fastTurnBased
// (byte). Each player's own bos.cfg filled them, and the simulation reads them: combat start 0x506b30 takes the
// turn-based mode from +0x44 (World+0x78d), so a joiner whose option differed started Individual turn-based where the
// host started Squad, and the copies split (turnbased --joiner-turnbased 1). The host is right: its values go out as
// a co-op record (kGameSettings) and act on every copy at the same frame; every frame after that, every copy puts the
// last values back, so a player's own options change nothing until the host's copy sends them. The host sends
// whenever its own values change (its options screen writes them) and at the start of every session (a late joiner).
// The record acts a few frames into a session, and those frames were enough: a joiner set to Individual started a
// turn-based round at frame 41 that the host never had (host_move --joiner-turnbased 1, frame 39 to 41). So the host's
// GEN replies carry the values too (" SET ...", save_transfer.inc), and a joiner puts them in from its first frame.
// The host answers from the game's options record 0x8bd998 (same layout as the World's copy; StandardApplication
// 0x6c4180 loads bos.cfg into it with 0x650550 before the menu), so the line is there before any session exists.
const uintptr_t kOptions = 0x8bd998;
const size_t kWorldSettings = 0x749;

struct GameSettings {
    int32_t difficulty, opponent_offset;
    float ctb_speed, friendly_fire;
    int32_t tb_single, tb_multi, violence;
    uint8_t fast_tb;
};

GameSettings ReadSettingsAt(const uint8_t* g) {
    GameSettings s;
    s.difficulty = *reinterpret_cast<const int32_t*>(g + 0x30);
    s.opponent_offset = *reinterpret_cast<const int32_t*>(g + 0x34);
    s.ctb_speed = *reinterpret_cast<const float*>(g + 0x3c);
    s.friendly_fire = *reinterpret_cast<const float*>(g + 0x40);
    s.tb_single = *reinterpret_cast<const int32_t*>(g + 0x44);
    s.tb_multi = *reinterpret_cast<const int32_t*>(g + 0x48);
    s.violence = *reinterpret_cast<const int32_t*>(g + 0x68);
    s.fast_tb = g[0x7f];
    return s;
}

GameSettings ReadSettings(const uint8_t* world) { return ReadSettingsAt(world + kWorldSettings); }

void HostSettingsLine(char* out, size_t size) {  // file server thread: the host's own options, as its GEN reply ends
    GameSettings s = ReadSettingsAt(reinterpret_cast<const uint8_t*>(kOptions));
    sprintf_s(out, size, " SET %d %d %.9g %.9g %d %d %d %d", s.difficulty, s.opponent_offset, s.ctb_speed,
              s.friendly_fire, s.tb_single, s.tb_multi, s.violence, s.fast_tb);
}

void WriteSettings(uint8_t* world, const GameSettings& s) {
    uint8_t* g = world + kWorldSettings;
    *reinterpret_cast<int32_t*>(g + 0x30) = s.difficulty;
    *reinterpret_cast<int32_t*>(g + 0x34) = s.opponent_offset;
    *reinterpret_cast<float*>(g + 0x3c) = s.ctb_speed;
    *reinterpret_cast<float*>(g + 0x40) = s.friendly_fire;
    *reinterpret_cast<int32_t*>(g + 0x44) = s.tb_single;
    *reinterpret_cast<int32_t*>(g + 0x48) = s.tb_multi;
    *reinterpret_cast<int32_t*>(g + 0x68) = s.violence;
    g[0x7f] = s.fast_tb;
}

bool SameSettings(const GameSettings& a, const GameSettings& b) {
    return a.difficulty == b.difficulty && a.opponent_offset == b.opponent_offset && a.ctb_speed == b.ctb_speed &&
           a.friendly_fire == b.friendly_fire && a.tb_single == b.tb_single && a.tb_multi == b.tb_multi &&
           a.violence == b.violence && a.fast_tb == b.fast_tb;
}

void LogSettings(const char* what, const GameSettings& s) {
    Log("co-op: settings %s: difficulty %d, opponent level %+d, CTB speed %.2f, friendly fire %.2f, turn-based %d "
        "(multiplayer %d), violence %d, fast turn-based %d", what, s.difficulty, s.opponent_offset, s.ctb_speed,
        s.friendly_fire, s.tb_single, s.tb_multi, s.violence, s.fast_tb);
}

GameSettings g_settings = {};     // the host's, as of the last kGameSettings record that acted
bool g_have_settings = false;
GameSettings g_settings_sent = {};
bool g_settings_pending = false;  // host: sent, not yet back

// Record: [7..10] difficulty, [11] opponent offset (signed), [12..15] CTB speed, [16..19] friendly fire,
// [20] turn-based single, [21] multi, [22] violence, [23] fast turn-based.
void SendSettings(const GameSettings& s) {
    uint8_t rec[1][27];
    CoopRecord(rec[0], kGameSettings, 0);
    memcpy(rec[0] + 7, &s.difficulty, 4);
    rec[0][11] = static_cast<uint8_t>(static_cast<int8_t>(s.opponent_offset));
    memcpy(rec[0] + 12, &s.ctb_speed, 4);
    memcpy(rec[0] + 16, &s.friendly_fire, 4);
    rec[0][20] = static_cast<uint8_t>(s.tb_single);
    rec[0][21] = static_cast<uint8_t>(s.tb_multi);
    rec[0][22] = static_cast<uint8_t>(s.violence);
    rec[0][23] = s.fast_tb;
    SendCoopRecords(rec, 1);
    g_settings_sent = s;
    g_settings_pending = true;
    LogSettings("sent to every copy", s);
}

void SettingsAtFrame(const uint8_t* r) {  // CoopCommandAtFrame, every copy, the same frame
    GameSettings s;
    memcpy(&s.difficulty, r + 7, 4);
    s.opponent_offset = static_cast<int8_t>(r[11]);
    memcpy(&s.ctb_speed, r + 12, 4);
    memcpy(&s.friendly_fire, r + 16, 4);
    s.tb_single = r[20], s.tb_multi = r[21], s.violence = r[22], s.fast_tb = r[23];
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world && !g_coop_host && (!g_have_settings || !SameSettings(s, g_settings))) {
        GameSettings mine = ReadSettings(world);
        if (!SameSettings(mine, s)) LogSettings("ours, replaced by the host's", mine);
    }
    g_settings = s;
    g_have_settings = true;
    if (g_coop_host) g_settings_pending = false;
    if (world) WriteSettings(world, s);
    LogSettings("the host's, from this frame", s);
}

// The entity serial counter 0x8be148: every entity made takes the next number (0x5ea6d0 and five others stamp it into
// the new entity), and a save does not keep it, so each copy goes on from its own count after a load. Copies that had
// run different sessions before (the owner's night: joiners that came and went) then differed in it from the first
// frame, and every load was followed by a drift check failing on the free list's hash and a correction, or, with two
// joiners reporting it, a reload. At the start of every session every copy takes one number at the same frame: the
// host's own, well past anything any copy has handed out yet.

void SendSerial() {
    if (g_test.enabled && g_test.no_serial_sync) return;  // mutation: each copy keeps its own count (0.10.5)
    uint8_t rec[1][27];
    CoopRecord(rec[0], kSerialSync, 0);
    uint32_t v = session::SessionSerial(*reinterpret_cast<uint32_t*>(kEntitySerial));
    memcpy(rec[0] + 7, &v, 4);
    SendCoopRecords(rec, 1);
}

void SerialAtFrame(const uint8_t* r) {  // CoopCommandAtFrame, every copy, the same frame
    uint32_t v;
    memcpy(&v, r + 7, 4);
    uint32_t* serial = reinterpret_cast<uint32_t*>(kEntitySerial);
    Log("co-op: entity serial %u -> %u (the host's, every copy at this frame)", *serial, v);
    *serial = v;
}

// The quartermasters' stock (the campaign's list: +0x151 template names, +0x161 counts) is what a quartermaster's shop
// is made from, on every copy (0x454330, HeadlessShopOpen). The copies' lists parted (the owner's night, 0.10.5: a
// joiner made every one of the host's quartermaster shops with 0 items, its free list and inventories went their own
// way from there, and on its screen the quartermaster had nothing to sell), so at the start of every session the
// host sends its list and every copy puts it in at the same frame: "name<TAB>count<LF>..." in kStockSync records of
// 20 bytes each (session_sync.h).
void LogStockSummary(const char* when);  // shop_sync.inc

int StockText(char* out, int cap) {
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(0x8bdd2c);
    if (!campaign) return -1;
    wchar_t** tb = *reinterpret_cast<wchar_t***>(campaign + 0x151);
    wchar_t** te = *reinterpret_cast<wchar_t***>(campaign + 0x155);
    int* cnt = *reinterpret_cast<int**>(campaign + 0x161);
    int n = 0;
    for (int i = 0; tb && tb + i < te; i++) {
        char name[260] = "";
        WideCharToMultiByte(CP_ACP, 0, tb[i] ? tb[i] : L"", -1, name, sizeof(name), nullptr, nullptr);
        int w = sprintf_s(out + n, cap - n, "%s\t%d\n", name, cnt ? cnt[i] : 0);
        if (w < 0 || n + w >= cap - 1) return -1;
        n += w;
    }
    return n;
}

void SendStock() {
    static char text[session::kStockMaxText + 1];
    int n = StockText(text, sizeof(text));
    if (n < 0) {
        Log("co-op: the quartermaster stock list is too long to send (more than %d bytes)", session::kStockMaxText);
        return;
    }
    int chunks = session::StockChunks(n);
    static uint8_t recs[session::kStockMaxChunks][27];
    for (int i = 0; i < chunks; i++) {
        CoopRecord(recs[i], kStockSync, static_cast<uint8_t>(i));
        recs[i][5] = static_cast<uint8_t>(chunks);
        session::StockChunk(text, n, i, recs[i] + 7);
    }
    SendCoopRecords(recs, chunks);
}

void StockAtFrame(const uint8_t* r) {  // CoopCommandAtFrame, every copy, the same frame
    static session::StockAssembler in;
    if (!in.Feed(r[4], r[5], r + 7)) return;
    static char mine[session::kStockMaxText + 1];
    if (StockText(mine, sizeof(mine)) >= 0 && strcmp(mine, in.text) == 0) {
        LogStockSummary("is the host's");
        return;
    }
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(0x8bdd2c);
    if (!campaign) return;
    LogStockSummary("before the host's list");
    // Emptied as the shop's close empties it (0x454f40), then the host's entries added in its order (0x6a6550 adds a
    // count to the entry of that name, or a new entry at the end), so the list is the host's to the order.
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(0x403010)(
        campaign + 0x14d, *reinterpret_cast<void**>(campaign + 0x151), *reinterpret_cast<void**>(campaign + 0x155));
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(0x458760)(
        campaign + 0x15d, *reinterpret_cast<void**>(campaign + 0x161), *reinterpret_cast<void**>(campaign + 0x165));
    session::ForEachStockLine(in.text, [campaign](const char* name_text, int count) {
        void* name = nullptr;  // the game's string, made from the name (0x6c8bf0: thiscall(str*, const char*, max))
        reinterpret_cast<void(__thiscall*)(void**, const char*, int)>(0x6c8bf0)(&name, name_text, 0x7fffffff);
        // kept: the list holds its own reference; one string per entry per session is all this leaks
        reinterpret_cast<void(__thiscall*)(void*, void**, int)>(0x6a6550)(campaign, &name, count);
    });
    LogStockSummary("now the host's");
}

void SettingsFrame(uint8_t* world) {  // FrameTickHook, before the world step
    static LONG session = -1;
    if (g_coop_host) {
        GameSettings mine = ReadSettings(world);
        bool changed = g_have_settings && !SameSettings(mine, g_settings) && !(g_settings_pending && SameSettings(mine, g_settings_sent));
        if (session != g_sessions_started || (!g_have_settings && !g_settings_pending) || changed) {
            if (session != g_sessions_started) {
                SendSerial();
                SendStock();
            }
            session = g_sessions_started;
            if (!g_settings_pending || !SameSettings(mine, g_settings_sent)) SendSettings(mine);
        }
        if (g_have_settings) WriteSettings(world, g_settings);  // a change of its own acts with its record, as everyone's
        return;
    }
    if (g_have_settings) {
        WriteSettings(world, g_settings);
        return;
    }
    // Before the host's first record: the values from its GEN replies.
    char line[96];
    AcquireSRWLockShared(&g_settings_line_lock);
    strcpy_s(line, g_settings_line);
    ReleaseSRWLockShared(&g_settings_line_lock);
    GameSettings s;
    int fast_tb = 0;
    if (sscanf_s(line, " SET %d %d %f %f %d %d %d %d", &s.difficulty, &s.opponent_offset, &s.ctb_speed,
                 &s.friendly_fire, &s.tb_single, &s.tb_multi, &s.violence, &fast_tb) != 8) return;
    s.fast_tb = static_cast<uint8_t>(fast_tb);
    GameSettings mine = ReadSettings(world);
    if (SameSettings(mine, s)) return;
    LogSettings("ours, replaced by the host's from its lobby", mine);
    WriteSettings(world, s);
}
