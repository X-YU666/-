/*
 * agent.c — 日志采集代理 (Linux/UDP)
 * v1: Named Pipe → UDP socket 通信层迁移
 *
 * 改动：只换通信层，行为不变
 *   - CreateFile/WriteFile → socket/sendto
 *   - 新增服务器 IP/端口参数
 *   - JSON 格式不变: node_id + message + vector_clock
 *   - 交互不变：空行读文件、show、exit
 *
 * 用法: ./agent <A|B|C> <日志文件> [服务器IP] [端口]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "vector_clock.h"

#define MAX_LINE  1024
#define MAX_JSON  2048
#define MAX_LINES 100
#define DEFAULT_PORT 9876
#define DEFAULT_ADDR "127.0.0.1"

static VectorClock g_vc;
static char        g_node_id[16];
static long        g_file_offset = 0;
static int         g_sock = -1;
static struct sockaddr_in g_server_addr;

/* 通过 UDP 发送一条 JSON 日志 */
static int send_line(const char *node, const char *msg,
                     const VectorClock *vc) {
    char json[MAX_JSON], vj[256];
    vc_to_json(vc, vj, sizeof(vj));
    int n = snprintf(json, sizeof(json),
        "{\"node_id\":\"%s\",\"message\":\"%s\",\"vector_clock\":%s}\n",
        node, msg, vj);

    ssize_t sent = sendto(g_sock, json, (size_t)n, 0,
                          (struct sockaddr *)&g_server_addr,
                          sizeof(g_server_addr));
    if (sent < 0) {
        perror("  [错误] UDP 发送失败");
        return 0;
    }
    return 1;
}

/* 从日志文件读取新行（跳过已读过的偏移量） */
static int read_lines(const char *path, int maxl,
                      char (*lines)[MAX_LINE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);

    /* 文件变小 → logrotate 轮转 → 重置 */
    if (sz < g_file_offset) {
        printf("  [轮转] 检测到日志文件轮转，重置读取位置\n");
        g_file_offset = 0;
    }

    if (sz <= g_file_offset) { fclose(f); return 0; }

    fseek(f, g_file_offset, SEEK_SET);
    int cnt = 0;
    while (cnt < maxl && fgets(lines[cnt], MAX_LINE, f)) {
        int l = (int)strlen(lines[cnt]);
        while (l > 0 && (lines[cnt][l-1]=='\n'||lines[cnt][l-1]=='\r'))
            lines[cnt][--l] = 0;
        if (l > 0) cnt++;
    }
    g_file_offset = ftell(f);
    fclose(f);
    return cnt;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    if (argc < 3) {
        fprintf(stderr, "用法: %s <A|B|C> <日志文件> [服务器IP] [端口]\n", argv[0]);
        fprintf(stderr, "默认服务器: %s:%d\n", DEFAULT_ADDR, DEFAULT_PORT);
        return 1;
    }

    strncpy(g_node_id, argv[1], sizeof(g_node_id)-1);
    g_node_id[sizeof(g_node_id)-1] = '\0';
    const char *logf = argv[2];
    const char *srv_addr = argc > 3 ? argv[3] : DEFAULT_ADDR;
    int         srv_port = argc > 4 ? atoi(argv[4]) : DEFAULT_PORT;
    vc_init(&g_vc);

    /* 记录初始文件偏移 */
    {
        FILE *f = fopen(logf, "rb");
        if (f) { fseek(f, 0, SEEK_END); g_file_offset = ftell(f); fclose(f); }
    }

    /* 创建 UDP 套接字 */
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { perror("创建 socket 失败"); return 1; }

    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_port   = htons(srv_port);
    if (inet_pton(AF_INET, srv_addr, &g_server_addr.sin_addr) <= 0) {
        fprintf(stderr, "无效服务器地址: %s\n", srv_addr);
        close(g_sock); return 1;
    }

    printf("========================================\n");
    printf(" 日志采集代理 Agent [%s] (Linux/UDP)\n", g_node_id);
    printf(" 目标: %s:%d\n", srv_addr, srv_port);
    printf("========================================\n");
    printf(" 打字→发送 | 空行→读文件 | show | exit\n\n");

    char buf[MAX_LINE];
    while (1) {
        printf("[%s] > ", g_node_id); fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        int il = (int)strlen(buf);
        while (il>0 && (buf[il-1]=='\n'||buf[il-1]=='\r')) buf[--il]=0;

        if (il == 0) {
            char lines[MAX_LINES][MAX_LINE];
            int n = read_lines(logf, MAX_LINES, lines);
            for (int i = 0; i < n; i++) {
                vc_increment(&g_vc, g_node_id);
                send_line(g_node_id, lines[i], &g_vc);
                printf("  [发送] %s\n", lines[i]);
            }
            if (!n) printf("  (无新行)\n");
        } else if (strcmp(buf, "exit") == 0) {
            printf("[%s] 正在退出...\n", g_node_id);
            break;
        } else if (strcmp(buf, "show") == 0) {
            printf("  vc: "); vc_print(&g_vc); printf("\n");
        } else {
            vc_increment(&g_vc, g_node_id);
            if (send_line(g_node_id, buf, &g_vc))
                printf("  [发送] %s\n", buf);
        }
    }

    if (g_sock >= 0) close(g_sock);
    printf("[%s] 已退出\n", g_node_id);
    return 0;
}
