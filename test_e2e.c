/**
 * test_e2e.c — 端到端集成测试 (Linux版)
 *
 * 模拟：Server 接收来自 ABC 三个 Agent 的日志，验证因果排序和搜索
 * 用法: gcc -o test_e2e test_e2e.c vector_clock.c indexer.c && ./test_e2e
 */
#include <stdio.h>
#include <string.h>
#include "vector_clock.h"
#include "indexer.h"

static int passed = 0, failed = 0;

#define TEST(name) do { printf("  [TEST] %-40s ", name); } while(0)
#define PASS() do { printf("[OK]\n"); passed++; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); failed++; } while(0)
#define CHECK(cond) if (cond) PASS(); else FAIL(#cond)

int main(void) {
    setbuf(stdout, NULL);
    printf("\n==============================================\n");
    printf("  项目八 · C语言版 集成测试 (Linux)\n");
    printf("==============================================\n\n");

    /* ---- 1. 向量时钟 ---- */
    printf("--- 向量时钟 ---\n");
    {
        VectorClock a, b, c;
        vc_init(&a); vc_init(&b); vc_init(&c);

        vc_increment(&a, "A");
        vc_increment(&a, "A");
        vc_increment(&a, "A");
        TEST("A 3次自增 = {'A':3}");
        CHECK(vc_get(&a, "A") == 3);

        vc_increment(&b, "B");
        TEST("B 1次自增 = {'B':1}");
        CHECK(vc_get(&b, "B") == 1);

        TEST("A vs B 并发");
        CHECK(vc_compare(&a, &b) == VC_CONCURRENT);

        VectorClock a2;
        vc_init(&a2);
        vc_set(&a2, "A", 2);
        vc_receive(&b, &a2, "B");
        TEST("B 收到 A:2 后 = {'A':2,'B':2}");
        CHECK(vc_get(&b, "A") == 2 && vc_get(&b, "B") == 2);
    }

    /* ---- 2. 分词 ---- */
    printf("\n--- 分词 ---\n");
    {
        char tokens[MAX_TOKENS][TOKEN_LEN];
        int n = tokenize("数据库连接超时", tokens, MAX_TOKENS);
        TEST("'数据库连接超时' 分词");
        printf("[%d tokens: ", n);
        for (int i = 0; i < n; i++) printf("%s ", tokens[i]);
        printf("] ");
        CHECK(n >= 3);
    }

    /* ---- 3. 倒排索引 + 搜索 ---- */
    printf("\n--- 倒排索引 ---\n");
    {
        InvertedIndex idx;
        idx_init(&idx);

        LogEntry e;
        memset(&e, 0, sizeof(e));
        strcpy(e.node_id, "A");
        strcpy(e.message, "服务器启动完毕");
        idx_add(&idx, &e);

        strcpy(e.message, "数据库连接超时");
        idx_add(&idx, &e);

        strcpy(e.node_id, "B");
        strcpy(e.message, "处理用户请求");
        idx_add(&idx, &e);

        TEST("索引 3 条日志");
        CHECK(idx.entry_count == 3);

        LogEntry results[MAX_ENTRIES];
        int n = idx_search(&idx, "数据库", results);
        TEST("搜索 '数据库' → 1 条");
        CHECK(n == 1 && strstr(results[0].message, "数据库"));

        n = idx_search(&idx, "请求", results);
        TEST("搜索 '请求' → 1 条");
        CHECK(n == 1 && strstr(results[0].message, "请求"));
    }

    /* ---- 4. 因果排序缓冲区 ---- */
    printf("\n--- 因果排序 ---\n");
    {
        CausalBuffer buf;
        buf_init(&buf);
        InvertedIndex idx;
        idx_init(&idx);

        LogEntry entries[5];
        strcpy(entries[0].node_id, "A");
        strcpy(entries[0].message, "A启动");
        entries[0].sequence = 0;
        vc_init(&entries[0].vector_clock);
        vc_set(&entries[0].vector_clock, "A", 1);

        strcpy(entries[1].node_id, "A");
        strcpy(entries[1].message, "A处理请求");
        entries[1].sequence = 1;
        vc_init(&entries[1].vector_clock);
        vc_set(&entries[1].vector_clock, "A", 2);

        strcpy(entries[2].node_id, "B");
        strcpy(entries[2].message, "B启动");
        entries[2].sequence = 2;
        vc_init(&entries[2].vector_clock);
        vc_set(&entries[2].vector_clock, "B", 1);

        strcpy(entries[3].node_id, "B");
        strcpy(entries[3].message, "B收到A心跳");
        entries[3].sequence = 3;
        vc_init(&entries[3].vector_clock);
        vc_set(&entries[3].vector_clock, "A", 3);
        vc_set(&entries[3].vector_clock, "B", 2);

        strcpy(entries[4].node_id, "A");
        strcpy(entries[4].message, "A发心跳");
        entries[4].sequence = 4;
        vc_init(&entries[4].vector_clock);
        vc_set(&entries[4].vector_clock, "A", 3);

        for (int i = 0; i < 5; i++) {
            LogEntry delivered[MAX_ENTRIES];
            int dc = buf_add(&buf, &entries[i], delivered);
            for (int j = 0; j < dc; j++) {
                idx_add(&idx, &delivered[j]);
                printf("    [交付] [%s] %s\n", delivered[j].node_id, delivered[j].message);
            }
            if (dc == 0) {
                printf("    [缓存] [%s] %s\n", entries[i].node_id, entries[i].message);
            }
        }

        TEST("5 条消息全部交付");
        CHECK(idx.entry_count == 5);

        TEST("B收到A心跳 在 A发心跳 之后");
        LogEntry node_logs[MAX_NODE_LOGS];
        int nb = idx_get_by_node(&idx, "B", node_logs);
        int found = 0;
        for (int i = 0; i < nb; i++) {
            if (strcmp(node_logs[i].message, "B收到A心跳") == 0) {
                found = 1;
                break;
            }
        }
        CHECK(found == 1);
    }

    printf("\n==============================================\n");
    printf("  结果: %d 通过, %d 失败\n", passed, failed);
    printf("==============================================\n");

    return failed > 0 ? 1 : 0;
}
