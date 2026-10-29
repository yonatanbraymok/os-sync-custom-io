# Operating Systems Assignment 2 — Threads Master Lecture
## Synchronization & Custom I/O: Deep Study Guide

---

# TABLE OF CONTENTS

1. [Introduction — The Foundations](#introduction)
   - Processes vs Threads
   - File Descriptors and the Kernel File Table
   - The `fork()` Memory Model
   - Race Conditions
   - Synchronization Primitives Overview
   - The Custom Buffer (shared across all parts)
   - `dup2` and I/O Redirection
   - `write_all` and Short Writes

2. [Part 1 — Naive Processes (No Synchronization)](#part-1)
3. [Part 2 — Threads + Mutex](#part-2)
4. [Part 3 — Processes + System V Semaphore](#part-3)
5. [Example Exam Questions](#exam-questions)

---

# INTRODUCTION — THE FOUNDATIONS {#introduction}

## 1. Processes vs Threads

### Process
A **process** is an independent program in execution. It has:
- Its own **virtual address space** (code, stack, heap, globals)
- Its own **file descriptor table**
- Its own **PID**

When you call `fork()`, the OS creates a **child process** that is an almost-exact copy of the parent. The child gets:
- A **copy of the parent's entire address space** (via Copy-On-Write — the OS is lazy and copies pages only when written)
- A **copy of the file descriptor table** — but it points to the **same entries** in the kernel's open file table
- The child gets a return value of `0` from `fork()`; the parent gets the child's PID

**Key truth:** After `fork()`, global variables like `buffer` and `buffer_pos` in each child are **physically separate copies in RAM**. Changing one process's `buffer` has absolutely no effect on another process's `buffer`.

### Thread
A **thread** (POSIX thread / pthread) is a unit of execution that lives **inside a process**. All threads of the same process:
- **Share the same virtual address space** — the same code, globals, heap
- **Share the same file descriptor table**
- Have their **own stack** (separate local variables)

**Key truth:** If thread A and thread B both access a global `buffer`, they are reading and writing the **exact same bytes in RAM**. This is why threads need a mutex.

### The Central Insight
```
                After fork()                After pthread_create()
                ─────────────────────────   ─────────────────────────────
buffer[]        SEPARATE COPIES             THE SAME ARRAY IN MEMORY
buffer_pos      SEPARATE COPIES             THE SAME VARIABLE IN MEMORY
file offset     SHARED (kernel object)      SHARED (kernel object)
fd table        SEPARATE COPIES pointing    SAME fd table
                to SAME kernel entries
```

This is the entire assignment. Everything else is consequence.

---

## 2. File Descriptors and the Kernel Open File Table

This is one of the most important concepts in the whole assignment.

### Three Levels of Indirection

```
Process A fd table:          Kernel Open File Table:         Disk Inode:
┌────────────────┐           ┌───────────────────────┐       ┌──────────┐
│ fd 0 → ───────┼──────────▶│ entry: offset=0, ...  │──────▶│output.txt│
│ fd 1 → ───────┼──────┐    └───────────────────────┘       │ inode    │
│ fd 2 → ...    │      │              ▲                      └──────────┘
└────────────────┘      │              │
                        │    ┌─────────┘
Process B fd table:     │    │
┌────────────────┐      │    │
│ fd 0 → ...    │      │    │
│ fd 1 → ───────┼──────┘    │ (SAME kernel entry!)
│ fd 2 → ...    │           │
└────────────────┘           │
```

**After `fork()`:** Both parent and child's `fd 1` point to the **same kernel open file table entry**. This means they share the **same file offset counter** (the cursor position). When process A writes 64 bytes and moves the offset forward by 64, process B immediately sees the new offset — even though they have separate virtual address spaces.

**After `dup2(fd, 1)`:** fd 1 in this process now points to the same kernel entry as `fd`. After `close(fd)`, fd 1 is the only handle left. Since `fork()` copies the fd table, all children also have `fd 1` pointing to this same entry.

---

## 3. The `fork()` Memory Model — What's Copied, What's Shared

```c
// In main():
int fd = open("output.txt", ...);   // kernel entry created; refcount=1
dup2(fd, STDOUT_FILENO);             // STDOUT_FILENO now points to same entry; refcount=2
close(fd);                           // refcount=1 (only fd 1 remains)

pid_t child = fork();
// NOW:
// - child fd 1 → same kernel entry (refcount=2 again)
// - child buffer[] is a COPY of parent's buffer[] (private memory)
// - child buffer_pos is a COPY of parent's buffer_pos (private memory)
// - child g_semid (Part 3) is a COPY of the integer — but the KERNEL OBJECT it
//   refers to is not copied; it's the same semaphore in the kernel
```

### Why `_exit(0)` instead of `exit(0)` in children?

`exit(0)` runs C library cleanup: it flushes `stdio` buffers (like `stdout`'s internal buffer), calls `atexit()` handlers, etc. Since our `fd 1` is redirected to `output.txt`, any stale bytes in stdio's internal buffer would get written to `output.txt`, corrupting the output. `_exit(0)` goes directly to the kernel without any cleanup. Children call `_exit(0)`; the parent (which handles final flush and timing) calls `exit(0)` or `return` from `main`.

---

## 4. Race Conditions — Why Chaos Happens

A **race condition** occurs when the result of a program depends on the interleaving of operations from concurrent threads/processes, and at least one of those operations writes to shared state.

### The File Offset Race (affects Parts 1 and 3 without semaphore)

Imagine 2 processes, each writing a 50-byte message. With `CHUNK_SIZE=64`:
- Process A's message fills the buffer to 50 bytes. The buffer is not full yet (needs 64), so no `write()` call happens yet.
- Process B now writes its first 14 bytes into its own buffer... wait, in processes they each have **their own buffer** so there's no buffer race.
- The race is purely at the file offset level:

```
Timeline:
Process A: writes 64 bytes (SYSCALL #1) → kernel moves offset by 64
                            [CONTEXT SWITCH]
Process B: writes 64 bytes (SYSCALL #2) → kernel moves offset by 64
                            [CONTEXT SWITCH]
Process A: writes remaining 6 bytes     → kernel moves offset by 6
```

This produces in the file: `[A's first 64B][B's 64B][A's last 6B]` — A's message is fragmented.

### The Buffer Race (affects Part 2 without mutex)

Threads share `buffer[]` and `buffer_pos` in RAM. Imagine two threads both calling `my_write()`:

```
Thread A reads buffer_pos = 20
Thread B reads buffer_pos = 20   ← both see 20, both think they own slot 20
Thread A writes to buffer[20]
Thread B writes to buffer[20]    ← overwrites A's byte!
Thread A increments buffer_pos → 21
Thread B increments buffer_pos → 21  ← count is now wrong
```

This is a classic **read-modify-write** race on `buffer_pos`. The mutex prevents this.

---

## 5. Synchronization Primitives

### Mutex (Mutual Exclusion Lock)
- A **mutex** is a binary lock: either locked or unlocked.
- Only ONE thread can hold it at a time. Others block until it's released.
- Lives **in user memory** (a `pthread_mutex_t` struct). This is fast but means it only works when all competitors are in the **same process** (share the same address space).
- Initialized with `PTHREAD_MUTEX_INITIALIZER` (static init) or `pthread_mutex_init()`.
- Lock: `pthread_mutex_lock()` — blocks if already held.
- Unlock: `pthread_mutex_unlock()`.
- Destroy: `pthread_mutex_destroy()` — must be called before program exits.

**Why a mutex cannot synchronize separate processes:** After `fork()`, the `pthread_mutex_t` struct in memory is copied. Each process has its own independent copy of the mutex. Locking one copy has no effect on the other. They're completely different kernel-unaware variables.

### Semaphore
A **semaphore** is a counter-based synchronization object. It has an integer value >= 0.
- **P operation (wait/down):** Atomically decrement the value. If the value is 0, block until it becomes positive.
- **V operation (signal/up/post):** Atomically increment the value, waking a blocked waiter if any.

A **binary semaphore** (initialized to 1) behaves exactly like a mutex: value 1 = unlocked, value 0 = locked.

#### System V Semaphores (used in Part 3)
- Live in the **kernel IPC table** — a completely separate structure from process memory.
- Identified by an integer **semid** (like a file descriptor but for IPC objects).
- After `fork()`, the child inherits the integer `semid`. Since the semaphore is in the kernel, **both parent and child** use `semop()` to operate on the **exact same kernel semaphore object**.
- `ftok(path, proj_id)` → derives a unique key from a file's inode + a project ID byte.
- `semget(key, nsems, flags)` → creates or opens a semaphore set; returns `semid`.
- `semctl(semid, semnum, cmd, ...)` → control operations (SETVAL to initialize, IPC_RMID to delete).
- `semop(semid, ops, nops)` → perform P or V operations.

```
Kernel IPC table:
┌──────────────────────────────────┐
│ semid=5, value=1                 │  ← created by parent before fork
└──────────────────────────────────┘
         ▲              ▲
         │              │
   Parent holds       Child holds
   integer 5          integer 5
   (copied by fork)   (both see same kernel object)
```

#### POSIX Semaphores (`sem_t`) — FORBIDDEN in this assignment
POSIX semaphores (`sem_open`, `sem_wait`, `sem_post`) are a different API, also cross-process capable but not used here. The assignment explicitly requires **System V** semaphores.

---

## 6. The Custom Buffer — Used in All Three Parts

### Why a Buffer?
The assignment forbids calling `write()` once per character or once per message. A raw `write()` syscall is expensive (it crosses the user-kernel boundary). Batching writes into a 64-byte buffer amortizes this cost.

### `CHUNK_SIZE = 64` — The Magic Number
A formatted line is approximately 50 bytes long:
```
[Type: PROCESS] [ID: 0] [Seq: 000] Hello\n  ≈ 50 bytes
```
With `CHUNK_SIZE=64`, a single line **often crosses a chunk boundary**. This means a logical write requires **two `write()` syscalls**: one when the buffer fills up to 64, and one for the remaining tail bytes. Without this, each message would fit in one `write()` call, and Part 1's race condition would often be invisible.

### Buffer Mechanics
```c
#define CHUNK_SIZE 64
char buffer[CHUNK_SIZE];   // the staging area
int  buffer_pos = 0;       // how many bytes are currently in buffer
```

**`my_write(str)`** — copies bytes from `str` into `buffer`:
1. Calculate how many bytes fit: `space = CHUNK_SIZE - buffer_pos`.
2. Copy `min(len, space)` bytes into `buffer + buffer_pos`.
3. Advance `buffer_pos`.
4. If `buffer_pos == CHUNK_SIZE` (buffer is full): call `write_all(STDOUT_FILENO, buffer, CHUNK_SIZE)` and reset `buffer_pos = 0`.
5. Repeat until all bytes of `str` are consumed.

**`flush_buffer()`** — writes remaining bytes that didn't fill a chunk:
- If `buffer_pos > 0`: write those bytes to stdout, reset `buffer_pos = 0`.
- Called at the very end (by parent after all workers finish, or by child before `_exit`).

**Critical difference between parts:**
- **Part 1:** `buffer` and `buffer_pos` are per-process (each child has its own copy). No locking needed for buffer access. Race is on file offset only.
- **Part 2:** `buffer` and `buffer_pos` are **shared** between threads. Must lock before touching them.
- **Part 3:** `buffer` and `buffer_pos` are per-process again (like Part 1). But we must flush all leftover bytes **before releasing the semaphore** to prevent fragmentation.

---

## 7. `dup2` and I/O Redirection

```c
int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
dup2(fd, STDOUT_FILENO);   // STDOUT_FILENO == 1
close(fd);
```

**Step by step:**
- `open(...)` creates a new file descriptor (e.g., fd=3) pointing to `output.txt`.
- `dup2(3, 1)` makes fd 1 (`STDOUT_FILENO`) point to the same kernel open file entry as fd 3. Now fd 1 **is** `output.txt`. If fd 1 was previously pointing to the terminal, that connection is severed.
- `close(3)` closes the original fd 3. Now only fd 1 remains pointing to `output.txt`.

**Why do this?** All subsequent `write(STDOUT_FILENO, ...)` calls automatically go to `output.txt`. When the parent `fork()`s, children inherit this fd table, so their fd 1 also points to `output.txt`. All workers write to the same file without needing to open it themselves.

**`O_TRUNC`:** Truncates (empties) the file when opening. This ensures each run starts with a clean file.

---

## 8. `write_all` and Short Writes

A `write()` syscall can return fewer bytes written than requested — this is called a **short write**. It can happen due to signals interrupting the call (`errno == EINTR`) or kernel buffer constraints.

```c
static void write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w == -1) {
            if (errno == EINTR)
                continue;   // interrupted by signal, retry
            perror("write");
            exit(1);        // real error
        }
        off += (size_t)w;   // advance by however many bytes were written
    }
}
```

This loops until ALL `n` bytes have been written. The pattern `write(fd, buf + off, n - off)` means "write the remaining unwritten bytes." This is a standard idiom in systems programming. Every `write()` in the codebase goes through this wrapper.

---

# PART 1 — NAIVE PROCESSES (NO SYNCHRONIZATION) {#part-1}

## Objective
Demonstrate the chaos of concurrent unsynchronized file writes. This part intentionally produces corrupted output to prove the race condition exists.

## Architecture Walkthrough

### 1. Argument Parsing and Validation
```c
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
```
- `atoi()` converts a string to an integer. Non-positive integers are rejected.
- All error output goes to `stderr` so it doesn't contaminate `output.txt`.

### 2. File Redirection
```c
int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
if (fd == -1) { perror("open"); exit(1); }
if (dup2(fd, STDOUT_FILENO) == -1) { perror("dup2"); exit(1); }
if (close(fd) == -1) { perror("close"); exit(1); }
```
Happens in the parent, **before** any fork. Every child inherits this redirected fd 1.

### 3. The Fork Loop — All Children Before Any Wait
```c
pid_t pids[num_workers];
for (int i = 0; i < num_workers; i++) {
    pids[i] = fork();
    if (pids[i] == -1) { perror("fork"); exit(1); }
    if (pids[i] == 0) {
        worker(i, message, writes_per_worker);
        _exit(0);
    }
}
```
**Why separate the fork loop from the wait loop?**
If you `waitpid()` inside the fork loop (immediately after each fork), the parent blocks until child `i` finishes before forking child `i+1`. This **serializes** execution — children run one at a time and never overlap. The race condition becomes invisible. The assignment explicitly forbids this.

By storing all PIDs and waiting in a second loop, ALL children run concurrently, creating genuine parallel writes.

**In the child after `fork()`:**
- `pids[i] == 0` is true only in the child.
- The child calls `worker()` and then `_exit(0)`.
- The parent's `fork()` loop continues to the next iteration (child does not loop back).

### 4. The Wait Loop — Reaping Children
```c
for (int i = 0; i < num_workers; i++) {
    if (waitpid(pids[i], NULL, 0) == -1) {
        perror("waitpid");
        exit(1);
    }
}
```
`waitpid(pid, status, options)` blocks until the specific child process with `pid` terminates. Passing `NULL` for status means we don't care about the exit code. Passing `0` for options means blocking wait. This prevents zombie processes (dead children whose exit status hasn't been collected yet).

### 5. The Worker Function
```c
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
    flush_buffer();
}
```
- `snprintf` formats the line into a local stack buffer `line[256]`. The `%03d` format zero-pads the sequence number to 3 digits.
- `n < 0` catches formatting errors; `n >= sizeof line` catches truncation (message too long).
- `my_write(line)` passes the formatted string through the 64-byte buffer.
- `flush_buffer()` at the end ensures any tail bytes (the last partial chunk) are written before the child exits.

**Local `line[256]` is stack memory — private to each process (and each thread call).** The race is not here. The race is when `my_write` calls `write(STDOUT_FILENO, ...)` and two processes have their write()s interleaved at the file offset.

### 6. The Race Condition — Illustrated

With 2 workers and `CHUNK_SIZE=64`, a 50-byte message fills the buffer to 50 bytes on the first call to `my_write`. The buffer is not full — no `write()` yet. On the second call, 14 bytes fill the buffer to 64 → `write()` is called (64 bytes). Then 36 bytes remain → they go into the buffer (not full yet). At the end, `flush_buffer()` writes those 36 bytes.

So for each message, there are potentially TWO `write()` calls. Between those two calls, a context switch can occur, and another process can issue its own `write()` calls, inserting bytes between the first and second halves of the first process's message.

**Total byte count is always correct** — no bytes are lost, just reordered/interleaved.

### 7. Timing
```c
struct timeval start, end;
gettimeofday(&start, NULL);
// ... fork loop, wait loop, flush ...
gettimeofday(&end, NULL);
fprintf(stderr, "elapsed_ms=%.3f\n", tdiff_ms(start, end));
```
`gettimeofday()` gives wall-clock time with microsecond precision. The timing goes to **stderr** so it doesn't appear in `output.txt`.

### 8. The Buffer in Part 1 — Key Facts

- `buffer` and `buffer_pos` are **global variables** declared at file scope.
- After `fork()`, each child has its **own private copy** of these globals in its own virtual address space.
- No locking is needed for the buffer itself — each process's buffer is completely private.
- The only shared state is the **file offset** inside the kernel open file table entry.

---

# PART 2 — THREADS + MUTEX {#part-2}

## Objective
Fix the race condition by switching from processes to threads and protecting the shared buffer with a mutex. Output must be perfectly clean.

## What Changes from Part 1

| Aspect | Part 1 | Part 2 |
|---|---|---|
| Worker type | `fork()` child process | `pthread_create()` thread |
| Buffer location | Private copy per process | **Shared** single buffer |
| File offset | Shared (kernel) | Shared (same process, same fd) |
| Synchronization | None | `pthread_mutex_t` around buffer |
| Worker exit | `_exit(0)` | `return NULL` (thread return) |
| Flush strategy | Each child flushes its own buffer | Parent flushes once after all joins |

## The Shared Buffer Crisis

In Part 2, `buffer[]` and `buffer_pos` are global variables, and **all threads share them**. Without a mutex, two threads can simultaneously:
1. Read `buffer_pos` (both get value 20)
2. Copy bytes into `buffer[20]` (both overwrite the same location)
3. Increment `buffer_pos` (increments to 21 twice, should be 22)

The mutex makes the entire body of `my_write` atomic from the perspective of other threads.

## The Mutex

```c
pthread_mutex_t buf_mtx = PTHREAD_MUTEX_INITIALIZER;
```

`PTHREAD_MUTEX_INITIALIZER` is a static initializer — it initializes the mutex at compile time without needing to call `pthread_mutex_init()`. This is valid for global mutexes.

### `my_write` as a Critical Section
```c
static void my_write(const char *str)
{
    if (pthread_mutex_lock(&buf_mtx) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }

    // ... copy bytes into buffer, flush when full ...

    if (pthread_mutex_unlock(&buf_mtx) != 0) {
        perror("pthread_mutex_unlock");
        exit(1);
    }
}
```

**Why lock the entire body?** You cannot release the lock in the middle of `my_write`. If you released after "buffer is full" but before calling `write_all()`, another thread could modify `buffer` between those two steps, causing the wrong bytes to be written. The critical section must be **all or nothing**: from first byte copy to last flush.

**Why NOT flush inside threads?** Because there's ONE shared buffer, threads do not need to flush individually. After all threads join, the parent flushes whatever remains. This is safe because no thread is running anymore when the parent flushes.

### Thread Creation
```c
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
```

**`pthread_create(thread, attr, start_routine, arg)`:**
- `thread` — output: handle to the new thread.
- `attr` — `NULL` means default attributes (joinable thread).
- `start_routine` — the function the thread will execute. Must have signature `void* f(void*)`.
- `arg` — the single argument passed to `start_routine` as `void*`.

**Why a `struct worker_arg`?**
`pthread_create` only allows ONE argument (a `void*`). To pass multiple values (id, message, writes_per_worker), we pack them into a struct and pass a pointer to it. Each thread gets a pointer to `args[i]`, which is a separate array entry — each thread has its own arguments. The structs are on the stack in `main()` and remain valid until `main()` returns (which is after all `pthread_join` calls).

### The Worker Function Signature
```c
static void *worker(void *p)
{
    struct worker_arg *a = (struct worker_arg *)p;
    // ...
    return NULL;
}
```

The return type must be `void*` to match what `pthread_create` expects. We return `NULL` since we don't pass anything back. The `void *p` is cast to `struct worker_arg *a` to access the arguments.

### Thread Joining
```c
for (int i = 0; i < num_workers; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
        perror("pthread_join");
        exit(1);
    }
}
flush_buffer();   // drain remaining bytes
```

`pthread_join(thread, retval)` blocks until the specified thread finishes. This is the thread equivalent of `waitpid()`. The `NULL` second argument means we don't capture the thread's return value. **All threads must be joined** — unjoined threads are resource leaks.

### Mutex Destruction
```c
if (pthread_mutex_destroy(&buf_mtx) != 0) {
    perror("pthread_mutex_destroy");
    exit(1);
}
```

Must be called after all threads are done using the mutex. Destroying a locked mutex is undefined behavior. Since we call this after `pthread_join` (all threads finished), it's safe.

### `flush_buffer` in Part 2
```c
static void flush_buffer(void)
{
    if (pthread_mutex_lock(&buf_mtx) != 0) { ... }
    if (buffer_pos > 0) {
        write_all(STDOUT_FILENO, buffer, (size_t)buffer_pos);
        buffer_pos = 0;
    }
    if (pthread_mutex_unlock(&buf_mtx) != 0) { ... }
}
```

Even though no other threads are running when the parent calls `flush_buffer()` (all have been joined), the mutex is still used for correctness and symmetry. Any function that touches the shared buffer should hold the lock.

### Why Part 2 Output Is Always Clean

With the mutex, `my_write` is **serialized** — only one thread at a time can execute it. The buffer is always in a consistent state when a thread acquires the lock. Reads and writes to `buffer_pos` and `buffer[]` happen atomically from the perspective of other threads. The file offset is also effectively serialized because writes happen while holding the lock — thread A completes its entire write sequence before thread B even starts.

---

# PART 3 — PROCESSES + SYSTEM V SEMAPHORE {#part-3}

## Objective
Return to `fork()` (like Part 1), but synchronize using a **kernel-level semaphore** that works across process boundaries. The output must be clean, like Part 2.

## Why a Mutex Won't Work Here

After `fork()`, if we tried to use a `pthread_mutex_t`:
```
Parent: mutex at virtual address 0x601050, value = UNLOCKED
fork()
Child A: mutex at virtual address 0x601050, value = UNLOCKED  ← COPY, different physical memory
Child B: mutex at virtual address 0x601050, value = UNLOCKED  ← another COPY
```

Child A locking its copy has absolutely no effect on Child B's copy. They each see their own independent mutex, so there's no mutual exclusion. This is why we need a **kernel object**.

## The System V Semaphore — Complete API Reference

### Creating a Semaphore
```c
key_t key = ftok(argv[0], 'A');
```
`ftok(pathname, proj_id)` hashes the inode number of `pathname` with the byte `proj_id` to produce a repeatable `key_t`. Using `argv[0]` (the program binary itself) is a reliable choice since the binary exists throughout the program's lifetime. The project ID `'A'` is arbitrary but must be the same across all callers that want the same semaphore.

```c
int semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644);
```
- `semget(key, nsems, semflg)` → returns a semaphore set ID (`semid`).
- `nsems=1` → we want a set with **one** semaphore.
- `IPC_CREAT` → create if it doesn't exist.
- `IPC_EXCL` → fail with `EEXIST` if it already exists (like `O_EXCL` for files).
- `0644` → permissions (owner read/write, others read).

### Handling Stale Semaphores (EEXIST)
```c
static int create_semaphore(key_t key)
{
    int semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644);
    if (semid == -1 && errno == EEXIST) {
        semid = semget(key, 1, 0);         // open existing (no create flags)
        if (semid == -1) { perror("semget"); exit(1); }
        if (semctl(semid, 0, IPC_RMID) == -1) { perror("semctl"); exit(1); }
        semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644); // retry
    }
    if (semid == -1) { perror("semget"); exit(1); }
    return semid;
}
```

If the program crashed previously without calling `IPC_RMID`, the semaphore stays in the kernel. `EEXIST` detects this. The fix: open the existing semaphore (no `IPC_EXCL`), remove it with `IPC_RMID`, then create fresh. This ensures a clean starting state every run.

### Initializing the Semaphore Value
```c
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

union semun arg;
arg.val = 1;   // binary semaphore: 1 = unlocked
semctl(g_semid, 0, SETVAL, arg);
```

`union semun` is **not declared in any Linux header** — you must declare it yourself in your source file. The union allows `semctl` to accept different types depending on the command.

`semctl(semid, semnum, cmd, arg)`:
- `semnum=0` → we're operating on the first (only) semaphore in the set.
- `SETVAL` → set the value to `arg.val`.
- Initializing to `1` makes it a binary semaphore (mutex-like): one process can hold it at a time.

### P and V Operations
```c
/* P (wait, acquire, down) — decrement */
static void sem_wait_op(void)
{
    struct sembuf op = {0, -1, 0};
    if (semop(g_semid, &op, 1) == -1) { ... }
}

/* V (signal, release, up, post) — increment */
static void sem_post_op(void)
{
    struct sembuf op = {0, +1, 0};
    if (semop(g_semid, &op, 1) == -1) { ... }
}
```

`struct sembuf`:
- `sem_num = 0` → which semaphore in the set (we only have one, so always 0)
- `sem_op = -1` → P operation (decrement). If value would go negative, block.
- `sem_op = +1` → V operation (increment). Wake a blocked waiter if any.
- `sem_flg = 0` → no special flags.

`semop(semid, ops, nops)` → perform an array of operations atomically. We always do one operation at a time.

**Why are P/V operations atomic?** The kernel executes them as a single indivisible unit. No context switch can occur in the middle of a semaphore operation. This is the guarantee that makes them useful for synchronization.

### The `my_write` in Part 3 — The Drain Rule

```c
static void my_write(const char *str)
{
    sem_wait_op();      // P: acquire semaphore (may block)

    size_t len = strlen(str);
    size_t i = 0;
    while (i < len) {
        int space = CHUNK_SIZE - buffer_pos;
        size_t chunk = len - i;
        if (chunk > (size_t)space) chunk = (size_t)space;

        memcpy(buffer + buffer_pos, str + i, chunk);
        buffer_pos += (int)chunk;
        i += chunk;

        if (buffer_pos == CHUNK_SIZE) {
            write_all(STDOUT_FILENO, buffer, CHUNK_SIZE);
            buffer_pos = 0;
        }
    }

    flush_buffer_locked();  // DRAIN before releasing! Part 3 requirement

    sem_post_op();      // V: release semaphore (wake a waiter)
}
```

**The Drain Rule — why is it critical?**

In Part 2 (threads), there is ONE shared buffer. When thread A releases the mutex with `buffer_pos = 14` (14 bytes sitting in the buffer), thread B acquires the mutex and continues writing to `buffer + 14`. The state carries over correctly.

In Part 3 (processes), **each process has its own private buffer**. If Process A releases the semaphore with 14 bytes remaining in its buffer (e.g., a 50-byte message was split: 50 bytes fit, buffer flushed, 14 leftover from the next message), then:
1. Process B acquires the semaphore
2. Process B writes 64 bytes to the file (its own buffer fills up)
3. Process A eventually calls `flush_buffer()`
4. Process A's 14 leftover bytes now appear AFTER Process B's 64 bytes

The file contains: `[A's first half][B's bytes][A's last 14B]` — A's message is fragmented across the file, even though each individual `write()` was atomic.

**The fix:** Before calling `sem_post_op()`, always call `flush_buffer_locked()` which writes any remaining bytes in THIS process's buffer. Now when A releases the semaphore, its buffer is empty. No residual bytes can be inserted out of order.

### `flush_buffer_locked` vs `flush_buffer`
```c
/* Internal flush — caller must already hold the semaphore */
static void flush_buffer_locked(void)
{
    if (buffer_pos > 0) {
        write_all(STDOUT_FILENO, buffer, (size_t)buffer_pos);
        buffer_pos = 0;
    }
}

/* External flush — acquires and releases the semaphore itself */
static void flush_buffer(void)
{
    sem_wait_op();
    flush_buffer_locked();
    sem_post_op();
}
```

Two versions exist to avoid **deadlock**: if `my_write` already holds the semaphore and called `flush_buffer()` (which tries to `sem_wait_op()`), the process would block waiting for a semaphore it already holds — a deadlock. So `my_write` calls `flush_buffer_locked()` (no sem acquire, assumes caller holds it), and the external `flush_buffer()` (called by the parent after all children finish) acquires the semaphore itself.

### `g_parent_pid` — Tracking Who Is Parent
```c
static pid_t g_parent_pid;   // set in main() before fork
// ...
g_parent_pid = getpid();
```

In error handlers inside `write_all`:
```c
if (getpid() != g_parent_pid)
    _exit(1);   // child: use _exit
exit(1);        // parent: use exit
```

After `fork()`, children have a different PID than `g_parent_pid`. This allows shared error-handling code to use the right exit call depending on whether the caller is the parent or a child.

### Semaphore Cleanup — CRITICAL
```c
if (semctl(g_semid, 0, IPC_RMID) == -1) {
    perror("semctl");
    exit(1);
}
```

System V semaphores are **NOT automatically cleaned up when the process exits**. They persist in the kernel's IPC table until explicitly removed or until the system reboots. On shared lab machines, leaked semaphores occupy slots from a small fixed-size table.

**Only the parent calls `IPC_RMID`.** Children call `_exit(0)`. If a child called `IPC_RMID`, other processes (including the parent and remaining children) would lose access to the semaphore mid-run — catastrophic.

After exit, running `ipcs -s` should show no leftover semaphore from your program.

### `g_semid` — How It Survives `fork()`
```c
static int g_semid = -1;    // global integer, initialized before fork
```

The semaphore ID is just an integer (like a file descriptor). When `fork()` copies the parent's address space, this integer is copied to the child. Both parent and all children hold the **same integer value**. When they call `semop(g_semid, ...)`, they all reference the **same kernel IPC object**. The kernel semaphore is not in any process's memory — it's in the kernel — so copying the address space doesn't duplicate it.

### Why Part 3 Output Is Always Clean

The semaphore ensures that only ONE process at a time executes the body of `my_write`. Additionally, each call to `my_write` leaves the buffer empty (due to the drain rule). So the sequence of `write()` syscalls reaching the kernel for any given message is a contiguous, uninterrupted block. No other process's writes can be interleaved within a message.

---

# CROSS-PART COMPARISON TABLE

| Aspect | Part 1 | Part 2 | Part 3 |
|---|---|---|---|
| Worker type | Process (`fork`) | Thread (`pthread_create`) | Process (`fork`) |
| Buffer | Private copy per process | **Single shared buffer** | Private copy per process |
| File offset | Shared (race!) | Shared (protected by mutex) | Shared (protected by sem) |
| Sync primitive | **None** | `pthread_mutex_t` | System V semaphore |
| Sync scope | — | All of `my_write` | All of `my_write` + drain |
| Output | Garbled (intentional) | Clean | Clean |
| Exit method | `_exit(0)` | `return NULL` | `_exit(0)` |
| Flush strategy | Each child flushes own buffer | Parent flushes once after join | Each worker drains before sem_post |
| Cleanup | `waitpid` all children | `pthread_join` + `mutex_destroy` | `waitpid` + `semctl(IPC_RMID)` |
| Link flag | None | `-pthread` | None |
| Type string | `PROCESS` | `THREAD` | `PROCESS` |

---

# COMMON TRAPS AND IMPORTANT DETAILS

## 1. Waiting Inside the Fork Loop (FORBIDDEN in Parts 1 & 3)
```c
// WRONG — serializes children, hides the race
for (int i = 0; i < num_workers; i++) {
    pid = fork();
    if (pid == 0) { worker(...); _exit(0); }
    waitpid(pid, NULL, 0);  // ← blocks until this child finishes
}
```
This pattern makes children run sequentially, not concurrently. Part 1 would never show interleaving. The correct pattern stores all PIDs first, then waits in a separate loop.

## 2. `_exit` vs `exit` in Children
- `exit(0)` → runs `atexit` handlers + flushes stdio buffers → can write garbage to output.txt via fd 1.
- `_exit(0)` → goes directly to kernel, no cleanup. Always use in children.

## 3. `union semun` Must Be Declared Manually
Linux headers intentionally don't declare it. POSIX says the application should provide it. If you forget, `semctl(semid, 0, SETVAL, arg)` won't compile or will pass garbage.

## 4. Semaphore Not Cleaned Up = `ipcs -s` Shows Stale Entry
Always have the parent call `semctl(g_semid, 0, IPC_RMID)` before returning from `main()`.

## 5. Part 3 Buffer Drain — The Subtle Bug Without It
If you omit `flush_buffer_locked()` before `sem_post_op()`:
- A's 50-byte message: 44 bytes fill+flush (SYSCALL #1 under semaphore), 6 bytes sit in buffer.
- A releases semaphore.
- B acquires semaphore, writes its 64 bytes (SYSCALL #2 under semaphore), releases.
- A calls `flush_buffer()` (SYSCALL #3, but now ordering is A64, B64, A6 in the file).
- A's message is split across the file.

## 6. `fprintf(stderr, ...)` Is Always Allowed
Only `stdout` (fd 1) is redirected to `output.txt`. `stderr` (fd 2) still goes to the terminal. The timing line `elapsed_ms=...` and all `perror()` calls go to `stderr`.

## 7. `snprintf` Overflow Check
```c
int n = snprintf(line, sizeof line, "[Type: ...] ...", ...);
if (n < 0 || (size_t)n >= sizeof line) { ... _exit(1); }
```
`snprintf` returns the number of characters that **would have been written** without truncation. If `n >= sizeof line`, the output was truncated and the line is incomplete. This is a safety check.

## 8. The `O_TRUNC` Flag
Without `O_TRUNC`, the file keeps its previous contents. New writes start from the beginning, overwriting old bytes, but if the old file is larger, old bytes remain at the end. Always use `O_TRUNC` to start clean.

## 9. POSIX Semaphores vs System V Semaphores
| | POSIX (`sem_t`) | System V |
|---|---|---|
| Create | `sem_open()` | `semget()` |
| Wait | `sem_wait()` | `semop(..., {-1})` |
| Post | `sem_post()` | `semop(..., {+1})` |
| Destroy | `sem_unlink()` | `semctl(..., IPC_RMID)` |
| Identify by | Name (string) | Key → semid (integer) |

This assignment **requires System V** (`semget`/`semop`/`semctl`). POSIX semaphores are **forbidden**.

## 10. `CHUNK_SIZE = 64` Is Intentionally Chosen
A typical line is ~50 bytes. With a 64-byte chunk, writing one line always spans a buffer boundary (some bytes fill the current chunk and flush, the rest go into the next chunk). This makes the race visible. With `CHUNK_SIZE = 4096`, each message would fit cleanly in one chunk, and Part 1 might look correct (no interleaving visible) because each `write()` might be large enough to be handled atomically by the kernel.

---

# EXAMPLE EXAM QUESTIONS {#exam-questions}

These mirror the format of the test: the examiner will show you a snippet from your code and ask you to explain it.

---

## SECTION A: Concept Questions

**Q1.** After calling `dup2(fd, STDOUT_FILENO)` and then `close(fd)`, what happens to the open file table entry for `output.txt`? How many references to it remain?

> **Answer:** The open file table entry's reference count goes from 2 (one from the original `fd`, one from `STDOUT_FILENO`) down to 1 after `close(fd)`. The entry still exists because `STDOUT_FILENO` (fd 1) still references it. All subsequent `write(STDOUT_FILENO, ...)` calls go through this entry to `output.txt`.

---

**Q2.** Explain exactly why, after `fork()`, changes to `buffer[]` in the child don't affect the parent's `buffer[]`.

> **Answer:** `fork()` creates a copy of the parent's virtual address space using Copy-On-Write. Each process has its own page table mapping virtual addresses to physical pages. When the child (or parent) writes to `buffer[]`, the OS creates a new physical page for that process and maps it to the same virtual address. The other process continues to use the original physical page. So both processes see `buffer[]` at the same virtual address, but they're backed by different physical memory. They are completely independent.

---

**Q3.** Why can't we use `pthread_mutex_t` to synchronize across processes in Part 3?

> **Answer:** A `pthread_mutex_t` is a data structure in user-space memory. After `fork()`, each child gets its own copy of this memory (at the same virtual address but different physical memory). When Child A locks its copy of the mutex, that only modifies bytes in Child A's own address space. Child B has no way to observe this change — it reads its own copy and sees an unlocked mutex. There is no shared memory between the processes to coordinate through. A System V semaphore lives in the kernel IPC table, outside any process's address space, so all processes can reach the same object via the `semid` integer.

---

**Q4.** Why does Part 1 sometimes show clean output even without synchronization?

> **Answer:** Race conditions are timing-dependent. If the OS scheduler happens to run one child process for all its writes before switching to another (e.g., because the children are short-lived or the machine has enough CPU time for sequential scheduling), the output may appear clean. The chaos only manifests when a context switch occurs in the middle of a multi-syscall write sequence. Part 1's chaos is a feature — you may need to run with many workers and many writes to reliably see it.

---

**Q5.** What is `ftok()` and why is `argv[0]` a good argument for it?

> **Answer:** `ftok(pathname, proj_id)` generates a `key_t` by hashing the inode number of `pathname` with the single-byte `proj_id`. It's used to generate a consistent key for System V IPC objects so multiple processes can agree on the same semaphore without an out-of-band channel. `argv[0]` is the program binary itself — it's guaranteed to exist on disk as long as the program is running, and its inode is stable. This makes the derived key stable and repeatable.

---

## SECTION B: Code Snippet Questions

**Q6.** You are shown this code:
```c
pid_t pids[num_workers];
for (int i = 0; i < num_workers; i++) {
    pids[i] = fork();
    if (pids[i] == -1) { perror("fork"); exit(1); }
    if (pids[i] == 0) {
        worker(i, message, writes_per_worker);
        _exit(0);
    }
}
for (int i = 0; i < num_workers; i++) {
    if (waitpid(pids[i], NULL, 0) == -1) { perror("waitpid"); exit(1); }
}
```
Why are there two separate loops? What would go wrong if `waitpid` was inside the first loop?

> **Answer:** The two loops ensure all children are spawned (and running concurrently) before any waiting begins. If `waitpid` were inside the first loop, the parent would block waiting for child `i` to finish before forking child `i+1`. This serializes the children — they run one after another rather than in parallel. Part 1's race condition (which requires simultaneous writes) would never manifest. The assignment explicitly requires all-before-wait to demonstrate genuine concurrency.

---

**Q7.** You are shown:
```c
if (pids[i] == 0) {
    worker(i, message, writes_per_worker);
    _exit(0);
}
```
What does `pids[i] == 0` mean? Why is it `_exit(0)` and not `exit(0)`?

> **Answer:** After `fork()`, the return value in the child is `0`, and in the parent it is the child's PID. So `pids[i] == 0` is only true in the child process. `_exit(0)` is used instead of `exit(0)` because `exit()` runs C library cleanup including flushing `stdio` buffers. Since fd 1 is redirected to `output.txt`, any leftover bytes in `stdio`'s internal buffers would be written to the file, corrupting the output. `_exit()` terminates immediately without cleanup.

---

**Q8.** You are shown:
```c
pthread_mutex_t buf_mtx = PTHREAD_MUTEX_INITIALIZER;

static void my_write(const char *str) {
    pthread_mutex_lock(&buf_mtx);
    // ... buffer manipulation ...
    pthread_mutex_unlock(&buf_mtx);
}
```
Could we make the lock scope narrower — e.g., only lock around the increment of `buffer_pos`? What would go wrong?

> **Answer:** No. If we only locked the increment of `buffer_pos`, two threads could still simultaneously read the old value of `buffer_pos`, compute the same destination slot in `buffer`, and both copy bytes to the same location (overwriting each other). Also, a thread could read `buffer_pos`, determine the buffer is not yet full, then be preempted. Another thread flushes the buffer and resets `buffer_pos = 0`. The first thread wakes up and writes to the now-stale position, placing bytes beyond the reset `buffer_pos`, creating garbage. The entire sequence from "read buffer_pos" through "copy bytes" through "check if full" through "flush if full" must be atomic.

---

**Q9.** You are shown:
```c
struct sembuf op = {0, -1, 0};
semop(g_semid, &op, 1);
```
What do the three fields of `struct sembuf` mean? What does `semop` do if the semaphore's current value is 0?

> **Answer:** The three fields are `sem_num`, `sem_op`, and `sem_flg`. `sem_num=0` selects the first semaphore in the set. `sem_op=-1` is the P operation: atomically decrement the semaphore value. `sem_flg=0` means block (the default). If the current value is 0, decrementing would make it negative, which is not allowed. So `semop` blocks the calling process until the semaphore value becomes positive (i.e., until another process does a V operation / `+1`). This is how mutual exclusion is achieved: only one process can decrement from 1 to 0; all others block.

---

**Q10.** You are shown:
```c
static void my_write(const char *str)
{
    sem_wait_op();
    // ... copy bytes, flush when buffer full ...
    flush_buffer_locked();   // drain before post
    sem_post_op();
}
```
Why is `flush_buffer_locked()` called inside `my_write` before `sem_post_op()`? What would happen if it were omitted?

> **Answer:** Each process has its own private `buffer` (fork creates separate copies). If `my_write` returns with bytes still in the buffer (e.g., 14 bytes from a 50-byte message after a 44+flush), then another process can acquire the semaphore and write its own bytes to the file. When the first process later calls `flush_buffer()`, those 14 bytes land after the second process's bytes, fragmenting the first process's message across the file. By draining before releasing the semaphore, we guarantee each process's logical write is a contiguous block in the file, with no other process's bytes interleaved.

---

**Q11.** You are shown the `create_semaphore` function with `EEXIST` handling:
```c
if (semid == -1 && errno == EEXIST) {
    semid = semget(key, 1, 0);
    semctl(semid, 0, IPC_RMID);
    semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644);
}
```
When would `EEXIST` happen and why must we handle it?

> **Answer:** `EEXIST` happens when a semaphore with the same key already exists in the kernel IPC table. This occurs when the program crashed or was killed in a previous run without calling `semctl(IPC_RMID)`. Unlike file descriptors, System V semaphores are **not automatically cleaned up** when a process exits — they persist until explicitly removed or the system reboots. If we don't handle `EEXIST`, `semget` with `IPC_EXCL` fails and the program exits with an error, even though it could recover. The fix is to open the existing semaphore, delete it, and create a fresh one with a known-good initial value.

---

**Q12.** You are shown:
```c
static void write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w == -1) {
            if (errno == EINTR) continue;
            perror("write"); exit(1);
        }
        off += (size_t)w;
    }
}
```
Why is this function necessary? What is a "short write" and when does `EINTR` occur?

> **Answer:** A single `write()` syscall is not guaranteed to write all requested bytes — it can return fewer, called a "short write." This can happen due to kernel buffer constraints or signal interruption. `EINTR` specifically means the `write()` was interrupted by a signal before any bytes were transferred; the correct response is to retry the call. `write_all` loops until all `n` bytes are written by tracking how many have been sent in `off` and always writing the remainder `buf + off` with `n - off` bytes remaining. Without this, partial writes would silently drop bytes.

---

**Q13.** Compare the output format specifiers:
```c
"[Type: PROCESS] [ID: %d] [Seq: %03d] %s\n"
```
What does `%03d` do and why is the sequence number formatted this way?

> **Answer:** `%03d` prints an integer as a decimal number, **zero-padded to a minimum width of 3 digits**. If the number is less than 100, leading zeros are added (e.g., `7` becomes `007`, `42` becomes `042`). This ensures the Seq field is always exactly 3 digits wide, which the grader validates against the regex `\[Seq: (\d{3})\]`. Using consistent-width numbers also makes the output easier to read and sort.

---

**Q14.** The assignment says "Total bytes always correct" for Part 1. Why are no bytes lost even when output is garbled?

> **Answer:** The kernel's `write()` syscall is atomic at the byte level — it either writes bytes or it doesn't, but it never silently loses bytes (assuming no real errors). When two processes write to the same fd simultaneously, their individual `write()` calls are serialized at the kernel's file offset lock. Each `write()` atomically advances the offset by however many bytes it writes. So all bytes from all workers end up in the file — just in a different order than intended, producing garbled lines. The total count is always `num_workers x writes_per_worker x len(formatted_line)`.

---

**Q15.** Why does Part 2 use `pthread_create` while Parts 1 and 3 use `fork`? What are the trade-offs?

> **Answer:** `fork()` creates a separate process with its own address space. It's heavier (more memory, more kernel structures) but provides isolation — bugs in one child don't corrupt other children's memory. File offset races exist but buffer races don't (each process has its own buffer). `pthread_create()` creates a lightweight thread within the same process. It's cheaper to create and shares all memory — which means buffer races now exist and must be handled with a mutex. The assignment uses this contrast deliberately: Part 2 shows the thread memory-sharing problem (solved by mutex in user space), while Part 3 shows the cross-process file-offset problem (solvable only by a kernel-level synchronization object like a System V semaphore).

---

## SECTION C: Deep Reasoning Questions

**Q16.** Could the semaphore in Part 3 be initialized to 2 instead of 1? What would happen?

> **Answer:** No, it must be 1 (binary semaphore). If initialized to 2, two processes could simultaneously acquire it (each decrementing by 1: 2 to 1 and 1 to 0), entering the critical section at the same time. The purpose is mutual exclusion — only ONE process in `my_write` at a time. A semaphore initialized to `N` allows up to `N` concurrent holders, which is useful for counting resources but wrong for a mutex.

---

**Q17.** In Part 2, why is `flush_buffer()` called by the parent AFTER all `pthread_join()` calls, rather than inside each thread's `worker()` function?

> **Answer:** There is only ONE shared `buffer` in Part 2. If each thread flushed at the end of its `worker()`, two threads finishing concurrently would race on the flush — one might flush while the other is still in `my_write`, leading to a partial flush of mixed data. By waiting until all threads are joined (meaning no thread is running anymore), the parent can safely flush without any concurrent access. The mutex is still acquired in `flush_buffer()` for correctness, but since no threads are running, the lock will always be immediately available.

---

**Q18.** Part 3's `worker()` function ends with:
```c
flush_buffer();   /* defensive; my_write already drained each time */
```
If `my_write` already drains the buffer before releasing the semaphore, why is this call there at all?

> **Answer:** It's a defensive belt-and-suspenders check. Because `my_write` always drains before releasing the semaphore, `buffer_pos` should always be 0 when `worker()` ends. The final `flush_buffer()` will do nothing (it checks `if (buffer_pos > 0)` which is false). However, if the logic were ever changed (e.g., an intermediate version that doesn't drain), this call would catch the residual bytes. It also mirrors the pattern from Part 1 where the flush is genuinely needed, making the code structure consistent across parts.

---

**Q19.** What is a zombie process and how does the code prevent them?

> **Answer:** A zombie is a process that has finished executing but whose exit status hasn't been collected by its parent. The kernel keeps the process's entry in the process table (with its PID and exit status) until the parent calls `wait()` or `waitpid()`. Zombies consume a process table slot. The code prevents zombies by calling `waitpid(pids[i], NULL, 0)` for every child PID in a loop after the fork loop. This collects each child's exit status and removes it from the process table. The `NULL` argument means we don't need the exit status value.

---

**Q20.** Explain the difference between `sem_num`, `sem_op`, and `sem_flg` in `struct sembuf`. When would you use `IPC_NOWAIT` as a flag?

> **Answer:** `sem_num` selects which semaphore within the set (we use a set of 1, so always 0). `sem_op` is the operation: negative = P (decrement, acquire), positive = V (increment, release), zero = wait until the semaphore reaches 0 (rarely used). `sem_flg` modifies behavior: `IPC_NOWAIT` makes the operation non-blocking — if it would block (e.g., semaphore is 0 and we're trying to P), it returns immediately with `errno = EAGAIN` instead of blocking. `SEM_UNDO` tells the kernel to automatically undo the operation if the process exits unexpectedly. In this assignment we use flag `0` (blocking, no undo), which is appropriate for a straightforward mutex.

---

*End of Lecture Document*

> **Study Tip:** For each exam question, trace through the exact code in your implementation. The examiner will show real snippets from your files. Know every line and be able to explain the "why" behind design decisions: why this primitive, why this scope, why this exit call, why this flush placement.
