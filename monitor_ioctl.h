#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#define IOCTL_MAGIC 'k'
#define IOCTL_ADD_PROCESS    _IOW(IOCTL_MAGIC, 1, struct process_info)
#define IOCTL_REMOVE_PROCESS _IOW(IOCTL_MAGIC, 2, struct process_info)

struct process_info {
    int pid;
    int soft_limit;   /* MB */
    int hard_limit;   /* MB */
};

#endif
