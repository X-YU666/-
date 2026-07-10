/*
 * vector_clock.h — 向量时钟 (Vector Clock)
 *
 * 大白话说明：
 * 分布式系统中，不同机器的"几点几分"不可靠（时钟可能不准）。
 * 向量时钟用"我见过各节点到第几个事件"来代替物理时间。
 * 每个节点维护一个 N 元向量，自己写日志就自增自己的分量；
 * 收到别人的消息就把别人的时钟合并进来，逐分量取最大值。
 */

#ifndef VECTOR_CLOCK_H
#define VECTOR_CLOCK_H

#include <stdbool.h>

#define VC_MAX_NODES 8      /* 最多支持 8 个节点 */
#define VC_NODE_ID_LEN 16   /* 节点名字最长 16 字节 */

/* 向量时钟的单个分量：哪个节点 + 该节点到第几个事件了 */
typedef struct {
    char node_id[VC_NODE_ID_LEN];
    int  counter;
} VCComponent;

/* 向量时钟本体：由多个分量组成 */
typedef struct {
    int        count;                    /* 当前有几个有效分量 */
    VCComponent comps[VC_MAX_NODES];     /* 分量列表 */
} VectorClock;

/* ---------- 公开 API ---------- */

/* 初始化一个全零时钟（清空分量） */
void vc_init(VectorClock *vc);

/* 获取某节点的分量值，不存在则返回 0 */
int  vc_get(const VectorClock *vc, const char *node_id);

/* 设置某节点分量值（已存在就改，不存在就新增） */
void vc_set(VectorClock *vc, const char *node_id, int value);

/* 本地事件：自身分量 +1（比如 A 新写了一条日志） */
void vc_increment(VectorClock *vc, const char *own_node);

/* 合并两个时钟：逐分量取最大值，结果写入 dst */
void vc_merge(VectorClock *dst, const VectorClock *a, const VectorClock *b);

/* 接收消息：先合并别人的时钟，再自增自己的计数器 */
void vc_receive(VectorClock *vc, const VectorClock *incoming, const char *own_node);

/* 比较两个时钟的因果顺序（四种结果） */
typedef enum {
    VC_BEFORE,      /* a → b  (a 发生在 b 之前) */
    VC_AFTER,       /* b → a  (a 发生在 b 之后) */
    VC_CONCURRENT,  /* a ∥ b (并发，分不清先后) */
    VC_EQUAL        /* a = b (完全相同) */
} VCOrder;

VCOrder vc_compare(const VectorClock *a, const VectorClock *b);
const char *vc_order_str(VCOrder o);

/* 序列化为 JSON 字符串（写入 buf，返回写入字节数） */
int  vc_to_json(const VectorClock *vc, char *buf, int buf_size);

/* 从 JSON 解析（支持 {"A":1,"B":2} 格式） */
bool vc_from_json(VectorClock *vc, const char *json);

/* 调试打印 */
void vc_print(const VectorClock *vc);

#endif
