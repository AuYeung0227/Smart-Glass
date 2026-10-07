#include "ogg_opus.h"

#include <string.h>

#include "config.h"

// Ogg 页必须整页写入：页头已含长度与 CRC，部分写入会整页损坏，
// 因此任何短写都视为致命的 SD 错误。
static bool write_page(File &f, ogg_page *og)
{
    if (f.write(og->header, og->header_len) != (size_t)og->header_len) {
        return false;
    }
    if (og->body_len > 0 &&
        f.write(og->body, og->body_len) != (size_t)og->body_len) {
        return false;
    }
    return true;
}

bool ogg_opus_begin(OggOpusWriter *w, File &f, uint32_t serial)
{
    memset(w, 0, sizeof(*w));
    w->serial = (int)serial;
    w->packetno = 2;   // 0=OpusHead, 1=OpusTags，音频包从 2 开始
    w->header_done = false;

    if (ogg_stream_init(&w->os, w->serial) != 0) {
        return false;
    }

    // --- OpusHead（19 字节，声道映射族 0）---
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                 // 容器版本
    head[9] = 1;                 // 声道数（单声道）
    head[10] = 0;                // pre-skip 低字节
    head[11] = 0;                // pre-skip 高字节
    uint32_t rate = MIC_SAMPLE_RATE;
    memcpy(head + 12, &rate, 4); // 原始输入采样率（仅供参考）
    head[16] = 0;                // 输出增益低字节
    head[17] = 0;                // 输出增益高字节
    head[18] = 0;                // 声道映射族 0

    ogg_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.packet = head;
    pkt.bytes = (long)sizeof(head);
    pkt.b_o_s = 1;
    pkt.packetno = 0;
    if (ogg_stream_packetin(&w->os, &pkt) != 0) {
        ogg_stream_clear(&w->os);
        return false;
    }

    // --- OpusTags：vendor 字符串 + 零条用户注释 ---
    static const char kVendor[] = "omi-glass";
    uint8_t tags[8 + 4 + sizeof(kVendor) - 1 + 4];
    size_t n = 0;
    memcpy(tags + n, "OpusTags", 8);
    n += 8;
    uint32_t vlen = (uint32_t)(sizeof(kVendor) - 1);
    memcpy(tags + n, &vlen, 4);
    n += 4;
    memcpy(tags + n, kVendor, vlen);
    n += vlen;
    uint32_t ucount = 0;         // 无用户注释
    memcpy(tags + n, &ucount, 4);
    n += 4;

    memset(&pkt, 0, sizeof(pkt));
    pkt.packet = tags;
    pkt.bytes = (long)n;
    pkt.packetno = 1;
    if (ogg_stream_packetin(&w->os, &pkt) != 0) {
        ogg_stream_clear(&w->os);
        return false;
    }

    // 两个头包必须各自成页、先于任何音频落盘。
    ogg_page og;
    while (ogg_stream_flush(&w->os, &og) > 0) {
        if (!write_page(f, &og)) {
            ogg_stream_clear(&w->os);
            return false;
        }
    }

    w->header_done = true;
    return true;
}

bool ogg_opus_write(OggOpusWriter *w, File &f, const uint8_t *pkt, size_t len,
                    int64_t granule48)
{
    if (!w->header_done || len == 0) {
        return false;
    }

    ogg_packet op;
    memset(&op, 0, sizeof(op));
    op.packet = (unsigned char *)pkt;
    op.bytes = (long)len;
    op.granulepos = granule48;             // 帧结束位置的绝对采样号 × 3
    op.packetno = w->packetno++;

    if (ogg_stream_packetin(&w->os, &op) != 0) {
        return false;
    }

    ogg_page og;
    while (ogg_stream_pageout(&w->os, &og) > 0) {
        if (!write_page(f, &og)) {
            return false;
        }
    }
    return true;
}

bool ogg_opus_end(OggOpusWriter *w, File &f, bool eos)
{
    bool ok = true;

    // 置 e_o_s 让 libogg 给随后发出的页打 end-of-stream 标记，
    // 否则播放器可能提示文件截断。
    if (eos) {
        w->os.e_o_s = 1;
    }

    ogg_page og;
    while (ogg_stream_flush(&w->os, &og) > 0) {
        if (!write_page(f, &og)) {
            ok = false;
            break;
        }
    }

    ogg_stream_clear(&w->os);
    w->header_done = false;
    return ok;
}

// ---------------------------------------------------------------------------
// 读侧
// ---------------------------------------------------------------------------
bool ogg_opus_open_read(OggOpusReader *r)
{
    memset(r, 0, sizeof(*r));
    if (ogg_sync_init(&r->sync) != 0) {
        return false;
    }
    return true;
}

void ogg_opus_close_read(OggOpusReader *r)
{
    if (r->stream_init) {
        ogg_stream_clear(&r->os);
    }
    ogg_sync_clear(&r->sync);
    r->stream_init = false;
}

// 从文件读一块喂进 sync；返回读到的字节数（0 = EOF）。
static size_t feed_file(File &f, ogg_sync_state *sync)
{
    char *buf = ogg_sync_buffer(sync, OGG_SYNC_BUF);
    if (buf == nullptr) {
        // sync 里还有未取走的页（理论上 pageout 已排空，不会到这）。
        return 0;
    }
    size_t n = f.read((uint8_t *)buf, OGG_READ_CHUNK);
    ogg_sync_wrote(sync, (long)n);
    return n;
}

bool ogg_opus_read_packet(OggOpusReader *r, File &f, uint8_t *out, size_t cap,
                          size_t *len, int64_t *granule48)
{
    bool eof = false;

    for (;;) {
        // 1) 先取流里已解出的包
        if (r->stream_init) {
            ogg_packet p;
            while (ogg_stream_packetout(&r->os, &p) == 1) {
                // 前两个包是 OpusHead/OpusTags，丢弃。
                if (r->header_count < 2) {
                    r->header_count++;
                    continue;
                }
                if (p.bytes < 0 || (size_t)p.bytes > cap) {
                    return false;   // 异常包（写入时不该出现）
                }
                memcpy(out, p.packet, (size_t)p.bytes);
                *len = (size_t)p.bytes;
                *granule48 = p.granulepos;
                return true;
            }
        }

        // 2) 从 sync 切页并喂进流
        ogg_page og;
        while (ogg_sync_pageout(&r->sync, &og) == 1) {
            if (!r->stream_init) {
                if (ogg_stream_init(&r->os, ogg_page_serialno(&og)) != 0) {
                    return false;
                }
                r->stream_init = true;
            }
            ogg_stream_pagein(&r->os, &og);
        }

        // 3) 读文件；EOF 后再转一圈，把最后一批页/包排空。
        if (eof) {
            return false;
        }
        if (feed_file(f, &r->sync) == 0) {
            eof = true;
        }
    }
}
