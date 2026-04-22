# Operating Systems - Mini Project

## Project Title
Multi-Container Runtime with Kernel Memory Monitoring

## Team Information
Team Member 1: Sruthi Vejju [PES1UG24CS472]  
Team Member 2: Shree Krishna [PES1UG24CS440]

---

## 1. Project Overview

This project builds a lightweight Linux container runtime in C with:

- a long-running supervisor
- multiple containers running at the same time
- a CLI for container management
- bounded-buffer logging
- a Linux kernel module for RSS-based memory monitoring and enforcement

The user-space runtime creates isolated container environments using Linux namespaces and `chroot`, while the kernel module tracks container processes through `ioctl` and enforces soft and hard memory limits.

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

- Ubuntu 22.04 or 24.04
- VirtualBox VM
- Secure Boot OFF
- Kernel headers installed

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential dkms linux-headers-$(uname -r)

---

## 5. Build Instructions

Build the project:

```bash
make

- Build the user-space engine (if needed):

```bash
gcc -pthread -o engine engine.c

- Clean build files:

```bash
make clean
rm -f engine mem_test test

---

## 6. Kernel Module Setup

Load module:

```bash
sudo insmod monitor.ko

Verify device:

ls -l /dev/container_monitor

Check kernel logs:

sudo dmesg | tail

Unload module:

sudo rmmod monitor
7. Running the Project

Start the supervisor:

sudo ./engine supervisor <base-rootfs>

Example:

sudo ./engine supervisor /

Start a container:

sudo ./engine start <id> <container-rootfs> <command> [soft_mb] [hard_mb]

Run a one-shot container:

sudo ./engine run <id> <container-rootfs> <command> [soft_mb] [hard_mb]

Example:

sudo ./engine run c1 / bin/ls

List containers:

sudo ./engine ps

View logs:

sudo ./engine logs <id>

Stop container:

sudo ./engine stop <id>
8. CLI / Runtime Commands
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [soft_mb] [hard_mb]
engine run <id> <container-rootfs> <command> [soft_mb] [hard_mb]
engine ps
engine logs <id>
engine stop <id>
9. Design Summary

User-space side:

Creates containers
Manages lifecycle
Communicates with kernel using ioctl
Tracks metadata
Handles logging and cleanup

Kernel-side:

Creates /dev/container_monitor
Stores process data
Monitors RSS
Triggers soft warnings
Enforces hard limits (kills process)
10. Team Work Split

Team Member 1:

engine.c
Container creation
CLI / supervisor
Rootfs handling
Namespace setup
Logging

Team Member 2:

monitor.c
monitor_ioctl.h
Device creation
ioctl handling
Kernel linked list
Memory monitoring
11. Demo Screenshots

⚠️ Create a folder named screenshots/ in your repo and place images there.

11.1 Supervisor running

11.2 Multiple containers

11.3 ps output

11.4 Logs output

11.5 Soft limit warning (dmesg)

11.6 Hard limit kill (dmesg)

11.7 Scheduler experiment

11.8 Clean teardown

12. Cleanup
make clean
rm -f engine mem_test test
sudo rmmod monitor
13. Troubleshooting

insmod: File exists
→ Module already loaded:

sudo rmmod monitor
sudo insmod monitor.ko

Permission denied (GitHub)
→ Use Personal Access Token instead of password

dmesg not permitted:

sudo dmesg | tail
14. Notes

This project demonstrates:

Container isolation using namespaces
Kernel-user communication using ioctl
Process monitoring and tracking
Memory enforcement
Scheduling behavior
Proper resource cleanup
