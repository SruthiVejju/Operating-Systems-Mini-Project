#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

int main() {
    int fd;
    struct process_info p;

    // Open device
    fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    // Fill data
    p.pid = 6512;
    p.soft_limit = 10;
    p.hard_limit = 20;

    // Send to kernel
    if (ioctl(fd, IOCTL_ADD_PROCESS, &p) < 0) {
        perror("ioctl failed");
        close(fd);
        return 1;
    }

    printf("Data sent to kernel\n");

    close(fd);
    return 0;
}
