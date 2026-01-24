# Bug Report

## Security Vulnerabilities

### 1. Kernel GDB Stub Buffer Overflow Risk
*   **File:** `src/system/kernel/debug/gdb.cpp`
*   **Bug:** `gdb_reply` uses `vsprintf` into a static 512-byte buffer `sReply` without size checking.
*   **Details:** `vsprintf(sReply + 1, format, args);` does not enforce the 512-byte limit. If a debug message exceeds this length, it causes a kernel stack or global data corruption.
*   **Impact:** Potential kernel crash or exploitation via the GDB stub interface.

### 2. Network Resolver Thread-Safety Violations
*   **File:** `src/system/libnetwork/netresolv/resolv/res_debug.c`
*   **Bug:** Multiple functions (`sym_ntos`, `sym_ntop`, `p_option`, `p_time`, `p_secstodate`) use static buffers to return string results.
*   **Details:** Comments like `/*%< XXX nonreentrant */` confirm this. In a multi-threaded environment, concurrent calls will race and overwrite shared buffers.
*   **Impact:** Data corruption in network diagnostics and logging.

## Concurrency & Stability (Critical)

### 3. Race Condition in Kernel Mutex Destruction
*   **File:** `src/system/kernel/locks/lock.cpp`
*   **Bug:** `mutex_destroy` has a race condition with `mutex_lock_with_timeout`.
*   **Details:** `// TODO: There is still a race condition during mutex destruction`. Thread resumption from timeout might access invalid memory if mutex is destroyed concurrently.
*   **Impact:** Kernel panic or memory corruption during high-load concurrency scenarios.

### 4. Missing Deadlock Detection in VFS
*   **File:** `src/system/kernel/fs/vfs.cpp`
*   **Bug:** No deadlock detection mechanism for VFS operations.
*   **Details:** `// TODO: do deadlock detection!`. Complex file system operations can lock multiple nodes, risking cyclical dependencies.
*   **Impact:** System hangs requiring a reboot.

## Data Integrity & Drivers

### 5. GPT Partition Table Corruption Recovery Missing
*   **File:** `src/add-ons/kernel/partitioning_systems/gpt/gpt.cpp`
*   **Bug:** No validation or recovery for corrupt GPT headers.
*   **Details:** `// TODO: implement, validate CRCs and restore from backup area if corrupt`.
*   **Impact:** Potential data loss or unmountable drives if the primary GPT header is damaged.

### 6. BFS Journal Replay Logic Flaw
*   **File:** `src/add-ons/kernel/file_systems/bfs/Journal.cpp`
*   **Bug:** Journal replay logic fails if transaction size equals log size.
*   **Details:** `// TODO: this logic won't work whenever the size of the pending transaction equals the size of the log`.
*   **Impact:** Filesystem corruption or mount failure after a crash if the log was full.

### 7. Missing Packet Corruption Reporting in Modem Driver
*   **File:** `src/add-ons/kernel/network/ppp/modem/ModemDevice.cpp`
*   **Bug:** Corrupted packets are silently dropped without notification.
*   **Impact:** Poor network diagnostics.

## POSIX Compliance & Kernel Logic

### 8. Incorrect Return Values in POSIX Semaphore/Barrier
*   **File:** `src/system/libroot/posix/semaphore.cpp`, `src/system/libroot/posix/pthread/pthread_barrier.cpp`
*   **Bug:** Functions return positive error codes (semaphores) or negative Haiku codes (barriers) incorrectly.
*   **Impact:** API breakage; standard error checks fail.

### 9. Incorrect `errno` Values in Environment Functions
*   **File:** `src/system/libroot/posix/stdlib/env.cpp`
*   **Bug:** `setenv` sets `errno` to negative Haiku status codes.
*   **Impact:** API breakage.

### 10. Kernel Team Iteration Broken on ID Wrap
*   **File:** `src/system/kernel/team.cpp`
*   **Bug:** `_get_next_team_info` unsafe loop logic terminates early if IDs wrap.
*   **Impact:** Process listing tools fail on long-running systems.

## Kit & Server Issues

### 11. Media Kit Thread Safety
*   **File:** `src/kits/media/MediaRecorderNode.h`
*   **Bug:** `SetAcceptedFormat` and `AcceptedFormat` are not thread-safe.
*   **Details:** Explicitly marked `// TODO these are not thread safe; we should fix that...`.
*   **Impact:** Potential race conditions in media recording applications.

### 12. Potential Memory Leaks in Interface Layout
*   **File:** `src/kits/interface/TwoDimensionalLayout.cpp`
*   **Bug:** Unverified memory management in layout logic.
*   **Details:** `// TODO: Check for memory leaks!`.
*   **Impact:** Long-running GUI applications might leak memory.

## General Coding Errors (Minor)

### 13. Hardcoded Paths
*   **Files:** `src/libs/print/libprint/DbgMsg.cpp`, `src/preferences/joysticks/JoyWin.cpp`
*   **Bug:** Usage of hardcoded paths like `/boot/home/libprint.log` or `/boot/home/config/...`.
*   **Details:** Should use `find_directory` API to support multi-user environments or non-standard layouts.
*   **Impact:** Applications fail or write to wrong locations in different user environments.

### 14. Inefficient `remove` Implementation
*   **File:** `src/system/libroot/posix/stdio/remove.c`
*   **Bug:** Two syscalls used for every removal.

### 15. Unoptimized Bitmap Operations
*   **File:** `src/system/kernel/util/Bitmap.cpp`
*   **Bug:** Bit-by-bit iteration instead of word-at-a-time.
