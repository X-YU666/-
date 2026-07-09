/*
 * agent.c — 日志采集代理 (Linux/UDP)
 * v3: 新增日志级别解析和时间戳字段
 *
 * 改动：
 *   - 新增 parse_level()：自动提取行首 [INFO]/[WARN]/[ERROR]
 *   - JSON 扩展为 5 字段：node_id / timestamp / level / message / vector_clock
 *   - 自动监控和手动模式共享同一套增强的 JSON 输出
 *
 * 用法: ./agent <A|B|C> <日志文件> [服务器IP] [端口]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include "vector_clock.h"

#define MAX_LINE  1024
#define MAX_JSON  2048
#define MAX_LINES 100
#define DEFAULT_PORT 9876
#define DEFAULT_ADDR "127.0.0.1"

static VectorClock g_vc;
static char        g_node_id[16];
static char        g_log_path[260];
static long        g_file_offset = 0;
static int         g_sock = -1;
static struct sockaddr_in g_server_addr;
static volatile int g_running    = 1;
static volatile int g_monitoring = 1;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- 日志级别解析 --- */

static const char *parse_level(char *line, char *level_out, int level_sz) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '[') {
        p++;
        char buf[16];
        int i = 0;
        while (*p && *p != ']' && i < (int)sizeof(buf) - 1)
            buf[i++] = *p++;
        if (*p == ']' && i > 0) {
            buf[i] = '\0';
            if (strcmp(buf, "INFO") == 0 || strcmp(buf, "WARN") == 0
                || strcmp(buf, "ERROR") == 0) {
                strncpy(level_out, buf, level_sz - 1);
                level_out[level_sz - 1] = '\0';
                p++;
                while (*p == ' ') p++;
                if (*p == '[') {
                    while (*p && *p != ']') p++;
                    if (*p == ']') p++;
                }
                while (*p == ' ') p++;
                int remaining = (int)strlen(p);
                memmove(line, p, remaining + 1);
                return level_out;
            }
        }
    }
    strncpy(level_out, "INFO", level_sz - 1);
    return level_out;
}

/* --- UDP 发送（5 字段 JSON） --- */

static int send_line(const char *node, const char *level,
                     const char *msg, const VectorClock *vc) {
    char json[MAX_JSON], vj[256];
    vc_to_json(vc, vj, sizeof(vj));
    time_t ts = time(NULL);
    int n = snprintf(json, sizeof(json),
        "{\"node_id\":\"%s\",\"timestamp\":%lld,\"level\":\"%s\","
        "\"message\":\"%s\",\"vector_clock\":%s}\n",
        node, (long long)ts, level, msg, vj);

    pthread_mutex_lock(&g_send_mutex);
    ssize_t sent = sendto(g_sock, json, (size_t)n, 0,
                          (struct sockaddr *)&g_server_addr,
                          sizeof(g_server_addr));
    pthread_mutex_unlock(&g_send_mutex);
    if (sent < 0) { perror("  [错误] UDP 发送失败"); return 0; }
    return 1;
}

/* --- 增量读取日志文件 --- */

static int read_lines(const char *path, int maxl,
                      char (*lines)[MAX_LINE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);

    if (sz <= g_file_offset) {
        if (sz < g_file_offset) g_file_offset = 0;
        fclose(f); return 0;
    }

    fseek(f, g_file_offset, SEEK_SET);
    int cnt = 0;
    while (cnt < maxl && fgets(lines[cnt], MAX_LINE, f)) {
        int l = (int)strlen(lines[cnt]);
        while (l>0 && (lines[cnt][l-1]=='\n'||lines[cnt][l-1]=='\r'))
            lines[cnt][--l] = 0;
        if (l > 0) cnt++;
    }
    g_file_offset = ftell(f);
    fclose(f);
    return cnt;
}

/* --- inotify 文件监控线程 --- */

static int g_inotify_fd = -1;
static int g_inotify_wd  = -1;

static int setup_inotify_watch(const char *path) {
    if (g_inotify_wd >= 0) {
        inotify_rm_watch(g_inotify_fd, g_inotify_wd);
        g_inotify_wd = -1;
    }
    if (access(path, F_OK) == 0)
        g_inotify_wd = inotify_add_watch(g_inotify_fd, path, IN_MODIFY);
    return g_inotify_wd;
}

static void process_new_lines(void) {
    if (!g_monitoring) return;
    char lines[MAX_LINES][MAX_LINE];
    int n = read_lines(g_log_path, MAX_LINES, lines);
    for (int i = 0; i < n; i++) {
        char level_buf[16] = "INFO";
        char *raw = lines[i];
        parse_level(raw, level_buf, sizeof(level_buf));
        vc_increment(&g_vc, g_node_id);
        send_line(g_node_id, level_buf, raw, &g_vc);
        printf("\n  [自动][%s][%s] %s\n", g_node_id, level_buf, raw);
        printf("[%s] > ", g_node_id); fflush(stdout);
    }
}

static void *monitor_thread(void *arg) {
    (void)arg;
    g_inotify_fd = inotify_init();
    if (g_inotify_fd < 0) { perror("inotify_init 失败"); return NULL; }

    setup_inotify_watch(g_log_path);

    char evbuf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_inotify_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int ret = select(g_inotify_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) {
            if (g_inotify_wd < 0) setup_inotify_watch(g_log_path);
            continue;
        }

        ssize_t len = read(g_inotify_fd, evbuf, sizeof(evbuf));
        if (len <= 0) continue;

        for (char *p = evbuf; p < evbuf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->mask & (IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF)) {
                g_inotify_wd = -1;
                g_file_offset = 0;
            }
            if (ev->mask & IN_MODIFY) process_new_lines();
            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    if (g_inotify_wd >= 0) inotify_rm_watch(g_inotify_fd, g_inotify_wd);
    close(g_inotify_fd);
    return NULL;
}

/* --- 信号处理 --- */

static void sig_handler(int signo) { (void)signo; g_running = 0; }

/* --- 主函数 --- */

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    if (argc < 3) {
        fprintf(stderr, "用法: %s <A|B|C> <日志文件> [服务器IP] [端口]\n", argv[0]);
        fprintf(stderr, "默认服务器: %s:%d\n", DEFAULT_ADDR, DEFAULT_PORT);
        return 1;
    }

    strncpy(g_node_id, argv[1], sizeof(g_node_id)-1);
    g_node_id[sizeof(g_node_id)-1] = '\0';
    strncpy(g_log_path, argv[2], sizeof(g_log_path)-1);
    g_log_path[sizeof(g_log_path)-1] = '\0';
    const char *srv_addr = argc > 3 ? argv[3] : DEFAULT_ADDR;
    int         srv_port = argc > 4 ? atoi(argv[4]) : DEFAULT_PORT;
    vc_init(&g_vc);

    {
        FILE *f = fopen(g_log_path, "rb");
        if (f) { fseek(f, 0, SEEK_END); g_file_offset = ftell(f); fclose(f); }
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { perror("创建 socket 失败"); return 1; }

    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_port   = htons(srv_port);
    if (inet_pton(AF_INET, srv_addr, &g_server_addr.sin_addr) <= 0) {
        fprintf(stderr, "无效服务器地址: %s\n", srv_addr);
        close(g_sock); return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("========================================\n");
    printf(" 日志采集代理 Agent [%s] (Linux/UDP)\n", g_node_id);
    printf(" 目标: %s:%d\n", srv_addr, srv_port);
    printf(" 监控: %s（inotify 事件驱动）\n", g_log_path);
    printf("========================================\n");
    printf(" 打字→发送 | 空行→读文件 | show | exit\n\n");

    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    char buf[MAX_LINE];
    while (g_running) {
        printf("[%s] > ", g_node_id); fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        int il = (int)strlen(buf);
        while (il>0 && (buf[il-1]=='\n'||buf[il-1]=='\r')) buf[--il]=0;

        if (il == 0) {
            char lines[MAX_LINES][MAX_LINE];
            int n = read_lines(g_log_path, MAX_LINES, lines);
            for (int i = 0; i < n; i++) {
                char level_buf[16] = "INFO";
                char *raw = lines[i];
                parse_level(raw, level_buf, sizeof(level_buf));
                vc_increment(&g_vc, g_node_id);
                send_line(g_node_id, level_buf, raw, &g_vc);
                printf("  [发送][%s] %s\n", level_buf, raw);
            }
            if (!n) printf("  (无新行)\n");
        } else if (strcmp(buf, "exit") == 0) break;
        else if (strcmp(buf, "show") == 0) {
            printf("  vc: "); vc_print(&g_vc); printf("\n");
        } else {
            char level_buf[16] = "INFO";
            parse_level(buf, level_buf, sizeof(level_buf));
            vc_increment(&g_vc, g_node_id);
            send_line(g_node_id, level_buf, buf, &g_vc);
            printf("  [发送][%s] %s\n", level_buf, buf);
        }
    }

    g_running = 0;
    pthread_join(monitor, NULL);
    if (g_sock >= 0) close(g_sock);
    printf("[%s] 已退出\n", g_node_id);
    return 0;
}
