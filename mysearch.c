#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <limits.h>
#include <regex.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define DEFAULT_QUEUE_CAP 8192
#define MAX_THREADS 256

typedef struct {
    char **items;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
    int closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} WorkQueue;

typedef struct {
    WorkQueue *queue;
    const char *line_pattern;
} WorkerArgs;

static pthread_mutex_t g_print_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_ullong g_matches = 0;
static atomic_int g_had_error = 0;

static void mark_error(void) {
    atomic_store_explicit(&g_had_error, 1, memory_order_relaxed);
}

static void warn_errno_path(const char *what, const char *path) {
    int e = errno;
    pthread_mutex_lock(&g_print_mu);
    fprintf(stderr, "mysearch: %s: %s: %s\n", what, path, strerror(e));
    pthread_mutex_unlock(&g_print_mu);
    mark_error();
}

static void warn_msg(const char *msg) {
    pthread_mutex_lock(&g_print_mu);
    fprintf(stderr, "mysearch: %s\n", msg);
    pthread_mutex_unlock(&g_print_mu);
    mark_error();
}

static int queue_init(WorkQueue *q, size_t cap) {
    memset(q, 0, sizeof(*q));
    q->items = (char **)calloc(cap, sizeof(char *));
    if (!q->items) return -1;
    q->cap = cap;
    if (pthread_mutex_init(&q->mu, NULL) != 0) return -1;
    if (pthread_cond_init(&q->not_empty, NULL) != 0) return -1;
    if (pthread_cond_init(&q->not_full, NULL) != 0) return -1;
    return 0;
}

static void queue_destroy(WorkQueue *q) {
    if (!q) return;
    free(q->items);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static int queue_push(WorkQueue *q, char *path) {
    if (pthread_mutex_lock(&q->mu) != 0) return -1;
    while (q->count == q->cap && !q->closed) {
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    if (q->closed) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->items[q->tail] = path;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static char *queue_pop(WorkQueue *q) {
    if (pthread_mutex_lock(&q->mu) != 0) return NULL;
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->mu);
        return NULL;
    }
    char *path = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return path;
}

static void queue_close(WorkQueue *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

static char *join_path(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    int need_slash = (dl > 0 && dir[dl - 1] != '/');

    if (dl > (size_t)-1 - nl - (size_t)need_slash - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    char *out = (char *)malloc(dl + (size_t)need_slash + nl + 1);
    if (!out) return NULL;

    memcpy(out, dir, dl);
    size_t pos = dl;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, name, nl);
    out[pos + nl] = '\0';
    return out;
}

static int extension_matches(const char *path, regex_t *ext_re) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    const char *dot = strrchr(base, '.');
    if (!dot || dot[1] == '\0') return 0;

    const char *ext = dot + 1;
    return regexec(ext_re, ext, 0, NULL, 0) == 0;
}

static int compile_ext_regex(regex_t *re, const char *pattern) {
    size_t n = strlen(pattern);
    char *anchored = (char *)malloc(n + 5);  // ^( + pattern + )$ + \0
    if (!anchored) return REG_ESPACE;

    anchored[0] = '^';
    anchored[1] = '(';
    memcpy(anchored + 2, pattern, n);
    anchored[n + 2] = ')';
    anchored[n + 3] = '$';
    anchored[n + 4] = '\0';

    int rc = regcomp(re, anchored, REG_EXTENDED | REG_NOSUB);
    free(anchored);
    return rc;
}


static size_t regoff_max_size(void) {
    if (sizeof(regoff_t) <= sizeof(int)) return (size_t)INT_MAX;
    if (sizeof(regoff_t) <= sizeof(long)) return (size_t)LONG_MAX;
    return (size_t)LLONG_MAX;
}

static int regex_matches_line(regex_t *re, const char *start, size_t len) {
#ifdef REG_STARTEND
    regmatch_t m;
    if (len > regoff_max_size()) {
        return 0;
    }
    m.rm_so = 0;
    m.rm_eo = (regoff_t)len;
    return regexec(re, start, 1, &m, REG_STARTEND) == 0;
#else
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        warn_msg("out of memory while copying a long line for regex matching");
        return 0;
    }
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    int ok = (regexec(re, tmp, 0, NULL, 0) == 0);
    free(tmp);
    return ok;
#endif
}

static void print_match(const char *path, unsigned long long line_no,
                        const char *line, size_t len) {
    pthread_mutex_lock(&g_print_mu);
    printf("%s:%llu:", path, line_no);
    if (len > 0) fwrite(line, 1, len, stdout);
    putchar('\n');
    pthread_mutex_unlock(&g_print_mu);

    atomic_fetch_add_explicit(&g_matches, 1, memory_order_relaxed);
}

static void process_file(const char *path, regex_t *line_re) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        warn_errno_path("open", path);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        warn_errno_path("fstat", path);
        close(fd);
        return;
    }
    if (!S_ISREG(st.st_mode) || st.st_size == 0) {
        close(fd);
        return;
    }

    size_t size = (size_t)st.st_size;
    if ((off_t)size != st.st_size) {
        pthread_mutex_lock(&g_print_mu);
        fprintf(stderr, "mysearch: skip too-large file: %s\n", path);
        pthread_mutex_unlock(&g_print_mu);
        mark_error();
        close(fd);
        return;
    }

    char *data = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        warn_errno_path("mmap", path);
        return;
    }

#ifdef MADV_SEQUENTIAL
    (void)madvise(data, size, MADV_SEQUENTIAL);
#endif

    const char *p = data;
    const char *end = data + size;
    unsigned long long line_no = 1;

    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t len = (size_t)(line_end - p);

        // Strip CR in CRLF files.
        if (len > 0 && p[len - 1] == '\r') len--;

        if (regex_matches_line(line_re, p, len)) {
            print_match(path, line_no, p, len);
        }

        if (!nl) break;
        p = nl + 1;
        line_no++;
    }

    munmap(data, size);
}

static void *worker_main(void *argp) {
    WorkerArgs *args = (WorkerArgs *)argp;
    regex_t line_re;
    int rc = regcomp(&line_re, args->line_pattern, REG_EXTENDED);
    if (rc != 0) {
        char buf[256];
        regerror(rc, &line_re, buf, sizeof(buf));
        pthread_mutex_lock(&g_print_mu);
        fprintf(stderr, "mysearch: line regex compile failed in worker: %s\n", buf);
        pthread_mutex_unlock(&g_print_mu);
        mark_error();
        return NULL;
    }

    for (;;) {
        char *path = queue_pop(args->queue);
        if (!path) break;
        process_file(path, &line_re);
        free(path);
    }

    regfree(&line_re);
    return NULL;
}

static void walk_dir(const char *dir, regex_t *ext_re, WorkQueue *queue) {
    DIR *dp = opendir(dir);
    if (!dp) {
        warn_errno_path("opendir", dir);
        return;
    }

    errno = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char *path = join_path(dir, name);
        if (!path) {
            warn_errno_path("join_path", name);
            continue;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            warn_errno_path("lstat", path);
            free(path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            walk_dir(path, ext_re, queue);
            free(path);
        } else if (S_ISREG(st.st_mode)) {
            if (extension_matches(path, ext_re)) {
                if (queue_push(queue, path) != 0) {
                    free(path);
                    warn_msg("internal queue closed unexpectedly");
                    break;
                }
                // Ownership moved to worker queue.
            } else {
                free(path);
            }
        } else {
            free(path);
        }
        errno = 0;
    }

    if (errno != 0) warn_errno_path("readdir", dir);
    closedir(dp);
}

static int thread_count_from_env(void) {
    const char *env = getenv("MYSEARCH_THREADS");
    if (env && *env) {
        char *end = NULL;
        errno = 0;
        long n = strtol(env, &end, 10);
        if (errno == 0 && end != env && *end == '\0' && n > 0) {
            if (n > MAX_THREADS) n = MAX_THREADS;
            return (int)n;
        }
    }

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 4;
    if (ncpu > MAX_THREADS) ncpu = MAX_THREADS;
    return (int)ncpu;
}

static void print_usage(FILE *out, const char *argv0) {
    fprintf(out,
            "usage: %s <dir> <extension-regex> <line-regex>\n"
            "example: %s ./src 'py|ts|tsx' 'reward_(type|field)'\n\n"
            "Notes:\n"
            "  - extension-regex is matched against the extension without dot, exactly.\n"
            "  - regex syntax is POSIX Extended Regular Expression, not PCRE.\n"
            "  - set MYSEARCH_THREADS=N to override the worker count.\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    const char *root = argv[1];
    const char *ext_pattern = argv[2];
    const char *line_pattern = argv[3];

    regex_t ext_re;
    int rc = compile_ext_regex(&ext_re, ext_pattern);
    if (rc != 0) {
        char buf[256];
        regerror(rc, &ext_re, buf, sizeof(buf));
        fprintf(stderr, "mysearch: invalid extension regex: %s\n", buf);
        return 2;
    }

    // Validate once here so workers do not each report the same bad regex.
    regex_t tmp_line_re;
    rc = regcomp(&tmp_line_re, line_pattern, REG_EXTENDED);
    if (rc != 0) {
        char buf[256];
        regerror(rc, &tmp_line_re, buf, sizeof(buf));
        fprintf(stderr, "mysearch: invalid line regex: %s\n", buf);
        regfree(&ext_re);
        return 2;
    }
    regfree(&tmp_line_re);

    WorkQueue queue;
    if (queue_init(&queue, DEFAULT_QUEUE_CAP) != 0) {
        fprintf(stderr, "mysearch: failed to initialize queue\n");
        regfree(&ext_re);
        return 2;
    }

    int nthreads = thread_count_from_env();
    pthread_t *threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    if (!threads) {
        fprintf(stderr, "mysearch: out of memory allocating threads\n");
        queue_destroy(&queue);
        regfree(&ext_re);
        return 2;
    }

    WorkerArgs args = {.queue = &queue, .line_pattern = line_pattern};
    int started = 0;
    for (int i = 0; i < nthreads; i++) {
        int prc = pthread_create(&threads[i], NULL, worker_main, &args);
        if (prc != 0) {
            pthread_mutex_lock(&g_print_mu);
            fprintf(stderr, "mysearch: pthread_create: %s\n", strerror(prc));
            pthread_mutex_unlock(&g_print_mu);
            mark_error();
            break;
        }
        started++;
    }

    walk_dir(root, &ext_re, &queue);
    queue_close(&queue);

    for (int i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }

    unsigned long long matches = atomic_load_explicit(&g_matches, memory_order_relaxed);
    int had_error = atomic_load_explicit(&g_had_error, memory_order_relaxed);

    free(threads);
    queue_destroy(&queue);
    regfree(&ext_re);

    if (had_error) return 2;
    return matches ? 0 : 1;
}
