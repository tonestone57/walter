# Missing POSIX Functions in Haiku OS

Based on the [os-test](https://sortix.org/os-test/include/) results, the following POSIX headers and functions are missing or incomplete in Haiku. The difficulty to implement each is estimated below.

## Missing Headers

### 1. `aio.h` (Asynchronous Input and Output)
*   **Missing:** All functions (`aio_cancel`, `aio_error`, `aio_fsync`, `aio_read`, `aio_return`, `aio_suspend`, `aio_write`, `lio_listio`) and types (`struct aiocb`).
*   **Difficulty: Hard**
    *   Requires implementing a POSIX-compliant Asynchronous I/O subsystem in the kernel and/or `libroot`. While Haiku has an asynchronous-friendly architecture, bridging it to the specific POSIX AIO semantics is significant work.

### 2. `mqueue.h` (Message Queues)
*   **Missing:** All functions (`mq_close`, `mq_getattr`, `mq_notify`, `mq_open`, `mq_receive`, `mq_send`, `mq_setattr`, `mq_timedreceive`, `mq_timedsend`, `mq_unlink`) and types.
*   **Difficulty: Hard**
    *   Requires implementing POSIX message queues. Haiku has native `BPort` message passing, but POSIX message queues have different semantics (priorities, notification methods, file-descriptor-like access) that would likely require a dedicated kernel implementation or a complex userland wrapper.

### 3. `sys/shm.h` (XSI Shared Memory)
*   **Status: Implemented**
    *   Functions `shmat`, `shmctl`, `shmdt`, `shmget` are now implemented in the kernel and libroot.
*   **Implementation Details:**
    *   Kernel implementation in `src/system/kernel/posix/xsi_shared_memory.cpp`.
    *   Userland wrappers in `src/system/libroot/posix/sys/xsi_shm.cpp`.
    *   Lifecycle management (fork/exec/exit cleanup) integrated via `src/system/kernel/team.cpp` and `xsi_shm_context`.

### 4. `ndbm.h` (Database Operations)
*   **Missing:** All functions (`dbm_clearerr`, `dbm_close`, `dbm_delete`, `dbm_error`, `dbm_fetch`, `dbm_firstkey`, `dbm_nextkey`, `dbm_open`, `dbm_store`) and types.
*   **Difficulty: Medium**
    *   These functions are typically provided by a library like Berkeley DB (libdb) or GDBM. The difficulty lies in porting/integrating such a library into the base system or creating a compatibility wrapper, rather than kernel development.

### 5. `iconv.h` (Codeset Conversion)
*   **Missing:** All functions (`iconv`, `iconv_close`, `iconv_open`) and types.
*   **Difficulty: Medium**
    *   Requires a character set conversion library. This is standard functionality (often via `libiconv` or implementation in `libc`). Haiku has its own `libtextencoding`, so this would involve either porting `libiconv` to the base system or writing a wrapper around Haiku's native encoding kit.

### 6. `libintl.h` (Internationalization / gettext)
*   **Missing:** Most functions (`bindtextdomain`, `gettext`, `dgettext`, etc.).
*   **Difficulty: Medium**
    *   These are the standard GNU gettext functions. Haiku uses the `BLocale` kit. Providing these requires integrating an implementation (like GNU gettext's runtime) into `libroot` or providing a compatibility layer.

### 7. `wordexp.h` (Word Expansion)
*   **Missing:** `wordexp`, `wordfree`.
*   **Difficulty: Medium**
    *   Requires implementing shell-style word expansion rules (variable substitution, command substitution, etc.). This usually involves invoking a shell or interacting with a shell library.

### 8. `fmtmsg.h` (Message Display)
*   **Missing:** `fmtmsg`.
*   **Difficulty: Medium**
    *   Requires implementing the formatted message display function in `libc`. This is logically self-contained but requires adherence to specific formatting rules and console interaction.

### 9. `cpio.h` (CPIO Archive Values)
*   **Status: Implemented**
    *   Header added with standard definitions.

## Incomplete Headers

### 10. `dirent.h` (Directory Entries)
*   **Missing:** `d_type` field in `struct dirent`, `DT_*` macros (e.g., `DT_DIR`, `DT_REG`), and `posix_getdents`.
*   **Difficulty: Hard**
    *   Adding `d_type` requires changing the `struct dirent` ABI. More importantly, it requires updating the kernel `readdir` syscall and filesystem implementations to return file type information within the directory listing (to avoid `stat` penalties), which is a significant cross-cutting change.

### 11. `unistd.h` (Standard Symbolic Constants and Types)
*   **Missing:**
    *   `dup3`, `pipe2` (Newer POSIX atomic-close-on-exec variants). **Difficulty: Medium** (Requires kernel support).
    *   `fexecve`. **Difficulty: Medium**.
    *   Various `_SC_` feature macros (e.g., `_SC_THREADS`, `_SC_SEMAPHORES`, `_SC_ASYNCHRONOUS_IO`) are currently undeclared or unimplemented in `sysconf`. **Difficulty: Easy** (if feature exists) to **Hard** (if feature is missing).

### 12. `spawn.h` (Process Spawning)
*   **Missing:** Support for `SPN PS` (Process Scheduling) option in `posix_spawn`.
*   **Difficulty: Medium**
    *   Requires implementing `posix_spawnattr_setschedparam`, `posix_spawnattr_setschedpolicy`, etc., and ensuring the kernel `spawn` machinery honors these attributes.

### 13. `pthread.h` (POSIX Threads)
*   **Missing:**
    *   `pthread_mutex_consistent`
    *   `pthread_setschedprio`
    *   `pthread_mutexattr_getrobust`, `pthread_mutexattr_setrobust` (Robust Mutex support)
*   **Difficulty: Hard**
    *   Robust mutexes require kernel support to handle owner death ("EOWNERDEAD"). `pthread_setschedprio` requires dynamic priority adjustment logic.
*   **Verified Present:**
    *   `pthread_cancel`, `pthread_detach`
    *   `pthread_cond_init`, `pthread_cond_timedwait`
    *   `pthread_rwlock_rdlock`, `pthread_rwlock_wrlock`, `pthread_rwlock_timedrdlock`, `pthread_rwlock_timedwrlock`

### 14. `fcntl.h` (File Control)
*   **Missing:**
    *   `F_OFD_SETLK`, `F_OFD_SETLKW`, `F_OFD_GETLK` (Open File Description Locks).
*   **Difficulty: Hard**
    *   Requires updating `src/system/kernel/fs/vfs.cpp` (specifically `common_fcntl` and the advisory lock implementation) to support locks associated with the file description rather than the process/thread context. This involves changing the kernel's `advisory_lock` structure and logic to handle the new lock types and their ownership semantics (persisting across `fork`, not closing on `close` unless all references are gone, etc.).

## Files Needing Updates

To implement the missing features, the following files and directories would likely need modification or creation:

*   **Open File Description Locks (`F_OFD_SETLK`)**:
    *   `src/system/kernel/fs/vfs.cpp` (locking logic in `common_fcntl`, `acquire_advisory_lock`, `release_advisory_lock`, `test_advisory_lock`)
    *   `headers/posix/fcntl.h` (add macros)

*   **POSIX Spawn (`spawn.h` scheduling support)**:
    *   `src/system/libroot/posix/spawn.cpp` (implement attribute setters/getters)
    *   `src/system/kernel/team.cpp` (or similar, to honor scheduling attributes during thread creation if not handled in userland)

*   **Pthread Robust Mutexes & Priority**:
    *   `src/system/libroot/posix/pthread/pthread_mutex.cpp`
    *   `src/system/libroot/posix/pthread/pthread_mutexattr.c`
    *   `src/system/kernel/locks/user_mutex.cpp` (kernel support for robust mutexes)

*   **Sysconf Feature Macros**:
    *   `src/system/libroot/posix/unistd/conf.cpp` (add cases to `__sysconf`)
    *   `headers/posix/unistd.h` (define constants)

*   **New Subsystems (AIO, MQueue, SysV SHM)**:
    *   Would require creating new directories and files under `src/system/kernel/` and `src/system/libroot/posix/`.

*   **Iconv/Intl**:
    *   Would likely involve importing 3rd party code (like `libiconv` or `gettext`) into `src/system/libroot/posix/glibc/` or similar locations, or linking against existing system libraries.
