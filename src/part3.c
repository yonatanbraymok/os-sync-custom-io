/* ============================================================================
 * PART 3 — Processes + System V semaphore (synchronize cross-process I/O)
 * ============================================================================
 *
 * TASK:
 *   Back to fork() like Part 1, but output must be clean. Each child has its
 *   own buffer in memory; they share only the file offset. Wrap entire my_write
 *   in a kernel semaphore so only one process at a time completes one logical
 *   write (all chunk write()s + drain) before the next process runs.
 *
 * RUN:  cd src && make part3
 *       ./part3 "<message>" <num_workers> <writes_per_worker>
 *       ipcs -s    (should show nothing after exit — parent calls IPC_RMID)
 *
 * VS PART 1:
 *   - Same fork pattern; adds SysV sem (semget/semop/semctl), not pthread mutex.
 *
 * VS PART 2:
 *   - Mutex in user memory does NOT work across fork (each child gets a copy).
 *   - Semaphore lives in kernel; g_semid inherited by children.
 *
 * DRAIN RULE (Part 3 only):
 *   Before sem_post, flush leftover bytes in THIS process's buffer.
 *   Else another process can write between your chunk writes → split message.
 *
 * CLEANUP:
 *   - Parent only: semctl(IPC_RMID). Children use _exit(0).
 *   - EEXIST on semget: remove stale sem from crashed run, retry.
 * ============================================================================ */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>

#define CHUNK_SIZE 64

/* Required on Linux for semctl(..., SETVAL, ...). */
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* Private per process after fork — not shared between children. */
char buffer[CHUNK_SIZE];
int  buffer_pos = 0;

static int g_semid = -1;       /* kernel IPC handle; same integer in parent + children */
static pid_t g_parent_pid;     /* child errors use _exit, parent uses exit */

static void write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w == -1) {
            if (errno == EINTR)
                continue;
            perror("write");
            if (getpid() != g_parent_pid)
                _exit(1);
            exit(1);
        }
        off += (size_t)w;
    }
}

/* P operation: decrement sem; block if already 0. */
static void sem_wait_op(void)
{
    struct sembuf op = {0, -1, 0};
    if (semop(g_semid, &op, 1) == -1) {
        perror("semop");
        if (getpid() != g_parent_pid)
            _exit(1);
        exit(1);
    }
}

/* V operation: increment sem; wake a waiter if any. */
static void sem_post_op(void)
{
    struct sembuf op = {0, +1, 0};
    if (semop(g_semid, &op, 1) == -1) {
        perror("semop");
        if (getpid() != g_parent_pid)
            _exit(1);
        exit(1);
    }
}

/* Internal flush — caller must already hold the semaphore. */
static void flush_buffer_locked(void)
{
    if (buffer_pos > 0) {
        write_all(STDOUT_FILENO, buffer, (size_t)buffer_pos);
        buffer_pos = 0;
    }
}

static void flush_buffer(void)
{
    sem_wait_op();
    flush_buffer_locked();
    sem_post_op();
}

/*
 * One logical message = hold sem for full copy loop + drain leftovers.
 * Do not release sem with bytes still in buffer (Part 3 requirement).
 */
static void my_write(const char *str)
{
    sem_wait_op();

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

    flush_buffer_locked();   /* drain before post — prevents cross-process fragmentation */

    sem_post_op();
}

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
    flush_buffer();   /* defensive; my_write already drained each time */
}

static double tdiff_ms(struct timeval a, struct timeval b)
{
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}

static int create_semaphore(key_t key)
{
    int semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644);
    if (semid == -1 && errno == EEXIST) {
        semid = semget(key, 1, 0);
        if (semid == -1) {
            perror("semget");
            exit(1);
        }
        if (semctl(semid, 0, IPC_RMID) == -1) {
            perror("semctl");
            exit(1);
        }
        semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644);
    }
    if (semid == -1) {
        perror("semget");
        exit(1);
    }
    return semid;
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

    g_parent_pid = getpid();

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

    key_t key = ftok(argv[0], 'A');
    if (key == -1) {
        perror("ftok");
        exit(1);
    }
    g_semid = create_semaphore(key);

    union semun arg;
    arg.val = 1;   /* binary semaphore (mutex-like) */
    if (semctl(g_semid, 0, SETVAL, arg) == -1) {
        perror("semctl");
        exit(1);
    }

    struct timeval start, end;
    if (gettimeofday(&start, NULL) == -1) {
        perror("gettimeofday");
        exit(1);
    }

    pid_t pids[num_workers];
    for (int i = 0; i < num_workers; i++) {
        pids[i] = fork();
        if (pids[i] == -1) {
            perror("fork");
            exit(1);
        }
        if (pids[i] == 0) {
            worker(i, message, writes_per_worker);
            _exit(0);
        }
    }

    for (int i = 0; i < num_workers; i++) {
        if (waitpid(pids[i], NULL, 0) == -1) {
            perror("waitpid");
            exit(1);
        }
    }

    flush_buffer();

    if (gettimeofday(&end, NULL) == -1) {
        perror("gettimeofday");
        exit(1);
    }
    fprintf(stderr, "elapsed_ms=%.3f\n", tdiff_ms(start, end));

    if (semctl(g_semid, 0, IPC_RMID) == -1) {
        perror("semctl");
        exit(1);
    }

    return 0;
}
