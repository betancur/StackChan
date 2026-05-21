/*
 * AppRozAgent — OGG/Opus writer (ported from Dory/encoder.c, proven working).
 *
 * Uses the exact OGG CRC32 table algorithm (polynomial 0x04C11DB7, MSB-first)
 * as required by the OGG spec. esp_rom_crc32_be is NOT equivalent.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace roz {

// ── OGG CRC32 (OGG spec, 0x04C11DB7, MSB-first) ──────────────────────────────

static uint32_t s_ogg_crc_table[256];
static bool     s_ogg_crc_ready = false;

static void ogg_crc_init()
{
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)i << 24;
        for (int j = 0; j < 8; j++) {
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
        }
        s_ogg_crc_table[i] = r;
    }
    s_ogg_crc_ready = true;
}

static uint32_t ogg_crc(const uint8_t* data, size_t len)
{
    if (!s_ogg_crc_ready) ogg_crc_init();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc >> 24) ^ data[i]);
        crc = (crc << 8) ^ s_ogg_crc_table[idx];
    }
    return crc;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static inline void write_le16(uint8_t* p, uint16_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static inline void write_le32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline void write_le64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; i++) { p[i] = (v >> (8 * i)) & 0xFF; }
}

// ── OGG page writer ───────────────────────────────────────────────────────────

class OggWriter {
public:
    explicit OggWriter(uint32_t serial = 0x526F7A21u /* "Roz!" */)
        : _serial(serial) {}

    const uint8_t* data() const { return _buf.data(); }
    size_t         size() const { return _buf.size(); }

    // Write OpusHead + OpusTags header pages.
    // pre_skip: encoder lookahead samples (scaled to 48 kHz clock).
    void writeOpusHeaders(uint32_t sample_rate, uint8_t channels, uint16_t pre_skip)
    {
        // OpusHead (page 0, BOS)
        uint8_t head[19] = {};
        memcpy(head, "OpusHead", 8);
        head[8] = 1;                            // version
        head[9] = channels;
        write_le16(&head[10], pre_skip);
        write_le32(&head[12], sample_rate);
        // output_gain = 0, mapping_family = 0 already zero
        writePage(0x02 /*BOS*/, 0, head, sizeof(head));

        // OpusTags (page 1)
        const char* vendor = "ESP32";
        const uint32_t vlen = (uint32_t)strlen(vendor);
        const size_t   tsz  = 8 + 4 + vlen + 4;
        uint8_t tags[32];
        memcpy(tags, "OpusTags", 8);
        write_le32(&tags[8], vlen);
        memcpy(&tags[12], vendor, vlen);
        write_le32(&tags[12 + vlen], 0);  // user_comment_count = 0
        writePage(0x00, 0, tags, tsz);
    }

    // Write one Opus packet as an OGG page.
    // granule = cumulative samples at 48 kHz clock.
    // last    = set EOS flag on this page.
    void writeAudioPage(const uint8_t* pkt, size_t pkt_len,
                        uint64_t granule, bool last = false)
    {
        writePage(last ? 0x04u : 0x00u, granule, pkt, pkt_len);
    }

private:
    uint32_t             _serial;
    uint32_t             _seqno = 0;
    std::vector<uint8_t> _buf;

    void writePage(uint8_t htype, uint64_t granule,
                   const uint8_t* payload, size_t payload_len)
    {
        // Build segment table (255 bytes per segment, Lacing values)
        uint8_t  segtab[255];
        uint8_t  nseg  = 0;
        size_t   rem   = payload_len;
        do {
            uint8_t s = (rem >= 255) ? 255u : (uint8_t)rem;
            segtab[nseg++] = s;
            rem -= s;
        } while (rem > 0 || (nseg == 1 && payload_len == 0));

        const size_t page_size = 27u + nseg + payload_len;
        const size_t base      = _buf.size();
        _buf.resize(base + page_size, 0);
        uint8_t* p = _buf.data() + base;

        // Fixed-size header
        memcpy(p, "OggS", 4);
        p[4] = 0;           // version
        p[5] = htype;
        write_le64(p + 6,  granule);
        write_le32(p + 14, _serial);
        write_le32(p + 18, _seqno++);
        write_le32(p + 22, 0);         // checksum placeholder
        p[26] = nseg;
        memcpy(p + 27, segtab, nseg);
        if (payload && payload_len) {
            memcpy(p + 27 + nseg, payload, payload_len);
        }

        // Compute and store CRC over the entire page
        uint32_t crc = ogg_crc(p, page_size);
        write_le32(p + 22, crc);
    }
};

}  // namespace roz
