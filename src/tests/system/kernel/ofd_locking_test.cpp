#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

#ifndef F_OFD_SETLK
#define F_OFD_GETLK		36
#define F_OFD_SETLK		37
#define F_OFD_SETLKW	38
#endif

void check(int ret, const char* msg) {
    if (ret == -1) {
        perror(msg);
        exit(1);
    }
}

void test_ofd_basic() {
    printf("Testing basic OFD lock...\n");
    int fd = open("test_ofd.lock", O_CREAT | O_RDWR, 0666);
    check(fd, "open");

    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 10;
    fl.l_pid = 0;

    check(fcntl(fd, F_OFD_SETLK, &fl), "F_OFD_SETLK");

    // Check if we can place another lock on same fd (should succeed as it is same description)
    check(fcntl(fd, F_OFD_SETLK, &fl), "F_OFD_SETLK 2");

    close(fd);
    printf("Basic OFD lock passed.\n");
}

void test_ofd_conflict_diff_fd() {
    printf("Testing OFD conflict with different FD (same file)...\n");
    int fd1 = open("test_ofd.lock", O_CREAT | O_RDWR, 0666);
    int fd2 = open("test_ofd.lock", O_CREAT | O_RDWR, 0666);
    check(fd1, "open 1");
    check(fd2, "open 2");

    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 10;
    fl.l_pid = 0;

    check(fcntl(fd1, F_OFD_SETLK, &fl), "fd1 lock");

    // fd2 should fail to lock
    if (fcntl(fd2, F_OFD_SETLK, &fl) == 0) {
        fprintf(stderr, "Error: fd2 locked but should conflict with fd1!\n");
        exit(1);
    } else if (errno != EAGAIN && errno != EACCES) {
        perror("fd2 lock unexpected error");
        exit(1);
    }

    close(fd1);
    close(fd2);
    printf("OFD conflict diff FD passed.\n");
}

void test_ofd_conflict_posix_same_process() {
    printf("Testing OFD vs POSIX conflict (same process)...\n");
    int fd = open("test_ofd.lock", O_CREAT | O_RDWR, 0666);
    check(fd, "open");

    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 10;
    fl.l_pid = 0;

    // Place POSIX lock
    check(fcntl(fd, F_SETLK, &fl), "POSIX lock");

    // Try to place OFD lock (should fail)
    if (fcntl(fd, F_OFD_SETLK, &fl) == 0) {
        fprintf(stderr, "Error: OFD lock succeeded but should conflict with POSIX lock in same process!\n");
        exit(1);
    }

    // Release POSIX
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);

    // Place OFD lock
    fl.l_type = F_WRLCK;
    check(fcntl(fd, F_OFD_SETLK, &fl), "OFD lock");

    // Try to place POSIX lock (should fail)
    if (fcntl(fd, F_SETLK, &fl) == 0) {
        fprintf(stderr, "Error: POSIX lock succeeded but should conflict with OFD lock in same process!\n");
        exit(1);
    }

    close(fd);
    printf("OFD vs POSIX conflict (same process) passed.\n");
}

int main() {
    test_ofd_basic();
    test_ofd_conflict_diff_fd();
    test_ofd_conflict_posix_same_process();

    unlink("test_ofd.lock");
    return 0;
}
