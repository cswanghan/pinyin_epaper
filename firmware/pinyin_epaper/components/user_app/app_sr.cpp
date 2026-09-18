/*****************************************************************************
 * 离线语音识别 —— WakeNet 唤醒 + MultiNet 中文命令词
 *
 * 数据流:
 *   麦克风 --audio_playback_read--> feed 任务 --afe->feed--> AFE 前端
 *                                                             |
 *   回调 <-- detect 任务 <--afe->fetch-- 唤醒检测 / 命令词识别 --+
 *
 * 关键设计:
 *   1. 板载喇叭和麦克风距离极近，朗读时声音必然被拾取。用 app_sr_set_muted()
 *      在播放期间给 AFE 喂静音，避免自激误触发。（比 AEC 简单可靠，
 *      因为我们拿不到播放端的参考信号。）
 *   2. 查字「对原始文本」: 清单里 2500 字共 1.4 万条说法（「如果的如」），MultiNet7 一次
 *      最多注册 400 条，放不下。试过常驻每个音节一条「de X」、听到了再换上这一份重认，
 *      但短命令在句尾几乎不触发；而 MultiNet 不套命令词表、逐字听出的拼音（raw_string）
 *      很准:「如果的果」→ " ru guo de guo"。所以命令词表只放固定命令，查字取原始文本里
 *      最后的「de X」，跟以 X（和容易听混的音节）结尾的说法逐条比，取最像的（见 lookup）。
 *      说法必须以「de <音节>」结尾。
 *   3. 等这句话说完再执行，以最后的结果为准。「说完」= 原始文本 STABLE_MS 没变；周围一直
 *      有人说话时，听到后最多再等 COMMIT_MAX_MS。不看 VAD：实测 6 秒的窗口里 VAD 判人声
 *      5.7 秒以上。「认识的认」会先触发固定命令「认识」（= 我会了），接着原始文本里才出现
 *     「de ren」，这时查字优先。
 *   4. 连续对话：命令执行后继续听 follow_up_ms，不用每次都喊唤醒词。
 *      播放期间暂停计时，读完字音后孩子仍有完整的时间说下一句。
 *      窗口按送进 MultiNet 的录音时长计，不按墙钟（见 detect_task）。
 *   5. WakeNet 和 MultiNet 都在 CPU1 的 detect 任务里跑，160 MHz 下一起跑算不过来
 *      （任务看门狗报过 IDLE1 饿死）。所以 CPU 开到 240 MHz、数据缓存 64 KB
 *      （sdkconfig，esp-sr 推荐配置），并且等命令期间关掉 WakeNet。
 *   6. 查字表可能在识别运行中重建（app_sr_set_lookup），和 detect 任务、控制台查字之间
 *      用互斥锁隔开。
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "app_sr.h"
#include "audio_bsp.h"

#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

#define TAG "SR"

#define SR_MAX_PHRASES   400        /* MultiNet7 单次最多注册的说法条数（ESP_MN_MAX_PHRASE_NUM）*/
#define PHRASE_MAX       63         /* 一条说法最长字节数（ESP_MN_MAX_PHRASE_LEN）*/
#define SYL_MAX          512        /* 音节种数上限（普通话约 400 种）*/
#define SYL_LEN          8          /* 音节最长 6 个字母（zhuang），留余量 */
#define SYL_HASH_N       1024       /* 音节哈希表大小，2 的幂 */

#define TOK_MAX          32         /* 原始文本最多看最后多少个音节 */
#define LOOK_TOK         12         /* 查字取「de X」连同前面的音节，最多几个 */
#define PH_TOK           12         /* 一条说法最多几个音节，再长的不比 */
#define TRAIL_MAX        2          /* 「de X」后面还可以跟几个音节（「如果的果啊」）*/
#define CAND_SYL         8          /* 最多比几份（听到的音节 + 容易听混的）*/
#define BEST_N           3          /* 日志里列出最像的几个字 */
#define COST_ALT         0.25f      /* 声母或韵母换成容易听混的（zh/z、in/ing…）*/
#define COST_DIFF        0.5f       /* 声母或韵母不一样 */
#define COST_GAP         1.0f       /* 多听到或少听到一个音节 */
#define ACCEPT_COST      0.34f      /* 平均每个音节差这么多以内才算对上：两个字的词可以错一个字 */
#define AMBIG_COST       0.04f      /* 最像的两个字差得比这还少（只听到「guo de guo」），说不准是哪个 */
#define WAKE_TH_HIGH     0.45f      /* 灵敏档的唤醒阈值；模型自带 0.58（模型名末尾那两个数）*/

#define STABLE_MS        640        /* 原始文本这么久没变 = 这句话说完了 */
#define COMMIT_MAX_MS    1500       /* 听到后原始文本一直在变（周围有人说话），最多再等这么久 */
#define VAD_NOISE_MS     256        /* VAD 连续多久非人声才转成静音（默认 1000 ms）*/
#define IDLE_GAP_MS      480        /* 待唤醒时人声停了这么久算一段话 */
#define IDLE_MIN_MS      640        /* 这么长的人声没唤醒才记一笔 */
#define IDLE_LONG_MS     10000      /* 人声一直不停（周围在说话），这么久记一笔 */

static const esp_afe_sr_iface_t *s_afe   = NULL;
static esp_afe_sr_data_t        *s_afe_d = NULL;
static const esp_mn_iface_t     *s_mn    = NULL;
static model_iface_data_t       *s_mn_d  = NULL;

static sr_cmd_cb_t       s_cb    = NULL;
static sr_config_t       s_cfg   = { false, 0.0f, 6000, 6000 };
static SemaphoreHandle_t s_lock  = NULL;     /* 保护查字表（重建 / 查）*/
static volatile bool     s_muted = false;
static volatile bool     s_wake_req = false;   /* 按键要求唤醒，detect 任务下一帧消费 */
static int               s_phrase_n = 0;     /* 已注册条数，用于截断 */
static int               s_spms  = 16;       /* 每毫秒采样数 */

/* 查字表按说法的最后一个音节分份: 音节 k 的说法是 s_ph[s_syl_first[k] .. + s_syl_cnt[k]) */
typedef struct { const char *p; int id; uint8_t len; } phrase_t;   /* p 指向应用层的字符串，不以 \0 结尾 */
static phrase_t *s_ph   = NULL;              /* PSRAM */
static int       s_ph_n = 0;
static char      s_syl[SYL_MAX][SYL_LEN];
static int       s_syl_first[SYL_MAX];
static int       s_syl_cnt[SYL_MAX];
static int       s_syl_n = 0;
static int16_t   s_syl_hash[SYL_HASH_N];     /* 音节 → 序号，-1 = 空 */

/* 一个音节，il = 声母长度（zh/ch/sh 2，零声母 0）*/
typedef struct { char s[SYL_LEN]; uint8_t il; } syl_t;

/* 这句话的原始文本。MultiNet 触发固定命令、超时后要 clean，clean 会清空 raw_string，
 * 所以先把它接到 s_utt 后面（「认识」+「的认」= 认识的认）。*/
static char  s_utt[256];
static char  s_raw[256];                     /* MultiNet 这一段的 raw_string */
static syl_t s_look[LOOK_TOK];               /* 最后听到的「… de X」，说完了拿它查字 */
static int   s_look_n = 0;

void app_sr_set_muted(bool muted) { s_muted = muted; }
void app_sr_wake(void)            { s_wake_req = true; }

/* ---------- 固定命令词 ----------
 * 无声调拼音，空格分隔；同一命令的多种说法用逗号分隔。命令词表里只有这些（查字不注册），
 * 每条命令可以多放几种说法。不要用「de」结尾的说法（「错的」「不会的」）：
 * 后面带个语气词就成了「de X」，会被当成查字。
 * 「认识」和「认识的认」开头一样，靠「等这句话说完、查字优先」区分（见 detect_task）。
 */
struct fixed_cmd { int id; const char *phrases; };
static const struct fixed_cmd FIXED[] = {
    { SR_CMD_NEXT,     "xia yi ge,huan yi ge,xia yi ti,tiao guo" },
    { SR_CMD_PREV,     "shang yi ge,hui qu,gang cai na ge" },
    { SR_CMD_REPEAT,   "zai du yi bian,zai shuo yi bian,mei ting qing,zai lai yi ci" },
    { SR_CMD_WORDS,    "du ci zu,zen me zu ci,zu ge ci,you shen me ci" },
    { SR_CMD_MASTERED, "wo hui le,hui le,ren shi" },
    { SR_CMD_REVIEW,   "fu xi" },
    { SR_CMD_FORGOT,   "wang le,wo wang le,mei ji zhu" },
};

/* 从 "a,b,c" 里取下一条说法，返回起点（不以 \0 结尾），*len 为长度，没有了返回 NULL。
 *
 * 不用 strtok：esp_mn_commands_add 内部会调用同样使用 strtok 的函数
 * （flite_g2p / check_speech_command），在外面用 strtok 拆会让内部状态被冲掉，
 * 结果每条命令只有第一种说法被注册，且不报任何错误。
 */
static const char *next_phrase(const char **pp, int *len)
{
    const char *p = *pp;
    while (*p == ' ' || *p == ',') p++;
    if (!*p) { *pp = p; return NULL; }
    const char *s = p;
    while (*p && *p != ',') p++;
    int n = (int)(p - s);
    while (n > 0 && s[n - 1] == ' ') n--;
    *pp = p;
    *len = n;
    return s;
}

/* 「ru guo de ru」→ 最后一个音节「ru」；不是「… de X」的形式返回 NULL */
static const char *final_syl(const char *p, int len, int *sl)
{
    if (len > PHRASE_MAX) return NULL;
    int i = len - 1;
    while (i >= 0 && p[i] != ' ') i--;
    if (i < 4 || memcmp(p + i - 3, " de ", 4) != 0) return NULL;   /* 前面至少还有一个音节 */
    *sl = len - i - 1;
    return (*sl > 0 && *sl < SYL_LEN) ? p + i + 1 : NULL;
}

/* 音节 → 序号（FNV-1a + 开放寻址），add = 没有就新建 */
static int syl_index(const char *s, int n, bool add)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    for (int k = 0; k < SYL_HASH_N; k++) {
        int slot = (int)((h + k) & (SYL_HASH_N - 1));
        int idx = s_syl_hash[slot];
        if (idx < 0) {
            if (!add || s_syl_n >= SYL_MAX) return -1;
            idx = s_syl_n++;
            memcpy(s_syl[idx], s, n);
            s_syl[idx][n] = '\0';
            s_syl_cnt[idx] = 0;
            s_syl_hash[slot] = (int16_t)idx;
            return idx;
        }
        if (memcmp(s_syl[idx], s, n) == 0 && s_syl[idx][n] == '\0') return idx;
    }
    return -1;
}

/* 把查字表按最后一个音节分份。*bad = 不是「… de X」形式的说法条数 */
static bool build_index(const int *ids, const char *const *phrases, int n, int *bad)
{
    s_syl_n = 0;
    s_ph_n  = 0;
    memset(s_syl_hash, 0xff, sizeof(s_syl_hash));
    *bad = 0;

    int total = 0;                                     /* 第一遍: 数每个音节多少条 */
    for (int i = 0; i < n; i++) {
        const char *pp = phrases[i] ? phrases[i] : "";
        const char *p;
        int len, sl;
        while ((p = next_phrase(&pp, &len))) {
            const char *s = final_syl(p, len, &sl);
            int k = s ? syl_index(s, sl, true) : -1;
            if (k < 0) { (*bad)++; continue; }
            s_syl_cnt[k]++;
            total++;
        }
    }
    free(s_ph);
    s_ph = total ? (phrase_t *)heap_caps_malloc(total * sizeof(phrase_t), MALLOC_CAP_SPIRAM) : NULL;
    if (total && !s_ph) {
        ESP_LOGE(TAG, "查字表要 %u 字节，内存不够", (unsigned)(total * sizeof(phrase_t)));
        s_syl_n = 0;
        return false;
    }
    for (int k = 0, acc = 0; k < s_syl_n; k++) {
        s_syl_first[k] = acc;
        acc += s_syl_cnt[k];
        s_syl_cnt[k] = 0;
    }
    for (int i = 0; i < n; i++) {                      /* 第二遍: 放进各自的份 */
        const char *pp = phrases[i] ? phrases[i] : "";
        const char *p;
        int len, sl;
        while ((p = next_phrase(&pp, &len))) {
            const char *s = final_syl(p, len, &sl);
            int k = s ? syl_index(s, sl, false) : -1;
            if (k < 0) continue;
            phrase_t *ph = &s_ph[s_syl_first[k] + s_syl_cnt[k]++];
            ph->p   = p;
            ph->id  = ids[i];
            ph->len = (uint8_t)len;
        }
    }
    s_ph_n = total;
    return true;
}

static bool add_one(int cmd_id, const char *p, int len)
{
    char buf[PHRASE_MAX + 1];
    if (len <= 0 || len > PHRASE_MAX || s_phrase_n >= SR_MAX_PHRASES) return false;
    memcpy(buf, p, len);
    buf[len] = '\0';
    if (esp_mn_commands_add(cmd_id, buf) != ESP_OK) return false;
    s_phrase_n++;
    return true;
}

/* 登记固定命令，返回被模型拒绝的条数 */
static int register_fixed(void)
{
    esp_mn_commands_clear();
    s_phrase_n = 0;
    for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++) {
        const char *csv = FIXED[i].phrases, *p;
        int len;
        while ((p = next_phrase(&csv, &len)))
            add_one(FIXED[i].id, p, len);
    }
    /* 返回的是"模型无法接受的短语"列表，而非错误码 —— 正好用来暴露拼音写错的命令词 */
    esp_mn_error_t *err = esp_mn_commands_update();
    int rejected = (err && err->num > 0) ? err->num : 0;
    for (int i = 0; i < rejected && i < 10; i++)
        if (err->phrases[i]) ESP_LOGW(TAG, "  模型不接受: \"%s\"", err->phrases[i]->string);
    return rejected;
}

/* ---------- 音节比较 ----------
 * 孩子说话、方言口音常常前后鼻音不分、平翘舌不分、n/l 不分，MultiNet 也会听混。
 * 这些差别只算一小半（COST_ALT），听到「de zang」也去「de zhang」那份里找。
 * 一次只换一处（「zhang」→ zang / chang / zhan，不会到 can）。
 */
static const char *const FINAL_ALT[][3] = {
    { "an", "ang" },   { "ang", "an" },   { "en", "eng" },   { "eng", "en" },
    { "in", "ing" },   { "ing", "in" },   { "ian", "iang" }, { "iang", "ian" },
    { "uan", "uang" }, { "uang", "uan" }, { "uo", "o", "ou" }, { "o", "uo" }, { "ou", "uo" },
};
static const char *const INIT_ALT[][3] = {
    { "zh", "z", "ch" }, { "ch", "c", "zh" }, { "sh", "s", "ch" },
    { "z", "zh", "c" },  { "c", "ch", "z" },  { "s", "sh", "c" },
    { "n", "l" },        { "l", "n", "r" },   { "r", "l" },
    { "f", "h" },        { "h", "f", "k" },
    { "j", "q", "x" },   { "q", "j", "x" },   { "x", "q", "j" },
    { "g", "k" },        { "k", "g", "h" },
    { "b", "p" },        { "p", "b" },        { "d", "t" },  { "t", "d" },
};
#define N_FINAL_ALT (sizeof(FINAL_ALT) / sizeof(FINAL_ALT[0]))
#define N_INIT_ALT  (sizeof(INIT_ALT) / sizeof(INIT_ALT[0]))

static int init_len(const char *s)
{
    if ((s[0] == 'z' || s[0] == 'c' || s[0] == 's') && s[1] == 'h') return 2;
    return (s[0] && strchr("bpmfdtnlgkhjqxrzcsyw", s[0])) ? 1 : 0;
}

static void syl_set(syl_t *y, const char *p, int n)
{
    memcpy(y->s, p, n);
    y->s[n] = '\0';
    y->il = (uint8_t)init_len(y->s);
}

static bool alt_has(const char *const tab[][3], size_t rows, const char *a, const char *b)
{
    for (size_t r = 0; r < rows; r++)
        if (!strcmp(tab[r][0], a)) {
            for (int j = 1; j < 3 && tab[r][j]; j++)
                if (!strcmp(tab[r][j], b)) return true;
            return false;
        }
    return false;
}

/* 两个音节差多少: 一样 0，声母、韵母各算一半，容易听混的只算 COST_ALT */
static float syl_cost(const syl_t *a, const syl_t *b)
{
    if (!strcmp(a->s, b->s)) return 0.0f;
    char ai[3] = { 0 }, bi[3] = { 0 };
    memcpy(ai, a->s, a->il);
    memcpy(bi, b->s, b->il);
    const char *af = a->s + a->il, *bf = b->s + b->il;
    float c = 0.0f;
    if (strcmp(ai, bi)) c += alt_has(INIT_ALT, N_INIT_ALT, ai, bi) ? COST_ALT : COST_DIFF;
    if (strcmp(af, bf)) c += alt_has(FINAL_ALT, N_FINAL_ALT, af, bf) ? COST_ALT : COST_DIFF;
    return c;
}

static void push_syl(int16_t *out, int *n, int max, const char *a, const char *b)
{
    char buf[SYL_LEN * 2];
    int len = snprintf(buf, sizeof(buf), "%s%s", a, b);
    if (*n >= max || len >= SYL_LEN) return;
    int k = syl_index(buf, len, false);                /* 只要清单里有的音节 */
    if (k < 0) return;
    for (int i = 0; i < *n; i++)
        if (out[i] == k) return;
    out[(*n)++] = (int16_t)k;
}

/* 听到的音节 x（清单里有的话排第一个），和换一处后容易听混、清单里又有的音节 */
static int syl_cands(const syl_t *x, int16_t *out, int max)
{
    char init[3] = { 0 };
    memcpy(init, x->s, x->il);
    const char *fin = x->s + x->il;
    int n = 0;
    push_syl(out, &n, max, init, fin);
    for (size_t r = 0; r < N_FINAL_ALT; r++)
        if (!strcmp(FINAL_ALT[r][0], fin))
            for (int j = 1; j < 3 && FINAL_ALT[r][j]; j++) push_syl(out, &n, max, init, FINAL_ALT[r][j]);
    for (size_t r = 0; x->il && r < N_INIT_ALT; r++)
        if (!strcmp(INIT_ALT[r][0], init))
            for (int j = 1; j < 3 && INIT_ALT[r][j]; j++) push_syl(out, &n, max, INIT_ALT[r][j], fin);
    return n;
}

/* 把空格分隔的拼音（len 字节，不必以 \0 结尾）接到 tok[0..nt) 后面，满了丢掉最前面的。返回音节数 */
static int tok_add(syl_t *tok, int nt, int max, const char *s, int len)
{
    const char *e = s + len;
    while (s < e) {
        while (s < e && *s == ' ') s++;
        const char *b = s;
        while (s < e && *s != ' ') s++;
        int n = (int)(s - b);
        if (n <= 0 || n >= SYL_LEN) continue;          /* 太长的不是音节 */
        if (nt == max) { memmove(tok, tok + 1, (max - 1) * sizeof(*tok)); nt--; }
        syl_set(&tok[nt++], b, n);
    }
    return nt;
}

/* 最后一个「de X」里 X 的位置，「de」前面至少还有一个音节。X 后面最多跟 TRAIL_MAX 个音节
 * （「如果的果啊」）。没有返回 -1 */
static int find_de(const syl_t *tok, int nt)
{
    for (int t = 0; t <= TRAIL_MAX; t++) {
        int j = nt - 1 - t;
        if (j >= 2 && !strcmp(tok[j - 1].s, "de")) return j;
    }
    return -1;
}

/* ---------- 查字 ---------- */

/* 说法「de」前面的音节 p[0..m) 跟听到的 h[0..nh) 末尾对齐，返回差多少。
 * h 前面多出来的不算（「你好小智如果的果」），中间多听、少听一个音节算 COST_GAP。*/
static float align(const syl_t *p, int m, const syl_t *h, int nh)
{
    int hn = nh < m + 3 ? nh : m + 3;
    h += nh - hn;
    float d[PH_TOK][PH_TOK + 2];
    for (int j = 0; j <= hn; j++) d[0][j] = 0.0f;
    for (int i = 1; i <= m; i++) {
        d[i][0] = i * COST_GAP;
        for (int j = 1; j <= hn; j++) {
            float v = d[i - 1][j - 1] + syl_cost(&p[i - 1], &h[j - 1]);
            v = fminf(v, d[i - 1][j] + COST_GAP);
            v = fminf(v, d[i][j - 1] + COST_GAP);
            d[i][j] = v;
        }
    }
    return d[m][hn];
}

typedef struct { int id; float cost; int m; const char *p; int len; } best_t;

/* 差得少的更像；一样时说法长的优先（对上的音节多）*/
static bool better(float cost, int m, const best_t *b)
{
    return cost < b->cost - 1e-4f || (cost < b->cost + 1e-4f && m > b->m);
}

/* 放进最像的前 BEST_N 名，同一个字只留最像的一条 */
static void best_put(best_t *b, int *nb, const phrase_t *ph, int m, float cost)
{
    for (int i = 0; i < *nb; i++) {
        if (b[i].id != ph->id) continue;
        if (!better(cost, m, &b[i])) return;
        memmove(&b[i], &b[i + 1], (*nb - i - 1) * sizeof(*b));
        (*nb)--;
        break;
    }
    int pos = *nb;
    while (pos > 0 && better(cost, m, &b[pos - 1])) pos--;
    if (pos >= BEST_N) return;
    if (*nb < BEST_N) (*nb)++;
    memmove(&b[pos + 1], &b[pos], (*nb - 1 - pos) * sizeof(*b));
    b[pos].id   = ph->id;
    b[pos].cost = cost;
    b[pos].m    = m;
    b[pos].p    = ph->p;
    b[pos].len  = ph->len;
}

/* 第一名跟后面某个字差不多，就说不准是哪个。例外：那个字的说法更短、差得也不比第一名少——
 * 它只对上了听到的后半截（「公交车的交」里的「轿车的轿」），第一名把前面的音节也对上了，更像 */
static bool ambiguous(const best_t *b, int nb)
{
    for (int i = 1; i < nb && b[i].cost - b[0].cost < AMBIG_COST; i++)
        if (!(b[i].m < b[0].m && b[i].cost > b[0].cost - 1e-4f)) return true;
    return false;
}

/* 听到的 tok[0..j]（tok[j-1] 是 de，tok[j] 是 X）跟查字表比：去 X 和容易听混的音节那几份里
 * 逐条算差多少（平均到每个音节），取最像的。返回 SR_CMD_CHAR_BASE + 字 id；
 * 差太多、或者前两名差不多（说不准是哪个，跳错了孩子不一定发现），返回 SR_EVT_NOT_FOUND。调用时持锁。*/
static int lookup(const syl_t *tok, int j)
{
    int64_t t0 = esp_timer_get_time();
    int16_t cs[CAND_SYL];
    int nc = s_ph ? syl_cands(&tok[j], cs, CAND_SYL) : 0;
    best_t best[BEST_N];
    int nb = 0, cmp_n = 0;
    char names[64] = "";
    int nl = 0;
    for (int c = 0; c < nc; c++) {
        int k = cs[c];
        syl_t xk;
        syl_set(&xk, s_syl[k], (int)strlen(s_syl[k]));
        float xc = syl_cost(&tok[j], &xk);
        for (int i = 0; i < s_syl_cnt[k]; i++) {
            const phrase_t *ph = &s_ph[s_syl_first[k] + i];
            syl_t p[PH_TOK];
            int np = tok_add(p, 0, PH_TOK, ph->p, ph->len);
            int m = np - 2;                            /* 去掉「de X」*/
            if (m < 1 || np == PH_TOK) continue;       /* 满了说明可能被截断，不比 */
            float cost = (align(p, m, tok, j - 1) + xc) / (m + 1);
            best_put(best, &nb, ph, m, cost);
            cmp_n++;
        }
        if (nl < (int)sizeof(names))
            nl += snprintf(names + nl, sizeof(names) - nl, "%s%s", c ? "/" : "", s_syl[k]);
    }
    bool ok    = nb > 0 && best[0].cost <= ACCEPT_COST;
    bool ambig = ok && ambiguous(best, nb);

    char heard[96] = "";
    int hl = 0;
    for (int i = j + 1 > LOOK_TOK ? j + 1 - LOOK_TOK : 0; i <= j && hl < (int)sizeof(heard); i++)
        hl += snprintf(heard + hl, sizeof(heard) - hl, "%s%s", hl ? " " : "", tok[i].s);
    ESP_LOGI(TAG, "查字「%s」: 比了 %s 共 %d 条，%d ms → %s", heard,
             !s_ph ? "（还没登记查字表）" : nc ? names : "（清单里没有这个音节）",
             cmp_n, (int)((esp_timer_get_time() - t0) / 1000),
             !ok ? "对不上" : ambig ? "说不准是哪个" : "对上了");
    for (int i = 0; i < nb; i++)
        ESP_LOGI(TAG, "  %d. 字 id %d 差 %.2f \"%.*s\"", i + 1, best[i].id, best[i].cost, best[i].len, best[i].p);
    return ok && !ambig ? SR_CMD_CHAR_BASE + best[0].id : SR_EVT_NOT_FOUND;
}

/* ---------- 这句话的原始文本 ---------- */

/* MultiNet 的 raw_string 变了就记下来、打一行，返回是否变了 */
static bool raw_update(const char *raw)
{
    if (!strcmp(raw, s_raw)) return false;
    strlcpy(s_raw, raw, sizeof(s_raw));
    ESP_LOGI(TAG, "  原始: \"%s%s\"", s_utt, s_raw);
    return true;
}

/* MultiNet 要 clean 了，把这一段接到本句后面；太长只留后面的 */
static void utt_fold(void)
{
    if (!s_raw[0]) return;
    size_t ul = strlen(s_utt), rl = strlen(s_raw) + 1;   /* +1: 前面补个空格 */
    if (ul + rl >= sizeof(s_utt)) {
        size_t keep = rl < sizeof(s_utt) ? sizeof(s_utt) - 1 - rl : 0;
        const char *p = s_utt + ul - keep;
        while (*p && *p != ' ') p++;                     /* 不留半个音节 */
        memmove(s_utt, p, strlen(p) + 1);
    }
    strlcat(s_utt, " ", sizeof(s_utt));
    strlcat(s_utt, s_raw, sizeof(s_utt));
    s_raw[0] = '\0';
}

/* 本句最后的「… de X」存进 s_look。跟上次不一样（新说了一句）返回 true */
static bool look_update(void)
{
    syl_t tok[TOK_MAX];
    int nt = tok_add(tok, 0, TOK_MAX, s_utt, (int)strlen(s_utt));
    nt = tok_add(tok, nt, TOK_MAX, s_raw, (int)strlen(s_raw));
    int j = find_de(tok, nt);
    if (j < 0) return false;
    int from = j + 1 > LOOK_TOK ? j + 1 - LOOK_TOK : 0, ln = j + 1 - from;
    bool same = ln == s_look_n;
    for (int i = 0; same && i < ln; i++) same = !strcmp(s_look[i].s, tok[from + i].s);
    if (same) return false;
    memcpy(s_look, tok + from, ln * sizeof(syl_t));
    s_look_n = ln;
    return true;
}

int app_sr_set_lookup(const int *ids, const char *const *phrases, int n)
{
    if (!s_lock) { ESP_LOGE(TAG, "语音识别未启动"); return -1; }

    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int bad = 0;
    bool ok = build_index(ids, phrases, n, &bad);
    xSemaphoreGive(s_lock);

    int big = 0;
    for (int k = 1; k < s_syl_n; k++)
        if (s_syl_cnt[k] > s_syl_cnt[big]) big = k;
    ESP_LOGI(TAG, "查字表: %d 字 %d 条说法，按最后一个音节分 %d 份（最大「的%s」%d 条），耗时 %d ms",
             n, s_ph_n, s_syl_n, s_syl_n ? s_syl[big] : "", s_syl_n ? s_syl_cnt[big] : 0,
             (int)((esp_timer_get_time() - t0) / 1000));
    if (bad)
        ESP_LOGW(TAG, "%d 条说法不是「… de X」的形式，查不到（用 gen_scopes.py 重新生成清单）", bad);
    return ok ? s_ph_n : -1;
}

int app_sr_lookup_text(const char *text)
{
    syl_t tok[TOK_MAX];
    int nt = tok_add(tok, 0, TOK_MAX, text, (int)strlen(text));
    int j = find_de(tok, nt);
    if (j < 0 || !s_lock) {
        ESP_LOGI(TAG, "「%s」%s", text, s_lock ? "里没有「… de X」" : ": 语音识别未启动");
        return SR_EVT_NOT_FOUND;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int cmd = lookup(tok, j);
    xSemaphoreGive(s_lock);
    return cmd;
}

/* 诊断开关：把左右两路的峰值都打出来。板载麦克风到底落在哪个声道、
 * 电平够不够，靠耳朵和 VAD 的单一数字都看不出来 —— VAD 只报我们喂进去的那一路。
 * 换板子/换麦克风时打开，平时置 0。*/
#define SR_MIC_PROBE  0
#define MIC_PROBE_MS  2000

/* 增益扫描：每隔几秒换一档 PGA，把各档的底噪/峰值排出来看。
 * 靠单一档位的数字判断不了「增益偏高」，得有对比。查完置 0。*/
#define SR_GAIN_SWEEP   0
#define SWEEP_STEP_MS   6000

#if SR_MIC_PROBE
/* 把一段原始采样按 base64 吐到串口，Mac 上还原成 wav 看波形和频谱。
 * 峰值数字分不清「底噪」和「破掉的人声」，波形能。
 * 注意：吐的时候 feed 会卡住，AFE 会丢帧 —— 只在查麦克风时用。*/
#define DUMP_SAMPLES  16000        /* 1 秒 @16k */
#define DUMP_TIMES    0            /* 要抓原始波形时改成 3 */
#define DUMP_FIRST_S  8
#define DUMP_GAP_S    10

/* 每行独立可解：`MD <行号> <384 字节的 base64>`。
 * USB CDC 满了会丢字节，不带行号的话丢一点后面整段都错位、对不齐。*/
#define DUMP_LINE_BYTES  384

static void mic_dump(const int16_t *pcm, int n)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = (const uint8_t *)pcm;
    int bytes = n * 2;
    int lines = (bytes + DUMP_LINE_BYTES - 1) / DUMP_LINE_BYTES;
    char out[DUMP_LINE_BYTES / 3 * 4 + 1];

    printf("\n<<<MICDUMP %d %d\n", bytes, lines);
    fflush(stdout);
    for (int ln = 0; ln < lines; ln++) {
        int off = ln * DUMP_LINE_BYTES;
        int len = bytes - off; if (len > DUMP_LINE_BYTES) len = DUMP_LINE_BYTES;
        int o = 0;
        for (int i = 0; i < len; i += 3) {
            uint32_t v = p[off + i] << 16;
            if (i + 1 < len) v |= p[off + i + 1] << 8;
            if (i + 2 < len) v |= p[off + i + 2];
            out[o++] = b64[(v >> 18) & 63];
            out[o++] = b64[(v >> 12) & 63];
            out[o++] = (i + 1 < len) ? b64[(v >> 6) & 63] : '=';
            out[o++] = (i + 2 < len) ? b64[v & 63]        : '=';
        }
        out[o] = 0;
        printf("MD %d %s\n", ln, out);
        fflush(stdout);
        if ((ln & 7) == 7) vTaskDelay(1);      /* 让 USB 把缓冲吐干净 */
    }
    printf(">>>MICDUMP\n");
    fflush(stdout);
}
#endif

/* ---------- feed 任务：麦克风 → AFE ---------- */
static void feed_task(void *arg)
{
    int chunk    = s_afe->get_feed_chunksize(s_afe_d);       /* 每通道采样数 */
    int feed_ch  = s_afe->get_feed_channel_num(s_afe_d);
    /* codec 以双声道打开，需要把读到的立体声降成 AFE 要的通道数 */
    int16_t *stereo = (int16_t *)heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *feed   = (int16_t *)heap_caps_malloc(chunk * feed_ch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(stereo && feed);
    ESP_LOGI(TAG, "feed 任务启动: chunk=%d feed_ch=%d", chunk, feed_ch);

#if SR_MIC_PROBE
    int pk_l = 0, pk_r = 0, probe_n = 0;
    const int probe_max = MIC_PROBE_MS * 16 / chunk;      /* 16 采样/ms */
    int64_t sq_l = 0;
    int16_t *dump = DUMP_TIMES ? (int16_t *)heap_caps_malloc(DUMP_SAMPLES * 2, MALLOC_CAP_SPIRAM) : NULL;
    int dump_n = 0, dump_done = 0;
    int64_t dump_at = esp_timer_get_time() + (int64_t)DUMP_FIRST_S * 1000000;
#if SR_GAIN_SWEEP
    static const float sweep[] = { 42, 36, 30, 24, 18, 12 };
    int sweep_i = -1;
    int64_t sweep_at = esp_timer_get_time();
#endif
#endif

    for (;;) {
        audio_playback_read(stereo, chunk * 2 * sizeof(int16_t));
#if SR_MIC_PROBE
        if (!s_muted) {
#if SR_GAIN_SWEEP
            if (esp_timer_get_time() >= sweep_at &&
                sweep_i + 1 < (int)(sizeof(sweep) / sizeof(sweep[0]))) {
                sweep_i++;
                audio_record_set_gain(sweep[sweep_i]);
                ESP_LOGW(TAG, "== PGA 增益 → %.0f dB ==", sweep[sweep_i]);
                sweep_at = esp_timer_get_time() + (int64_t)SWEEP_STEP_MS * 1000;
                pk_l = pk_r = 0; sq_l = 0; probe_n = 0;      /* 换档后重新统计 */
            }
#endif
            for (int i = 0; i < chunk; i++) {
                int l = abs(stereo[i * 2]), r = abs(stereo[i * 2 + 1]);
                if (l > pk_l) pk_l = l;
                if (r > pk_r) pk_r = r;
                sq_l += (int64_t)stereo[i * 2] * stereo[i * 2];
            }
            if (dump && dump_done < DUMP_TIMES && esp_timer_get_time() >= dump_at) {
                for (int i = 0; i < chunk && dump_n < DUMP_SAMPLES; i++)
                    dump[dump_n++] = stereo[i * 2];
                if (dump_n >= DUMP_SAMPLES) {
                    mic_dump(dump, dump_n);
                    dump_n = 0;
                    dump_done++;
                    dump_at = esp_timer_get_time() + (int64_t)DUMP_GAP_S * 1000000;
                }
            }
            if (++probe_n >= probe_max) {
                double rms = sqrt((double)sq_l / ((double)probe_n * chunk));
                ESP_LOGI(TAG, "mic: 左 底噪 %.0f dB 峰值 %.0f dB | 右 %s",
                         20 * log10((rms > 1 ? rms : 1) / 32768.0),
                         20 * log10((pk_l ? pk_l : 1) / 32768.0),
                         pk_r ? "有信号" : "静默");
                pk_l = pk_r = 0; sq_l = 0; probe_n = 0;
            }
        }
#endif
        if (s_muted) {
            /* 朗读中：喂静音，既保持 AFE 时序又不会被喇叭声误触发 */
            memset(feed, 0, chunk * feed_ch * sizeof(int16_t));
        } else {
            for (int i = 0; i < chunk; i++)          /* 取左声道 */
                for (int c = 0; c < feed_ch; c++)
                    feed[i * feed_ch + c] = stereo[i * 2];
        }
        s_afe->feed(s_afe_d, feed);
    }
}

/* ---------- detect 任务：唤醒 + 命令词 ----------
 * IDLE   只跑唤醒词
 * LISTEN 关掉唤醒词，录音送 MultiNet 识别，窗口用完回 IDLE
 *        唤醒后没说任何命令就超时 → 回调 SR_EVT_TIMEOUT（提示音）
 *        连续对话窗口结束 → 静默回 IDLE
 *
 * 窗口按「送进 MultiNet 的录音时长」计，不看墙钟：算得慢时录音在 AFE 缓冲里排队，
 * 按墙钟会把还没算到的话当成超时扔掉。同理，等锁（重建查字表）时不取数，不丢帧。
 * 播放期间 feed 任务喂的是全零，这样的帧不计时，放完重新给满一个窗口。按帧内容
 * 而不是 s_muted 判断，因为排队处理到这一帧时标志可能早就变了。
 */

/* 一个窗口的诊断数据，窗口结束打一行：没识别到时看是哪一环出了问题 */
typedef struct {
    int64_t t0;          /* 窗口开始（墙钟 us）*/
    int     heard;       /* 送进 MultiNet 的采样数 */
    int     muted;       /* 播放中屏蔽掉的采样数 */
    int     speech;      /* 其中 VAD 判为人声的采样数 */
    int     speech_at;   /* 第一次听到人声时已送了多少采样，-1 = 没听到 */
    float   peak_db;     /* 送进去的录音的最大音量 */
    float   min_free;    /* AFE 缓冲最少剩多少，越小说明排队越多 */
    int64_t wait_us;     /* 等锁（在重建查字表）*/
    int64_t det_us;      /* detect() 总耗时 */
    int     det_max_us;
} win_stat_t;

static void win_reset(win_stat_t *w)
{
    memset(w, 0, sizeof(*w));
    w->t0        = esp_timer_get_time();
    w->speech_at = -1;
    w->peak_db   = -100.0f;
    w->min_free  = 1.0f;
}

static void win_log(const win_stat_t *w, const char *how)
{
    int64_t heard_us = (int64_t)w->heard * 1000 / s_spms;
    char sp[48] = "没有";
    if (w->speech_at >= 0)
        snprintf(sp, sizeof(sp), "%d ms，从第 %d ms 开始", w->speech / s_spms, w->speech_at / s_spms);
    ESP_LOGI(TAG, "诊断[%s] 听了 %d ms / 过了 %d ms | 静音 %d ms 等锁 %d ms | "
             "MultiNet 占实时 %d%% 最长一帧 %d ms | 缓冲最少剩 %.0f%% | 人声 %s，峰值 %.0f dB",
             how, (int)(heard_us / 1000), (int)((esp_timer_get_time() - w->t0) / 1000),
             w->muted / s_spms, (int)(w->wait_us / 1000),
             heard_us ? (int)(w->det_us * 100 / heard_us) : 0, w->det_max_us / 1000,
             w->min_free * 100, sp, w->peak_db);
}

/* 待唤醒时的人声：说了一段话却没唤醒就记一笔，漏唤醒能数出来、看得到当时的音量 */
typedef struct { int speech; int gap; float peak; } idle_stat_t;

static void idle_reset(idle_stat_t *s) { s->speech = 0; s->gap = 0; s->peak = -100.0f; }

static void idle_track(idle_stat_t *s, const afe_fetch_result_t *res, int n)
{
    if (res->vad_state == VAD_SPEECH) {
        s->speech += n;
        s->gap = 0;
        if (res->data_volume > s->peak) s->peak = res->data_volume;
        if (s->speech < IDLE_LONG_MS * s_spms) return;
        ESP_LOGI(TAG, "待唤醒: 人声 %d 秒没停（周围在说话？），峰值 %.0f dB", IDLE_LONG_MS / 1000, s->peak);
    } else {
        if (!s->speech || (s->gap += n) < IDLE_GAP_MS * s_spms) return;
        if (s->speech >= IDLE_MIN_MS * s_spms)
            ESP_LOGI(TAG, "待唤醒: 听到 %d ms 人声，峰值 %.0f dB，没唤醒", s->speech / s_spms, s->peak);
    }
    idle_reset(s);
}

static bool all_zero(const int16_t *d, int n)
{
    for (int i = 0; i < n; i++)
        if (d[i]) return false;
    return true;
}

static void to_idle(void)
{
    s_afe->enable_wakenet(s_afe_d);
    s_afe->reset_buffer(s_afe_d);
}

static void detect_task(void *arg)
{
    ESP_LOGI(TAG, "detect 任务启动: mn_chunk=%d afe_chunk=%d",
             s_mn->get_samp_chunksize(s_mn_d), s_afe->get_fetch_chunksize(s_afe_d));

    bool listening  = false;
    bool got_cmd    = false;   /* 本次唤醒后是否已识别到命令 */
    bool need_clean = false;
    int  left       = 0;       /* 窗口还剩多少采样 */
    int  pend       = -1;      /* 听到的固定命令，等这句话说完再执行，-1 = 没有 */
    bool look       = false;   /* 原始文本里有「… de X」（s_look），说完了去查字 */
    int  quiet      = 0;       /* 原始文本多久没变了 */
    int  age        = 0;       /* 听到命令 /「de X」后又过了多久 */
    const int stable_n = STABLE_MS * s_spms, age_n = COMMIT_MAX_MS * s_spms;
    win_stat_t  w;
    idle_stat_t idle;
    win_reset(&w);
    idle_reset(&idle);

    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_d);
        if (!res || res->ret_value == ESP_FAIL) continue;
        int n = res->data_size / (int)sizeof(int16_t);

        if (!listening) {
            bool by_key = s_wake_req;                /* 按键唤醒和唤醒词走同一条路 */
            if (!by_key && res->wakeup_state != WAKENET_DETECTED) { idle_track(&idle, res, n); continue; }
            s_wake_req = false;
            ESP_LOGI(TAG, "★ 唤醒（%s） 音量 %.1f dB", by_key ? "按键" : "你好小智", res->data_volume);
            s_afe->disable_wakenet(s_afe_d);         /* 等命令期间 CPU1 只跑 MultiNet */
            listening  = true;
            got_cmd    = false;
            need_clean = true;
            left       = s_cfg.listen_ms * s_spms;
            pend       = -1;
            look       = false;
            s_look_n   = 0;
            win_reset(&w);
            if (s_cb) s_cb(SR_EVT_WAKE);
            continue;
        }
        /* 已经在听的时候又按了一次唤醒键: 只把窗口续满。标志必须在这儿也清掉，
         * 否则它会一直挂着，等这次窗口超时回到待唤醒后又立刻假唤醒一次 */
        if (s_wake_req) { s_wake_req = false; left = s_cfg.listen_ms * s_spms; }

        if (res->ringbuff_free_pct < w.min_free) w.min_free = res->ringbuff_free_pct;
        bool zero = all_zero(res->data, n);
        if (zero && pend < 0 && !look) {             /* 播放中：不计时，放完给满一个窗口 */
            w.muted   += n;
            left       = (got_cmd ? s_cfg.follow_up_ms : s_cfg.listen_ms) * s_spms;
            need_clean = true;
            continue;
        }
        if (xSemaphoreTake(s_lock, 0) != pdTRUE) {   /* 正在重建查字表 */
            int64_t t = esp_timer_get_time();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            w.wait_us += esp_timer_get_time() - t;
        }

        if (!zero) {
            if (need_clean) { s_mn->clean(s_mn_d); need_clean = false; s_utt[0] = s_raw[0] = '\0'; }
            int64_t t0 = esp_timer_get_time();
            esp_mn_state_t st = s_mn->detect(s_mn_d, res->data);
            esp_mn_results_t *r = s_mn->get_results(s_mn_d);
            int us = (int)(esp_timer_get_time() - t0);
            bool changed = r && raw_update(r->raw_string);
            if (st == ESP_MN_STATE_DETECTED && r && r->num > 0) {
                pend    = r->command_id[0];
                age     = 0;
                changed = true;
                ESP_LOGI(TAG, "听到: \"%s\" id=%d prob=%.3f", r->string, pend, r->prob[0]);
            }
            if (st == ESP_MN_STATE_DETECTED || st == ESP_MN_STATE_TIMEOUT) {
                utt_fold();                          /* clean 会清空原始文本，先接到本句后面 */
                s_mn->clean(s_mn_d);                 /* 接着听，这句话可能还没说完 */
            }
            if (changed) {
                quiet = 0;
                if (look_update()) { look = true; age = 0; }
            } else {
                quiet += n;
            }
            w.det_us += us;
            if (us > w.det_max_us) w.det_max_us = us;
            if (res->vad_state == VAD_SPEECH) {
                if (w.speech_at < 0) w.speech_at = w.heard;
                w.speech += n;
            }
            if (res->data_volume > w.peak_db) w.peak_db = res->data_volume;
            w.heard += n;
            left    -= n;
        }

        /* 这句话说完了（或放起了声音、窗口用完）：有「de X」就查字，否则执行听到的固定命令 */
        int  cmd  = 0;
        bool fire = false;
        if (pend >= 0 || look) {
            age += n;
            if (zero || left <= 0 || quiet >= stable_n || age >= age_n) {
                cmd  = look ? lookup(s_look, s_look_n - 1) : pend;
                fire = true;
            }
        }
        if (fire) {
            win_log(&w, cmd == SR_EVT_NOT_FOUND ? "没对上" : "识别到");
            if (s_cb) s_cb(cmd);
            pend     = -1;
            look     = false;
            s_look_n = 0;
        }
        xSemaphoreGive(s_lock);

        if (fire) {
            got_cmd = true;
            if (s_cfg.follow_up_ms > 0) {
                left       = s_cfg.follow_up_ms * s_spms;
                need_clean = true;
                win_reset(&w);
            } else {
                listening = false;
                idle_reset(&idle);
                to_idle();
            }
        } else if (left <= 0) {
            ESP_LOGI(TAG, "%s", got_cmd ? "连续对话结束，回到待唤醒" : "没听到命令，回到待唤醒");
            win_log(&w, got_cmd ? "连续对话" : "超时");
            if (!got_cmd && s_cb) s_cb(SR_EVT_TIMEOUT);
            listening = false;
            idle_reset(&idle);
            to_idle();
        }
    }
}

/* ---------- 初始化 ---------- */
bool app_sr_start(sr_cmd_cb_t cb, const sr_config_t *cfg)
{
    s_cb = cb;
    if (cfg) s_cfg = *cfg;
    if (s_cfg.listen_ms < 2000) s_cfg.listen_ms = 2000;
    if (s_cfg.follow_up_ms < 0) s_cfg.follow_up_ms = 0;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    srmodel_list_t *models = esp_srmodel_init("model");   /* 对应分区表里的 model 分区 */
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "未找到语音模型 —— 确认 model 分区已烧录");
        return false;
    }
    for (int i = 0; i < models->num; i++)
        ESP_LOGI(TAG, "模型[%d]: %s", i, models->model_name[i]);

    /* 单麦输入。不启用 AEC：拿不到播放端参考信号，改用播放期间静音输入的办法。*/
    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_cfg) { ESP_LOGE(TAG, "afe_config_init 失败"); return false; }
    afe_cfg->aec_init = false;
    /* wn10 的系数只在 DET_MODE_90 下从 flash 加载；实测切到 DET_MODE_95 后一次都叫不醒，
     * 所以模式不动，灵敏度改成创建之后直接压唤醒阈值（见下面 set_wakenet_threshold）*/
    ESP_LOGI(TAG, "WakeNet 模式 %d（wn10 固定用 DET_MODE_90）", (int)afe_cfg->wakenet_mode);
    /* VAD 只用于诊断（窗口里的人声时长、待唤醒时没唤醒的人声），默认要连续 1 秒非人声才转静音，太粗 */
    afe_cfg->vad_min_noise_ms = VAD_NOISE_MS;
    if (afe_cfg->afe_ringbuf_size < 128)     /* 默认 50 帧，刷屏/播放占着 CPU 时会丢帧，漏唤醒 */
        afe_cfg->afe_ringbuf_size = 128;
    ESP_LOGI(TAG, "VAD %s 模式 %d，静音判定 %d ms | AFE 缓冲 %d 帧，内存模式 %d",
             afe_cfg->vad_init ? "开" : "关", (int)afe_cfg->vad_mode, afe_cfg->vad_min_noise_ms,
             afe_cfg->afe_ringbuf_size, (int)afe_cfg->memory_alloc_mode);

    s_afe   = esp_afe_handle_from_config(afe_cfg);
    s_afe_d = s_afe->create_from_config(afe_cfg);
    if (!s_afe_d) { ESP_LOGE(TAG, "AFE 创建失败"); return false; }
    s_spms = s_afe->get_samp_rate(s_afe_d) / 1000;
    if (s_cfg.wake_high) {
        int r = s_afe->set_wakenet_threshold(s_afe_d, 1, WAKE_TH_HIGH);
        ESP_LOGI(TAG, "唤醒阈值 → %.2f: %s", WAKE_TH_HIGH, r == 1 ? "成功" : "失败（用模型自带的）");
    }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) { ESP_LOGE(TAG, "没有中文命令词模型"); return false; }
    s_mn = esp_mn_handle_from_name(mn_name);
    int mn_timeout = s_cfg.listen_ms;
    if (s_cfg.follow_up_ms > mn_timeout) mn_timeout = s_cfg.follow_up_ms;
    if (mn_timeout < 6000) mn_timeout = 6000;
    s_mn_d = s_mn->create(mn_name, mn_timeout);
    if (!s_mn_d) { ESP_LOGE(TAG, "MultiNet 创建失败"); return false; }

    esp_mn_commands_alloc(s_mn, s_mn_d);          /* 命令词表绑定到这个模型实例 */
    memset(s_syl_hash, 0xff, sizeof(s_syl_hash));
    int rejected = register_fixed();
    if (s_cfg.mn_threshold > 0.0f) s_mn->set_det_threshold(s_mn_d, s_cfg.mn_threshold);
    ESP_LOGI(TAG, "命令词模型: %s  唤醒模式=%s  阈值=%s%.2f  等命令 %d ms  连续对话 %d ms  固定命令 %d 条（被拒 %d）",
             mn_name, s_cfg.wake_high ? "灵敏" : "常规",
             s_cfg.mn_threshold > 0.0f ? "" : "默认/", s_cfg.mn_threshold,
             s_cfg.listen_ms, s_cfg.follow_up_ms, s_phrase_n, rejected);

    xTaskCreatePinnedToCore(feed_task,   "sr_feed",   4 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8 * 1024, NULL, 5, NULL, 1);
    return true;
}
