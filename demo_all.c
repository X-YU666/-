/* demo_all.c — 单进程演示：直接用 CausalBuffer + Indexer 跑完整流程 */
#include <stdio.h>
#include <string.h>
#include "vector_clock.h"
#include "indexer.h"

int main(void) {
    printf("========================================\n");
    printf("  向量时钟分布式日志聚合系统 C 语言版\n");
    printf("========================================\n\n");
    setbuf(stdout, NULL);

    CausalBuffer buf; buf_init(&buf);
    InvertedIndex idx; idx_init(&idx);

    /* 模拟 12 条消息乱序到达（B收到A心跳 依赖 A的第3条消息） */
    struct { const char *node; const char *msg; const char *cks; } msgs[] = {
        {"A", "服务器A启动",        "{\"A\":1}"},
        {"A", "处理用户请求",       "{\"A\":2}"},
        {"A", "发送心跳到B和C",     "{\"A\":3}"},
        {"B", "服务器B启动",        "{\"B\":1}"},
        {"B", "数据库连接超时",     "{\"B\":2}"},
        {"B", "收到A的心跳",        "{\"A\":3,\"B\":3}"},
        {"C", "处理用户请求",       "{\"C\":1}"},
        {"A", "收到B和C的回复",     "{\"A\":4}"},
        {"B", "报告状态正常",       "{\"A\":3,\"B\":4}"},
        {"C", "收到A和B的状态",     "{\"A\":3,\"B\":4,\"C\":2}"},
        {"B", "开始数据同步",       "{\"A\":3,\"B\":5}"},
        {"C", "汇总所有节点状态",   "{\"A\":3,\"B\":4,\"C\":3}"},
    };

    int delivered_total = 0;
    for (int i = 0; i < 12; i++) {
        LogEntry e; memset(&e, 0, sizeof(e));
        strcpy(e.node_id, msgs[i].node);
        strcpy(e.message, msgs[i].msg);
        vc_from_json(&e.vector_clock, msgs[i].cks);
        e.sequence = i;

        LogEntry d[MAX_ENTRIES];
        int dc = buf_add(&buf, &e, d);
        if (dc > 0) {
            for (int j = 0; j < dc; j++) {
                idx_add(&idx, &d[j]);
                printf("[交付] [%s] %-20s  vc:", d[j].node_id, d[j].message);
                vc_print(&d[j].vector_clock); printf("\n");
                delivered_total++;
            }
        } else {
            printf("[缓存] [%s] %-20s  (等待依赖)\n", e.node_id, e.message);
        }
    }

    printf("\n========================================\n");
    printf("  交付: %d/%d  缓冲: %d\n",
           delivered_total, 12, buf.buf_count);
    printf("========================================\n");

    if (buf.buf_count > 0) {
        printf("\n缓冲区中未交付的消息:\n");
        for (int i = 0; i < buf.buf_count; i++)
            printf("  [%s] %s\n", buf.buffer[i].node_id, buf.buffer[i].message);
    }

    printf("\n--- 按节点查询 ---\n");
    for (int n = 0; n < 3; n++) {
        const char *nd = n==0 ? "A" : n==1 ? "B" : "C";
        LogEntry res[MAX_NODE_LOGS];
        int nc = idx_get_by_node(&idx, nd, res);
        printf("[%s] %d 条:\n", nd, nc);
        for (int i = 0; i < nc; i++) printf("  %s\n", res[i].message);
    }

    printf("\n--- 关键词搜索 ---\n");
    const char *qs[] = {"数据库", "心跳", "请求", "状态"};
    for (int i = 0; i < 4; i++) {
        LogEntry res[MAX_ENTRIES];
        int n = idx_search(&idx, qs[i], res);
        printf("  \"%s\" → %d 条\n", qs[i], n);
    }

    printf("\n演示完成!\n");
    return 0;
}
