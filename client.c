#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FIB_DEV "/dev/fibonacci"
#define MAX_BUF_SIZE 106  // fib(500)

static inline u_int64_t get_nanotime()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec * 1e9 + t.tv_nsec;
}

int main()
{
    long long sz;

    char buf[MAX_BUF_SIZE];
    char write_buf[] = "testing writing";
    int offset = 200; /* TODO: try test something bigger than the limit */

    FILE *perf_log = fopen("userspace.log", "w");
    if (!perf_log) {
        perror("Failed to open userspace.log");
        exit(1);
    }

    int fd = open(FIB_DEV, O_RDWR);
    if (fd < 0) {
        perror("Failed to open character device");
        exit(1);
    }

    for (int i = 0; i <= offset; i++) {
        sz = write(fd, write_buf, strlen(write_buf));
        printf("Writing to " FIB_DEV ", returned the sequence %lld\n", sz);
    }

    for (int i = 0; i <= offset; i++) {
        lseek(fd, i, SEEK_SET);
        u_int64_t start = get_nanotime();
        sz = read(fd, buf, MAX_BUF_SIZE);
        u_int64_t user = get_nanotime() - start;
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%s.\n",
               i, buf);
        fprintf(perf_log, "%d %lld\n", i, user);
    }

    for (int i = offset; i >= 0; i--) {
        lseek(fd, i, SEEK_SET);
        sz = read(fd, buf, MAX_BUF_SIZE);
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%s.\n",
               i, buf);
    }

    close(fd);
    return 0;
}
