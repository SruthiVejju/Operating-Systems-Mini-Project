# Operating Systems - Mini Project

## Project Title
**Multi-Container Runtime with Kernel Memory Monitoring**

## Team Information
- **Team Member 1:** _[Name / SRN]_
- **Team Member 2:** _[Name / SRN]_

---

## 1. Project Overview

This project builds a lightweight Linux container runtime in C with:

- a **long-running supervisor**
- **multiple containers** running at the same time
- a **CLI** for container management
- **bounded-buffer logging**
- a **Linux kernel module** for RSS-based memory monitoring and enforcement

The user-space runtime creates isolated container environments using Linux namespaces and chroot, while the kernel module tracks container processes through `ioctl` and enforces soft and hard memory limits.

---

## 2. Main Features

- Start, run, stop, and inspect containers
- Manage multiple containers concurrently
- Isolated PID, UTS, and mount namespaces
- Separate writable root filesystem per container
- `/proc` support inside container
- Safe logging through pipes and a bounded buffer
- Kernel-side process tracking using a character device and `ioctl`
- Soft-limit warning and hard-limit kill behavior
- Scheduler experiment support using different workloads and priorities

---

## 3. Repository Contents

Expected main files:

- `engine.c` — user-space runtime / supervisor / CLI
- `monitor.c` — kernel module
- `monitor_ioctl.h` — shared ioctl definitions
- `Makefile` — build instructions
- `mem_test.c` — memory stress test
- `test.c` — ioctl test program
- `README.md` — this file

---

## 4. Environment Requirements

Use:

- **Ubuntu 22.04 or 24.04**
- **VirtualBox VM**
- **Secure Boot OFF**
- Kernel headers installed

### Install dependencies
```bash
sudo apt update
sudo apt install -y build-essential dkms linux-headers-$(uname -r)
```

---

## 5. Build Instructions

### Build the kernel module
```bash
make
```

### Build the user-space engine
```bash
gcc engine.c -o engine
```

### Clean build files
```bash
make clean
rm -f engine mem_test test
```

---

## 6. Kernel Module Setup

### Load module
```bash
sudo insmod monitor.ko
```

### Verify device
```bash
ls /dev/container_monitor
```

### Check kernel logs
```bash
sudo dmesg | tail
```

### Unload module
```bash
sudo rmmod monitor
```

---

## 7. Running the Project

### Start the supervisor / engine
```bash
sudo ./engine
```

### Run a memory stress test inside container
```bash
python3 -c "a=[]; [a.append('A'*1000000) for _ in range(50)]"
```

### Check kernel output
```bash
sudo dmesg | tail
```

Expected kernel output includes messages similar to:

- `Added PID: ... | Soft: ... | Hard: ...`
- `PID ... exceeded SOFT ...`
- `PID ... exceeded HARD ... -> KILLING`

---

## 8. CLI / Runtime Commands

The intended runtime command style is:

```bash
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

---

## 9. Design Summary

### User-space side
- creates containers
- manages container lifecycle
- connects to kernel module using `ioctl`
- tracks container metadata
- handles logging and cleanup

### Kernel-side
- creates `/dev/container_monitor`
- stores monitored process data
- checks RSS periodically
- sends warnings for soft limit
- kills process for hard limit

---

## 10. Team Work Split

### Team Member 1
Responsible for:
- `engine.c`
- container creation
- CLI / supervisor logic
- rootfs handling
- namespace setup
- log handling

### Team Member 2
Responsible for:
- `monitor.c`
- `monitor_ioctl.h`
- device creation
- ioctl handling
- kernel linked list
- RSS monitoring and enforcement

---

## 11. Demo Checklist

For submission screenshots, include:

- Supervisor running
- Multiple containers tracked
- `ps` showing metadata
- Log output
- `dmesg` showing soft-limit warning
- `dmesg` showing hard-limit kill
- Scheduler experiment output
- Clean teardown

---

## 12. Cleanup

Before final submission:

```bash
make clean
rm -f engine mem_test test
sudo rmmod monitor
```

---

## 13. Troubleshooting

### `insmod: File exists`
The module is already loaded. Remove it first:
```bash
sudo rmmod monitor
sudo insmod monitor.ko
```

### `Permission denied` while pushing to GitHub
Use a GitHub **Personal Access Token** instead of your password.

### `dmesg: Operation not permitted`
Use:
```bash
sudo dmesg | tail
```

---

## 14. Notes

This project was implemented as part of an Operating Systems mini-project to demonstrate:
- container isolation
- kernel-user communication
- process tracking
- memory monitoring
- scheduling behavior
- resource cleanup

