/*
 * indexer.c — 倒排索引 + 分词 + 因果缓冲区实现
 */
#include "indexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============ 分词 ============ */
int tokenize(const char *text, char tokens[][TOKEN_LEN], int max_tokens) {
    int count = 0;
    const char *p = text;

    while (*p && count < max_tokens) {
        /* 跳过空白和标点 */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
               || *p == ',' || *p == '.' || *p == '!' || *p == '?') p++;
        if (!*p) break;

        /* 英文字母 → 连续收集 */
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            int ti = 0;
            while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                    || (*p >= '0' && *p <= '9')) && ti < TOKEN_LEN - 1) {
                tokens[count][ti++] = *p++;
            }
            tokens[count][ti] = '\0';
            count++;
        }
        /* 中文字符 → 单字切分 (简洁方案，确保召回率) */
        else if ((unsigned char)*p >= 0x80) {
            int len = 3;
            if (((unsigned char)p[0] & 0xE0) == 0xE0) len = 3;
            else if (((unsigned char)p[0] & 0xC0) == 0xC0) len = 2;
            else len = 1;
            if (len < TOKEN_LEN && count < max_tokens) {
                memcpy(tokens[count], p, len);
                tokens[count][len] = '\0';
                count++;
            }
            p += len;
        }
        /* 数字 */
        else if (*p >= '0' && *p <= '9') {
            int ti = 0;
            while (*p >= '0' && *p <= '9' && ti < TOKEN_LEN - 1)
                tokens[count][ti++] = *p++;
            tokens[count][ti] = '\0';
            count++;
        } else {
            p++;
        }
    }
    return count;
}

/* ============ 倒排索引 ============ */
static int idx_find_node(InvertedIndex *idx, const char *node_id) {
    for (int i = 0; i < idx->node_index_node_count; i++) {
        if (strcmp(idx->node_index_ids[i], node_id) == 0)
            return i;
    }
    return -1;
}

static int idx_add_node(InvertedIndex *idx, const char *node_id) {
    if (idx->node_index_node_count >= VC_MAX_NODES) return -1;
    int i = idx->node_index_node_count++;
    strncpy(idx->node_index_ids[i], node_id, VC_NODE_ID_LEN - 1);
    idx->node_index_ids[i][VC_NODE_ID_LEN - 1] = '\0';
    idx->node_index_offsets[i] = idx->entry_count;
    idx->node_index_counts[i] = 0;
    return i;
}

void idx_init(InvertedIndex *idx) {
    memset(idx, 0, sizeof(*idx));
}

void idx_add(InvertedIndex *idx, const LogEntry *entry) {
    if (idx->entry_count >= MAX_ENTRIES) return;
    idx->entries[idx->entry_count] = *entry;

    /* 更新节点索引 */
    int ni = idx_find_node(idx, entry->node_id);
    if (ni < 0) ni = idx_add_node(idx, entry->node_id);
    if (ni >= 0) idx->node_index_counts[ni]++;

    idx->entry_count++;
}

int idx_search(const InvertedIndex *idx, const char *query,
               LogEntry results[]) {
    /* 对 query 分词 */
    char q_tokens[MAX_TOKENS][TOKEN_LEN];
    int q_count = tokenize(query, q_tokens, MAX_TOKENS);
    if (q_count == 0) return 0;

    int res_count = 0;

    /* 遍历所有条目，检查是否包含所有 query token */
    for (int ei = 0; ei < idx->entry_count && res_count < MAX_ENTRIES; ei++) {
        /* 对 message 分词 */
        char msg_tokens[MAX_TOKENS][TOKEN_LEN];
        int m_count = tokenize(idx->entries[ei].message, msg_tokens, MAX_TOKENS);

        bool all_found = true;
        for (int qi = 0; qi < q_count; qi++) {
            bool found = false;
            for (int mi = 0; mi < m_count; mi++) {
                if (strcmp(q_tokens[qi], msg_tokens[mi]) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) { all_found = false; break; }
        }

        if (all_found) {
            results[res_count++] = idx->entries[ei];
        }
    }
    return res_count;
}

int idx_get_by_node(const InvertedIndex *idx, const char *node_id,
                    LogEntry results[]) {
    int ni = idx_find_node((InvertedIndex *)idx, node_id);
    if (ni < 0) return 0;

    /* 简单扫描全部条目（节点索引只记了计数） */
    int count = 0;
    for (int ei = 0; ei < idx->entry_count && count < MAX_NODE_LOGS; ei++) {
        if (strcmp(idx->entries[ei].node_id, node_id) == 0) {
            results[count++] = idx->entries[ei];
        }
    }
    return count;
}

/* ============ 搜索增强 ============ */

int idx_search_time(const InvertedIndex *idx, const char *query,
                    LogEntry results[], double from, double to) {
    LogEntry tmp[MAX_ENTRIES];
    int n = idx_search(idx, query, tmp);
    int out = 0;
    for (int i = 0; i < n && out < MAX_ENTRIES; i++) {
        if (from > 0 && tmp[i].timestamp < from) continue;
        if (to   > 0 && tmp[i].timestamp > to)   continue;
        results[out++] = tmp[i];
    }
    return out;
}

int idx_search_or(const InvertedIndex *idx, const char *query,
                  LogEntry results[]) {
    char q_tokens[MAX_TOKENS][TOKEN_LEN];
    int q_count = tokenize(query, q_tokens, MAX_TOKENS);
    if (q_count == 0) return 0;

    /* 对每个条目，匹配任一 token */
    int res_count = 0;
    for (int ei = 0; ei < idx->entry_count && res_count < MAX_ENTRIES; ei++) {
        char msg_tokens[MAX_TOKENS][TOKEN_LEN];
        int m_count = tokenize(idx->entries[ei].message, msg_tokens, MAX_TOKENS);

        bool any_found = false;
        for (int qi = 0; qi < q_count; qi++) {
            for (int mi = 0; mi < m_count; mi++) {
                if (strcmp(q_tokens[qi], msg_tokens[mi]) == 0) {
                    any_found = true;
                    break;
                }
            }
            if (any_found) break;
        }
        if (any_found) results[res_count++] = idx->entries[ei];
    }
    return res_count;
}

static int cmp_by_time_desc(const void *a, const void *b) {
    double ta = ((const LogEntry *)a)->timestamp;
    double tb = ((const LogEntry *)b)->timestamp;
    if (ta < tb) return 1;
    if (ta > tb) return -1;
    return 0;
}

void idx_sort_by_time(LogEntry results[], int count) {
    qsort(results, count, sizeof(LogEntry), cmp_by_time_desc);
}

/* ============ 因果排序缓冲区 ============ */
void buf_init(CausalBuffer *buf) {
    memset(buf, 0, sizeof(*buf));
    vc_init(&buf->max_vc);
}

bool buf_can_deliver(const CausalBuffer *buf, const LogEntry *entry) {
    /* 对于消息中每个其他节点 n，已交付的 n 分量 >= 消息中 n 分量 */
    for (int i = 0; i < entry->vector_clock.count; i++) {
        const char *n = entry->vector_clock.comps[i].node_id;
        int ts = entry->vector_clock.comps[i].counter;
        if (strcmp(n, entry->node_id) == 0) continue;
        if (ts > vc_get(&buf->max_vc, n))
            return false;
    }
    return true;
}

void buf_mark_delivered(CausalBuffer *buf, const LogEntry *entry) {
    for (int i = 0; i < entry->vector_clock.count; i++) {
        int cur = vc_get(&buf->max_vc, entry->vector_clock.comps[i].node_id);
        if (entry->vector_clock.comps[i].counter > cur)
            vc_set(&buf->max_vc, entry->vector_clock.comps[i].node_id,
                   entry->vector_clock.comps[i].counter);
    }
}

int buf_add(CausalBuffer *buf, const LogEntry *entry,
            LogEntry delivered[MAX_ENTRIES]) {
    if (buf->buf_count >= MAX_ENTRIES) return 0;

    /* 加入缓冲区 */
    buf->buffer[buf->buf_count++] = *entry;
    int delivered_count = 0;

    /* 拓扑排序：反复扫描 */
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < buf->buf_count; i++) {
            if (buf->buffer[i].sequence == -1) continue; /* 已交付标记 */
            if (buf_can_deliver(buf, &buf->buffer[i])) {
                /* 交付 */
                delivered[delivered_count++] = buf->buffer[i];
                buf_mark_delivered(buf, &buf->buffer[i]);
                buf->buffer[i].sequence = -1; /* 标记为已交付 */
                progress = true;
            }
        }
    }

    /* 压缩缓冲区（仅在有交付时才清理已交付条目） */
    if (delivered_count > 0) {
        int write = 0;
        for (int i = 0; i < buf->buf_count; i++) {
            if (buf->buffer[i].sequence != -1)
                buf->buffer[write++] = buf->buffer[i];
        }
        buf->buf_count = write;
    }
    return delivered_count;
}
