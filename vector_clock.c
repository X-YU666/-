/*
 * vector_clock.c — 向量时钟 v2：加入分布式通信与因果序 (v2)
 *
 * v1: init / get / set / increment
 * v2: + merge / receive / compare / order_str
 *
 * merge: 逐分量取最大值，合并两个向量时钟
 * receive: merge 之后自增自身分量（happened-before 的关键）
 * compare: 判断两个时钟的因果关系（before/after/concurrent/equal）
 */
#include "vector_clock.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

void vc_init(VectorClock *vc) {
    vc->count = 0;
}

int vc_get(const VectorClock *vc, const char *node_id) {
    for (int i = 0; i < vc->count; i++) {
        if (strcmp(vc->comps[i].node_id, node_id) == 0)
            return vc->comps[i].counter;
    }
    return 0;
}

void vc_set(VectorClock *vc, const char *node_id, int value) {
    for (int i = 0; i < vc->count; i++) {
        if (strcmp(vc->comps[i].node_id, node_id) == 0) {
            vc->comps[i].counter = value;
            return;
        }
    }
    if (vc->count < VC_MAX_NODES) {
        int i = vc->count++;
        strncpy(vc->comps[i].node_id, node_id, VC_NODE_ID_LEN - 1);
        vc->comps[i].node_id[VC_NODE_ID_LEN - 1] = '\0';
        vc->comps[i].counter = value;
    }
}

void vc_increment(VectorClock *vc, const char *own_node) {
    int val = vc_get(vc, own_node);
    vc_set(vc, own_node, val + 1);
}

/* ======== v2 新增 ======== */

void vc_merge(VectorClock *dst, const VectorClock *a, const VectorClock *b) {
    vc_init(dst);
    /* 遍历 a，逐分量与 b 取最大值 */
    for (int i = 0; i < a->count; i++) {
        int vb = vc_get(b, a->comps[i].node_id);
        int mx = a->comps[i].counter > vb ? a->comps[i].counter : vb;
        vc_set(dst, a->comps[i].node_id, mx);
    }
    /* 补上 b 中有而 a 中没有的分量 */
    for (int i = 0; i < b->count; i++) {
        if (vc_get(a, b->comps[i].node_id) == 0)
            vc_set(dst, b->comps[i].node_id, b->comps[i].counter);
    }
}


const char *vc_order_str(VCOrder o) {
    switch (o) {
        case VC_BEFORE:     return "before";
        case VC_AFTER:      return "after";
        case VC_CONCURRENT: return "concurrent";
        case VC_EQUAL:      return "equal";
        default:            return "unknown";
    }
}
