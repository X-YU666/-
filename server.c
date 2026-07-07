/*
 * server.c v9 — 新增：--from/--to + --or 组合过滤
 * OR 搜索结果再按时间范围过滤
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>
#include <stdbool.h>
#include "vector_clock.h"
#include "indexer.h"
#define MAX_BUF 8192
#define PIPE_BUF 4096
static InvertedIndex g_idx; static CausalBuffer g_buf; static volatile LONG g_running = 1; static int g_error_count = 0;
static FILE *g_pf = NULL; static const char *PERSIST_PATH = "logs.jsonl";
static void li(const char*f,...){va_list a;va_start(a,f);time_t t=time(NULL);struct tm*tm=localtime(&t);printf("[%02d:%02d:%02d] ",tm->tm_hour,tm->tm_min,tm->tm_sec);vprintf(f,a);printf("\n");fflush(stdout);va_end(a);}
static void le(const char*f,...){va_list a;va_start(a,f);time_t t=time(NULL);struct tm*tm=localtime(&t);printf("[ERR %02d:%02d:%02d] ",tm->tm_hour,tm->tm_min,tm->tm_sec);vprintf(f,a);printf("\n");fflush(stdout);g_error_count++;va_end(a);}
static BOOL WINAPI sh(DWORD s){if(s==CTRL_C_EVENT||s==CTRL_BREAK_EVENT){li("收到关闭信号");InterlockedExchange(&g_running,0);return TRUE;}return FALSE;}
static int vpn(const char*n){if(strncmp(n,"\\\\.\\pipe\\",9)!=0){le("管道名格式错误");return 0;}if(strlen(n)>256){le("管道名过长");return 0;}return 1;}
static int js(const char*s,const char*k,char*o,int n){char p[128];snprintf(p,sizeof(p),"\"%s\":\"",k);s=strstr(s,p);if(!s)return 0;s+=strlen(p);int i=0;while(*s&&*s!='"'&&i<n-1){if(*s=='\\'&&s[1]){s++;switch(*s){case'n':o[i++]='\n';break;case't':o[i++]='\t';break;default:o[i++]=*s;break;}}else{o[i++]=*s;}s++;}o[i]=0;return i;}
static int jvc(const char*s,VectorClock*v){s=strstr(s,"\"vector_clock\":");if(!s)return 0;return vc_from_json(v,s+15)?1:0;}
static long long ji(const char*s,const char*k){char p[128];snprintf(p,sizeof(p),"\"%s\":",k);s=strstr(s,p);if(!s)return -1;s+=strlen(p);return strtoll(s,NULL,10);}
static double pts(const char*s){int y,m,d,hh=0,mm=0,ss=0;int n=sscanf(s,"%d-%d-%d %d:%d:%d",&y,&m,&d,&hh,&mm,&ss);if(n>=3){struct tm tm={0};tm.tm_year=y-1900;tm.tm_mon=m-1;tm.tm_mday=d;tm.tm_hour=hh;tm.tm_min=mm;tm.tm_sec=ss;return (double)mktime(&tm);}return atof(s);}
static void je(const char*s,char*d,int n){int i=0;while(*s&&i<n-1){switch(*s){case'"':if(i<n-2){d[i++]='\\';d[i++]='"';}break;case'\\':if(i<n-2){d[i++]='\\';d[i++]='\\';}break;case'\n':if(i<n-2){d[i++]='\\';d[i++]='n';}break;case'\r':if(i<n-2){d[i++]='\\';d[i++]='r';}break;case'\t':if(i<n-2){d[i++]='\\';d[i++]='t';}break;default:d[i++]=*src;}src++;}d[i]='\0';}
static void se(const LogEntry*e){if(!g_pf)return;char v[256];vc_to_json(&e->vector_clock,v,sizeof(v));char m[1024];je(e->message,m,sizeof(m));fprintf(g_pf,"{\"node_id\":\"%s\",\"timestamp\":%.0f,\"level\":\"%s\",\"message\":\"%s\",\"vector_clock\":%s}\n",e->node_id,e->timestamp,e->level,m,v);fflush(g_pf);}
static int pjl(const char*l,LogEntry*e){memset(e,0,sizeof(*e));if(!js(l,"node_id",e->node_id,sizeof(e->node_id)))return 0;if(!js(l,"message",e->message,sizeof(e->message)))return 0;if(!jvc(l,&e->vector_clock))return 0;e->timestamp=(double)ji(l,"timestamp");if(e->timestamp<0)e->timestamp=0;if(!js(l,"level",e->level,sizeof(e->level)))strcpy(e->level,"INFO");return 1;}
static void lp(void){FILE*f=fopen(PERSIST_PATH,"r");if(!f){li("日志文件不存在，全新启动");return;}char l[PIPE_BUF];int ld=0,er=0;while(fgets(l,sizeof(l),f)){int len=(int)strlen(l);while(len>0&&(l[len-1]=='\n'||l[len-1]=='\r'))l[--len]=0;if(len==0)continue;LogEntry e;if(pjl(l,&e)){idx_add(&g_idx,&e);ld++;}else er++;}fclose(f);li("加载持久化: %d 条 (错误 %d)",ld,er);}
static void hl(const char*line){LogEntry e;if(!pjl(line,&e))return;li("收到 [%s][%s] %s",e.node_id,e.level,e.message);printf("       ts: %.0f  vc: ",e.timestamp);vc_print(&e.vector_clock);printf("\n");LogEntry d[MAX_ENTRIES];int dc=buf_add(&g_buf,&e,d);for(int i=0;i<dc;i++){idx_add(&g_idx,&d[i]);se(&d[i]);li("交付 [%s] %s",d[i].node_id,d[i].message);}if(dc==0)li("缓存 [%s] %s",e.node_id,e.message);if(g_buf.buf_count>=MAX_ENTRIES-5)li("缓冲接近上限: %d/%d",g_buf.buf_count,MAX_ENTRIES);printf("[server] > ");fflush(stdout);}
typedef struct{HANDLE pipe;}ClientCtx;
static DWORD WINAPI pt(LPVOID a){ClientCtx*ctx=(ClientCtx*)a;char l[PIPE_BUF];int lp=0;while(g_running){DWORD n;char c[1024];if(!ReadFile(ctx->pipe,c,sizeof(c),&n,NULL)||n==0){le("Agent 断开");break;}for(DWORD i=0;i<n&&lp<(int)sizeof(l)-1;i++){if(c[i]=='\n'){if(lp>0){l[lp]=0;hl(l);lp=0;}}else if(c[i]!='\r'){l[lp++]=c[i];}}}DisconnectNamedPipe(ctx->pipe);CloseHandle(ctx->pipe);free(ctx);return 0;}
static void ln(const char*id){int cnt=0;for(int i=0;i<g_idx.entry_count;i++){if(strcmp(g_idx.entries[i].node_id,id)==0){printf("  [%s] %s\n",g_idx.entries[i].level,g_idx.entries[i].message);cnt++;}}if(cnt==0)printf("  (无节点 %s 的日志)\n",id);}
static void cmd(const char*c){
    if(strcmp(c,"exit")==0){li("exit 命令");InterlockedExchange(&g_running,0);return;}
    if(strcmp(c,"help")==0){printf("  search <词>               — 全文检索（AND）\n  search --or 词1 词2        — OR 匹配\n  search <词> --node A      — 只看节点 A\n  search <词> --from <时间> --to <时间>\n  search --or 词1 词2 --from <时间> --to <时间>  — OR+时间\n  list / list A / count / errors / exit\n");return;}
    if(strcmp(c,"errors")==0){printf("  错误次数: %d\n",g_error_count);return;}
    if(strcmp(c,"count")==0){printf("  已交付: %d  缓冲: %d/%d  错误: %d\n",g_idx.entry_count,g_buf.buf_count,MAX_ENTRIES,g_error_count);return;}
    if(strcmp(c,"list")==0){if(g_idx.entry_count==0){printf("  (暂无)\n");return;}for(int i=0;i<g_idx.entry_count;i++)printf("  [%s][%s] %s\n",g_idx.entries[i].node_id,g_idx.entries[i].level,g_idx.entries[i].message);return;}
    if(strncmp(c,"list ",5)==0){ln(c+5);return;}
    if(strncmp(c,"search ",7)==0){
        const char*r=c+7;bool use_or=false;double from=0,to=0;
        char a[512];strncpy(a,r,sizeof(a)-1);a[sizeof(a)-1]=0;
        char q[256]="",nf[32]="";char*t=strtok(a," ");
        while(t){
            if(strcmp(t,"--or")==0){use_or=true;t=strtok(NULL," ");}
            else if(strcmp(t,"--node")==0){t=strtok(NULL," ");if(t){strncpy(nf,t,sizeof(nf)-1);t=strtok(NULL," ");}}
            else if(strcmp(t,"--from")==0||strcmp(t,"--to")==0){int f=(t[2]=='f');t=strtok(NULL," ");if(t){char v[64];strncpy(v,t,sizeof(v)-1);t=strtok(NULL," ");if(t&&strchr(t,':')){strncat(v," ",sizeof(v)-strlen(v)-1);strncat(v,t,sizeof(v)-strlen(v)-1);t=strtok(NULL," ");}if(f)from=pts(v);else to=pts(v);}}
            else{if(q[0])strncat(q," ",sizeof(q)-strlen(q)-1);strncat(q,t,sizeof(q)-strlen(q)-1);t=strtok(NULL," ");}
        }
        if(!q[0]){printf("  用法: search [--or] [--node <节点>] [--from <时间>] [--to <时间>] <关键词>\n");return;}
        LogEntry r[MAX_ENTRIES];int n;
        if(use_or){n=idx_search_or(&g_idx,q,r);if(from>0||to>0){int wn=0;for(int i=0;i<n;i++){if((from<=0||r[i].timestamp>=from)&&(to<=0||r[i].timestamp<=to))r[wn++]=r[i];}n=wn;}}
        else n=idx_search_time(&g_idx,q,r,from,to);
        idx_sort_by_time(r,n);
        if(nf[0]){int wn=0;for(int i=0;i<n;i++){if(strcmp(r[i].node_id,nf)==0)r[wn++]=r[i];}n=wn;}
        printf("  \"%s\"",q);if(from>0)printf(" from:%.0f",from);if(to>0)printf(" to:%.0f",to);if(nf[0])printf(" node:%s",nf);if(use_or)printf(" [OR]");printf(" = %d 条:\n",n);
        for(int i=0;i<n;i++)printf("    [%s][%s] %s (ts:%.0f)\n",r[i].node_id,r[i].level,r[i].message,r[i].timestamp);
        return;
    }
    printf("  未知命令 (help 查看)\n");
}
static int rcl(char*b,int m){HANDLE h=GetStdHandle(STD_INPUT_HANDLE);int p=0;DWORD mo,om;GetConsoleMode(h,&om);mo=om;mo&=~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT);mo|=ENABLE_PROCESSED_INPUT;SetConsoleMode(h,mo);while(p<m-1){INPUT_RECORD rec;DWORD n;if(!ReadConsoleInputA(h,&rec,1,&n)||n==0)continue;if(rec.EventType!=KEY_EVENT||!rec.Event.KeyEvent.bKeyDown)continue;char ch=rec.Event.KeyEvent.uChar.AsciiChar;if(ch=='\r'||ch=='\n'){printf("\n");break;}if(ch=='\b'){if(p>0){p--;printf("\b \b");}continue;}if(ch>=' '){b[p++]=ch;printf("%c",ch);}}b[p]=0;SetConsoleMode(h,om);return p;}
int main(int argc,char*argv[]){setbuf(stdout,NULL);SetConsoleOutputCP(65001);SetConsoleCtrlHandler(sh,TRUE);
    const char*pn=argc>1?argv[1]:"\\\\.\\pipe\\vc_log_agg";if(argc>1&&!vpn(argv[1]))return 1;
    idx_init(&g_idx);buf_init(&g_buf);li("索引初始化完成");
    g_pf=fopen(PERSIST_PATH,"a");if(!g_pf)le("无法打开持久化文件");else li("持久化文件: %s",PERSIST_PATH);lp();
    printf("+------------------------------------------+\n| 向量时钟分布式日志聚合服务器 v9           |\n| OR+时间组合                               |\n+------------------------------------------+\n");li("管道: %s",pn);printf("等待 Agent 连接...\n\n[server] > ");fflush(stdout);
    HANDLE h=CreateNamedPipeA(pn,PIPE_ACCESS_INBOUND,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,PIPE_UNLIMITED_INSTANCES,PIPE_BUF,PIPE_BUF,0,NULL);
    if(h==INVALID_HANDLE_VALUE){le("创建管道失败");return 1;}
    OVERLAPPED ov;memset(&ov,0,sizeof(ov));ov.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);if(!ov.hEvent){le("创建事件失败");return 1;}
    BOOL cp=ConnectNamedPipe(h,&ov);if(cp||GetLastError()==ERROR_PIPE_CONNECTED)SetEvent(ov.hEvent);else if(GetLastError()!=ERROR_IO_PENDING){le("连接失败");return 1;}
    while(g_running){
        HANDLE w[2]={ov.hEvent,GetStdHandle(STD_INPUT_HANDLE)};DWORD wr=WaitForMultipleObjects(2,w,FALSE,200);
        if(wr==WAIT_OBJECT_0){li("Agent 连接");ClientCtx*ctx=malloc(sizeof(ClientCtx));ctx->pipe=h;CreateThread(NULL,0,pt,ctx,0,NULL);h=CreateNamedPipeA(pn,PIPE_ACCESS_INBOUND,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,PIPE_UNLIMITED_INSTANCES,PIPE_BUF,PIPE_BUF,0,NULL);if(h==INVALID_HANDLE_VALUE)break;CloseHandle(ov.hEvent);ov.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);cp=ConnectNamedPipe(h,&ov);if(cp||GetLastError()==ERROR_PIPE_CONNECTED)SetEvent(ov.hEvent);else if(GetLastError()!=ERROR_IO_PENDING)break;continue;}
        HANDLE hi=GetStdHandle(STD_INPUT_HANDLE);DWORD a;if(GetNumberOfConsoleInputEvents(hi,&a)&&a>0){char inp[512];int len=rcl(inp,sizeof(inp));if(len==0){printf("[server] > ");continue;}cmd(inp);if(!g_running)break;printf("[server] > ");}
    }
    if(g_pf)fclose(g_pf);li("关闭完毕: 交付 %d 条, 残留 %d 条, 错误 %d 次",g_idx.entry_count,g_buf.buf_count,g_error_count);CloseHandle(ov.hEvent);CloseHandle(h);return 0;
}
