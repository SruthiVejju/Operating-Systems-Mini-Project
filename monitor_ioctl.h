#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#define IOCTL_ADD_PROCESS _IOW('a', 'a', struct process_info *)

struct process_info {
    int pid;
    int soft_limit;
    int hard_limit;
};

#endif
