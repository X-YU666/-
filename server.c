/*
 * server.c — 聚合服务器 (Linux/UDP)
 * 用法: ./server [端口]
 * 默认端口: 9876
 *
 * 与 Windows 版 (Named Pipe) 主要差异：
 *   - UDP 套接字 (recvfrom)
 *   - pthread 线程
 *   - POSIX 标准 I/O 和信号
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#include "vector_clock.h"
#include "indexer.h"

#define MAX_PKT   8192
#define MAX_BUF   8192
#define PIPE_BUF  4096
#define DEFAULT_PORT 9876

static InvertedIndex g_idx;
static CausalBuffer  g_buf;
static volatile int  g_running = 1;
static int           g_error_count = 0;
static int           g_sock = -1;

/* ========== 时间戳日志 ========== */

static void log_info(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    printf("[%02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    vprintf(fmt, args); printf("\n"); fflush(stdout);
    va_end(args);
}

static void log_error(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    printf("[ERR %02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    vprintf(fmt, args); printf("\n"); fflush(stdout);
    g_error_count++;
    va_end(args);
}

/* ========== JSON 解析 ========== */

static int json_str(const char *s, const char *key, char *out, int n) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    s = strstr(s, pat);
    if (!s) return 0;
    s += strlen(pat);
    int i = 0;
    while (*s && *s != '"' && i < n - 1) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *s;   break;
            }
        } else {
            out[i++] = *s;
        }
        s++;
    }
    out[i] = 0;
    return i;
}

static int json_vc(const char *s, VectorClock *v) {
    s = strstr(s, "\"vector_clock\":");
    if (!s) return 0;
    return vc_from_json(v, s + 15) ? 1 : 0;
}

static long long json_int(const char *s, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    s = strstr(s, pat);
    if (!s) return -1;
    s += strlen(pat);
    return strtoll(s, NULL, 10);
}

/* 解析时间戳：Unix 数字 | YYYY-MM-DD | YYYY-MM-DD HH:MM:SS */
static double parse_ts(const char *s) {
    int y, m, d, hh = 0, mm = 0, ss = 0;
    int n = sscanf(s, "%d-%d-%d %d:%d:%d", &y, &m, &d, &hh, &mm, &ss);
    if (n >= 3) {
        struct tm tm = {0};
        tm.tm_year = y - 1900;
        tm.tm_mon  = m - 1;
        tm.tm_mday = d;
        tm.tm_hour = hh;
        tm.tm_min  = mm;
        tm.tm_sec  = ss;
        time_t t = mktime(&tm);
        return (double)t;
    }
    return atof(s);
}

/* ========== 持久化 ========== */

static FILE *g_pf = NULL;
static const char *PERSIST_PATH = "logs.jsonl";

static void json_escape(const char *src, char *dst, int n) {
    int i = 0;
    while (*src && i < n - 1) {
        switch (*src) {
            case '"':  if (i < n - 2) { dst[i++] = '\\'; dst[i++] = '"'; } break;
            case '\\': if (i < n - 2) { dst[i++] = '\\'; dst[i++] = '\\'; } break;
            case '\n': if (i < n - 2) { dst[i++] = '\\'; dst[i++] = 'n'; } break;
            case '\r': if (i < n - 2) { dst[i++] = '\\'; dst[i++] = 'r'; } break;
            case '\t': if (i < n - 2) { dst[i++] = '\\'; dst[i++] = 't'; } break;
            default:   dst[i++] = *src; break;
        }
        src++;
    }
    dst[i] = '\0';
}

static void save_entry(const LogEntry *e) {
    if (!g_pf) return;
    char vc_json[256];
    vc_to_json(&e->vector_clock, vc_json, sizeof(vc_json));
    char msg_esc[1024];
    json_escape(e->message, msg_esc, sizeof(msg_esc));
    fprintf(g_pf, "{\"node_id\":\"%s\",\"timestamp\":%.0f,\"level\":\"%s\","
                  "\"message\":\"%s\",\"vector_clock\":%s}\n",
            e->node_id, e->timestamp, e->level, msg_esc, vc_json);
    fflush(g_pf);
}

static int parse_jsonl_line(const char *line, LogEntry *e) {
    memset(e, 0, sizeof(*e));
    if (!json_str(line, "node_id", e->node_id, sizeof(e->node_id))) {
        log_error("缺少 node_id");
        return 0;
    }
    if (!json_str(line, "message", e->message, sizeof(e->message))) {
        log_error("缺少 message");
        return 0;
    }
    if (!json_vc(line, &e->vector_clock)) {
        log_error("缺少 vector_clock");
        return 0;
    }
    e->timestamp = (double)json_int(line, "timestamp");
    if (e->timestamp < 0) e->timestamp = 0;
    if (!json_str(line, "level", e->level, sizeof(e->level)))
        strcpy(e->level, "INFO");
    return 1;
}

static void load_persisted(void) {
    FILE *f = fopen(PERSIST_PATH, "r");
    if (!f) {
        log_info("日志文件 %s 不存在，全新启动", PERSIST_PATH);
        return;
    }
    char line[PIPE_BUF];
    int loaded = 0, errors = 0;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = 0;
        if (len == 0) continue;
        LogEntry e;
        if (parse_jsonl_line(line, &e)) {
            idx_add(&g_idx, &e);
            loaded++;
        } else {
            errors++;
        }
    }
    fclose(f);
    log_info("加载持久化: %d 条 (错误 %d)", loaded, errors);
}

/* ========== 处理消息 ========== */

static void handle_line(const char *line) {
    LogEntry e;
    if (!parse_jsonl_line(line, &e))
        return;

    log_info("收到 [%s][%s] %s", e.node_id, e.level, e.message);
    printf("       ts: %.0f  vc: ", e.timestamp);
    vc_print(&e.vector_clock); printf("\n");

    LogEntry d[MAX_ENTRIES];
    int dc = buf_add(&g_buf, &e, d);
    for (int i = 0; i < dc; i++) {
        idx_add(&g_idx, &d[i]);
        save_entry(&d[i]);
        log_info("交付 [%s] %s", d[i].node_id, d[i].message);
    }
    if (dc == 0) {
        log_info("缓存 [%s] %s", e.node_id, e.message);
    }

    if (g_buf.buf_count >= MAX_ENTRIES - 5) {
        log_info("缓冲接近上限: %d/%d", g_buf.buf_count, MAX_ENTRIES);
    }
}

/* ========== UDP 接收线程 ========== */

static void *udp_recv_thread(void *arg) {
    (void)arg;
    char pkt[MAX_PKT];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (g_running) {
        int n = (int)recvfrom(g_sock, pkt, sizeof(pkt) - 1, 0,
                              (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            if (g_running) perror("recvfrom 错误");
            break;
        }
        pkt[n] = '\0';

        while (n > 0 && (pkt[n-1] == '\n' || pkt[n-1] == '\r'))
            pkt[--n] = 0;
        if (n == 0) continue;

        handle_line(pkt);
    }
    return NULL;
}

/* ========== 命令处理 ========== */

static void list_node(const char *node_id) {
    int cnt = 0;
    for (int i = 0; i < g_idx.entry_count; i++) {
        if (strcmp(g_idx.entries[i].node_id, node_id) == 0) {
            printf("  %s\n", g_idx.entries[i].message);
            cnt++;
        }
    }
    if (cnt == 0) printf("  (无节点 %s 的日志)\n", node_id);
}

static void cmd(const char *c) {
    if (strcmp(c, "exit") == 0) {
        log_info("exit 命令");
        g_running = 0;
        return;
    }
    if (strcmp(c, "help") == 0) {
        printf("  search <词>               — 全文检索\n");
        printf("  search <词> --node A      — 只看节点 A\n");
        printf("  search <词> --from 2026-07-01 --to 2026-07-06\n");
        printf("                           — 限定日期范围\n");
        printf("  search <词> --from 2026-07-06 14:00:00 --to 2026-07-06 15:30:00\n");
        printf("                           — 精确到分钟\n");
        printf("  search --or 超时 失败     — OR 匹配\n");
        printf("  list                      — 列出全部\n");
        printf("  list A                    — 列出节点 A\n");
        printf("  count                     — 统计\n");
        printf("  errors                    — 错误计数\n");
        printf("  exit                      — 关闭\n");
        return;
    }
    if (strcmp(c, "errors") == 0) {
        printf("  错误次数: %d\n", g_error_count);
        return;
    }
    if (strcmp(c, "count") == 0) {
        printf("  已交付: %d  缓冲: %d/%d  错误: %d\n",
               g_idx.entry_count, g_buf.buf_count,
               MAX_ENTRIES, g_error_count);
        return;
    }
    if (strcmp(c, "list") == 0) {
        if (g_idx.entry_count == 0) { printf("  (暂无)\n"); return; }
        for (int i = 0; i < g_idx.entry_count; i++)
            printf("  [%s][%s] %s\n", g_idx.entries[i].node_id,
                   g_idx.entries[i].level, g_idx.entries[i].message);
        return;
    }
    if (strncmp(c, "list ", 5) == 0) { list_node(c + 5); return; }
    if (strncmp(c, "search ", 7) == 0) {
        const char *rest = c + 7;
        bool use_or = false;
        double from = 0, to = 0;

        char args[512];
        strncpy(args, rest, sizeof(args) - 1);
        args[sizeof(args) - 1] = 0;

        char query[256] = "";
        char node_filter[32] = "";
        char *tok = strtok(args, " ");
        while (tok) {
            if (strcmp(tok, "--or") == 0) {
                use_or = true;
                tok = strtok(NULL, " ");
            } else if (strcmp(tok, "--node") == 0) {
                tok = strtok(NULL, " ");
                if (tok) { strncpy(node_filter, tok, sizeof(node_filter)-1); tok = strtok(NULL, " "); }
            } else if (strcmp(tok, "--from") == 0 || strcmp(tok, "--to") == 0) {
                int is_from = (tok[2] == 'f');
                tok = strtok(NULL, " ");
                if (tok) {
                    char ts_val[64];
                    strncpy(ts_val, tok, sizeof(ts_val)-1);
                    tok = strtok(NULL, " ");
                    /* 如果下一个 token 含冒号 → 时间部分，合并 */
                    if (tok && strchr(tok, ':')) {
                        strncat(ts_val, " ", sizeof(ts_val)-strlen(ts_val)-1);
                        strncat(ts_val, tok, sizeof(ts_val)-strlen(ts_val)-1);
                        tok = strtok(NULL, " ");
                    }
                    if (is_from) from = parse_ts(ts_val);
                    else         to   = parse_ts(ts_val);
                }
            } else {
                if (query[0]) strncat(query, " ", sizeof(query)-strlen(query)-1);
                strncat(query, tok, sizeof(query)-strlen(query)-1);
                tok = strtok(NULL, " ");
            }
        }

        if (!query[0]) {
            printf("  用法: search [--or] [--node <节点>] [--from <时间>] [--to <时间>] <关键词>\n");
            printf("  help 查看完整示例\n");
            return;
        }

        LogEntry r[MAX_ENTRIES];
        int n;
        if (use_or) {
            n = idx_search_or(&g_idx, query, r);
            if (from > 0 || to > 0) {
                int wn = 0;
                for (int i = 0; i < n; i++) {
                    if ((from <= 0 || r[i].timestamp >= from) &&
                        (to   <= 0 || r[i].timestamp <= to))
                        r[wn++] = r[i];
                }
                n = wn;
            }
        } else {
            n = idx_search_time(&g_idx, query, r, from, to);
        }
        idx_sort_by_time(r, n);

        if (node_filter[0]) {
            int wn = 0;
            for (int i = 0; i < n; i++) {
                if (strcmp(r[i].node_id, node_filter) == 0)
                    r[wn++] = r[i];
            }
            n = wn;
        }

        printf("  \"%s\"", query);
        if (from > 0) printf(" from:%.0f", from);
        if (to > 0)   printf(" to:%.0f", to);
        if (node_filter[0]) printf(" node:%s", node_filter);
        if (use_or)   printf(" [OR]");
        printf(" = %d 条:\n", n);
        for (int i = 0; i < n; i++)
            printf("    [%s][%s] %s (ts:%.0f)\n", r[i].node_id,
                   r[i].level, r[i].message, r[i].timestamp);
        return;
    }
    printf("  未知命令 (help 查看)\n");
}

/* ==================================================================== */

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;

    idx_init(&g_idx);
    buf_init(&g_buf);

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        perror("创建 socket 失败");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("绑定端口失败");
        close(g_sock);
        return 1;
    }

    printf("+------------------------------------------+\n");
    printf("| 向量时钟分布式日志聚合服务器 (Linux/UDP)  |\n");
    printf("+------------------------------------------+\n");
    log_info("监听 UDP 端口: %d", port);

    g_pf = fopen(PERSIST_PATH, "a");
    if (!g_pf)
        log_error("无法打开持久化文件 %s", PERSIST_PATH);
    else
        log_info("持久化文件: %s", PERSIST_PATH);
    load_persisted();

    printf("命令用法:\n");
    printf("  search <词>               — 全文检索\n");
    printf("  search <词> --node A      — 只看节点 A\n");
    printf("  search <词> --from 2026-07-01 --to 2026-07-06\n");
    printf("                           — 限定日期范围\n");
    printf("  search <词> --from 2026-07-06 14:00:00 --to 2026-07-06 15:30:00\n");
    printf("                           — 精确到分钟\n");
    printf("  search --or 超时 失败     — OR 匹配\n");
    printf("  list                      — 列出全部\n");
    printf("  list A                    — 列出节点 A\n");
    printf("  count                     — 统计\n");
    printf("  errors                    — 错误计数\n");
    printf("  exit                      — 退出\n");
    printf("------------------------------------------------\n");
    printf("等待 Agent 连接...\n");

    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    pthread_t recv_thread;
    pthread_create(&recv_thread, NULL, udp_recv_thread, NULL);

    while (g_running) {
        printf("[server] > "); fflush(stdout);
        char input[512];
        if (!fgets(input, sizeof(input), stdin)) break;
        int len = (int)strlen(input);
        while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
            input[--len] = 0;
        if (len > 0) {
            const char *trimmed = input;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            if (*trimmed) cmd(trimmed);
        }
    }

    g_running = 0;
    if (g_pf) fclose(g_pf);
    log_info("关闭完毕: 交付 %d 条, 残留 %d 条, 错误 %d 次",
             g_idx.entry_count, g_buf.buf_count, g_error_count);
    close(g_sock);
    pthread_join(recv_thread, NULL);
    return 0;
}
