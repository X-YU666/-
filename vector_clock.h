/*
 * vector_clock.h — 向量时钟 (Vector Clock)
 *
 * 分布式系统中追踪事件因果关系的逻辑时钟。
 * 每个节点维护 N 元向量，本地事件自增自身分量，
 * 接收消息时逐分量取最大值合并。
 */

#ifndef VECTOR_CLOCK_H
#define VECTOR_CLOCK_H

#include <stdbool.h>

#define VC_MAX_NODES 8
#define VC_NODE_ID_LEN 16

/* 向量时钟单个分量 */
typedef struct {
    char node_id[VC_NODE_ID_LEN];
    int  counter;
} VCComponent;

/* 向量时钟 */
typedef struct {
    int        count;                    /* 当前有效分量数 */
    VCComponent comps[VC_MAX_NODES];     /* 分量列表 */
} VectorClock;

/* ---------- 公开 API ---------- */

/* 初始化一个全零时钟 */
void vc_init(VectorClock *vc);

/* 获取某节点的分量值，不存在则返回 0 */
int  vc_get(const VectorClock *vc, const char *node_id);

/* 设置某节点分量值 */
void vc_set(VectorClock *vc, const char *node_id, int value);

/* 本地事件：自身分量 +1 */
void vc_increment(VectorClock *vc, const char *own_node);

/* 逐分量取最大值合并 a 和 b，结果写入 dst */
void vc_merge(VectorClock *dst, const VectorClock *a, const VectorClock *b);

/* 接收消息：合并收到的时钟后再自增 */
void vc_receive(VectorClock *vc, const VectorClock *incoming, const char *own_node);

/* 比较两个时钟的因果顺序 */
typedef enum {
    VC_BEFORE,      /* a → b  (a happened-before b) */
    VC_AFTER,       /* b → a */
    VC_CONCURRENT,  /* a ∥ b (并发) */
    VC_EQUAL        /* a = b */
} VCOrder;

VCOrder vc_compare(const VectorClock *a, const VectorClock *b);
const char *vc_order_str(VCOrder o);

/* 序列化为 JSON 字符串（写入 buf，返回写入字节数） */
int  vc_to_json(const VectorClock *vc, char *buf, int buf_size);

/* 从 JSON 解析（简单解析器，支持 {"A":1,"B":2} 格式） */
bool vc_from_json(VectorClock *vc, const char *json);

/* 调试打印 */
void vc_print(const VectorClock *vc);

#endif
