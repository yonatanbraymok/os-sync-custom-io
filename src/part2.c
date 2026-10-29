/* ============================================================================
 * PART 2 — Threads + mutex (synchronize shared memory)
 * ============================================================================
 *
 * TASK:
 *   Same buffered writes to output.txt, but workers are pthreads in ONE process.
 *   All threads share the same buffer and buffer_pos → must use a mutex around
 *   every touch of that buffer. Output must be perfectly clean (every line valid).
 *
 * RUN:  cd src && make part2
 *       ./part2 "<message>" <num_workers> <writes_per_worker>
 *
 * VS PART 1:
 *   - pthread instead of fork; Type is THREAD not PROCESS.
 *   - Race was on file offset only → now also on buffer bytes / buffer_pos in RAM.
 *   - Mutex fixes memory; threads still share one file offset but writes are ordered.
 *
 * VS PART 3:
 *   - Mutex works because threads share address space.
 *   - Parent flushes once after all joins (threads do not flush individually).
 *
 * RULES:
 *   - Lock covers entire my_write (copy + full flush), never unlock mid-operation.
 *   - No fork, no sem_*; exactly one mutex.
 * ============================================================================ */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

#define CHUNK_SIZE 64

/* SHARED by all threads — this is why we need buf_mtx. */
char buffer[CHUNK_SIZE];
int  buffer_pos = 0;

pthread_mutex_t buf_mtx = PTHREAD_MUTEX_INITIALIZER;

static void write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w == -1) {
            if (errno == EINTR)
                continue;
            perror("write");
            exit(1);
        }
        off += (size_t)w;
    }
}

/* Called from main after pthread_join — no thread is still in my_write. */
static void flush_buffer(void)
{
    if (pthread_mutex_lock(&buf_mtx) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }
    if (buffer_pos > 0) {
        write_all(STDOUT_FILENO, buffer, (size_t)buffer_pos);
        buffer_pos = 0;
    }
    if (pthread_mutex_unlock(&buf_mtx) != 0) {
        perror("pthread_mutex_unlock");
        exit(1);
    }
}

/* Critical section: one thread at a time owns buffer until unlock. */
static void my_write(const char *str)
{
    if (pthread_mutex_lock(&buf_mtx) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }

    size_t len = strlen(str);
    size_t i = 0;
    while (i < len) {
        int space = CHUNK_SIZE - buffer_pos;
        size_t chunk = len - i;
        if (chunk > (size_t)space)
            chunk = (size_t)space;

        memcpy(buffer + buffer_pos, str + i, chunk);
        buffer_pos += (int)chunk;
        i += chunk;

        if (buffer_pos == CHUNK_SIZE) {
            write_all(STDOUT_FILENO, buffer, CHUNK_SIZE);
            buffer_pos = 0;
        }
    }

    if (pthread_mutex_unlock(&buf_mtx) != 0) {
        perror("pthread_mutex_unlock");
        exit(1);
    }
}

struct worker_arg {
    int id;
    const char *user_msg;
    int writes_per_worker;
};

/* Thread entry point: pthread_create passes void* to this function. */
static void *worker(void *p)
{
    struct worker_arg *a = (struct worker_arg *)p;

    for (int seq = 0; seq < a->writes_per_worker; seq++) {
        char line[256];
        int n = snprintf(line, sizeof line,
                         "[Type: THREAD] [ID: %d] [Seq: %03d] %s\n",
                         a->id, seq, a->user_msg);
        if (n < 0 || (size_t)n >= sizeof line) {
            fprintf(stderr, "snprintf failed in worker %d\n", a->id);
            return NULL;
        }
        my_write(line);
    }
    /* No flush here — shared buffer drained once in main after join. */
    return NULL;
}

static double tdiff_ms(struct timeval a, struct timeval b)
{
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <message> <num_workers> <writes_per_worker>\n", argv[0]);
        return 1;
    }
    const char *message = argv[1];
    int num_workers = atoi(argv[2]);
    int writes_per_worker = atoi(argv[3]);
    if (num_workers <= 0 || writes_per_worker <= 0) {
        fprintf(stderr, "num_workers and writes_per_worker must be positive integers\n");
        return 1;
    }

    int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("open");
        exit(1);
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        perror("dup2");
        exit(1);
    }
    if (close(fd) == -1) {
        perror("close");
        exit(1);
    }

    struct timeval start, end;
    if (gettimeofday(&start, NULL) == -1) {
        perror("gettimeofday");
        exit(1);
    }

    pthread_t threads[num_workers];
    struct worker_arg args[num_workers];

    for (int i = 0; i < num_workers; i++) {
        args[i].id = i;
        args[i].user_msg = message;
        args[i].writes_per_worker = writes_per_worker;
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    for (int i = 0; i < num_workers; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("pthread_join");
            exit(1);
        }
    }

    flush_buffer();   /* tail bytes left after last thread's my_write */

    if (gettimeofday(&end, NULL) == -1) {
        perror("gettimeofday");
        exit(1);
    }
    fprintf(stderr, "elapsed_ms=%.3f\n", tdiff_ms(start, end));

    if (pthread_mutex_destroy(&buf_mtx) != 0) {
        perror("pthread_mutex_destroy");
        exit(1);
    }

    return 0;
}
