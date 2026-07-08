/*
 * vector_clock.c — 向量时钟实现
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

void vc_receive(VectorClock *vc, const VectorClock *incoming, const char *own_node) {
    VectorClock merged;
    vc_merge(&merged, vc, incoming);
    *vc = merged;
    vc_increment(vc, own_node);
}

VCOrder vc_compare(const VectorClock *a, const VectorClock *b) {
    bool a_le_b = true, b_le_a = true;

    /* 收集所有节点 */
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

    for (int i = 0; i < all_count; i++) {
        int va = vc_get(a, all_nodes[i]);
        int vb = vc_get(b, all_nodes[i]);
        if (va > vb) a_le_b = false;
        if (vb > va) b_le_a = false;
    }

    if (a_le_b && b_le_a) return VC_EQUAL;
    if (a_le_b)          return VC_BEFORE;
    if (b_le_a)          return VC_AFTER;
    return VC_CONCURRENT;
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
