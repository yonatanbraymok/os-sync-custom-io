/* ============================================================================
 * PART 1 — Naive processes (no synchronization)
 * ============================================================================
 *
 * TASK:
 *   Several child processes write formatted lines to output.txt at the same
 *   time, with NO locks. Show that the OS does not keep lines intact when
 *   many writers share one file offset.
 *
 * RUN:  cd src && make part1
 *       ./part1 "<message>" <num_workers> <writes_per_worker>
 *       cat output.txt
 *
 * WHAT YOU SHOULD SEE:
 *   - Total bytes always correct (sanity check counts them).
 *   - Lines may be garbled OR look clean — both are OK; races are timing-based.
 *   - If output looks clean, scheduler often ran one child for many writes first.
 *
 * KEY IDEAS:
 *   - dup2: stdout (fd 1) → output.txt before fork.
 *   - fork: each child gets its OWN buffer in RAM; SHARED file offset in kernel.
 *   - Fork all children, then waitpid all (never wait inside the fork loop).
 *   - Children use _exit(0), not exit(0).
 *   - 64-byte buffer: one line often needs 2+ write() calls → easier to interleave.
 * ============================================================================ */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>      /* fprintf, snprintf — stderr + line formatting only */
#include <stdlib.h>     /* exit, atoi */
#include <string.h>     /* strlen, memcpy */
#include <unistd.h>     /* write, fork, _exit, close, dup2 */
#include <fcntl.h>      /* open */
#include <sys/wait.h>   /* waitpid */
#include <sys/time.h>   /* gettimeofday */
#include <errno.h>      /* errno, EINTR */

#define CHUNK_SIZE 64   /* ~50-byte lines → flushes often split a line (on purpose) */

/* Per-process after fork: each child has its own copy (not shared). */
char buffer[CHUNK_SIZE];
int  buffer_pos = 0;

/* --- Reliable write(): loop until all bytes sent (handles short write + EINTR) --- */
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

/* Partial flush: write 0..buffer_pos bytes (end of worker or parent cleanup). */
static void flush_buffer(void)
{
    if (buffer_pos == 0)
        return;
    write_all(STDOUT_FILENO, buffer, (size_t)buffer_pos);
    buffer_pos = 0;
}

/*
 * Batch string into buffer; syscall only when buffer is full (64 bytes).
 * No lock → safe only because each process has its own buffer; file offset races remain.
 */
static void my_write(const char *str)
{
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
}

/* Child work: format lines with [Type: PROCESS], push through my_write. */
static void worker(int id, const char *user_msg, int writes_per_worker)
{
    for (int seq = 0; seq < writes_per_worker; seq++) {
        char line[256];
        int n = snprintf(line, sizeof line,
                         "[Type: PROCESS] [ID: %d] [Seq: %03d] %s\n",
                         id, seq, user_msg);
        if (n < 0 || (size_t)n >= sizeof line) {
            fprintf(stderr, "snprintf failed in worker %d\n", id);
            _exit(1);
        }
        my_write(line);
    }
    flush_buffer();   /* don't lose tail bytes in this child's buffer */
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

    /* Redirect fd 1 to output.txt (all later write(1,...) go to the file). */
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

    /*
     * Create ALL children first (concurrent). waitpid here would serialize them
     * and hide the race — forbidden pattern for this part.
     */
    pid_t pids[num_workers];
    for (int i = 0; i < num_workers; i++) {
        pids[i] = fork();
        if (pids[i] == -1) {
            perror("fork");
            exit(1);
        }
        if (pids[i] == 0) {
            worker(i, message, writes_per_worker);
            _exit(0);   /* not exit(): avoids stdio flushing our manual buffer */
        }
    }

    for (int i = 0; i < num_workers; i++) {
        if (waitpid(pids[i], NULL, 0) == -1) {
            perror("waitpid");
            exit(1);
        }
    }

    flush_buffer();   /* parent: usually empty; harmless */

    if (gettimeofday(&end, NULL) == -1) {
        perror("gettimeofday");
        exit(1);
    }
    fprintf(stderr, "elapsed_ms=%.3f\n", tdiff_ms(start, end));

    return 0;
}
