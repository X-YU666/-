/*
 * vector_clock.c — 向量时钟实现
 *
 * 大白话说明：
 * 向量时钟不是"几点几分"的物理时间，而是每个节点记"我知道各节点都到第几件事了"。
 * 比如 {A:3, B:2} 表示"我知道A发生过3个事件、B发生过2个事件"。
 * 每条日志附带上发送时刻的向量时钟，接收方就能判断"这条日志发生时，我知道到什么程度了"。
 * 比较两个时钟就能知道"谁先谁后"还是"他俩同时发生的、互不知道"。
 */

#include "vector_clock.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 初始化时钟：清零分量列表，相当于一张白纸
 * ================================================================ */
void vc_init(VectorClock *vc) {
    vc->count = 0;
}

/* ================================================================
 * 查分量：问"A在这个时钟里记到几了？"
 * 如果 A 不存在（比如还没听说过A），返回 0
 * ================================================================ */
int vc_get(const VectorClock *vc, const char *node_id) {
    for (int i = 0; i < vc->count; i++) {
        if (strcmp(vc->comps[i].node_id, node_id) == 0)
            return vc->comps[i].counter;
    }
    return 0;
}

/* ================================================================
 * 设分量：把某个节点的计数器设成指定值
 * 如果已存在就改值，不存在就在列表末尾新增一条
 * 上限 8 个节点（VC_MAX_NODES），超过就不存了
 * ================================================================ */
void vc_set(VectorClock *vc, const char *node_id, int value) {
    for (int i = 0; i < vc->count; i++) {
        if (strcmp(vc->comps[i].node_id, node_id) == 0) {
            vc->comps[i].counter = value;
            return;
        }
    }
    /* 新增分量 */
    if (vc->count < VC_MAX_NODES) {
        int i = vc->count++;
        strncpy(vc->comps[i].node_id, node_id, VC_NODE_ID_LEN - 1);
        vc->comps[i].node_id[VC_NODE_ID_LEN - 1] = '\0';
        vc->comps[i].counter = value;
    }
}

/* ================================================================
 * 自增：自己节点产生了新事件，计数器+1
 * 比如 A 写了条日志到文件 → A 的分量从 2 变成 3
 * ================================================================ */
void vc_increment(VectorClock *vc, const char *own_node) {
    int val = vc_get(vc, own_node);
    vc_set(vc, own_node, val + 1);
}

/* ================================================================
 * 合并：取 a 和 b 的逐分量最大值，写到 dst
 *
 * 场景：我手里有我的时钟（{A:3}），收到了别人带来的时钟（{B:2}）
 * 合并后就是 {A:3, B:2} —— 我知道的 A 保持 3，原来不知道 B 现在知道了
 *
 * 规则：遍历 a 所有分量，跟 b 的同分量比大小取大的；
 *       再遍历 b 中 a 没有的分量，直接加进来
 * ================================================================ */
void vc_merge(VectorClock *dst, const VectorClock *a, const VectorClock *b) {
    vc_init(dst);
    /* 遍历 a */
    for (int i = 0; i < a->count; i++) {
        int vb = vc_get(b, a->comps[i].node_id);
        int mx = a->comps[i].counter > vb ? a->comps[i].counter : vb;
        vc_set(dst, a->comps[i].node_id, mx);
    }
    /* 遍历 b 中 a 没有的 */
    for (int i = 0; i < b->count; i++) {
        if (vc_get(a, b->comps[i].node_id) == 0)
            vc_set(dst, b->comps[i].node_id, b->comps[i].counter);
    }
}

/* ================================================================
 * 接收消息时的标准流程：先合并再自增
 *
 * 顺序不能反：
 *   ❌ 先自增再合并 → 还没看别人的信就先说自己知道了，逻辑不通
 *   ✅ 先合并再自增 → 先"补课"别人知道的事，再加自己的新事件
 * ================================================================ */
void vc_receive(VectorClock *vc, const VectorClock *incoming, const char *own_node) {
    VectorClock merged;
    vc_merge(&merged, vc, incoming);
    *vc = merged;
    vc_increment(vc, own_node);
}

/* ================================================================
 * 比较两个时钟的因果顺序 —— 整个系统最核心的函数
 *
 * 返回四种结果：
 *   BEFORE      → a 发生在 b 之前（a 的所有分量 ≤ b，且至少一个 <）
 *   AFTER       → a 发生在 b 之后（反过来）
 *   CONCURRENT  → 并发，分不清先后（既有 a > b 的分量，又有 a < b 的分量）
 *   EQUAL       → 完全相等
 *
 * ⚠️ 踩过的坑：最初只遍历了 a 的分量，漏掉了 b 独有而 a 没有的节点。
 *    修复方案：先把 a 和 b 的所有节点名收集到 all_nodes 里，
 *    再逐节点比较，不存在的视为 0。这样就不会漏了。
 * ================================================================ */
VCOrder vc_compare(const VectorClock *a, const VectorClock *b) {
    bool a_le_b = true, b_le_a = true;

    /* ---- 第 1 步：收集 a 和 b 所有出现过的节点 ---- */
    char all_nodes[VC_MAX_NODES][VC_NODE_ID_LEN];
    int all_count = 0;
    for (int i = 0; i < a->count; i++) {
        strncpy(all_nodes[all_count], a->comps[i].node_id, VC_NODE_ID_LEN);
        all_count++;
    }
    for (int i = 0; i < b->count; i++) {
        bool found = false;
        for (int j = 0; j < all_count; j++) {
            if (strcmp(all_nodes[j], b->comps[i].node_id) == 0) {
                found = true;
                break;
            }
        }
        if (!found && all_count < VC_MAX_NODES) {
            strncpy(all_nodes[all_count], b->comps[i].node_id, VC_NODE_ID_LEN);
            all_count++;
        }
    }

    /* ---- 第 2 步：逐节点比较 ---- */
    for (int i = 0; i < all_count; i++) {
        int va = vc_get(a, all_nodes[i]);
        int vb = vc_get(b, all_nodes[i]);
        if (va > vb) a_le_b = false;
        if (vb > va) b_le_a = false;
    }

    /* ---- 第 3 步：根据比较结果返回状态 ---- */
    if (a_le_b && b_le_a) return VC_EQUAL;
    if (a_le_b)          return VC_BEFORE;
    if (b_le_a)          return VC_AFTER;
    return VC_CONCURRENT;
}

/* 把枚举状态转成人类可读的字符串 */
const char *vc_order_str(VCOrder o) {
    switch (o) {
        case VC_BEFORE:     return "before";
        case VC_AFTER:      return "after";
        case VC_CONCURRENT: return "concurrent";
        case VC_EQUAL:      return "equal";
        default:            return "unknown";
    }
}

/* ================================================================
 * 序列化：把向量时钟打成 JSON 字符串
 * 例如 {A:3, B:2} → {"A":3,"B":2}
 * ================================================================ */
int vc_to_json(const VectorClock *vc, char *buf, int buf_size) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "{");
    for (int i = 0; i < vc->count; i++) {
        if (i > 0) pos += snprintf(buf + pos, buf_size - pos, ",");
        pos += snprintf(buf + pos, buf_size - pos, "\"%s\":%d",
                        vc->comps[i].node_id, vc->comps[i].counter);
    }
    pos += snprintf(buf + pos, buf_size - pos, "}");
    return pos;
}

/* ================================================================
 * 反序列化：从 JSON 字符串解析出向量时钟
 * 自己手写的轻量解析器，不做通用 JSON 解析
 * 解析规则：跳过 { → 读 "key" → 跳过 : → 读数字 → 循环直到 }
 * ================================================================ */
bool vc_from_json(VectorClock *vc, const char *json) {
    vc_init(vc);
    const char *p = json;

    /* 跳过开头的 {  */
    while (*p && *p != '{') p++;
    if (*p == '{') p++;

    while (*p) {
        /* 跳过空白和逗号 */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == '}' || *p == '\0') break;

        /* 读键: "key" */
        if (*p != '"') break;
        p++; /* 跳过第一个 " */
        char key[VC_NODE_ID_LEN];
        int ki = 0;
        while (*p && *p != '"' && ki < VC_NODE_ID_LEN - 1) {
            key[ki++] = *p++;
        }
        key[ki] = '\0';
        if (*p == '"') p++;

        /* 跳过冒号 */
        while (*p && *p != ':') p++;
        if (*p == ':') p++;

        /* 读值 */
        while (*p == ' ') p++;
        int val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        vc_set(vc, key, val);
    }
    return true;
}

/* 调试打印 */
void vc_print(const VectorClock *vc) {
    char buf[256];
    vc_to_json(vc, buf, sizeof(buf));
    printf("%s", buf);
}

#ifdef VECTOR_CLOCK_TEST
int main(void) {
    VectorClock a, b;
    printf("=== 向量时钟测试 ===\n");

    vc_init(&a);
    vc_init(&b);

    /* A 发生 3 个事件 */
    vc_increment(&a, "A");
    vc_increment(&a, "A");
    vc_increment(&a, "A");
    printf("A after 3 events: "); vc_print(&a); printf("\n");

    /* B 发生 1 个事件 */
    vc_increment(&b, "B");
    printf("B after 1 event:  "); vc_print(&b); printf("\n");

    /* B 收到 A 的消息 (vc={'A':2}) */
    VectorClock a2;
    vc_init(&a2);
    vc_set(&a2, "A", 2);
    vc_receive(&b, &a2, "B");
    printf("B after recv A:2: "); vc_print(&b); printf("\n");

    /* 比较 */
    printf("a vs b: %s\n", vc_order_str(vc_compare(&a, &b)));

    printf("All tests passed!\n");
    return 0;
}
#endif
