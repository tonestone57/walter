#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main() {
    long id = gethostid();
    printf("Initial hostid: %ld\n", id);

    if (sethostid(12345) != 0) {
        printf("sethostid failed, errno=%d\n", errno);
        if (errno == EPERM || errno == EACCES) {
            printf("sethostid failed with EPERM/EACCES (expected if not root)\n");
        } else {
            perror("sethostid");
            return 1;
        }
    } else {
        printf("sethostid succeeded\n");
        long new_id = gethostid();
        printf("New hostid: %ld\n", new_id);
        if (new_id != 12345) {
            fprintf(stderr, "Error: Host ID not updated!\n");
            return 1;
        }
    }
    return 0;
}
