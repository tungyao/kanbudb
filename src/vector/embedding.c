#include "embedding.h"
#include "dict_data.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct kanbudb_embed {
    uint32_t dimensions;
    uint32_t ngram_size;
    float*   proj_matrix;
    uint32_t hash_buckets;
};

static uint32_t fnv1a_hash(const uint8_t* data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* UTF-8 解码：从 src 读取一个 Unicode 码点，返回消耗的字节数；返回 0 表示非法字节 */
static int utf8_decode_cp(const uint8_t* src, size_t len, uint32_t* cp)
{
    if (len == 0) return 0;
    uint8_t b0 = src[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((uint32_t)(b0 & 0x1F) << 6) | (src[1] & 0x3F);
        return 2;
    } else if ((b0 & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((uint32_t)(b0 & 0x0F) << 12)
            | ((uint32_t)(src[1] & 0x3F) << 6)
            | (src[2] & 0x3F);
        return 3;
    } else if ((b0 & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((uint32_t)(b0 & 0x07) << 18)
            | ((uint32_t)(src[1] & 0x3F) << 12)
            | ((uint32_t)(src[2] & 0x3F) << 6)
            | (src[3] & 0x3F);
        return 4;
    }
    return 0; /* 非法首字节 */
}

/* 将单个码点编码为 UTF-8，返回写入的字节数 */
static int cp_to_utf8(uint32_t cp, char* out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* 二分查找：在字典中精确匹配 word（UTF-8 null-terminated），返回 1 找到 / 0 未找到 */
static int dict_lookup(const char* word)
{
    int lo = 0, hi = dict_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const char* entry = (const char*)(dict_pool + dict_offsets[mid]);
        int cmp = strcmp(word, entry);
        if (cmp == 0) return 1;
        if (cmp < 0)  hi = mid - 1;
        else          lo = mid + 1;
    }
    return 0;
}

/* 正向最大匹配分词：将 codepoints 分詞，结果写入 seg_buf（以空格间隔），返回写入的码点数 */
static int segment_codepoints(const uint32_t* cp, int num_cp,
                               uint32_t* seg_buf, int seg_cap)
{
    int out_pos = 0;
    int i = 0;
    while (i < num_cp && out_pos < seg_cap - 8) {
        int max_len = num_cp - i;
        if (max_len > DICT_MAX_WORD_LEN) max_len = DICT_MAX_WORD_LEN;

        int found_len = 0;
        /* 从最长开始尝试匹配 */
        for (int len = max_len; len >= 1; len--) {
            /* 将前 len 个码点编码为 UTF-8 */
            char buf[64];
            char* p = buf;
            for (int j = 0; j < len; j++) {
                p += cp_to_utf8(cp[i + j], p);
            }
            *p = '\0';
            if (dict_lookup(buf)) {
                found_len = len;
                break;
            }
        }

        if (found_len == 0) found_len = 1;  /* 未匹配则输出单字 */

        /* 将找到的词写入 seg_buf，最后加空格 */
        for (int j = 0; j < found_len && out_pos < seg_cap - 1; j++) {
            seg_buf[out_pos++] = cp[i + j];
        }
        if (out_pos < seg_cap - 1) {
            seg_buf[out_pos++] = 0x20;  /* 空格 */
        }
        i += found_len;
    }
    return out_pos;
}

static void seeded_srand(uint32_t seed, float* buf, uint32_t count)
{
    uint32_t state = seed;
    for (uint32_t i = 0; i < count; i++) {
        state = state * 1103515245u + 12345u;
        int32_t raw = (int32_t)(state >> 16);
        buf[i] = ((float)(raw & 0x7FFF) / 16384.0f) - 1.0f;
    }
}

int kanbudb_embed_create(uint32_t dimensions, uint32_t ngram_size,
                         kanbudb_embed_t** out)
{
    if (!out || dimensions == 0) return -1;
    if (ngram_size == 0) ngram_size = 3;

    kanbudb_embed_t* e = calloc(1, sizeof(*e));
    if (!e) return -1;

    e->dimensions   = dimensions;
    e->ngram_size   = ngram_size;
    e->hash_buckets = dimensions * 256;

    e->proj_matrix = malloc((size_t)e->hash_buckets * dimensions * sizeof(float));
    if (!e->proj_matrix) { free(e); return -1; }

    seeded_srand(42, e->proj_matrix, e->hash_buckets * dimensions);

    *out = e;
    return 0;
}

void kanbudb_embed_destroy(kanbudb_embed_t* embed)
{
    if (!embed) return;
    free(embed->proj_matrix);
    free(embed);
}

int kanbudb_embed_text(const kanbudb_embed_t* embed,
                       const char* text, size_t text_len,
                       float* out_vector)
{
    if (!embed || !text || !out_vector) return -1;

    uint32_t dim = embed->dimensions;
    uint32_t ng  = embed->ngram_size;

    memset(out_vector, 0, (size_t)dim * sizeof(float));

    /* 将 UTF-8 字节流解码为 Unicode 码点数组（码点级 n-gram 支持中文） */
    uint32_t cp_buf[2048];
    uint32_t* codepoints = cp_buf;
    int num_cp = 0;
    size_t pos = 0;
    const uint8_t* ustr = (const uint8_t*)text;

    while (pos < text_len && num_cp < 2048) {
        uint32_t cp;
        int consumed = utf8_decode_cp(ustr + pos, text_len - pos, &cp);
        if (consumed <= 0) { pos++; continue; }  /* 跳过非法字节 */
        codepoints[num_cp++] = cp;
        pos += (size_t)consumed;
    }

    if (num_cp == 0) return 0;

    /* 中文分词：正向最大匹配，结果以空格间隔 */
    uint32_t seg_buf[4096];
    int seg_len = segment_codepoints(codepoints, num_cp, seg_buf, 4096);
    if (seg_len == 0) return 0;
    uint32_t* text_cp = seg_buf;
    int text_len_cp = seg_len;

    /* 以码点为单位滑动 n-gram 窗口 */
    if ((size_t)text_len_cp < ng) {
        uint32_t h = fnv1a_hash((const uint8_t*)text_cp,
                                (size_t)text_len_cp * sizeof(uint32_t));
        uint32_t bucket = h % embed->hash_buckets;
        for (uint32_t d = 0; d < dim; d++)
            out_vector[d] += embed->proj_matrix[(size_t)bucket * dim + d];
    } else {
        for (int i = 0; i + ng <= (uint32_t)text_len_cp; i++) {
            uint32_t h = fnv1a_hash((const uint8_t*)(text_cp + i),
                                    (size_t)ng * sizeof(uint32_t));
            uint32_t bucket = h % embed->hash_buckets;
            for (uint32_t d = 0; d < dim; d++)
                out_vector[d] += embed->proj_matrix[(size_t)bucket * dim + d];
        }
    }

    float norm = 0.0f;
    for (uint32_t d = 0; d < dim; d++)
        norm += out_vector[d] * out_vector[d];
    if (norm > 1e-30f) {
        norm = 1.0f / sqrtf(norm);
        for (uint32_t d = 0; d < dim; d++)
            out_vector[d] *= norm;
    }

    return 0;
}

int kanbudb_embed_batch(const kanbudb_embed_t* embed,
                        const char** texts, const size_t* text_lens,
                        uint32_t count, float* out_vectors)
{
    if (!embed || !texts || !text_lens || !out_vectors || count == 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (kanbudb_embed_text(embed, texts[i], text_lens[i],
                               out_vectors + (size_t)i * embed->dimensions) != 0)
            return -1;
    }
    return 0;
}

uint32_t kanbudb_embed_dimensions(const kanbudb_embed_t* embed)
{
    return embed ? embed->dimensions : 0;
}
