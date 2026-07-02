/*
 * indexer.h — 倒排索引 + 日志条目 + 因果缓冲区
 *
 * 支持：
 *   - LogEntry（日志条目）
 *   - InvertedIndex（倒排索引，中文 bigram + 英文按空格分词）
 *   - CausalBuffer（因果拓扑排序缓冲区）
 */

#ifndef INDEXER_H
#define INDEXER_H

#include "vector_clock.h"
#include <stdbool.h>

#define MAX_ENTRIES     500
#define MAX_TOKENS      100
#define MAX_NODE_LOGS   500
#define TOKEN_LEN       64
#define INDEX_BUCKETS   256

/* ---- LogEntry ---- */
typedef struct {
    char        node_id[VC_NODE_ID_LEN];
    char        message[512];
    VectorClock vector_clock;
    double      timestamp;
    int         sequence;
} LogEntry;

/* ---- 分词 ---- */
/* 将 text 分词，结果写入 tokens（最多 max_tokens 个），返回 token 数量 */
int tokenize(const char *text, char tokens[][TOKEN_LEN], int max_tokens);

/* ---- 倒排索引 ---- */
typedef struct {
    /* 简单实现：线性存储，O(n) 搜索；对教育场景足够 */
    LogEntry entries[MAX_ENTRIES];
    int      entry_count;

    /* 按节点索引 */
    int node_index_offsets[VC_MAX_NODES];  /* entry 下标起始 */
    int node_index_counts[VC_MAX_NODES];
    char node_index_ids[VC_MAX_NODES][VC_NODE_ID_LEN];
    int node_index_node_count;
} InvertedIndex;

void idx_init(InvertedIndex *idx);
void idx_add(InvertedIndex *idx, const LogEntry *entry);

/* AND 搜索：消息包含所有关键词 */
int  idx_search(const InvertedIndex *idx, const char *query,
                LogEntry results[]);

/* 按节点查询 */
int  idx_get_by_node(const InvertedIndex *idx, const char *node_id,
                     LogEntry results[]);

/* ---- 因果排序缓冲区 ---- */
typedef struct {
    LogEntry buffer[MAX_ENTRIES];
    int      buf_count;
    VectorClock max_vc;  /* 已交付消息中每节点的最高分量 */
    int      next_seq;
} CausalBuffer;

void buf_init(CausalBuffer *buf);
bool buf_can_deliver(const CausalBuffer *buf, const LogEntry *entry);
void buf_mark_delivered(CausalBuffer *buf, const LogEntry *entry);

/* 添加条目，返回实际可交付的数量，delivered 输出数组 */
int  buf_add(CausalBuffer *buf, const LogEntry *entry,
             LogEntry delivered[MAX_ENTRIES]);

#endif
