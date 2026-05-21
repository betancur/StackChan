/*
 * AppRozAgent — Minimal streaming OGG page demuxer for Ogg Opus.
 *
 * Feeds raw bytes, emits Opus packets via callback starting from page 3
 * (skipping OpusHead page 0 and OpusTags page 1).
 *
 * Limitations (acceptable for TTS output):
 *  - Assumes single logical stream (one serial number).
 *  - Packets that span page boundaries are dropped (TTS frames fit in one page).
 *  - No CRC validation.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace roz {

class OggDemux {
public:
    using PacketCb = std::function<void(const uint8_t* data, size_t len)>;

    explicit OggDemux(PacketCb cb) : _cb(std::move(cb)) {}

    // Feed raw HTTP response bytes. Call whenever a chunk arrives.
    void feed(const uint8_t* data, size_t len)
    {
        _buf.insert(_buf.end(), data, data + len);
        process();
    }

    void reset()
    {
        _buf.clear();
        _page_idx = 0;
    }

private:
    PacketCb             _cb;
    std::vector<uint8_t> _buf;
    int                  _page_idx = 0;   // pages consumed so far

    void process()
    {
        while (true) {
            // Minimum OGG page header is 27 bytes
            if (_buf.size() < 27) return;

            // Locate the "OggS" capture pattern
            size_t start = find_magic();
            if (start == SIZE_MAX) {
                // Keep last 3 bytes in case "Ogg" spans a chunk boundary
                size_t keep = (_buf.size() > 3) ? 3 : _buf.size();
                _buf.erase(_buf.begin(), _buf.end() - keep);
                return;
            }
            if (start > 0) _buf.erase(_buf.begin(), _buf.begin() + start);
            if (_buf.size() < 27) return;

            const uint8_t* h     = _buf.data();
            uint8_t        nsegs = h[26];
            size_t         hdr   = 27u + nsegs;

            if (_buf.size() < hdr) return;  // wait for more data

            // Total payload = sum of segment table
            size_t payload = 0;
            for (uint8_t i = 0; i < nsegs; i++) payload += h[27 + i];

            size_t page_len = hdr + payload;
            if (_buf.size() < page_len) return;  // wait for more data

            // Pages 0 & 1 are Opus header/comment — skip them
            if (_page_idx >= 2) {
                emit_packets(h, nsegs, _buf.data() + hdr, payload);
            }

            _page_idx++;
            _buf.erase(_buf.begin(), _buf.begin() + page_len);
        }
    }

    // Walk the segment table and emit complete packets (segments < 255 end a packet).
    void emit_packets(const uint8_t* hdr, uint8_t nsegs,
                      const uint8_t* payload, size_t /*total_payload*/)
    {
        size_t pkt_start = 0;
        size_t pkt_len   = 0;

        for (uint8_t i = 0; i < nsegs; i++) {
            uint8_t seg = hdr[27 + i];
            pkt_len += seg;
            if (seg < 255) {
                // Packet complete
                if (pkt_len > 0) {
                    _cb(payload + pkt_start, pkt_len);
                }
                pkt_start += pkt_len;
                pkt_len    = 0;
            }
            // seg == 255 → packet continues in next segment (or next page)
        }
        // pkt_len > 0 here means the last packet spans into the next page → drop it
    }

    size_t find_magic()
    {
        for (size_t i = 0; i + 4 <= _buf.size(); i++) {
            if (_buf[i]   == 'O' && _buf[i+1] == 'g' &&
                _buf[i+2] == 'g' && _buf[i+3] == 'S') {
                return i;
            }
        }
        return SIZE_MAX;
    }
};

}  // namespace roz
