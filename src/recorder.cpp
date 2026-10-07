#include "recorder.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <opus.h>

#include "config.h"
#include "ogg_opus.h"

// ---------------------------------------------------------------------------
// 环形缓存：滚动保存最近 30s 的处理后 PCM。
// ---------------------------------------------------------------------------
#define FRAME ((size_t)OPUS_FRAME_SAMPLES)                 // 一帧 = 320 样本 = 20ms
#define RING_SAMPLES ((size_t)RECORD_PREBUFFER_SECONDS * MIC_SAMPLE_RATE)
// 帧标签环的大小：比环形缓存多两帧，保证任意时刻「缓存内所有帧」都有标签。
#define NFRAMES ((RING_SAMPLES / FRAME) + 2)
#define JOURNAL_PATH RECORD_SD_DIR "/journal.txt"   // 主动打断被动的关联日志

static int16_t *s_ring = nullptr;      // PSRAM 环形 PCM 缓存
static size_t s_ring_write = 0;        // 生产者下一个写入位置
static size_t s_since_ring_start = 0;  // 已写入且未回绕的样本数（仅用于报告预缓冲秒数）

// 单字长的生产者/消费者交接：uint32 读写是原子字访问，跨核也安全。
static volatile uint32_t s_produced = 0;  // 累计写入的样本总数（绝对序号）

// 语音标签环：与样本对齐（帧号 = 绝对样本号 / 320），1 = 该帧判定为语音。
// 只由音频路径写、后台任务读；回绕由「全局帧号取模」自然处理。
static uint8_t *s_frame_tag = nullptr;
static int64_t s_frame_acc = 0;        // 当前帧内 |x| 累加（音频路径专用）

// ---------------------------------------------------------------------------
// 编码与写出缓冲（仅后台任务使用）
// ---------------------------------------------------------------------------
static OpusEncoder *s_enc = nullptr;
static int16_t s_enc_in[OPUS_FRAME_SAMPLES];
static uint8_t s_enc_out[OPUS_OUTPUT_MAX_BYTES];

// 会话控制用互斥锁：只在任务之间串行化，音频路径绝不加锁。
static SemaphoreHandle_t s_mtx = nullptr;

static bool s_sd_ok = false;
static TaskHandle_t s_task = nullptr;

// ---------------------------------------------------------------------------
// 墙钟（手机对时后有效）
// ---------------------------------------------------------------------------
static volatile uint32_t s_unix_at_sync = 0;    // 对时时刻的 unix 秒
static volatile uint32_t s_millis_at_sync = 0;  // 对时时刻的本地 millis
static volatile bool s_time_synced = false;

// ---------------------------------------------------------------------------
// 写会话
// ---------------------------------------------------------------------------
typedef struct {
    bool active;          // 正在写出
    bool speech_filter;   // true = 预缓冲段只写语音帧（被动录音）
    uint32_t start;       // 直播区起点（绝对样本号）；< start 的部分是预缓冲
    uint32_t flushed;     // 已消费到的绝对样本号（始终帧对齐）
    File file;
    OggOpusWriter ogg;
    uint32_t bytes;       // 本文件已写字节数（含 Ogg 包头），仅用于日志
    char name[64];       // 文件名（p_/a_<unix>_<pos>.opus），供日志/删除/回传
} RecSession;

static RecSession s_passive;  // 被动录音会话
static RecSession s_active;   // 主动录音会话

// 把绝对样本号向上对齐到帧边界，保证每次消费都是完整的一帧。
static inline uint32_t align_up(uint32_t x)
{
    return ((x + (uint32_t)FRAME - 1) / (uint32_t)FRAME) * (uint32_t)FRAME;
}

// ---------------------------------------------------------------------------
// 生产者（音频路径）
// ---------------------------------------------------------------------------
void recorder_feed(const int16_t *samples, size_t n)
{
    if (s_ring == nullptr || s_frame_tag == nullptr || samples == nullptr || n == 0) {
        return;
    }

    uint32_t pos = s_produced;   // samples[0] 的绝对序号
    size_t w = s_ring_write;

    for (size_t i = 0; i < n; i++) {
        int16_t v = samples[i];
        s_ring[w] = v;
        w++;
        if (w >= RING_SAMPLES) {
            w = 0;
        }

        int32_t a = (v < 0) ? -(int32_t)v : (int32_t)v;
        s_frame_acc += a;

        // 每当绝对位置跨过帧边界，就给刚结束的那一帧打语音标签。
        if (((pos + 1) % (uint32_t)FRAME) == 0) {
            uint32_t fid = (pos + 1) / (uint32_t)FRAME - 1;   // 0 起始的全局帧号
            float energy =
                ((float)s_frame_acc / (float)FRAME) / 32768.0f;
            s_frame_tag[fid % (uint32_t)NFRAMES] =
                (energy >= RECORD_VAD_ENERGY_THRESHOLD) ? 1 : 0;
            s_frame_acc = 0;
        }
        pos++;
    }

    s_ring_write = w;
    // 数据先就位，最后才发布序号，消费者读到的永远是完整数据。
    s_produced = pos;

    if (s_since_ring_start < RING_SAMPLES) {
        s_since_ring_start += n;
        if (s_since_ring_start > RING_SAMPLES) {
            s_since_ring_start = RING_SAMPLES;
        }
    }
}

// ---------------------------------------------------------------------------
// 消费者（后台任务）
// ---------------------------------------------------------------------------
// 从环形缓存的绝对位置 start 读 count 个样本（自动处理回绕）。
static void ring_read(uint32_t start, int16_t *dst, size_t count)
{
    size_t idx = (size_t)(start % (uint32_t)RING_SAMPLES);
    size_t first = RING_SAMPLES - idx;
    if (first > count) {
        first = count;
    }
    memcpy(dst, &s_ring[idx], first * sizeof(int16_t));
    if (count > first) {
        memcpy(dst + first, s_ring, (count - first) * sizeof(int16_t));
    }
}

// 预缓冲段是否应保留以 frame_start 开始的那一帧。
// 具备 PREBUFFER_SPEECH_HANGOVER_FRAMES 帧的尾部延长，避免把语音结尾截掉。
static bool prebuffer_speech(uint32_t frame_start)
{
    uint32_t fid = frame_start / (uint32_t)FRAME;
    for (uint32_t k = 0; k <= (uint32_t)PREBUFFER_SPEECH_HANGOVER_FRAMES; k++) {
        if (fid < k) {
            break;
        }
        if (s_frame_tag[(fid - k) % (uint32_t)NFRAMES]) {
            return true;
        }
    }
    return false;
}

// 消费一个会话的积压数据。返回 true 表示达到了单次上限、仍有活要干。
static bool service_session(RecSession *s)
{
    if (!s->active) {
        return false;
    }

    uint32_t produced = s_produced;
    uint32_t oldest = (produced > (uint32_t)RING_SAMPLES)
                          ? produced - (uint32_t)RING_SAMPLES
                          : 0;

    // 落后超过一圈时，最旧的样本已被覆盖，只能跳到仍有效的位置继续。
    if (s->flushed < oldest) {
        s->flushed = align_up(oldest);
        Serial.printf("【录音】缓存溢出，跳过 %lu 样本\n",
                      (unsigned long)(s->flushed - oldest));
    }
    if (s->start < oldest) {
        s->start = align_up(oldest);
    }

    // 只消费完整帧，末尾不足一帧的留到下次。
    uint32_t limit = (produced / (uint32_t)FRAME) * (uint32_t)FRAME;

    int done = 0;
    while (s->flushed + (uint32_t)FRAME <= limit) {
        uint32_t frame_start = s->flushed;
        bool in_live = (frame_start >= s->start);
        bool keep = in_live || !s->speech_filter || prebuffer_speech(frame_start);

        if (keep) {
            ring_read(frame_start, s_enc_in, FRAME);
            int n = opus_encode(s_enc, s_enc_in, (int)FRAME, s_enc_out,
                                OPUS_OUTPUT_MAX_BYTES);
            if (n > 0) {
                // granule = 帧结束位置的绝对样本号 × 3（48kHz 刻度）。
                // 它同时是「播放位置」与「捕获时刻」的载体（见 recorder.h）。
                int64_t g48 = (int64_t)((uint64_t)frame_start + (uint32_t)FRAME) * 3;
                if (!ogg_opus_write(&s->ogg, s->file, s_enc_out, (size_t)n, g48)) {
                    // 写卡失败（卡满/拔出）：直接收尾，不空转。
                    Serial.println("【录音】写卡失败，停止本次录音");
                    return false;
                }
                s->bytes += (uint32_t)n;
            }
        }

        s->flushed += (uint32_t)FRAME;
        if (++done >= RECORD_FRAMES_PER_TICK) {
            return true;  // 达到单次上限，让出 CPU
        }
    }
    return false;
}

// 排空一个会话的积压并收尾关闭。
static void session_close(RecSession *s)
{
    if (!s->active) {
        return;
    }
    // 反复消费直到追平（service_session 返回 false 即已追平）。
    for (int guard = 0; guard < 20000; guard++) {
        if (!service_session(s)) {
            break;
        }
    }

    ogg_opus_end(&s->ogg, s->file, true);
    s->file.flush();
    s->file.close();
    s->active = false;

    Serial.printf("【录音】已停止 %s，共 %lu 字节\n", s->name,
                  (unsigned long)s->bytes);
    s->bytes = 0;
}

// 打开一个新会话。flushed/start 由调用者决定（是否含预缓冲）。
static bool session_open(RecSession *s, bool speech_filter, uint32_t flushed,
                         uint32_t start, const char *kind)
{
    if (s->active) {
        return false;
    }
    if (!s_sd_ok) {
        Serial.println("【录音】无SD卡，无法录音");
        return false;
    }

    // 文件名携带 (open_unix, pos)：删除/回传时的排序与时间反推都靠它。
    uint32_t unix = recorder_now_unix();
    uint32_t pos = start;
    snprintf(s->name, sizeof(s->name), "%s_%lu_%lu.opus", kind,
             (unsigned long)unix, (unsigned long)pos);

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", RECORD_SD_DIR, s->name);

    s->file = SD.open(path, FILE_WRITE);
    if (!s->file) {
        Serial.printf("【录音】无法创建 %s\n", path);
        return false;
    }
    if (!ogg_opus_begin(&s->ogg, s->file, millis())) {
        s->file.close();
        Serial.println("【录音】写入 Ogg 文件头失败");
        return false;
    }

    s->speech_filter = speech_filter;
    s->flushed = flushed;
    s->start = start;
    s->bytes = 0;
    s->active = true;

    Serial.printf("【录音】开始写入 %s\n", path);
    return true;
}

// 在 journal 里追加一条打断关联：这条被动是被这条主动打断的。
static void journal_link(const char *a_name, const char *p_name)
{
    if (!s_sd_ok || a_name == nullptr || p_name == nullptr) {
        return;
    }
    File j = SD.open(JOURNAL_PATH, FILE_APPEND);
    if (j) {
        j.printf("link %s %s\n", a_name, p_name);
        j.close();
    }
}

// 每隔 SD_CAPACITY_CHECK_INTERVAL_MS 报告一次 SD 剩余容量。
static void check_sd_capacity()
{
    static uint32_t last = 0;
    if (!s_sd_ok) {
        return;
    }
    uint32_t now = millis();
    if (last != 0 && (now - last) < SD_CAPACITY_CHECK_INTERVAL_MS) {
        return;
    }
    last = now;

    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    uint64_t mb = 1024ULL * 1024ULL;
    uint64_t free_mb = (total > used) ? (total - used) / mb : 0;
    Serial.printf("【录音】SD 剩余 %llu MB / 总 %llu MB\n",
                  (unsigned long long)free_mb, (unsigned long long)(total / mb));
}

static void recorder_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool busy = false;

        if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
            if (s_passive.active) {
                busy |= service_session(&s_passive);
            }
            if (s_active.active) {
                busy |= service_session(&s_active);
            }
            xSemaphoreGive(s_mtx);
        }

        check_sd_capacity();

        // 有积压就只让出 1 tick 继续追赶，空闲时睡久一点。
        vTaskDelay(busy ? 1 : pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------------------
// 墙钟与北京时间
// ---------------------------------------------------------------------------
// 民用日历换算（Howard Hinnant 的 civil_from_days）：
// days = 自 1970-01-01 起的天数 → 公历年月日。
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (int)(*m <= 2);
}

static uint8_t bcd2(uint32_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void recorder_beijing_bcd(uint32_t unix, uint8_t bcd[6])
{
    if (unix == 0) {
        memset(bcd, 0xFF, 6);   // 非法 BCD：手机识别为「未知时间」
        return;
    }

    int64_t t = (int64_t)unix + 8 * 3600;   // 北京时间 = UTC + 8
    int64_t days = t / 86400;
    int64_t secs = t % 86400;

    int y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);

    bcd[0] = bcd2((uint32_t)(y % 100));           // 年：YY
    bcd[1] = bcd2(m);                             // 月
    bcd[2] = bcd2(d);                             // 日
    bcd[3] = bcd2((uint32_t)(secs / 3600));       // 时
    bcd[4] = bcd2((uint32_t)((secs / 60) % 60));  // 分
    bcd[5] = bcd2((uint32_t)(secs % 60));         // 秒
}

void recorder_sync_time(uint32_t unix)
{
    s_millis_at_sync = millis();
    s_unix_at_sync = unix;
    s_time_synced = true;

    uint8_t bcd[6];
    recorder_beijing_bcd(unix, bcd);
    Serial.printf("【时间】已对时：20%02X-%02X-%02X %02X:%02X:%02X（北京时间）\n",
                  bcd[0], bcd[1], bcd[2], bcd[3], bcd[4], bcd[5]);
}

uint32_t recorder_now_unix()
{
    if (!s_time_synced) {
        return 0;
    }
    return s_unix_at_sync + (millis() - s_millis_at_sync) / 1000;
}

// ---------------------------------------------------------------------------
// 录音管理（0x12 / 0x13 / 0x20 支持）
// ---------------------------------------------------------------------------
// 解析 "p_123_456.opus" / "a_123_456.opus"。
static bool parse_name(const char *name, char *kind, uint32_t *unix, uint32_t *pos)
{
    unsigned long u = 0, p = 0;
    if (name == nullptr || name[1] != '_' ||
        (name[0] != 'p' && name[0] != 'a')) {
        return false;
    }
    if (sscanf(name + 2, "%lu_%lu.opus", &u, &p) != 2) {
        return false;
    }
    *kind = name[0];
    *unix = (uint32_t)u;
    *pos = (uint32_t)p;
    return true;
}

void recorder_delete_last_active()
{
    // 先停掉正在写的会话（文件先落盘再删）。
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        if (s_active.active) {
            session_close(&s_active);
        }
        if (s_passive.active) {
            session_close(&s_passive);
        }
        xSemaphoreGive(s_mtx);
    }

    // 扫描 a_*，找「最近」的主动录音：open_unix 最大者；全为 0 时退化为 pos 最大。
    char best[64] = "";
    uint32_t best_unix = 0, best_pos = 0;
    File dir = SD.open(RECORD_SD_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            char name[64];
            strncpy(name, f.name(), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char kind;
            uint32_t u, p;
            if (parse_name(name, &kind, &u, &p) && kind == 'a') {
                bool newer = (best[0] == '\0')
                             || (u > best_unix)
                             || (u == best_unix && p > best_pos);
                if (newer) {
                    strcpy(best, name);
                    best_unix = u;
                    best_pos = p;
                }
            }
            f = dir.openNextFile();
        }
        dir.close();
    }

    if (best[0] == '\0') {
        Serial.println("【录音】没有可删除的主动录音");
        return;
    }

    // 删除主动文件本体。
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", RECORD_SD_DIR, best);
    bool ok_a = SD.remove(path);

    // 从 journal 找被它打断的被动录音，并重写 journal 去掉这些行。
    char linked[REPLAY_MAX_FILES][64];
    int n_linked = 0;
    File j = SD.open(JOURNAL_PATH, FILE_READ);
    if (j) {
        // 收集 link 行到 RAM（行很短，体积可控）。
        static char lines[64][96];   // 最多 64 行，每行 <96 字节
        int n_lines = 0;
        while (j.available() && n_lines < 64) {
            String s = j.readStringUntil('\n');
            if (s.length() > 0 && s.length() < 96) {
                strcpy(lines[n_lines], s.c_str());
                n_lines++;
            }
        }
        j.close();

        File jt = SD.open(RECORD_SD_DIR "/journal.tmp", FILE_WRITE);
        for (int i = 0; i < n_lines; i++) {
            char a[64], p[64];
            if (sscanf(lines[i], "link %63s %63s", a, p) == 2) {
                if (strcmp(a, best) == 0 && n_linked < REPLAY_MAX_FILES) {
                    // 该被动是被这条主动打断的：记录待删，不再写回 journal。
                    strcpy(linked[n_linked++], p);
                    continue;
                }
            }
            if (jt) {
                jt.println(lines[i]);
            }
        }
        if (jt) {
            jt.close();
            SD.remove(JOURNAL_PATH);
            SD.rename(RECORD_SD_DIR "/journal.tmp", JOURNAL_PATH);
        }
    }

    for (int i = 0; i < n_linked; i++) {
        snprintf(path, sizeof(path), "%s/%s", RECORD_SD_DIR, linked[i]);
        SD.remove(path);
    }

    if (n_linked > 0) {
        Serial.printf("【录音】已删除最近主动录音 %s（含被其打断的被动录音 %s）\n",
                      best, linked[0]);
    } else {
        Serial.printf("【录音】已删除最近主动录音 %s（%s）\n", best,
                      ok_a ? "成功" : "文件不存在");
    }
}

void recorder_delete_all()
{
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        if (s_active.active) {
            session_close(&s_active);
        }
        if (s_passive.active) {
            session_close(&s_passive);
        }
        xSemaphoreGive(s_mtx);
    }

    int count = 0;
    File dir = SD.open(RECORD_SD_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            char name[64];
            strncpy(name, f.name(), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            if (strstr(name, ".opus") != nullptr) {
                char path[72];
                snprintf(path, sizeof(path), "%s/%s", RECORD_SD_DIR, name);
                if (SD.remove(path)) {
                    count++;
                }
            }
            f = dir.openNextFile();
        }
        dir.close();
    }
    SD.remove(JOURNAL_PATH);
    Serial.printf("【录音】已删除全部录音，共 %d 个文件\n", count);
}

int recorder_collect_files(char names[][64], int max)
{
    int n = 0;
    File dir = SD.open(RECORD_SD_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && n < max) {
            char name[64];
            strncpy(name, f.name(), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char kind;
            uint32_t u, p;
            if (parse_name(name, &kind, &u, &p)) {
                strcpy(names[n], name);
                n++;
            }
            f = dir.openNextFile();
        }
        dir.close();
    }

    // 按 (open_unix, pos) 升序；unix=0（未对时）视为最旧，简单选择排序即可。
    for (int i = 0; i < n - 1; i++) {
        int b = i;
        char bk;
        uint32_t bu, bp;
        parse_name(names[b], &bk, &bu, &bp);
        for (int k = i + 1; k < n; k++) {
            char kk;
            uint32_t ku, kp;
            parse_name(names[k], &kk, &ku, &kp);
            bool less = (ku < bu) || (ku == bu && kp < bp);
            if (less) {
                b = k;
                bu = ku;
                bp = kp;
            }
        }
        if (b != i) {
            char tmp[64];
            strcpy(tmp, names[i]);
            strcpy(names[i], names[b]);
            strcpy(names[b], tmp);
        }
    }
    return n;
}

bool recorder_frame_time(const char *name, int64_t granule48, uint8_t bcd[6])
{
    char kind;
    uint32_t unix, pos;
    if (!parse_name(name, &kind, &unix, &pos) || unix == 0) {
        memset(bcd, 0xFF, 6);   // 录制时未对时 → 未知时间
        return false;
    }

    // 帧捕获时刻 = open_unix + (帧起始绝对样本号 - 打开时s_produced)/16000
    int64_t frame_end = granule48 / 3;
    int64_t frame_start = frame_end - (int64_t)FRAME;
    int64_t offset = frame_start - (int64_t)pos;
    if (offset < 0) {
        offset = 0;
    }
    uint32_t frame_unix = (uint32_t)((int64_t)unix + offset / 16000);
    recorder_beijing_bcd(frame_unix, bcd);
    return true;
}

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// SD 挂载：依次尝试不同 SPI 频率。
// 接线/走线质量差的卡在 4MHz 下会 CMD0 失败，降到 1MHz/400kHz 常能挂上。
// 返回成功的频率（0 = 全部失败）。
// ---------------------------------------------------------------------------
static bool s_sd_begun = false;   // 是否调用过 SD.begin（避免对未初始化的 FS 调 end）

static uint32_t mount_sd_best_speed(bool verbose)
{
    static const uint32_t speeds[] = {4000000, 1000000, 400000};
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        if (s_sd_begun) {
            SD.end();
            delay(10);
        }
        SPI.begin(7 /*SCK*/, 8 /*MISO*/, 9 /*MOSI*/, RECORD_SD_CS_PIN);
        s_sd_begun = true;
        if (SD.begin(RECORD_SD_CS_PIN, SPI, speeds[i])) {
            if (verbose && i > 0) {
                Serial.printf("【录音】在 %lu kHz 下挂载成功（4MHz 失败，接线/卡质量偏弱）\n",
                              (unsigned long)(speeds[i] / 1000));
            }
            return speeds[i];
        }
    }
    return 0;
}

bool recorder_sd_ok()
{
    return s_sd_ok;
}

// 读第 0 扇区（FAT/BPB），识别文件系统类型。
static const char *detect_fs_type()
{
    static uint8_t sec[512];
    if (!SD.readRAW(sec, 0)) {
        return "未知（读扇区失败）";
    }
    if (memcmp(sec + 3, "EXFAT   ", 8) == 0) {
        return "exFAT（不支持）";
    }
    if (memcmp(sec + 82, "FAT32   ", 8) == 0) {
        return "FAT32";
    }
    if (memcmp(sec + 54, "FAT16   ", 8) == 0) {
        return "FAT16";
    }
    if (memcmp(sec + 54, "FAT12   ", 8) == 0) {
        return "FAT12";
    }
    return "未知/非FAT（可能不支持）";
}

void recorder_sd_check()
{
    // 没挂上就再试一次（自动扫多个 SPI 频率）。
    if (!s_sd_ok) {
        s_sd_ok = (mount_sd_best_speed(true) != 0);
    }

    Serial.println("【录音】--- SD 卡诊断 ---");
    Serial.printf("【录音】SPI 引脚：SCK=GPIO7 MISO=GPIO8 MOSI=GPIO9 CS=GPIO%d\n",
                  RECORD_SD_CS_PIN);

    sdcard_type_t t = SD.cardType();
    switch (t) {
    case CARD_NONE:
        Serial.println("【录音】卡类型：未检测到卡（无卡 / 接触不良 / 未供电）");
        break;
    case CARD_MMC:
        Serial.println("【录音】卡类型：MMC");
        break;
    case CARD_SD:
        Serial.println("【录音】卡类型：SDSC（≤2GB）");
        break;
    case CARD_SDHC:
        Serial.println("【录音】卡类型：SDHC/SDXC（>2GB）");
        break;
    default:
        Serial.println("【录音】卡类型：未知");
        break;
    }

    if (t != CARD_NONE) {
        uint64_t total = SD.totalBytes();
        uint64_t used = SD.usedBytes();
        uint64_t mb = 1024ULL * 1024ULL;
        Serial.printf("【录音】容量：总 %.2f GB，已用 %llu MB\n",
                      (double)total / (1024.0 * 1024.0 * 1024.0),
                      (unsigned long long)(used / mb));

        const char *fs = detect_fs_type();
        Serial.printf("【录音】文件系统：%s\n", fs);
        if (strcmp(fs, "FAT32") != 0 && strcmp(fs, "FAT16") != 0) {
            Serial.println("【录音】提示：SD 库只支持 FAT16/FAT32，其它格式请重新格式化");
        }
    }

    Serial.println("【录音】--- 诊断结束 ---");
}

// ---------------------------------------------------------------------------
// 原始 SPI 探测：绕过 SD 库，直接跟卡对话
// ---------------------------------------------------------------------------
// 发一条 6 字节命令，返回它的响应首字节（R1）。
// 协议：CS 高时补 8 个时钟；CS 拉低后发命令；随后轮询，直到 MISO 出现
// 最高位为 0 的字节（R1），最多轮询 8 次；最后 CS 拉高再补 8 个时钟。
static uint8_t sd_cmd_r1(const uint8_t *cmd)
{
    digitalWrite(RECORD_SD_CS_PIN, HIGH);
    SPI.transfer(0xFF);
    digitalWrite(RECORD_SD_CS_PIN, LOW);
    for (int i = 0; i < 6; i++) {
        SPI.transfer(cmd[i]);
    }
    uint8_t r = 0xFF;
    for (int i = 0; i < 8; i++) {
        r = SPI.transfer(0xFF);
        if (!(r & 0x80)) {
            break;
        }
    }
    digitalWrite(RECORD_SD_CS_PIN, HIGH);
    SPI.transfer(0xFF);
    return r;
}

void recorder_sd_probe()
{
    Serial.println("【录音】--- SD 原始 SPI 探测（绕过 SD 库）---");

    // 让出 SD 库对 SPI 的占用，改由本函数手动驱动。
    if (s_sd_begun) {
        SD.end();
        delay(10);
        s_sd_begun = false;
        s_sd_ok = false;
    }

    pinMode(RECORD_SD_CS_PIN, OUTPUT);
    digitalWrite(RECORD_SD_CS_PIN, HIGH);
    SPI.begin(7 /*SCK*/, 8 /*MISO*/, 9 /*MOSI*/, RECORD_SD_CS_PIN);
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));  // 低速，最稳

    // CS 高、发 80 个时钟：让卡从原生模式切进 SPI 模式。
    digitalWrite(RECORD_SD_CS_PIN, HIGH);
    for (int i = 0; i < 10; i++) {
        SPI.transfer(0xFF);
    }

    // 关键判断：空闲（CS 高）时 MISO 读到什么？
    //   正常卡未选中时 MISO 高阻，靠上拉读到 0xFF；
    //   若读到 0x00，说明这条线被拉低（短地 / 引脚接错 / 没插卡/槽接触不良）。
    uint8_t idle[3];
    for (int i = 0; i < 3; i++) {
        idle[i] = SPI.transfer(0xFF);
    }
    Serial.printf("【录音】空闲 MISO = %02X %02X %02X（正常应 FF FF FF）\n",
                  idle[0], idle[1], idle[2]);

    // CMD0 = GO_IDLE_STATE，正确响应应为 0x01（idle）。
    const uint8_t cmd0[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    uint8_t r0 = sd_cmd_r1(cmd0);

    // CMD8 = SEND_IF_COND，校验电压与版本；应为 0x01 + 4 字节回读 0x000001AA。
    const uint8_t cmd8[6] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
    uint8_t r8 = sd_cmd_r1(cmd8);

    Serial.printf("【录音】CMD0 响应 = 0x%02X\n", r0);
    Serial.printf("【录音】CMD8 响应 = 0x%02X\n", r8);

    SPI.endTransaction();
    digitalWrite(RECORD_SD_CS_PIN, HIGH);   // CS 回到空闲高

    // 结论：0x00 与 0xFF 是两种完全不同的故障
    if (idle[0] == 0x00 && idle[1] == 0x00 && idle[2] == 0x00) {
        Serial.println("【录音】结论：MISO 空闲时被拉低(0x00) → 数据线没接对/被拉低");
        Serial.println("【录音】  常见于：卡没插进卡槽 / 卡槽接触不良 / 扩展板虚接");
    } else if (r0 == 0x01) {
        Serial.println("【录音】结论：卡应答正常(0x01)，问题在初始化/文件系统 → 再跑 sdcheck");
    } else if (r0 == 0xFF) {
        Serial.println("【录音】结论：卡无响应(0xFF) → 没插卡 / 接触不良 / 供电不足");
    } else {
        Serial.printf("【录音】结论：响应异常(0x%02X) → 换一张卡再试\n", r0);
    }
    Serial.println("【录音】--- 探测结束 ---");
}

// SD 四根线的电平探测：用内部上拉/下拉区分「被拉死」还是「悬空」。
void recorder_sd_pins()
{
    Serial.println("【录音】--- SD 引脚电平探测 ---");

    // 先解除 SPI 对引脚的占用，否则读不到真实电平。
    if (s_sd_begun) {
        SD.end();
        delay(10);
        s_sd_begun = false;
        s_sd_ok = false;
    }
    SPI.end();

    const int pins[] = {7 /*SCK*/, 8 /*MISO*/, 9 /*MOSI*/, RECORD_SD_CS_PIN /*CS*/};
    const char *names[] = {"SCK ", "MISO", "MOSI", "CS  "};

    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        int p = pins[i];

        pinMode(p, INPUT_PULLUP);
        delayMicroseconds(300);
        int up = digitalRead(p);

        pinMode(p, INPUT_PULLDOWN);
        delayMicroseconds(300);
        int dn = digitalRead(p);

        const char *verdict;
        if (up == 0 && dn == 0) {
            verdict = "被外部拉低（短路GND / 引脚接错 / 卡槽异常）";
        } else if (up == 1 && dn == 1) {
            verdict = "被外部拉高";
        } else if (up == 1 && dn == 0) {
            verdict = "悬空/高阻（正常，卡输入脚就应如此）";
        } else {
            verdict = "异常";
        }
        Serial.printf("【录音】GPIO%-2d %s  上拉读=%d 下拉读=%d  → %s\n",
                      p, names[i], up, dn, verdict);
        pinMode(p, INPUT);   // 松开，避免互相影响
    }

    Serial.println("【录音】--- 探测结束 ---");
}

// 原生 SD/MMC 1-bit 探测：某些 XIAO ESP32S3 Sense 的 microSD 走 SD/MMC 而非 SPI。
void recorder_sd_mmc()
{
    Serial.println("【录音】--- SD/MMC(1-bit) 探测 ---");

    // 先释放 SPI 侧的占用。
    if (s_sd_begun) {
        SD.end();
        delay(10);
        s_sd_begun = false;
        s_sd_ok = false;
    }
    SPI.end();

    // 候选 (CLK, CMD, D0)。依据：GPIO21 有上拉（SD 规范要求 CMD 上拉）。
    struct { int clk, cmd, d0; } cand[] = {
        {7, 21, 8},
        {7, 9, 8},
        {7, 8, 9},
    };

    for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        SD_MMC.end();
        delay(20);

        if (!SD_MMC.setPins(cand[i].clk, cand[i].cmd, cand[i].d0)) {
            Serial.printf("【录音】候选 CLK=%d CMD=%d D0=%d：setPins 失败\n",
                          cand[i].clk, cand[i].cmd, cand[i].d0);
            continue;
        }

        bool ok = SD_MMC.begin("/sdcard", true /*1-bit*/);
        if (ok) {
            Serial.printf("【录音】成功！SD/MMC 1-bit 引脚 CLK=%d CMD=%d D0=%d\n",
                          cand[i].clk, cand[i].cmd, cand[i].d0);
            uint64_t total = SD_MMC.totalBytes();
            Serial.printf("【录音】容量：总 %.2f GB\n",
                          (double)total / (1024.0 * 1024.0 * 1024.0));
            SD_MMC.end();
            Serial.println("【录音】→ 请把此结果告诉我：需要把存储层改用 SD_MMC");
            Serial.println("【录音】--- 探测结束 ---");
            return;
        }
        Serial.printf("【录音】候选 CLK=%d CMD=%d D0=%d：挂载失败\n",
                      cand[i].clk, cand[i].cmd, cand[i].d0);
        SD_MMC.end();
        delay(20);
    }

    Serial.println("【录音】结论：三组候选都没挂上。若卡确已插好，则是走 SPI 或卡/槽问题");
    Serial.println("【录音】--- 探测结束 ---");
}

bool recorder_init()
{
    if (s_ring == nullptr) {
        s_ring = (int16_t *)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM);
        if (s_ring == nullptr) {
            s_ring = (int16_t *)malloc(RING_SAMPLES * sizeof(int16_t));
        }
        if (s_ring == nullptr) {
            Serial.println("【录音】环形缓存分配失败");
            return false;
        }
        memset(s_ring, 0, RING_SAMPLES * sizeof(int16_t));

        s_frame_tag = (uint8_t *)malloc(NFRAMES);
        if (s_frame_tag == nullptr) {
            Serial.println("【录音】语音标签分配失败");
            return false;
        }
        memset(s_frame_tag, 0, NFRAMES);

        Serial.printf("【录音】预缓存 %u 样本（%u 秒）\n", (unsigned)RING_SAMPLES,
                      (unsigned)RECORD_PREBUFFER_SECONDS);
    }

    if (s_mtx == nullptr) {
        s_mtx = xSemaphoreCreateMutex();
    }

    // 录音专用的 Opus 编码器：参数与蓝牙编码器保持一致，音质相同。
    if (s_enc == nullptr) {
        int err = 0;
        s_enc = opus_encoder_create(MIC_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK || s_enc == nullptr) {
            Serial.printf("【录音】Opus 编码器创建失败: %d\n", err);
            s_enc = nullptr;
            return false;
        }
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(s_enc, OPUS_SET_VBR(OPUS_VBR));
        opus_encoder_ctl(s_enc, OPUS_SET_VBR_CONSTRAINT(0));
        opus_encoder_ctl(s_enc, OPUS_SET_LSB_DEPTH(16));
        opus_encoder_ctl(s_enc, OPUS_SET_DTX(0));
        opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(0));
    }

    // GPIO21 是扩展板的 SD 片选，同时兼作状态 LED，故挂卡后 LED 不再可靠。
    // 依次尝试 4MHz/1MHz/400kHz，容忍走线质量较差的卡。
    s_sd_ok = (mount_sd_best_speed(true) != 0);
    if (s_sd_ok) {
        if (!SD.exists(RECORD_SD_DIR)) {
            SD.mkdir(RECORD_SD_DIR);
        }
        Serial.println("【录音】SD卡已挂载");
    } else {
        Serial.println("【录音】无SD卡，仅预缓冲模式");
    }

    if (s_task == nullptr) {
        xTaskCreate(recorder_task, "recorder", 4096, nullptr, 1, &s_task);
    }
    return true;
}

bool recorder_start()
{
    if (s_enc == nullptr) {
        return false;
    }

    uint32_t now = s_produced;
    uint32_t start = align_up(now);
    uint32_t oldest = (now > (uint32_t)RING_SAMPLES)
                          ? now - (uint32_t)RING_SAMPLES
                          : 0;
    uint32_t flushed = align_up(oldest);

    bool ok = false;
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        // 被动录音不抢占主动：主动在录时忽略本次被动触发。
        if (s_active.active) {
            xSemaphoreGive(s_mtx);
            return false;
        }
        ok = session_open(&s_passive, true, flushed, start, "p");
        xSemaphoreGive(s_mtx);
    }
    return ok;
}

void recorder_stop()
{
    if (s_mtx == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        session_close(&s_passive);
        xSemaphoreGive(s_mtx);
    }
}

bool recorder_is_writing()
{
    return s_passive.active;
}

bool recorder_active_start()
{
    if (s_enc == nullptr) {
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        // 主动优先级更高：先停掉正在进行的被动录音（已写内容保留），
        // 并把打断关联写进 journal，0x12 删除时能连带删掉这条被动。
        if (s_passive.active) {
            Serial.println("【录音】主动录音抢占，先停止被动录音");
            char pname[64];
            strcpy(pname, s_passive.name);
            session_close(&s_passive);
            uint32_t p = align_up(s_produced);
            ok = session_open(&s_active, false, p, p, "a");
            if (ok) {
                journal_link(s_active.name, pname);
            }
        } else {
            uint32_t p = align_up(s_produced);
            ok = session_open(&s_active, false, p, p, "a");
        }
        xSemaphoreGive(s_mtx);
    }
    return ok;
}

void recorder_active_stop()
{
    if (s_mtx == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        session_close(&s_active);
        xSemaphoreGive(s_mtx);
    }
}

bool recorder_active_writing()
{
    return s_active.active;
}

size_t recorder_prebuffer_seconds()
{
    return (size_t)(s_since_ring_start / MIC_SAMPLE_RATE);
}
