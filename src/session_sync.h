// ---- What every copy takes from the host at a session's start, and when a re-sync may save: the parts with nothing
// of the game in them (settings_sync.inc and save_transfer.inc use them on the game; the fuzz target test/fuzz runs the
// same code on a fake world) ----
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace session {

// The quartermasters' stock goes out as "name<TAB>count<LF>..." in records of 20 bytes each ([4] the record's index, [5]
// how many there are). The last record ends with the text's 0.
const int kStockChunk = 20;
const int kStockMaxChunks = 255;
const int kStockMaxText = kStockMaxChunks * kStockChunk;

inline int StockChunks(int text_len) { return text_len / kStockChunk + 1; }

inline void StockChunk(const char* text, int text_len, int i, uint8_t* out) {
    memset(out, 0, kStockChunk);
    int len = text_len + 1 - i * kStockChunk;
    if (len > kStockChunk) len = kStockChunk;
    if (len > 0) memcpy(out, text + i * kStockChunk, len);
}

// Every copy, as the records act (the same frame everywhere). A record lost or out of order: this session keeps its
// own list (Feed never completes).
struct StockAssembler {
    char text[kStockMaxText + 1];
    int got = 0;
    bool Feed(int i, int chunks, const uint8_t* payload) {
        if (i == 0) got = 0;
        if (i != got || chunks <= 0 || chunks > kStockMaxChunks) return false;
        memcpy(text + i * kStockChunk, payload, kStockChunk);
        if (++got < chunks) return false;
        text[chunks * kStockChunk] = 0;
        return true;
    }
};

// Each "name<TAB>count" line of the text, in order (the text is cut up in place).
template <class F>
void ForEachStockLine(char* text, F f) {
    for (char* line = text; *line;) {
        char* tab = strchr(line, '\t');
        char* end = strchr(line, '\n');
        if (!tab || !end || tab > end) break;
        *tab = 0;
        *end = 0;
        f(line, atoi(tab + 1));
        line = end + 1;
    }
}

// The entity serial counter (0x8be148) is not in a save: at the start of every session every copy takes the host's,
// well past anything any copy has handed out yet.
const uint32_t kSerialMargin = 0x100000;
inline uint32_t SessionSerial(uint32_t host_serial) { return host_serial + kSerialMargin; }

// A re-sync's quick-save waits for any shop to close (saved meanwhile, the merchant's own inventory is set aside for
// the shop's stock), but not for ever: a player's shop may stay open a long time.
const uint32_t kResyncShopWaitMs = 120000;
enum class Gate { Save, Wait, SaveAfterWaiting };
inline Gate ResyncGate(bool shop_busy, bool forced, uint32_t waited_ms) {
    if (!shop_busy || forced) return Gate::Save;
    return waited_ms < kResyncShopWaitMs ? Gate::Wait : Gate::SaveAfterWaiting;
}

}  // namespace session
