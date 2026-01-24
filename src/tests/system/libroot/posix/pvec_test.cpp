#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void check(ssize_t ret, const char* msg) {
    if (ret == -1) {
        perror(msg);
        exit(1);
    }
}

int main() {
    int fd = open("test_pvec", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    char buf1[] = "Hello ";
    char buf2[] = "World";
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = strlen(buf1);
    iov[1].iov_base = buf2;
    iov[1].iov_len = strlen(buf2);

    printf("Testing pwritev at offset 5...\n");
    // Write "Hello World" at offset 5
    ssize_t written = pwritev(fd, iov, 2, 5);
    check(written, "pwritev");
    if (written != 11) {
        fprintf(stderr, "pwritev wrote %ld bytes, expected 11\n", written);
        return 1;
    }

    char read_buf1[6];
    char read_buf2[5];
    struct iovec riov[2];
    riov[0].iov_base = read_buf1;
    riov[0].iov_len = 6;
    riov[1].iov_base = read_buf2;
    riov[1].iov_len = 5;

    printf("Testing preadv at offset 5...\n");
    // Read "Hello World" from offset 5
    ssize_t read_bytes = preadv(fd, riov, 2, 5);
    check(read_bytes, "preadv");
    if (read_bytes != 11) {
        fprintf(stderr, "preadv read %ld bytes, expected 11\n", read_bytes);
        return 1;
    }

    if (memcmp(read_buf1, "Hello ", 6) != 0 || memcmp(read_buf2, "World", 5) != 0) {
        fprintf(stderr, "Data mismatch!\n");
        return 1;
    }

    printf("Verifying file position did not change...\n");
    if (lseek(fd, 0, SEEK_CUR) != 0) {
        fprintf(stderr, "File offset changed! preadv/pwritev should not change offset.\n");
        return 1;
    }

    close(fd);
    unlink("test_pvec");
    printf("preadv/pwritev tests passed.\n");
    return 0;
}
