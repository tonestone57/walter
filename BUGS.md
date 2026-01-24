# Bug Report

## Critical / Major Bugs

### 1. Incorrect Return Values in POSIX Semaphore Implementation
*   **File:** `src/system/libroot/posix/semaphore.cpp`
*   **Bug:** `sem_trywait`, `sem_wait`, `sem_timedwait` functions incorrectly return positive POSIX error codes (e.g., `EAGAIN`, `ETIMEDOUT`) directly on failure, instead of returning -1 and setting `errno`.
*   **Details:** The helper functions (e.g., `unnamed_sem_trywait`) return positive error codes. The macro `RETURN_AND_SET_ERRNO(err)` checks `if (err < 0)`. Since positive error codes are not `< 0`, the macro returns the error code directly as the function result.
*   **Impact:** Applications checking for `return == -1` will fail to detect errors, potentially leading to race conditions or undefined behavior.

### 2. Incorrect `errno` Values in Environment Functions
*   **File:** `src/system/libroot/posix/stdlib/env.cpp`
*   **Bug:** `setenv`, `unsetenv`, `putenv` set `errno` to Haiku internal status codes (e.g., `B_BAD_VALUE`, `B_NO_MEMORY` which are negative) instead of POSIX error codes (e.g., `EINVAL`, `ENOMEM` which are positive).
*   **Details:** `__set_errno(B_BAD_VALUE)` is used directly. `RETURN_AND_SET_ERRNO` is used with `B_NO_MEMORY`, which sets `errno` to the negative value.
*   **Impact:** Applications checking `errno == EINVAL` or `errno == ENOMEM` will fail to handle errors correctly.

### 3. Kernel Team Iteration Broken on ID Wrap
*   **File:** `src/system/kernel/team.cpp`
*   **Bug:** `_get_next_team_info` uses a loop from `slot` to `peek_next_thread_id()` to find teams. This logic is explicitly noted as broken in comments: `// TODO: This is broken, since the id can wrap around!`.
*   **Details:** If team IDs wrap around (after 2^31), `peek_next_thread_id()` may return a small value, causing the loop to terminate early or miss teams with higher IDs.
*   **Impact:** System monitoring tools (like ProcessController) may fail to list all running teams, or show incomplete lists after long system uptime.

### 4. Incorrect Return Values in Pthread Barrier Implementation
*   **File:** `src/system/libroot/posix/pthread/pthread_barrier.cpp`
*   **Bug:** Functions like `pthread_barrier_init`, `pthread_barrier_wait` return Haiku status codes (e.g., `B_BAD_VALUE` which is negative) instead of positive POSIX error codes (`EINVAL`).
*   **Details:** Pthread functions are specified to return the error code directly (positive). Returning negative Haiku codes is non-compliant.
*   **Impact:** Applications checking against `EINVAL` or `> 0` for errors will misinterpret the return value.

## Moderate Bugs / Missing Features

### 5. `posix_fallocate` Returns Raw Kernel Status
*   **File:** `src/system/libroot/posix/fcntl.cpp`
*   **Bug:** `posix_fallocate` returns the raw return value of `_kern_preallocate`.
*   **Details:** If the kernel call returns a negative status (e.g., `B_NO_MEMORY`), `posix_fallocate` returns it directly. It should map it to a positive POSIX error code (e.g., `ENOMEM`).
*   **Impact:** Non-compliant return value.

### 6. Potential Precision Loss in `log2l`
*   **File:** `src/system/libroot/posix/musl/math/log2l.c`
*   **Bug:** Stubbed implementation for 128-bit `long double`.
*   **Details:** `#elif LDBL_MANT_DIG == 113 ... // TODO: broken implementation ... return log2(x);`.
*   **Impact:** If compiled for an architecture with 128-bit `long double`, `log2l` will have reduced precision (double), leading to calculation errors.

## Minor Bugs / Optimizations

### 7. Inefficient `remove` Implementation
*   **File:** `src/system/libroot/posix/stdio/remove.c`
*   **Bug:** `remove()` attempts `unlink()` first, then checks for `B_IS_A_DIRECTORY` to try `rmdir()`.
*   **Details:** `// TODO: find a better way that does not require two syscalls for directories`. This causes two system calls for every directory removal.
*   **Impact:** Minor performance overhead when deleting directories.

### 8. Unoptimized Bitmap Operations
*   **File:** `src/system/kernel/util/Bitmap.cpp`
*   **Bug:** `SetRange`, `ClearRange`, `GetLowestClear`, and `GetLowestContiguousClear` iterate bit-by-bit.
*   **Details:** Multiple `// TODO: optimize` comments. These functions could perform bulk operations (word-at-a-time) for significant performance gains on large bitmaps.
*   **Impact:** Performance degradation in kernel subsystems utilizing large bitmaps (e.g., potentially slab allocator or resource managers).

## Analysis Notes
*   **POSIX Compliance:** There is a recurring pattern of mixing Haiku `status_t` (negative) and POSIX `errno` (positive) values in `libroot/posix`. The `RETURN_AND_SET_ERRNO` macro is dangerous when used with functions that might return positive values (like helper functions using `E*` codes) or when passed raw `B_*` codes without conversion.
*   **Kernel:** The team ID wrapping issue is a significant logic flaw for long-running systems.
