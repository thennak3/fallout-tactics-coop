// ---- The host's say over who talks to an NPC, with nothing of the game in it (private_sync.inc HostFilterTalks uses
// it on the game; the fuzz target test/fuzz runs the same code on a fake world) ----
// Two players clicking the same NPC at the same moment: the first talk (or claim) the host relays wins; a talk or claim
// on that NPC from anyone else is dropped until the winner's kSyncDone comes through. A lock this old (a talk lost, a
// player gone) no longer holds. A session's start clears them all (a re-sync while a joiner shopped left the merchant
// locked for the rest of the night, 0.10.3).
#pragma once
#include <cstdint>

namespace talk {

const uint32_t kLockMaxMs = 10 * 60 * 1000;

struct HostLock {
    uint16_t npc;
    int sender;
    uint32_t since;
};

struct HostLocks {
    HostLock locks[16] = {};

    // A talk or claim on npc from sender: true to pass it on, false to drop it (*holder: who has that NPC).
    bool Pass(uint16_t npc, int sender, uint32_t now, int* holder = nullptr) {
        HostLock* held = nullptr;
        for (HostLock& l : locks)
            if (l.npc == npc && now - l.since < kLockMaxMs) held = &l;
        if (held && held->sender != sender) {
            if (holder) *holder = held->sender;
            return false;
        }
        if (!held)
            for (HostLock& l : locks)
                if (!l.npc || now - l.since >= kLockMaxMs) {
                    held = &l;
                    break;
                }
        if (held) {
            held->npc = npc;
            held->sender = sender;
            held->since = now;
        }
        return true;
    }

    void Done(uint16_t npc, int sender) {  // the talker's kSyncDone
        for (HostLock& l : locks)
            if (l.npc == npc && l.sender == sender) l.npc = 0;
    }

    void Free(uint16_t npc) {  // its talker is gone
        for (HostLock& l : locks)
            if (l.npc == npc) l.npc = 0;
    }

    int Clear() {  // a new session: returns how many were held
        int n = 0;
        for (HostLock& l : locks)
            if (l.npc) n++, l.npc = 0;
        return n;
    }

    bool Held(uint16_t npc, uint32_t now) const {
        for (const HostLock& l : locks)
            if (l.npc == npc && now - l.since < kLockMaxMs) return true;
        return false;
    }
};

}  // namespace talk
