#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)

char stack[STACK_SIZE];

int child_func(void *arg) {
    printf("Inside container! PID = %d\n", getpid());

    sethostname("MyContainer", 12);

    chroot("/home/shreekrishna/container_root");
    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    execlp("/bin/bash", "bash", NULL);

    perror("execlp failed");
    return 1;
}

int main() {
    printf("Starting container...\n");

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS;

    int pid = clone(child_func, stack + STACK_SIZE, flags | SIGCHLD, NULL);

    if (pid == -1) {
        perror("clone failed");
        exit(1);
    }

    //  Wait properly
    sleep(2);

    //  Open device
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        exit(1);
    }

    struct process_info p;

    // IMPORTANT: still send host PID (correct for kernel lookup)
    p.pid = pid;

    //  LOW limits for guaranteed trigger
    p.soft_limit = 5;
    p.hard_limit = 10;

    if (ioctl(fd, IOCTL_ADD_PROCESS, &p) < 0) {
        perror("ioctl failed");
        close(fd);
        exit(1);
    }

    printf("Sent PID %d to kernel monitor\n", pid);

    close(fd);

    waitpid(pid, NULL, 0);

    printf("Container exited\n");
    return 0;
}
