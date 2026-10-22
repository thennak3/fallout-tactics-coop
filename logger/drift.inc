// ---- Drift check: the host re-syncs everyone when a joiner's game has drifted out of step ----
// Included from netlog.cpp after dots.inc. Every copy hashes every actor's position every kDriftEvery frames (at the
// client tick, the same point of the frame on every copy). Joiners send their latest hashes with their selection
// poll (dots.inc, "SEL ... H<gen>:<frame>:<hash>,..."); the host compares them with its own. Two checked frames in a
// row that differ make the host re-sync (quick-save, quick-load, everyone loads it), once nobody has a conversation
// or a shop open, and at most once a minute. A lockstep game never drifts by design; this is the safety net for
// what the mod does not sync yet.
const int kDriftEvery = 25;
const int kDriftRing = 8;
const DWORD kDriftSpacingMs = 60000;
struct DriftRec {
    long gen;
    int frame;
    uint32_t hash;
};
DriftRec g_drift_ring[kDriftRing] = {};
int g_drift_next = 0;
SRWLOCK g_drift_lock = SRWLOCK_INIT;
char g_drift_reason[96] = "";  // host: set by the file server thread, acted on by the main thread
DWORD g_drift_last_resync = 0;

uint32_t PositionHash(uint8_t* world) {
    uint32_t h = 2166136261u;
    for (int i = 1; i < EntityCount(world); i++) {
        if (!IsActor(world, i)) continue;
        h = Fnv(h, EntityPos(world, static_cast<uint16_t>(i)), 12);
        h = Fnv(h, &i, sizeof(i));
    }
    return h;
}

// Main thread, every client tick.
void DriftFrame(uint8_t* world) {
    static int last = -1;
    int frame = *reinterpret_cast<int*>(world + kWorldFrame);
    if (frame == last || frame % kDriftEvery) return;
    last = frame;
    DriftRec r = {g_coop_host ? static_cast<long>(g_serve_gen) : g_loaded_gen, frame, PositionHash(world)};
    AcquireSRWLockExclusive(&g_drift_lock);
    g_drift_ring[g_drift_next] = r;
    g_drift_next = (g_drift_next + 1) % kDriftRing;
    ReleaseSRWLockExclusive(&g_drift_lock);
}

// Joiner: " H<gen>:<frame>:<hash>,..." for the selection poll.
int FormatDrift(char* out, size_t cap) {
    int n = sprintf_s(out, cap, " H");
    AcquireSRWLockShared(&g_drift_lock);
    for (const DriftRec& r : g_drift_ring)
        if (r.frame) n += sprintf_s(out + n, cap - n, "%ld:%d:%08x,", r.gen, r.frame, r.hash);
    ReleaseSRWLockShared(&g_drift_lock);
    return n;
}

// Host, file server thread: a joiner's hashes, from the "H" part of its poll.
void DriftFromJoiner(unsigned id, const char* name, const char* p) {
    struct Seen {
        unsigned id;
        int last_frame, misses;
    };
    static Seen seen[8] = {};
    Seen* s = nullptr;
    for (Seen& x : seen)
        if (x.id == id || (!s && !x.id)) s = &x;
    for (Seen& x : seen)
        if (x.id == id) s = &x;
    if (!s) return;
    s->id = id;
    while (*p && *p != 'H') p++;
    if (*p) p++;
    long gen;
    int frame, used;
    unsigned hash;
    while (sscanf_s(p, "%ld:%d:%x,%n", &gen, &frame, &hash, &used) == 3) {
        p += used;
        if (frame <= s->last_frame || gen != g_serve_gen) continue;
        bool mine = false;
        uint32_t my_hash = 0;
        AcquireSRWLockShared(&g_drift_lock);
        for (const DriftRec& r : g_drift_ring)
            if (r.gen == gen && r.frame == frame) {
                mine = true;
                my_hash = r.hash;
            }
        ReleaseSRWLockShared(&g_drift_lock);
        if (!mine) continue;  // not ours yet (or long gone): compared on a later poll
        s->last_frame = frame;
        if (my_hash == hash) {
            s->misses = 0;
            continue;
        }
        Log("co-op: drift: %s's game differs from ours at frame %d (generation %ld)", name, frame, gen);
        if (++s->misses >= 2 && !g_drift_reason[0])
            sprintf_s(g_drift_reason, "%s's game drifted out of step", name);
    }
}

// Host, main thread, every client tick: re-sync when a joiner drifted and nobody is busy.
void DriftCheckHost() {
    extern bool g_private_open;
    if (!g_drift_reason[0] || g_private_open || g_resync_state != 0) return;
    for (const NpcLock& l : g_npc_locks)
        if (l.npc) return;  // someone's conversation or shop is open
    DWORD now = GetTickCount();
    if (g_drift_last_resync && now - g_drift_last_resync < kDriftSpacingMs) {
        g_drift_reason[0] = 0;
        return;
    }
    static char reason[96];
    strcpy_s(reason, g_drift_reason);
    g_drift_reason[0] = 0;
    g_drift_last_resync = now;
    Notice("Co-op: %s - saving and reloading to bring everyone back together", reason);
    RequestResync(reason);
}
