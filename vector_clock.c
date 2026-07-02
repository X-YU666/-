/*
 * vector_clock.c — 向量时钟核心操作 (v1)
 *
 * 实现最基本的向量时钟操作：
 *   vc_init   - 初始化
 *   vc_get    - 获取分量值
 *   vc_set    - 设置分量值
 *   vc_increment - 自增自身分量
 *
 * 后续迭代将逐步加入 merge/receive/compare/JSON 等功能。
 */
#include "vector_clock.h"
#include <stdio.h>
#include <string.h>

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
    /* 新增分量 */
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
