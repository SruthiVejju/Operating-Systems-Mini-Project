#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include "monitor_ioctl.h"

/* ── tunables ────────────────────────────────────────────── */
#define MAX_CTRS   16
#define STACK_SZ   (1 << 20)
#define SOCK_PATH  "/tmp/engine.sock"
#define LOG_DIR    "/tmp/container_logs"
#define LOGBUF_N   64
#define LOGBUF_SZ  1024
#define MON_DEV    "/dev/container_monitor"

/* ── container state ─────────────────────────────────────── */
typedef enum { CS_STARTING, CS_RUNNING, CS_STOPPED, CS_KILLED } CState;
static const char *cs_name[] = { "starting", "running", "stopped", "killed" };

typedef struct {
    char      name[64];
    pid_t     host_pid;
    time_t    start_ts;
    CState    state;
    int       soft_mb, hard_mb;
    char      logfile[256];
    int       exit_code;   /* -1 = still alive */
    int       pipe_rfd;    /* read-end for container output */
    pthread_t prod_tid;
    int       prod_started;
    int       active;
} Ctr;

/* ── bounded buffer ──────────────────────────────────────── */
typedef struct {
    int  cid;
    char data[LOGBUF_SZ];
    int  len;
} LogSlot;

typedef struct {
    LogSlot         s[LOGBUF_N];
    int             head, tail, cnt, done;
    pthread_mutex_t mu;
    pthread_cond_t  nf, ne;
} LogBuf;

/* ── globals ─────────────────────────────────────────────── */
static Ctr             ctrs[MAX_CTRS];
static pthread_mutex_t ctrs_mu = PTHREAD_MUTEX_INITIALIZER;
static LogBuf          lb;
static pthread_t       cons_tid;
static pthread_t       sig_tid;
static pthread_t       sock_tid;
static int             mon_fd = -1;
static volatile sig_atomic_t alive = 1;
static sigset_t       sigchld_set;

/* ── small helpers ───────────────────────────────────────── */
static void safe_copy(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    strncpy(dst, src ? src : "", dstsz - 1);
    dst[dstsz - 1] = '\0';
}

static int any_active_children(void) {
    int found = 0;
    pthread_mutex_lock(&ctrs_mu);
    for (int i = 0; i < MAX_CTRS; i++) {
        if (ctrs[i].active) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ctrs_mu);
    return found;
}

/* ═══════════════════════════════════════════════════════════
 *  BOUNDED BUFFER
 * ═══════════════════════════════════════════════════════════ */
static void lb_init(void) {
    lb.head = lb.tail = lb.cnt = lb.done = 0;
    pthread_mutex_init(&lb.mu, NULL);
    pthread_cond_init(&lb.nf, NULL);
    pthread_cond_init(&lb.ne, NULL);
}

static void lb_put(int cid, const char *d, int len) {
    pthread_mutex_lock(&lb.mu);
    while (lb.cnt == LOGBUF_N && !lb.done)
        pthread_cond_wait(&lb.nf, &lb.mu);

    if (!lb.done) {
        LogSlot *sl = &lb.s[lb.tail];
        sl->cid = cid;
        sl->len = (len < LOGBUF_SZ) ? len : (LOGBUF_SZ - 1);
        memcpy(sl->data, d, sl->len);
        lb.tail = (lb.tail + 1) % LOGBUF_N;
        lb.cnt++;
        pthread_cond_signal(&lb.ne);
    }
    pthread_mutex_unlock(&lb.mu);
}

static void *consumer_fn(void *arg) {
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&lb.mu);
        while (lb.cnt == 0 && !lb.done)
            pthread_cond_wait(&lb.ne, &lb.mu);

        if (lb.cnt == 0 && lb.done) {
            pthread_mutex_unlock(&lb.mu);
            break;
        }

        LogSlot sl = lb.s[lb.head];
        lb.head = (lb.head + 1) % LOGBUF_N;
        lb.cnt--;
        pthread_cond_signal(&lb.nf);
        pthread_mutex_unlock(&lb.mu);

        pthread_mutex_lock(&ctrs_mu);
        char path[256] = "";
        if (sl.cid >= 0 && sl.cid < MAX_CTRS && ctrs[sl.cid].active)
            safe_copy(path, sizeof path, ctrs[sl.cid].logfile);
        pthread_mutex_unlock(&ctrs_mu);

        if (path[0]) {
            FILE *f = fopen(path, "a");
            if (f) {
                fwrite(sl.data, 1, sl.len, f);
                fclose(f);
            }
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  PRODUCER THREAD
 * ═══════════════════════════════════════════════════════════ */
typedef struct {
    int cid, rfd;
} ProdArg;

static void *producer_fn(void *arg) {
    ProdArg *pa = (ProdArg *)arg;
    char buf[LOGBUF_SZ];

    for (;;) {
        ssize_t n = read(pa->rfd, buf, sizeof buf);
        if (n > 0) {
            lb_put(pa->cid, buf, (int)n);
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }

    close(pa->rfd);
    free(pa);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  CONTAINER CHILD
 * ═══════════════════════════════════════════════════════════ */
typedef struct {
    char rootfs[256];
    char cmd[256];
    char hostname[64];
    int  wfd;
} CloneCtx;

static int container_child(void *arg) {
    CloneCtx *cx = (CloneCtx *)arg;

    dup2(cx->wfd, STDOUT_FILENO);
    dup2(cx->wfd, STDERR_FILENO);
    close(cx->wfd);

    if (sethostname(cx->hostname, strlen(cx->hostname)) < 0)
        perror("sethostname");

    if (chroot(cx->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount(proc)");

    char *av[] = { cx->cmd, NULL };
    execvp(cx->cmd, av);
    perror("execvp");
    return 1;
}

/* ═══════════════════════════════════════════════════════════
 *  REAPING CHILDREN
 * ═══════════════════════════════════════════════════════════ */
static void reap_children(void) {
    for (;;) {
        int st;
        pid_t p = waitpid(-1, &st, WNOHANG);
        if (p <= 0) break;

        pthread_t tid = 0;
        int join_needed = 0;

        pthread_mutex_lock(&ctrs_mu);
        for (int i = 0; i < MAX_CTRS; i++) {
            if (!ctrs[i].active || ctrs[i].host_pid != p)
                continue;

            ctrs[i].state = (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)
                                ? CS_KILLED : CS_STOPPED;
            ctrs[i].exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

            tid = ctrs[i].prod_tid;
            join_needed = ctrs[i].prod_started;

            ctrs[i].state = CS_STOPPED;
            ctrs[i].pipe_rfd = -1;
            ctrs[i].prod_started = 0;

            if (mon_fd >= 0) {
                struct process_info pi = { .pid = p };
                ioctl(mon_fd, IOCTL_REMOVE_PROCESS, &pi);
            }
            break;
        }
        pthread_mutex_unlock(&ctrs_mu);

        if (join_needed)
            pthread_join(tid, NULL);
    }
}

static void *sigwait_thread(void *arg) {
    (void)arg;
    for (;;) {
        int sig = 0;
        if (sigwait(&sigchld_set, &sig) != 0)
            continue;

        reap_children();

        if (!alive && !any_active_children())
            break;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  SIGNAL HANDLER
 * ═══════════════════════════════════════════════════════════ */
static void on_sigterm(int s) {
    (void)s;
    alive = 0;
}

/* ═══════════════════════════════════════════════════════════
 *  LAUNCH A CONTAINER
 * ═══════════════════════════════════════════════════════════ */
static int do_launch(const char *name, const char *rootfs, const char *cmd,
                     int soft, int hard, char *out, int olen)
{
    pthread_mutex_lock(&ctrs_mu);
    int cid = -1;
    for (int i = 0; i < MAX_CTRS; i++) {
        if (ctrs[i].active && strcmp(ctrs[i].name, name) == 0) {
            snprintf(out, olen, "ERROR: '%s' already exists\n", name);
            pthread_mutex_unlock(&ctrs_mu);
            return -1;
        }
        if (!ctrs[i].active && cid < 0)
            cid = i;
    }
    pthread_mutex_unlock(&ctrs_mu);

    if (cid < 0) {
        snprintf(out, olen, "ERROR: max containers reached\n");
        return -1;
    }

    int pfd[2];
    if (pipe(pfd) < 0) {
        snprintf(out, olen, "ERROR: pipe: %s\n", strerror(errno));
        return -1;
    }

    CloneCtx *cx = (CloneCtx *)calloc(1, sizeof *cx);
    if (!cx) {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(out, olen, "ERROR: out of memory\n");
        return -1;
    }

    safe_copy(cx->rootfs, sizeof cx->rootfs, rootfs);
    safe_copy(cx->cmd, sizeof cx->cmd, cmd);
    safe_copy(cx->hostname, sizeof cx->hostname, name);
    cx->wfd = pfd[1];

    char *stk = (char *)malloc(STACK_SZ);
    if (!stk) {
        free(cx);
        close(pfd[0]);
        close(pfd[1]);
        snprintf(out, olen, "ERROR: out of memory\n");
        return -1;
    }

    pid_t pid = clone(container_child, stk + STACK_SZ,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cx);

    free(stk);
    free(cx);
    close(pfd[1]);

    if (pid < 0) {
        close(pfd[0]);
        snprintf(out, olen, "ERROR: clone: %s\n", strerror(errno));
        return -1;
    }

    pthread_mutex_lock(&ctrs_mu);
    Ctr *c = &ctrs[cid];
    memset(c, 0, sizeof *c);
    safe_copy(c->name, sizeof c->name, name);
    c->host_pid  = pid;
    c->start_ts  = time(NULL);
    c->state     = CS_RUNNING;
    c->soft_mb   = soft;
    c->hard_mb   = hard;
    c->exit_code = -1;
    c->pipe_rfd  = pfd[0];
    c->active    = 1;
    c->prod_started = 0;
    snprintf(c->logfile, sizeof c->logfile, "%s/%s.log", LOG_DIR, name);
    pthread_mutex_unlock(&ctrs_mu);

    if (mon_fd >= 0) {
        struct process_info pi = { pid, soft, hard };
        ioctl(mon_fd, IOCTL_ADD_PROCESS, &pi);
    }

    ProdArg *pa = (ProdArg *)malloc(sizeof *pa);
    if (!pa) {
        kill(pid, SIGKILL);
        close(pfd[0]);
        pthread_mutex_lock(&ctrs_mu);
        ctrs[cid].active = 0;
        pthread_mutex_unlock(&ctrs_mu);
        snprintf(out, olen, "ERROR: out of memory\n");
        return -1;
    }

    pa->cid = cid;
    pa->rfd = pfd[0];

    if (pthread_create(&ctrs[cid].prod_tid, NULL, producer_fn, pa) != 0) {
        free(pa);
        kill(pid, SIGKILL);
        close(pfd[0]);
        pthread_mutex_lock(&ctrs_mu);
        ctrs[cid].active = 0;
        pthread_mutex_unlock(&ctrs_mu);
        snprintf(out, olen, "ERROR: could not create producer thread\n");
        return -1;
    }

    pthread_mutex_lock(&ctrs_mu);
    ctrs[cid].prod_started = 1;
    pthread_mutex_unlock(&ctrs_mu);

    snprintf(out, olen, "OK: started '%s'  host_pid=%d  log=%s/%s.log\n",
             name, pid, LOG_DIR, name);
    return cid;
}

/* ═══════════════════════════════════════════════════════════
 *  COMMAND HANDLERS
 * ═══════════════════════════════════════════════════════════ */
static void do_ps(char *out, int olen) {
    snprintf(out, olen, "%-14s %-7s %-10s %-9s %6s %6s  LOG\n",
             "NAME", "PID", "STATE", "STARTED", "SOFT", "HARD");

    pthread_mutex_lock(&ctrs_mu);
    for (int i = 0; i < MAX_CTRS; i++) {
        if (!ctrs[i].active) continue;

        char ts[20];
        struct tm tmv;
        localtime_r(&ctrs[i].start_ts, &tmv);
        strftime(ts, sizeof ts, "%H:%M:%S", &tmv);

        char ln[512];
        snprintf(ln, sizeof ln, "%-14s %-7d %-10s %-9s %5dM %5dM  %s\n",
                 ctrs[i].name, ctrs[i].host_pid, cs_name[ctrs[i].state],
                 ts, ctrs[i].soft_mb, ctrs[i].hard_mb, ctrs[i].logfile);

        strncat(out, ln, olen - (int)strlen(out) - 1);
    }
    pthread_mutex_unlock(&ctrs_mu);
}

static void do_stop(const char *name, char *out, int olen) {
    pthread_mutex_lock(&ctrs_mu);
    for (int i = 0; i < MAX_CTRS; i++) {
        if (strcmp(ctrs[i].name, name) == 0){
            kill(ctrs[i].host_pid, SIGTERM);
            pthread_mutex_unlock(&ctrs_mu);
            snprintf(out, olen, "OK: SIGTERM -> '%s'\n", name);
            return;
        }
    }
    pthread_mutex_unlock(&ctrs_mu);
    snprintf(out, olen, "ERROR: '%s' not found\n", name);
}

static void do_logs(const char *name, char *out, int olen) {
    char lf[256] = "";

    pthread_mutex_lock(&ctrs_mu);
    for (int i = 0; i < MAX_CTRS; i++) {
        if (ctrs[i].active && strcmp(ctrs[i].name, name) == 0) {
            safe_copy(lf, sizeof lf, ctrs[i].logfile);
            break;
        }
    }
    pthread_mutex_unlock(&ctrs_mu);

    if (!lf[0]) {
        snprintf(out, olen, "ERROR: '%s' not found\n", name);
        return;
    }

    FILE *f = fopen(lf, "r");
    if (!f) {
        snprintf(out, olen, "(no log yet for '%s')\n", name);
        return;
    }

    snprintf(out, olen, "=== logs: %s ===\n", name);
    char ln[256];
    while (fgets(ln, sizeof ln, f))
        strncat(out, ln, olen - (int)strlen(out) - 1);
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════
 *  SOCKET SERVER
 * ═══════════════════════════════════════════════════════════ */
static void handle_client(int cfd) {
    char raw[512] = {0};
    ssize_t n = recv(cfd, raw, sizeof raw - 1, 0);
    if (n <= 0) {
        close(cfd);
        return;
    }

    raw[strcspn(raw, "\n")] = '\0';

    char resp[8192] = {0};

    if (strcmp(raw, "PS") == 0) {
        do_ps(resp, sizeof resp);

    } else if (strncmp(raw, "STOP ", 5) == 0) {
        do_stop(raw + 5, resp, sizeof resp);

    } else if (strncmp(raw, "LOGS ", 5) == 0) {
        do_logs(raw + 5, resp, sizeof resp);

    } else if (strncmp(raw, "START ", 6) == 0 || strncmp(raw, "RUN ", 4) == 0) {
        int off = (raw[1] == 'T') ? 6 : 4;
        char nm[64], rf[256], cmd[256];
        int soft = 50, hard = 100;

        nm[0] = rf[0] = cmd[0] = '\0';
        sscanf(raw + off, "%63s %255s %255s %d %d", nm, rf, cmd, &soft, &hard);

        int cid = do_launch(nm, rf, cmd, soft, hard, resp, sizeof resp);

        if (raw[1] == 'U' && cid >= 0) {
            for (;;) {
                pthread_mutex_lock(&ctrs_mu);
                int done = (ctrs[cid].state != CS_RUNNING);
                pthread_mutex_unlock(&ctrs_mu);
                if (done) break;
                usleep(100000);
            }
            strncat(resp, "Container finished.\n", sizeof resp - strlen(resp) - 1);
        }

    } else {
        snprintf(resp, sizeof resp, "ERROR: unknown command '%s'\n", raw);
    }

    send(cfd, resp, strlen(resp), 0);
    close(cfd);
}

typedef struct {
    int cfd;
} ClientArg;

static void *client_thread(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    handle_client(ca->cfd);
    free(ca);
    return NULL;
}

static void *sock_thread(void *arg) {
    (void)arg;

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return NULL;
    }

    unlink(SOCK_PATH);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    safe_copy(sa.sun_path, sizeof sa.sun_path, SOCK_PATH);

    if (bind(sfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("bind");
        close(sfd);
        return NULL;
    }

    if (listen(sfd, 8) < 0) {
        perror("listen");
        close(sfd);
        return NULL;
    }

    int fl = fcntl(sfd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(sfd, F_SETFL, fl | O_NONBLOCK);

    while (alive) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            }
            if (errno == EINTR)
                continue;
            break;
        }

        ClientArg *ca = (ClientArg *)malloc(sizeof *ca);
        if (!ca) {
            close(cfd);
            continue;
        }

        ca->cfd = cfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, ca) == 0)
            pthread_detach(tid);
        else {
            free(ca);
            close(cfd);
        }
    }

    close(sfd);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  CLI CLIENT
 * ═══════════════════════════════════════════════════════════ */
static int cli_send(const char *msg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    safe_copy(sa.sun_path, sizeof sa.sun_path, SOCK_PATH);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "Cannot connect — is the supervisor running?\n");
        close(fd);
        return 1;
    }

    send(fd, msg, strlen(msg), 0);

    char buf[8192] = {0};
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n > 0)
        printf("%s", buf);

    close(fd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════ */
static void usage(const char *p) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>\n"
        "  %s start <name> <rootfs> <cmd> [soft_mb] [hard_mb]\n"
        "  %s run <name> <rootfs> <cmd> [soft_mb] [hard_mb]\n"
        "  %s stop <name>\n"
        "  %s ps\n"
        "  %s logs <name>\n", p, p, p, p, p, p);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "ps") == 0)
        return cli_send("PS\n");

    if (strcmp(argv[1], "stop") == 0 && argc >= 3) {
        char m[128];
        snprintf(m, sizeof m, "STOP %s\n", argv[2]);
        return cli_send(m);
    }

    if (strcmp(argv[1], "logs") == 0 && argc >= 3) {
        char m[128];
        snprintf(m, sizeof m, "LOGS %s\n", argv[2]);
        return cli_send(m);
    }

    if ((strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) && argc >= 5) {
        int soft = argc >= 6 ? atoi(argv[5]) : 50;
        int hard = argc >= 7 ? atoi(argv[6]) : 100;

        char m[512];
        snprintf(m, sizeof m, "%s %s %s %s %d %d\n",
                 (argv[1][0] == 's') ? "START" : "RUN",
                 argv[2], argv[3], argv[4], soft, hard);
        return cli_send(m);
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        printf("[supervisor] PID=%d starting\n", getpid());
        mkdir(LOG_DIR, 0755);

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = on_sigterm;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);

        sigemptyset(&sigchld_set);
        sigaddset(&sigchld_set, SIGCHLD);
        pthread_sigmask(SIG_BLOCK, &sigchld_set, NULL);

        lb_init();
        pthread_create(&cons_tid, NULL, consumer_fn, NULL);

        mon_fd = open(MON_DEV, O_RDWR);
        if (mon_fd < 0)
            fprintf(stderr, "[supervisor] kernel monitor unavailable (load module first)\n");

        pthread_create(&sig_tid, NULL, sigwait_thread, NULL);
        pthread_create(&sock_tid, NULL, sock_thread, NULL);

        printf("[supervisor] ready  socket=%s  logs=%s\n", SOCK_PATH, LOG_DIR);

        while (alive)
            pause();

        printf("[supervisor] shutting down...\n");

        pthread_mutex_lock(&ctrs_mu);
        for (int i = 0; i < MAX_CTRS; i++) {
            if (ctrs[i].active)
                kill(ctrs[i].host_pid, SIGTERM);
        }
        pthread_mutex_unlock(&ctrs_mu);

        while (any_active_children())
            usleep(100000);

        pthread_kill(sig_tid, SIGCHLD);
        pthread_join(sig_tid, NULL);
        pthread_join(sock_tid, NULL);

        pthread_mutex_lock(&lb.mu);
        lb.done = 1;
        pthread_cond_broadcast(&lb.ne);
        pthread_cond_broadcast(&lb.nf);
        pthread_mutex_unlock(&lb.mu);
        pthread_join(cons_tid, NULL);

        if (mon_fd >= 0)
            close(mon_fd);

        unlink(SOCK_PATH);
        printf("[supervisor] clean exit.\n");
        return 0;
    }

    usage(argv[0]);
    return 1;
}
