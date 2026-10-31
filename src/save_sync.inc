// ---- Saving in step (included from netlog.cpp after recruit_sync.inc) ----
// Writing a save changes the running game on the copy that writes it: after the host's quick-save, the copies parted
// within 50 frames (savemenu on bunker.sav: NPC 432 took another action at frame 2046, positions/HP apart from 2047),
// though the actors, the World's first 0x800 bytes, the game's globals and what the actors point at showed nothing
// that mattered (2026-10-28; owner: "a sync after every save I made", 14 re-syncs in an evening). A 150 ms stall and
// a paused stall of the same length did not part them; the write did. So every copy writes at the same point: a save
// becomes a co-op record (kSaveHold) that acts at the same frame everywhere, and from the end of that frame no copy
// steps (the drift correction's step gate). The saver then saves (the quick-save, or the save screen it asked for);
// once it is done it sends kSaveDone, taken as it arrives, and every other copy writes a quick-save of its own (the
// player's QuickSave.sav set aside and put back, as for a re-sync) before it steps on. The writes' changes are then
// the same everywhere. Nobody waits longer than before: the save screen's own loop stops its copy's world, and with
// it every copy's ticks. Entry points: the quick-save (0x478340, key or menu) and the game menu's Save button
// (0x482180); a save screen opened another way (0x4734f0's keyboard path, 0x6b6650) still saves alone, and its drift
// is corrected as before. [coop] save_in_step=0 turns it off (mutation test).
const uintptr_t kSaveButton = 0x482180;                // void __thiscall(WGameMenu*, button), ret 4
const uintptr_t kSaveLoadModalSlot = 0x806edc + 0x2c;  // WSaveLoad's modal loop (the base window's 0x6c6190)
const uintptr_t kAppPumpSlot = 0x82dcdc + 0x10;        // the game's application class: its loop (0x736f50, +0xc) runs
                                                       // +0x10 (0x6bd850, input), +0x14, +0x18 on the main thread
const DWORD kSaveHoldMaxMs = 600000;                   // a saver gone quiet: everyone goes on
const DWORD kSaveHoldWaitMs = 3000;                    // asked, but no frame ran to hold at: save alone
const uint8_t kSaveQuick = 1, kSaveScreen = 2;
static const uint8_t kSavePrologue[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff};  // mov eax, fs:[0]; push -1
uint8_t* g_quicksave_tramp = nullptr;
uint8_t* g_savebutton_tramp = nullptr;
typedef int(__fastcall* ModalLoopFn)(void*, void*);
typedef void(__fastcall* PumpFn)(void*, void*);
ModalLoopFn g_saveload_modal = nullptr;
PumpFn g_app_pump = nullptr;
uint8_t* g_saveload_window = nullptr;  // the save screen while its loop runs
DWORD g_saveload_opened = 0;
int g_saveload_result = 0;             // its last result: 4 saved (or loaded), 0x10 cancelled
bool g_save_in_step = true;            // [coop] save_in_step
bool g_quicksave_direct = false;       // a quick-save of the mod's own: not turned into a request
bool g_save_requested = false;         // this copy asked to save and waits for its hold
uint8_t g_save_kind = 0;
DWORD g_save_asked = 0;
void* g_save_menu = nullptr;           // kSaveScreen: the menu and button to press once held
void* g_save_button = nullptr;
unsigned g_save_saver = 0;             // during a hold: who saves (0 the host, else a joiner's id)
bool g_save_mine = false;              // during a hold: this copy saves
bool g_save_done_due = false;          // during a hold: the saver is done; write ours (if it saved) and go on
bool g_save_shadow_due = false;
uint8_t g_save_hold_kind = 0;
DWORD g_save_hold_since = 0;
int g_save_hold_frame = 0;
void BotPumpTick();  // bot_scenarios.inc

unsigned MySaverId() { return g_coop_host ? 0 : (g_joiner_id ? g_joiner_id : 1); }
bool SaveInStep() { return g_save_in_step && (g_coop_join || (g_coop_host && LiveJoiners() > 0)); }

void OurQuickSave(void* game_main) {  // the game's quick-save, never a request
    g_quicksave_direct = true;
    reinterpret_cast<void(__fastcall*)(void*)>(g_quicksave_tramp ? g_quicksave_tramp : reinterpret_cast<uint8_t*>(kQuickSave))(game_main);
    g_quicksave_direct = false;
}

void PressSaveButton(void* menu, void* button) {  // the menu's Save as the game has it: opens the save screen
    reinterpret_cast<void(__thiscall*)(void*, void*)>(g_savebutton_tramp ? g_savebutton_tramp : reinterpret_cast<uint8_t*>(kSaveButton))(
        menu, button);
}

void RequestSaveHold(uint8_t kind) {
    uint8_t rec[1][27];
    CoopRecord(rec[0], kSaveHold, 0);
    rec[0][5] = kind;
    unsigned me = MySaverId();
    memcpy(rec[0] + 7, &me, 4);
    SendCoopRecords(rec, 1);
    g_save_requested = true;
    g_save_kind = kind;
    g_save_asked = GetTickCount();
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    Log("co-op: save: %s asked for at frame %d: every copy holds first", kind == kSaveQuick ? "a quick-save" : "the save screen",
        world ? *reinterpret_cast<int*>(world + kWorldFrame) : -1);
}

void __fastcall QuickSaveHook(void* game_main) {
    if (g_quicksave_direct || !SaveInStep()) {
        OurQuickSave(game_main);
        return;
    }
    if (!g_save_requested && !g_save_mine) RequestSaveHold(kSaveQuick);
}

void __fastcall SaveButtonHook(void* menu, void* /*edx*/, void* button) {
    if (!SaveInStep()) {
        PressSaveButton(menu, button);
        return;
    }
    if (g_save_requested || g_save_mine) return;
    g_save_menu = menu;
    g_save_button = button;
    RequestSaveHold(kSaveScreen);
}

// A copy that lags can get the saver's done before its own hold frame has run (the fuzzer's report: it was dropped, and
// that copy then held for 10 minutes). It is kept, and acts when the hold comes.
unsigned g_save_early_saver = 0;
bool g_save_early_shadow = false;
DWORD g_save_early_tick = 0;

void CloseAbandonedSave(unsigned joiner, const char* who);  // below
void SaveHoldAtFrame(const uint8_t* r) {  // CoopCommandAtFrame, the same frame on every copy
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    unsigned saver;
    memcpy(&saver, r + 7, 4);
    bool mine = saver == MySaverId() && g_save_requested;
    if (!world) return;
    if (g_save_hold) {  // another copy's save holds already: this one is not served
        if (mine) {
            g_save_requested = false;
            Notice("Co-op: another player is saving - save again when they are done");
        }
        Log("co-op: save: a second save's hold at frame %d: not served", *reinterpret_cast<int*>(world + kWorldFrame));
        return;
    }
    g_save_hold = true;
    g_save_saver = saver;
    g_save_mine = mine;
    g_save_requested = g_save_requested && !mine;
    g_save_hold_kind = r[5];
    g_save_done_due = g_save_shadow_due = false;
    g_save_hold_since = GetTickCount();
    g_save_hold_frame = *reinterpret_cast<int*>(world + kWorldFrame);
    extern unsigned g_gone_joiners[16];
    if (g_coop_host && saver)  // a saver that left after asking, before its hold acted: nobody else would end it
        for (unsigned gone : g_gone_joiners)
            if (gone == saver) {
                Log("co-op: save: the saver of this hold is gone already: everyone goes on");
                CloseAbandonedSave(saver, "the saver");
                break;
            }
    if (!mine && g_save_early_saver == saver && GetTickCount() - g_save_early_tick < 30000) {  // its done came first
        g_save_done_due = true;
        g_save_shadow_due = g_save_early_shadow;
        g_save_early_saver = 0;
    }
    Log("co-op: save: every copy holds after frame %d (%s saves: %s)", g_save_hold_frame, mine ? "this copy" : "another copy",
        r[5] == kSaveQuick ? "a quick-save" : "the save screen");
}

void ReleaseSaveHold(const char* why) {
    if (!g_save_hold) return;
    g_save_hold = false;
    g_save_mine = false;
    // as after a correction: the hashes from before it are not compared
    g_judge.CorrectionDone(g_coop_host ? static_cast<long>(g_serve_gen) : g_loaded_gen, g_save_hold_frame);
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (world) *reinterpret_cast<float*>(world + 0x2e7) = *reinterpret_cast<float*>(world + 0x2f7);  // no frames owed
    Log("co-op: save: going on (%s, held %lu ms)", why, GetTickCount() - g_save_hold_since);
}

void SaveDoneArrived(const uint8_t* r) {  // HandleCoopCommand: kSaveDone, as it arrives
    unsigned saver;
    memcpy(&saver, r + 7, 4);
    if (!g_save_hold && saver != MySaverId()) {
        g_save_early_saver = saver, g_save_early_shadow = (r[5] & 1) != 0, g_save_early_tick = GetTickCount();
        Log("co-op: save: the saver's done came before our hold: kept for it");
        return;
    }
    if (!g_save_hold || saver != g_save_saver || saver == MySaverId()) return;
    g_save_done_due = true;
    g_save_shadow_due = (r[5] & 1) != 0;
    Log("co-op: save: the saver is done (%s)", g_save_shadow_due ? "saved: our own save follows" : "nothing saved");
}

// Host, main thread: the saver is gone (left, or lost) during its save's hold. Every copy waited for its kSaveDone up
// to kSaveHoldMaxMs, 10 minutes (the fuzzer, test/fuzz: a joiner opened the save screen and left). The host sends it
// instead, as "nothing saved": every copy goes on, without a save, as when the saver cancels.
void CloseAbandonedSave(unsigned joiner, const char* who) {
    if (!g_coop_host || !g_save_hold || g_save_saver != joiner) return;
    uint8_t rec[1][27];
    CoopRecord(rec[0], kSaveDone, 0);
    rec[0][5] = 0;  // nothing saved
    memcpy(rec[0] + 7, &joiner, 4);
    SendCoopRecords(rec, 1);
    Log("co-op: save: %s left during their save: everyone goes on, nothing saved", who);
    Notice("Co-op: %s left while saving - the game goes on", who);
}

// Main thread, from the application's input pump: outside the world's step, as a click is.
void SavePumpTick() {
    DWORD now = GetTickCount();
    uint8_t* game_main = *reinterpret_cast<uint8_t**>(kGameMainPtr);
    if (g_save_requested && !g_save_hold && now - g_save_asked > kSaveHoldWaitMs) {
        g_save_requested = false;  // the world stood still (a pause, a held copy): save alone, as before
        Log("co-op: save: no frame to hold at after %lu ms: saving alone", now - g_save_asked);
        if (g_save_kind == kSaveQuick && game_main) OurQuickSave(game_main);
        if (g_save_kind == kSaveScreen && g_save_menu) PressSaveButton(g_save_menu, g_save_button);
        return;
    }
    if (!g_save_hold) return;
    if (g_save_mine) {
        g_save_mine = false;
        bool saved = false;
        if (g_save_hold_kind == kSaveQuick && game_main) {
            int notice = *reinterpret_cast<int*>(game_main + 0x15c);
            OurQuickSave(game_main);
            saved = *reinterpret_cast<int*>(game_main + 0x15c) != 0 && notice == 0;  // its "saved" notice: it wrote
        } else if (g_save_hold_kind == kSaveScreen && g_save_menu) {
            g_saveload_result = 0;
            PressSaveButton(g_save_menu, g_save_button);  // returns once the save screen has closed
            saved = g_saveload_result == 4;
        }
        uint8_t rec[1][27];
        CoopRecord(rec[0], kSaveDone, 0);
        rec[0][5] = saved ? 1 : 0;
        unsigned me = MySaverId();
        memcpy(rec[0] + 7, &me, 4);
        SendCoopRecords(rec, 1);
        Log("co-op: save: ours is done (%s): the others write theirs", saved ? "saved" : "nothing saved");
        ReleaseSaveHold(saved ? "we saved" : "we did not save");
        return;
    }
    if (g_save_done_due) {
        g_save_done_due = false;
        if (g_save_shadow_due && game_main) {
            // The same write here, so its changes are the same as the saver's. The quick-save's own "saved" notice
            // countdown only follows the saver's for a quick-save.
            int notice = *reinterpret_cast<int*>(game_main + 0x15c);
            *reinterpret_cast<int*>(game_main + 0x15c) = 0;
            GuardQuickSave();
            OurQuickSave(game_main);
            RestoreQuickSave();
            if (g_save_hold_kind != kSaveQuick) *reinterpret_cast<int*>(game_main + 0x15c) = notice;
        }
        ReleaseSaveHold(g_save_shadow_due ? "wrote the same save as the saver" : "the saver did not save");
        return;
    }
    if (now - g_save_hold_since > kSaveHoldMaxMs) ReleaseSaveHold("the saver went quiet");
}

void __fastcall AppPumpHook(void* app, void* edx) {
    g_app_pump(app, edx);
    if (g_coop_host || g_coop_join) SavePumpTick();
    if (g_test.enabled) BotPumpTick();
}

int __fastcall SaveLoadModalHook(uint8_t* self, void* edx) {
    g_saveload_window = self;
    g_saveload_opened = GetTickCount();
    int r = g_saveload_modal(self, edx);
    g_saveload_window = nullptr;
    g_saveload_result = r;
    return r;
}

void InstallSaveSyncHooks() {
    g_save_in_step = GetPrivateProfileIntA("coop", "save_in_step", 1, ".\\ftcoop.ini") != 0;
    g_quicksave_tramp = HookWithTrampoline(kQuickSave, kSavePrologue, sizeof(kSavePrologue), (void*)&QuickSaveHook);
    g_savebutton_tramp = HookWithTrampoline(kSaveButton, kSavePrologue, sizeof(kSavePrologue), (void*)&SaveButtonHook);
    bool modal = PatchSlot(kSaveLoadModalSlot, 0x6c6190, (void*)&SaveLoadModalHook, &g_saveload_modal);
    bool pump = PatchSlot(kAppPumpSlot, 0x6bd850, (void*)&AppPumpHook, &g_app_pump);
    Log("co-op: save hooks: quick-save %s, Save button %s, save screen %s, input pump %s%s", g_quicksave_tramp ? "ok" : "FAILED",
        g_savebutton_tramp ? "ok" : "FAILED", modal ? "ok" : "FAILED", pump ? "ok" : "FAILED",
        g_save_in_step ? "" : " (save_in_step=0: saves are not held)");
}
