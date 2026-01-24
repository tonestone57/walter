# Bug Report

## Security Vulnerabilities

### 1. Kernel GDB Stub Buffer Overflow Risk
*   **File:** `src/system/kernel/debug/gdb.cpp`
*   **Bug:** `gdb_reply` uses `vsprintf` into a static 512-byte buffer `sReply` without size checking.
*   **Details:** `vsprintf(sReply + 1, format, args);` does not enforce the 512-byte limit. If a debug message exceeds this length, it causes a kernel stack or global data corruption (depending on where `sReply` is, here it's static/global).
*   **Impact:** Potential kernel crash or exploitation via the GDB stub interface.

### 2. Network Resolver Thread-Safety Violations
*   **File:** `src/system/libnetwork/netresolv/resolv/res_debug.c`
*   **Bug:** Multiple functions (`sym_ntos`, `sym_ntop`, `p_option`, `p_time`, `p_secstodate`) use static buffers to return string results.
*   **Details:** Comments like `/*%< XXX nonreentrant */` confirm this. In a multi-threaded environment, concurrent calls to these resolver functions will race and overwrite shared buffers, leading to garbled logs or logic errors.
*   **Impact:** Data corruption in network diagnostics and logging.

## Concurrency & Stability (Critical)

### 3. Race Condition in Kernel Mutex Destruction
*   **File:** `src/system/kernel/locks/lock.cpp`
*   **Bug:** `mutex_destroy` has a race condition with `mutex_lock_with_timeout`.
*   **Details:** Comment: `// TODO: There is still a race condition during mutex destruction`. If a thread resumes from a timeout just as the mutex is being destroyed, it may access invalid memory.
*   **Impact:** Kernel panic or memory corruption during high-load concurrency scenarios.

### 4. Missing Deadlock Detection in VFS
*   **File:** `src/system/kernel/fs/vfs.cpp`
*   **Bug:** No deadlock detection mechanism for VFS operations.
*   **Details:** `// TODO: do deadlock detection!`. Complex file system operations (like renaming across directories) can lock multiple nodes. Without detection, cyclical locking dependencies will hang the affected threads indefinitely.
*   **Impact:** System hangs requiring a reboot.

## Data Integrity & Drivers

### 5. GPT Partition Table Corruption Recovery Missing
*   **File:** `src/add-ons/kernel/partitioning_systems/gpt/gpt.cpp`
*   **Bug:** No validation or recovery for corrupt GPT headers.
*   **Details:** `// TODO: implement, validate CRCs and restore from backup area if corrupt`. The driver does not verify checksums or attempt to restore the secondary GPT if the primary is corrupt.
*   **Impact:** Potential data loss or unmountable drives if the primary GPT header is slightly damaged.

### 6. Missing Packet Corruption Reporting in Modem Driver
*   **File:** `src/add-ons/kernel/network/ppp/modem/ModemDevice.cpp`
*   **Bug:** Corrupted packets are silently dropped or ignored without notifying the stack.
*   **Details:** `// TODO: report corrupted packets to KPPPInterface`.
*   **Impact:** poor network diagnostics and "silent" data loss.

## POSIX Compliance (Critical / Major)

### 7. Incorrect Return Values in POSIX Semaphore Implementation
*   **File:** `src/system/libroot/posix/semaphore.cpp`
*   **Bug:** `sem_trywait`, `sem_wait`, `sem_timedwait` return positive error codes directly.
*   **Details:** `RETURN_AND_SET_ERRNO` is used incorrectly with positive error codes.
*   **Impact:** API breakage; applications checking `return == -1` will fail.

### 8. Incorrect `errno` Values in Environment Functions
*   **File:** `src/system/libroot/posix/stdlib/env.cpp`
*   **Bug:** `setenv`, `unsetenv` set `errno` to negative Haiku status codes.
*   **Details:** `__set_errno(B_BAD_VALUE)` used instead of `EINVAL`.
*   **Impact:** API breakage; standard error checks fail.

### 9. Incorrect Return Values in Pthread Barrier
*   **File:** `src/system/libroot/posix/pthread/pthread_barrier.cpp`
*   **Bug:** Returns Haiku status codes (negative) instead of POSIX error codes (positive).
*   **Impact:** API breakage.

## Kernel Logic (Major)

### 10. Kernel Team Iteration Broken on ID Wrap
*   **File:** `src/system/kernel/team.cpp`
*   **Bug:** `_get_next_team_info` unsafe loop logic.
*   **Details:** Iterates up to `peek_next_thread_id()`. If IDs wrap, this loop terminates early.
*   **Impact:** Process listing tools fail on long-running systems.

## Moderate / Minor Bugs

### 11. `posix_fallocate` Returns Raw Kernel Status
*   **File:** `src/system/libroot/posix/fcntl.cpp`
*   **Impact:** Non-compliant return value.

### 12. Potential Precision Loss in `log2l`
*   **File:** `src/system/libroot/posix/musl/math/log2l.c`
*   **Impact:** Reduced precision on systems supporting 128-bit `long double`.

### 13. Inefficient `remove` Implementation
*   **File:** `src/system/libroot/posix/stdio/remove.c`
*   **Bug:** Two syscalls used for every removal.

### 14. Unoptimized Bitmap Operations
*   **File:** `src/system/kernel/util/Bitmap.cpp`
*   **Bug:** Bit-by-bit iteration instead of word-at-a-time.
