# Part 3 — Isolated Memory Crisis (Processes + System V Semaphore)

## Objective
Return to processes — but this time, fix the chaos. Discover that even though the buffer is no longer shared (each forked child has its own), you still need a synchronization primitive — and you need a *different kind* than in Part 2, because the resource you are protecting is no longer in your address space.

## The System V Semaphore API in 30 Seconds

System V semaphores are kernel objects identified by an integer **key**. The functions you'll use:

```c
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

key_t key = ftok(argv[0], 'A');         // derive a key from a stable path
int   semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0644);  // create one semaphore
                                                             // returns -1 on failure
// initialize value to 1 (so it acts as a binary semaphore / mutex)
union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
arg.val = 1;
semctl(semid, 0, SETVAL, arg);

// acquire (P-operation): atomically decrement, blocking if value is 0
struct sembuf op_wait = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
semop(semid, &op_wait, 1);

// release (V-operation): atomically increment
struct sembuf op_post = { .sem_num = 0, .sem_op = +1, .sem_flg = 0 };
semop(semid, &op_post, 1);

// destroy: remove the semaphore from the kernel IPC table
semctl(semid, 0, IPC_RMID);
```

A few things worth knowing:
- `semget` works with a **set** of semaphores; we ask for a set of size 1 and operate on `sem_num = 0`.
- `union semun` is **not** declared in any header on Linux — you must declare it yourself, exactly as shown above.
- A failed `semget` with `IPC_EXCL` because of a stale leftover from a crashed previous run shows up as `errno == EEXIST`. Handle this: in that case, get the existing semid (call `semget(key, 1, 0)`), remove it with `semctl(..., IPC_RMID)`, and try again. Your program must not crash on a stale leftover.
- The `ftok(path, proj_id)` call hashes the inode of `path` together with `proj_id` to produce a key. Use a path that is guaranteed to exist for the lifetime of the run — `argv[0]` (the program itself) is a good choice.

## Architecture
1. The **parent** sets up redirection, parses argv, records start time.
2. The parent creates a System V semaphore set of size 1, initialized to value 1 (binary mutex). On `EEXIST`, removes the stale set and retries.
3. The parent loops `num_workers` times and `fork()`s. **All children must be forked before any wait**, just like Part 1.
4. Each child runs the worker. It uses the inherited `semid` (an integer — passed via a global or a function argument; either is fine since `fork` duplicates the address space) to lock around the entire body of `my_write`.
5. The parent waits for every child, calls its own final `flush_buffer` (also semaphore-protected), records end time, prints to stderr, removes the semaphore via `IPC_RMID`, and exits.

## The Crisis (And Why It's Different from Part 2)

After `fork()`, each child has its **own private copy** of `buffer` and `buffer_pos`. They cannot corrupt each other's buffers — virtual memory isolates them. So the *memory race* of Part 2 is gone.

But all children share the **same entry in the open file table** (because they inherited fd 1 from the parent). And here's the crucial detail: a single call to `my_write` may trigger **multiple `write()` syscalls** when the input string spans chunk boundaries. For example, with `CHUNK_SIZE=64` and a 50-byte message arriving when `buffer_pos=20`:

```
   [........copy 44 bytes.........] -> buffer full -> write(64)  [SYSCALL #1]
   [..copy remaining 6 bytes...]    -> buffer at pos 6, no flush yet
   ...later, when next message arrives or worker ends...
   [................flush........]  -> write(6)                  [SYSCALL #2]
```

If process A executes SYSCALL #1 and then process B's flush lands *before* A executes SYSCALL #2, the file ends up with A's first 64 bytes, then B's chunk, then A's next 6 bytes — **A's message has been fragmented across the file**. Even though each individual `write()` is atomic at the kernel level on Linux, the *sequence of writes that together produce one logical message* is not.

So the semaphore must protect **the logical unit of work** — the entire `my_write` call — not just the syscall.

## Synchronization Requirement: Semaphore Around the Whole `my_write` Body — *and the Buffer Must Be Drained Before Releasing the Semaphore*

```c
static void my_write(const char *str) {
    struct sembuf op_wait = { 0, -1, 0 };
    struct sembuf op_post = { 0, +1, 0 };

    if (semop(semid, &op_wait, 1) == -1) { perror("semop wait"); _exit(1); }

    /* copy bytes into per-process buffer; flush as it fills */

    /* IMPORTANT: also flush whatever remains in the buffer before releasing,
     * so this process leaves the buffer empty for the next call. */
    if (buffer_pos > 0) {
        write(STDOUT_FILENO, buffer, buffer_pos);   /* + error-check */
        buffer_pos = 0;
    }

    if (semop(semid, &op_post, 1) == -1) { perror("semop post"); _exit(1); }
}
```

`flush_buffer` must also be semaphore-protected for the same reason.

### Why drain the buffer at the end?

In Part 2, only **one** buffer exists (shared between threads). Once the mutex is held, all bytes go through that single buffer in a deterministic order, and the buffer's residual state at the time the lock is released is fine — the next thread to acquire the mutex picks up where the previous one left off.

In Part 3, **each process has its own buffer**. If `my_write` returns with bytes left over in *this process's* buffer (e.g. the message was 50 bytes, and 16 bytes are now sitting in the buffer for next time), then *another process can run, fill its own buffer, and flush a chunk to the shared file before this process gets back* to flush those leftover 16 bytes. The file now contains: \[head of this process's message\] \[other process's chunk\] \[tail of this process's message\] — fragmented across the file, even though each `write()` was atomic and the semaphore was held throughout this process's `my_write`.

The fix is to ensure no carry-over: every `my_write` flushes its own residual bytes before releasing the semaphore. This costs you the ability to amortize syscalls across messages — you'll do roughly one `write()` per `my_write` call — but it's the price of cross-process buffering safety.

The key differences to the previous part are:

| Aspect | Part 2 (threads + mutex) | Part 3 (processes + System V semaphore) |
|---|---|---|
| What's shared between workers? | Everything: `buffer`, `buffer_pos`, fd, file offset | *Only* the fd and its offset (buffers are private) |
| What is the lock protecting? | Memory consistency of `buffer`/`buffer_pos` | Atomicity of a multi-syscall logical message |
| Sync primitive | `pthread_mutex_t` (in-process, user-space) | System V semaphore (kernel IPC object, cross-process) |
| Why this primitive? | Threads share a heap → user-space mutex is fast and sufficient | Processes don't share a heap → mutex in user memory is invisible to other procs → need a kernel object |

This is the central lesson of the exercise: **the choice of synchronization primitive depends on the address-space context, not on the apparent shape of the lock.** A user-space mutex is invisible across processes; a System V semaphore lives in the kernel IPC table and is reachable by any process that holds its `semid`.

## How the Semaphore Survives `fork()`

The `semid` returned by `semget` is just an integer index into a kernel IPC table. After `fork()`, the child inherits the parent's address space — including the integer holding the semid. Both parent and child use the same integer, and `semop`/`semctl` look up the same kernel object. There is nothing in user memory that needs to be shared — the kernel does the work.

> Contrast: a `pthread_mutex_t` placed at the same virtual address in two processes would still be **two different objects**, because each process has its own copy of that memory. That's why mutexes can't synchronize processes (without `mmap(MAP_SHARED)` shenanigans, anyway).

## Cleanup Requirements

This is **more important** than for POSIX semaphores: System V semaphores are stored in a small, system-wide IPC table. A leaked semaphore takes up a slot until the kernel reboots or someone removes it manually with `ipcrm`. On shared lab machines this is a real annoyance.

- The parent must call `semctl(semid, 0, IPC_RMID)` before exiting. Children should **not** call `IPC_RMID` — only the parent.
- After your program exits, `ipcs -s` should not show your semaphore.
- The parent reaps every child (no zombies).
- The output fd is closed.
- Children call `_exit(0)` (not `exit(0)` — same reason as Part 1).
- Handle `EEXIST` from `semget` by removing the stale leftover and retrying, so a previous crash doesn't break this run.

## Build
```bash
gcc -Wall -Wextra -O2 -std=c11 -o part3 part3.c
```
System V semaphore functions are in glibc proper — **no special linker flags needed**. The `Makefile` already builds part3 cleanly.

## Optional: What to Put in REPORT.md for Part 3
*(REPORT.md is optional; filling it in earns up to +5 bonus points capped at 100. Skip this section if you don't plan to fill in the report.)*

1. **Output check.** Run `./part3 "Hello iso world" 4 50` and confirm `output.txt` is clean. Paste 3 lines.
2. **The "why this scope" question.** Your Part 2 mutex and your Part 3 semaphore both wrap the entire `my_write` body. In **3–5 sentences**, explain *why each one is necessary*. Address: what would the corruption look like in Part 3 if you locked only the `write()` call instead of all of `my_write`? Be concrete — give an example interleaving in terms of bytes in the output file.
3. **Performance table.** Run all three configurations on **both** Part 2 and Part 3:

   | config | part2 elapsed_ms (median of 3) | part3 elapsed_ms (median of 3) |
   |---|---|---|
   | `"test" 1 1000`  | … | … |
   | `"test" 50 100`  | … | … |
   | `"test" 50 1000` | … | … |

   Then explain in 4–6 sentences:
   - Which is faster, and why?
   - How does spawn cost (`fork` vs `pthread_create`) factor in?
   - How does sync-primitive cost (in-process mutex vs kernel `semop` syscall) factor in?
   - At which configuration is each cost most visible?

## Acceptance Criteria
- Compiles cleanly with `-Wall -Wextra` (no warnings).
- Output is well-formed (same regex as Part 2); every `(ID, Seq)` exactly once; per-ID seqs are `0..writes_per_worker-1`.
- No deadlocks (generous timeout enforced).
- Semaphore removed via `IPC_RMID` — `ipcs -s` shows nothing after exit.
- All children are reaped.
- Stale-semaphore handling on `EEXIST` works.

## Forbidden in Part 3
- Threads (`pthread_*`). This part is processes-only.
- Mutexes (`pthread_mutex_*`).
- POSIX semaphores (`sem_open`, `sem_wait`, etc.) — this part is **System V** semaphores only.
- Lock files / `flock` / `fcntl(F_SETLK)`.
- Any form of `fopen`/`fprintf`/`puts`/etc. for `output.txt`.