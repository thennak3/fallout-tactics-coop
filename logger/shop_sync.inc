// ---- A shop that pauses nobody (the quartermaster's barter, [coop] shop_no_pause) ----
// Included from private_sync.inc, after the delta code. The owner's design: the shop's allocation happens on every
// copy at the same lockstep frame, the window only on the shopper's; the shopper's world keeps running; nobody waits;
// the shopper's character is locked; the deal reaches everyone when the shop closes.
//   Barter pressed (shopper): kShopOpen goes out instead of the screen opening (BarterButtonHook).
//   kShopOpen at its frame: the shopper opens the screen (the game's own handler: its constructor 0x454330 makes the
//     offer inventories and the stock); every other copy makes the same entities with the same calls (HeadlessShopOpen).
//   The screen closes (shopper): before its destructor 0x454f40 undoes anything, the deal goes out as a delta (the
//     buyer's and merchant's inventories, new stacks) and kShopClose (QmDtorHook).
//   kShopClose at its frame: every other copy applies the delta and does what the destructor does (HeadlessShopClose),
//     so the campaign's stock list (rebuilt from what is left) is the same everywhere.
// Calls and offsets: NOTES (the quartermaster's shop). EntityID as a u32: seq in the low half, slot in the high half.

struct ShopState {
    bool open, mine;
    uint32_t buyer, merchant;  // EntityIDs
    uint32_t orig_inventory;   // the merchant's own inventory, back at close
};
ShopState g_shop = {};

// Exploration (test mode): entity-table entries a step changed (used, next_free, seq), against the last mark.
SlotState g_step_before[kMaxSlots];
int g_step_n = 0, g_step_frames = 0;
void TableStepMark(uint8_t* world) {
    if (g_test.enabled && world) ReadSlots(world, g_step_before, &g_step_n);
}
void TableStepLog(uint8_t* world, const char* what) {
    if (!g_test.enabled || !world) return;
    static SlotState now[kMaxSlots];
    int n = 0;
    ReadSlots(world, now, &n);
    char line[1500] = "";
    int l = 0, k = 0;
    for (int i = 1; i < n && i < g_step_n && l < 1400; i++)
        if (now[i].used != g_step_before[i].used || now[i].next_free != g_step_before[i].next_free || now[i].seq != g_step_before[i].seq) {
            k++;
            l += sprintf_s(line + l, sizeof(line) - l, " %d:%d/%d/%d>%d/%d/%d", i, g_step_before[i].used, g_step_before[i].next_free,
                           g_step_before[i].seq, now[i].used, now[i].next_free, now[i].seq);
        }
    Log("tablestep: %s: %d entries (used/next/seq):%s; head %u", what, k, line, *reinterpret_cast<uint16_t*>(world + kWorldFreeHead));
    ReadSlots(world, g_step_before, &g_step_n);
}
extern bool g_shop_close_waits;
void RunDeferredNew(uint8_t* world);
bool g_shop_requested = false;  // this copy's Barter press went out as kShopOpen

bool g_shop_exec = false;       // this copy is running the game's own Barter handler for kShopOpen
uint8_t* g_shop_speech = nullptr;
void* g_shop_button = nullptr;
uint8_t* g_barter_trampoline = nullptr;  // the conversation's Barter button handler 0x4d0000
uint8_t* g_qm_dtor_trampoline = nullptr; // the quartermaster barter screen's destructor 0x454f40

uint16_t SlotOf(uint32_t id) { return static_cast<uint16_t>(id >> 16); }

uint8_t* EntityOf(uint8_t* world, uint32_t id) {  // nullptr if the EntityID is stale
    uint16_t slot = SlotOf(id);
    if (!slot || slot >= EntityCount(world)) return nullptr;
    if ((FullId(world, slot) & 0xffff) != (id & 0xffff)) return nullptr;
    uint8_t* e = EntityById(world, slot);
    return e == *reinterpret_cast<uint8_t**>(Entries(world)) ? nullptr : e;
}

template <typename R, typename... A>
R VCall(uint8_t* ent, int offset, A... args) {
    return reinterpret_cast<R(__thiscall*)(void*, A...)>((*reinterpret_cast<void***>(ent))[offset / 4])(ent, args...);
}

uint32_t IdOfEntity(uint8_t* world, uint8_t* ent) { return FullId(world, *reinterpret_cast<uint16_t*>(ent + 4)); }

void LogShopState(uint8_t* world, const char* when) {  // test mode
    if (!g_test.enabled || !world) return;
    char inv[2048] = "?";
    InvString(world, SlotOf(g_shop.merchant), inv, sizeof(inv));
    Log("co-op: shop %s: merchant's inventory %s", when, inv);
    for (int i = 0; i < g_shop_new_n; i++) {
        uint16_t id = g_shop_new[i];
        uint8_t* e = id < EntityCount(world) ? EntityById(world, id) : nullptr;
        bool used = e && e != *reinterpret_cast<uint8_t**>(Entries(world));
        Log("co-op: shop %s: new stack %u %s%s", when, id, used ? "alive" : "gone", used && e[0x141] ? " (being destroyed)" : "");
        if (used && strstr(when, "before"))
            for (int off = 0; off < 0x200; off += 64) {
                char hex[129];
                for (int q = 0; q < 64; q++) sprintf_s(hex + q * 2, 3, "%02x", e[off + q]);
                Log("SDUMP id=%u off=%04x %s", id, off, hex);
            }
    }
}

void LogCampaignStock(const char* when) {  // test mode: the campaign's stock list, templates and counts
    if (!g_test.enabled) return;
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(0x8bdd2c);
    if (!campaign) return;
    wchar_t** tb = *reinterpret_cast<wchar_t***>(campaign + 0x151);
    wchar_t** te = *reinterpret_cast<wchar_t***>(campaign + 0x155);
    int* cnt = *reinterpret_cast<int**>(campaign + 0x161);
    static char line[8192];
    size_t n = 0;
    for (int i = 0; tb && tb + i < te && n + 200 < sizeof(line); i++) {
        char name[160] = "";
        WideCharToMultiByte(CP_ACP, 0, tb[i] ? tb[i] : L"", -1, name, sizeof(name), nullptr, nullptr);
        const char* base = strrchr(name, '/');
        n += sprintf_s(line + n, sizeof(line) - n, " %s=%d", base ? base + 1 : name, cnt ? cnt[i] : -1);
    }
    Log("co-op: campaign stock %s:%s", when, line);
}

uint8_t* NewInventory() { return reinterpret_cast<uint8_t*(__cdecl*)()>(0x5b3a60)(); }

void DestroyEntity(uint8_t* ent) {
    if (ent && !ent[0x141]) VCall<void>(ent, 0x28);
}

// What the quartermaster barter screen's constructor 0x454330 allocates, in its order.
int HeadlessShopOpen(uint8_t* world) {
    *reinterpret_cast<uint32_t*>(0x8bc208) = g_shop.merchant;  // the pair the Barter handler sets
    *reinterpret_cast<uint32_t*>(0x8bc218) = g_shop.buyer;
    *reinterpret_cast<uint32_t*>(0x8bc20c) = g_shop.merchant;  // and the constructor's copies
    *reinterpret_cast<uint32_t*>(0x8bc210) = g_shop.buyer;
    uint8_t* buyer = EntityOf(world, g_shop.buyer);
    uint8_t* merchant = EntityOf(world, g_shop.merchant);
    if (!buyer || !merchant) {
        Log("co-op: shop: buyer or merchant missing here");
        return -1;
    }
    VCall<void, uint32_t>(buyer, 0x65c, IdOfEntity(world, NewInventory()));     // offer inventories (slot 407)
    VCall<void, uint32_t>(merchant, 0x65c, IdOfEntity(world, NewInventory()));
    if (!VCall<bool>(merchant, 0x3f4)) return 0;  // IsQuartermaster (253): no stock of its own to make
    uint32_t tmp = 0;
    g_shop.orig_inventory = *VCall<uint32_t*, uint32_t*>(merchant, 0x64c, &tmp);  // GetInventoryID (403)
    uint32_t stock = IdOfEntity(world, NewInventory());
    uint8_t* campaign = *reinterpret_cast<uint8_t**>(0x8bdd2c);
    uint8_t* begin = *reinterpret_cast<uint8_t**>(campaign + 0x151);
    uint8_t* end = *reinterpret_cast<uint8_t**>(campaign + 0x155);
    int* counts = *reinterpret_cast<int**>(campaign + 0x161);
    int made = 0;
    for (int i = 0; begin && begin + i * 4 < end; i++) {
        int count = counts[i];
        if (count <= 0) continue;
        uint32_t out[2] = {};
        reinterpret_cast<void*(__cdecl*)(uint32_t*, void*, int)>(0x5ea400)(out, begin + i * 4, -1);  // MakeEntity
        uint8_t* item = EntityOf(world, out[0]);
        if (!item) continue;
        VCall<void, int>(item, 0x1a4, count);  // SetCount (105)
        uint8_t res[32] = {};                  // InventoryActionResult
        uint32_t zero = 0;
        uint8_t* stock_ent = EntityOf(world, stock);
        if (stock_ent) VCall<void*, void*, uint32_t, uint32_t*, int, int>(stock_ent, 0x504, res, out[0], &zero, count, 1);
        if (!stock_ent || *reinterpret_cast<int*>(res)) DestroyEntity(item);  // AddToInventory (321) refused it
        else made++;
    }
    VCall<void, uint32_t>(merchant, 0x650, stock);  // SetInventoryID (404): the stock is what the merchant sells
    LogCampaignStock("at the open");
    return made;
}

// What the destructor 0x454f40 does (its UI parts left out).
void HeadlessShopClose(uint8_t* world) {
    uint8_t* buyer = EntityOf(world, g_shop.buyer);
    uint8_t* merchant = EntityOf(world, g_shop.merchant);
    if (merchant && VCall<bool>(merchant, 0x3f4)) {
        uint8_t* campaign = *reinterpret_cast<uint8_t**>(0x8bdd2c);
        // the stock list is rebuilt from what is left in the shop
        reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(0x403010)(
            campaign + 0x14d, *reinterpret_cast<void**>(campaign + 0x151), *reinterpret_cast<void**>(campaign + 0x155));
        reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(0x458760)(
            campaign + 0x15d, *reinterpret_cast<void**>(campaign + 0x161), *reinterpret_cast<void**>(campaign + 0x165));
        uint32_t tmp = 0;
        uint8_t* stock = EntityOf(world, *VCall<uint32_t*, uint32_t*>(merchant, 0x64c, &tmp));
        if (stock && *reinterpret_cast<uintptr_t*>(stock) == kInventoryVtable) {
            uint8_t* head = *reinterpret_cast<uint8_t**>(stock + kInventoryList);
            for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node && node != head;
                 node = *reinterpret_cast<uint8_t**>(node)) {
                uint8_t* item = EntityOf(world, *reinterpret_cast<uint32_t*>(node + 8));
                if (!item) continue;
                int count = VCall<int>(item, 0x188);
                reinterpret_cast<void(__thiscall*)(void*, void*, int)>(0x6a6550)(campaign, item + 0x199, count);
            }
        }
        // Destroying the stock takes its items with it, but not a stack only written into its list (a deal's new
        // stack made from its bytes here: the paid ringpulls stayed on this copy). So its items go first, in the
        // list's order; the stock's own destructor finds them already being destroyed.
        if (g_test.enabled)
            Log("co-op: shop close here: stock %08x (%s), merchant's +0x1006 %08x", stock ? IdOfEntity(world, stock) : 0,
                stock ? RttiName(stock) : "-", *reinterpret_cast<uint32_t*>(merchant + kActorInventory));
        if (stock && *reinterpret_cast<uintptr_t*>(stock) == kInventoryVtable) {
            uint8_t* head = *reinterpret_cast<uint8_t**>(stock + kInventoryList);
            for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node && node != head;
                 node = *reinterpret_cast<uint8_t**>(node)) {
                uint32_t id = *reinterpret_cast<uint32_t*>(node + 8);
                uint8_t* item = EntityOf(world, id);
                if (g_test.enabled && SlotOf(id) == 1081)
                    Log("co-op: shop close here: stock item %08x -> %p (table seq %04x)", id, item,
                        FullId(world, SlotOf(id)) & 0xffff);
                DestroyEntity(item);
            }
        }
        DestroyEntity(stock);
        VCall<void, uint32_t>(merchant, 0x650, g_shop.orig_inventory);
    }
    uint32_t tmp = 0;
    if (buyer) DestroyEntity(EntityOf(world, *VCall<uint32_t*, uint32_t*>(buyer, 0x658, &tmp)));    // offer inventories
    if (merchant) DestroyEntity(EntityOf(world, *VCall<uint32_t*, uint32_t*>(merchant, 0x658, &tmp)));
    *reinterpret_cast<uint32_t*>(0x8bc210) = 0;
    *reinterpret_cast<uint32_t*>(0x8bc20c) = 0;
    *reinterpret_cast<uint32_t*>(0x8bc204) = 0;
}

void __fastcall BarterButtonHook(uint8_t* speech, void* /*edx*/, void* button) {
    auto original = reinterpret_cast<void(__thiscall*)(void*, void*)>(g_barter_trampoline);
    extern bool g_my_conv_light;
    if (!g_my_conv_light || g_shop_exec || !(g_coop_host || g_coop_join)) {
        original(speech, button);
        return;
    }
    if (g_shop.open || g_shop_requested) {
        Notice("Co-op: someone is already trading - try again when they are done");
        return;
    }
    uint32_t buyer = *reinterpret_cast<uint32_t*>(speech + 0x100), merchant = *reinterpret_cast<uint32_t*>(speech + 0x104);
    uint8_t r[27];
    CoopRecord(r, kShopOpen, 0);
    memcpy(r + 7, &buyer, 4);
    memcpy(r + 11, &merchant, 4);
    memcpy(r + 15, &g_my_claim, 4);
    SendCoopRecords(reinterpret_cast<const uint8_t(*)[27]>(r), 1);
    g_shop_requested = true;
    g_shop_speech = speech;
    g_shop_button = button;
    Log("co-op: Barter pressed: shop of NPC %u for squad member %u goes out (claim %08x)", SlotOf(merchant), SlotOf(buyer),
        g_my_claim);
}

void ExecuteShopOpen(const uint8_t* r) {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    uint32_t buyer, merchant, claim;
    memcpy(&buyer, r + 7, 4);
    memcpy(&merchant, r + 11, 4);
    memcpy(&claim, r + 15, 4);
    g_shop = {};
    g_shop.open = true;
    g_shop.buyer = buyer;
    g_shop.merchant = merchant;
    g_shop.mine = g_shop_requested && claim && claim == g_my_claim;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    if (g_shop.mine && g_shop_speech && g_shop_speech == g_private_speech) {
        g_shop_exec = true;
        reinterpret_cast<void(__thiscall*)(void*, void*)>(g_barter_trampoline)(g_shop_speech, g_shop_button);
        g_shop_exec = false;
        TakePrivateSnapshot();  // the deal is measured from here: the stock exists on every copy now
        Log("co-op: shop of NPC %u open for this player at frame %d", SlotOf(merchant), frame);
    } else {
        if (g_shop.mine) {
            Log("co-op: our conversation closed before the shop opened: the stock is made without a window");
            g_shop.mine = false;
        }
        int made = HeadlessShopOpen(world);
        Log("co-op: shop of NPC %u open for another player at frame %d: stock made here without a window (%d items)",
            SlotOf(merchant), frame, made);
        Notice("Co-op: a player is trading - their character waits until they are done");
    }
    g_shop_requested = false;
}

void __fastcall QmDtorHook(uint8_t* self, void* /*edx*/) {
    auto original = reinterpret_cast<void(__thiscall*)(void*)>(g_qm_dtor_trampoline);
    bool closing = g_shop.open && g_shop.mine;
    if (closing) {  // before the destructor undoes anything: the deal, measured from the shop's opening
        // The destructor's first step, 0x4569a0, hands back what is still on the offer tables (out of each offer
        // inventory, vt+0x508, into its owner's, vt+0x504; a stack that merges is destroyed). Done here first, so the
        // deal includes it (trade_host, shop_no_pause: a stack the screen destroyed only on the shopper's copy);
        // the destructor's own call then finds the tables empty.
        uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
        int pending_before = world ? *reinterpret_cast<int*>(world + 0x75d) : -1;
        reinterpret_cast<void(__thiscall*)(void*)>(0x4569a0)(self);
        // The stacks the screen split off for the offer tables exist on this copy only, and handing them back marks
        // them for destruction (World::DestroyEntity 0x4e4a30: the pending list World+0x759, count +0x75d, seq + 1);
        // the frame after, World 0x4e4bb0 frees them (slot onto the free list, seq + 1 again). Measured before that,
        // the deal gave the others their slots as they were before being freed, and the shopper's free list then went
        // its own way (shoptalk: Brotherhood Scrip, the whole stack spent, four slots one seq apart). So they are freed
        // now, before the deal is measured, when nothing else was pending (then nothing else is freed early here).
        if (world && pending_before == 0 && *reinterpret_cast<int*>(world + 0x75d) > 0) {
            Log("co-op: shop: freeing the %d stacks handed back from the offer tables before measuring the deal",
                *reinterpret_cast<int*>(world + 0x75d));
            reinterpret_cast<void(__thiscall*)(void*)>(0x4e4bb0)(world);
        } else if (world && pending_before != 0) {
            Log("co-op: shop: %d destroys were pending before the hand-back: the handed-back stacks are freed next frame",
                pending_before);
        }
        extern int g_diff_mode;
        extern uint8_t g_done_kind;
        g_diff_mode = 2;
        g_done_kind = kShopClose;
        PrivateScreensDiff();
        extern bool g_my_conv_light;
        g_diff_mode = g_my_conv_light ? 1 : 0;
        g_done_kind = kSyncDone;
    }
    if (closing) LogShopState(*reinterpret_cast<uint8_t**>(kWorldPtr), "before the close");
    if (closing) TableStepMark(*reinterpret_cast<uint8_t**>(kWorldPtr));
    original(self);
    if (closing) {
        TableStepLog(*reinterpret_cast<uint8_t**>(kWorldPtr), "the real close (the shopper's destructor)");
        g_step_frames = 4;
        LogShopState(*reinterpret_cast<uint8_t**>(kWorldPtr), "after the close");
        LogCampaignStock("after the close");
        g_shop.mine = false;  // the others close at kShopClose's frame
        TakePrivateSnapshot();  // what the rest of the conversation changes (variables) is measured from here
    }
}

// ---- deltas applied at their lockstep frame (a shop's close; a conversation that paused nobody) ----
struct FrameDelta { uint8_t kind; bool ok, mine; int len; uint8_t* data; };
FrameDelta g_frame_deltas[4];
int g_frame_head = 0, g_frame_count = 0;

void QueueFrameDelta(uint8_t kind, bool ok, bool mine, const uint8_t* data, int len) {
    if (g_frame_count == 4) {
        Log("co-op: too many deltas waiting for their frame: the oldest is dropped");
        g_frame_head = (g_frame_head + 1) % 4;
        g_frame_count--;
    }
    FrameDelta& f = g_frame_deltas[(g_frame_head + g_frame_count) % 4];
    free(f.data);
    f = {kind, ok, mine, ok ? len : 0, ok && len > 0 ? static_cast<uint8_t*>(malloc(len)) : nullptr};
    if (f.data) memcpy(f.data, data, len);
    g_frame_count++;
}

bool TakeWorldFix(const uint8_t** data, int* len);  // drift.inc

// ActorActHook: a co-op command at its frame, on every copy.
void FixHoldAtFrame(const uint8_t* r);  // drift.inc

void CoopCommandAtFrame(const uint8_t* r) {
    uint8_t kind = r[3];
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (kind == kFixHold) {
        FixHoldAtFrame(r);
        return;
    }
    if (kind == kGameSettings) {
        SettingsAtFrame(r);
        return;
    }
    if (kind == kShopOpen) {
        ExecuteShopOpen(r);
        return;
    }
    bool at_frame = kind == kShopClose || (kind == kSyncDone && (r[8] & 4));
    if (!at_frame || !world) return;
    if (!g_frame_count) {
        Log("co-op: a delta's frame came, but nothing is waiting for it");
        return;
    }
    FrameDelta& f = g_frame_deltas[g_frame_head];
    g_frame_head = (g_frame_head + 1) % 4;
    g_frame_count--;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    g_applying_at_frame = true;
    g_shop_new_n = 0;
    TableStepMark(world);
    if (!f.mine && f.ok && f.len) ApplyDelta(f.data, f.len);
    uint16_t marker;
    memcpy(&marker, r + 5, 2);
    const uint8_t* fix = nullptr;
    int fix_len = 0;
    if (f.mine && marker == 0xffff && TakeWorldFix(&fix, &fix_len)) {  // the host's own drift correction, at its frame
        ApplyDelta(fix, fix_len);
        Log("co-op: drift correction applied here too at frame %d", frame);
    }
    if (!f.mine && kind == kShopClose) TableStepLog(world, "the deal applied here");
    g_applying_at_frame = false;
    if (kind == kShopClose) {
        if (!f.mine && g_shop.open && g_frame_b) {
            g_shop_close_waits = true;  // after the bookkeeping, at the start of the next frame
        } else if (!f.mine && g_shop.open) {
            LogShopState(world, "before the close");
            HeadlessShopClose(world);
            TableStepLog(world, "the headless close");
            LogShopState(world, "after the close");
            LogCampaignStock("after the close");
        }
        if (!g_shop_close_waits) g_shop.open = false;
        Log("co-op: shop closed at frame %d (%s)", frame, f.mine ? "ours" : "the others' deal applied here");
    } else {
        Log("co-op: a conversation's changes applied at frame %d (%s)", frame, f.mine ? "ours" : "theirs");
        // Its NPC is free from this frame on every copy: freed on arrival, the talker's copy (its own command
        // comes back first) let the NPC move a frame early, and the copies drifted apart (trade_host, 4 players).
        uint16_t npc;
        memcpy(&npc, r + 5, 2);
        if (NpcLock* l = FindLock(npc)) l->npc = 0;
        if (npc == g_host_hold_npc) HostReleaseHold("the other player is done");
        Log("co-op: NPC %u is free again (at its frame) at frame %d", npc, frame);
    }
}

// Client send, every copy but the shopper's while a shop is open: orders for the shopper's character are taken out
// of the message (it stays where it is until the deal is done), and a talk to a recruiter is refused (recruiting
// changes the squad the shopper's character belongs to). Returns the message's new length (0: nothing left to send).
int RefuseShopperOrders(uint8_t* msg, int len) {
    if (!g_shop.open || g_shop.mine || len < 6 || *reinterpret_cast<uint32_t*>(msg) != 2) return len;
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    int count = *reinterpret_cast<uint16_t*>(msg + 4);
    uint16_t buyer = SlotOf(g_shop.buyer);
    for (int i = 0; i < count && 6 + (i + 1) * 27 <= len;) {
        uint8_t* r = msg + 6 + i * 27;
        uint16_t actor, npc;
        memcpy(&actor, r + 1, 2);
        memcpy(&npc, r + 5, 2);
        bool drop = false;
        if (r[0] == kCoopCmd) {
        } else if (r[0] == 0x17 && world && npc < EntityCount(world) && IsActor(world, npc) &&
                   VCall<bool>(EntityById(world, npc), 0x3f0)) {  // IsRecruitMaster (252)
            Log("co-op: refused this player's talk to recruiter %u: a player is trading", npc);
            Notice("Co-op: a player is trading - recruit when they are done");
            drop = true;
        } else if (r[0] == 0x17) {
            // Any other conversation would hold every copy, the shopper's too, while its shop is open; the shop's close
            // then ran its destroys and bookkeeping under that hold in another order than everywhere else, and five
            // entity slots stayed one sequence number apart (shoptalk). So it waits until the shop is done.
            Log("co-op: refused this player's talk to NPC %u: a player is trading", npc);
            Notice("Co-op: a player is trading - talk to them when they are done");
            drop = true;
        } else if (actor == buyer) {
            Log("co-op: refused this player's order %02x for squad member %u: it is trading", r[0], actor);
            Notice("Co-op: that character is trading - wait for them");
            drop = true;
        }
        if (!drop) {
            i++;
            continue;
        }
        memmove(r, r + 27, len - (r + 27 - msg));
        len -= 27;
        count--;
        *reinterpret_cast<uint16_t*>(msg + 4) = static_cast<uint16_t>(count);
    }
    return count ? len : 0;
}

bool ShopOpenForOther(uint16_t* buyer) {
    if (!g_shop.open || g_shop.mine) return false;
    *buyer = SlotOf(g_shop.buyer);
    return true;
}

bool g_shop_close_waits = false;

// Frame hook, at the start of a frame: what a delta at its frame left for after its destroys finished: the creates
// that waited for their slot (DeferCreate), the bookkeeping, then the shop's close.
void RunDeferredCreates() {
    uint8_t* world = *reinterpret_cast<uint8_t**>(kWorldPtr);
    if (!world) return;
    if (g_step_frames > 0) {
        g_step_frames--;
        TableStepLog(world, "frame start");
    }
    RunDeferredNew(world);
    if (g_frame_b) TableStepLog(world, "the waiting create");
    if (g_frame_b) {
        ApplyBookkeeping(world, g_frame_b);
        TableStepLog(world, "the deal's bookkeeping");
        g_step_frames = 4;
        free(g_frame_b);
        g_frame_b = nullptr;
        Log("co-op: delta bookkeeping written after its destroys finished");
    }
    if (g_shop_close_waits) {
        g_shop_close_waits = false;
        LogShopState(world, "before the close");
        HeadlessShopClose(world);
        TableStepLog(world, "the headless close (a frame late)");
        LogShopState(world, "after the close");
        LogCampaignStock("after the close");
        g_shop.open = false;
        Log("co-op: shop closed here a frame after its deal, at frame %d", *reinterpret_cast<int*>(world + kWorldFrame));
    }
}

void RunDeferredNew(uint8_t* world) {
    if (!g_ndeferred_new) return;
    for (int i = 0; i < g_ndeferred_new; i++) {
        DeferredNew& d = g_deferred_new[i];
        int made = 0, kids = 0;
        int conflicts = ApplyNewEntities(world, &d.ne, 1, &made, &kids);
        // Its slot's old entity is gone now and this is a new one: off the list of destroys to repeat (destroying
        // "what is still there" destroyed this new stack: shoptalk, a garbage count on the joiner).
        extern uint16_t g_destroyed[512];
        extern int g_ndestroyed;
        for (int k = 0; k < g_ndestroyed; k++)
            if (g_destroyed[k] == d.ne.slot) g_destroyed[k] = 0xffff;
        uint8_t* e = Entries(world);
        if (d.have_b) {
            *reinterpret_cast<uint16_t*>(e + d.ne.slot * 8 + 4) = d.next_free;
            *reinterpret_cast<int16_t*>(e + d.ne.slot * 8 + 6) = d.seq;
        }
        Log("co-op: entity %u made a frame late, its slot free now (%d made, %d conflicts)", d.ne.slot, made, conflicts);
        free(d.copy);
    }
    g_ndeferred_new = 0;
}
