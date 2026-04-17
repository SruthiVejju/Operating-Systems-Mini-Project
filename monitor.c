#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FINAL Container Monitor");

// ===================== GLOBALS =====================

static int major;
static struct class* cls = NULL;
static struct device* dev = NULL;
static struct task_struct *monitor_thread;

// ===================== LINKED LIST =====================

struct process_node {
    struct process_info data;
    struct list_head list;
};

static LIST_HEAD(process_list);

// ===================== DEVICE =====================

static int dev_open(struct inode *inodep, struct file *filep) {
    return 0;
}

static int dev_release(struct inode *inodep, struct file *filep) {
    return 0;
}

// ===================== IOCTL =====================

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {

    switch(cmd) {

        case IOCTL_ADD_PROCESS: {
            struct process_node *new_node;

            new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
            if (!new_node) return -ENOMEM;

            if (copy_from_user(&new_node->data,
                               (struct process_info *)arg,
                               sizeof(struct process_info))) {
                kfree(new_node);
                return -EFAULT;
            }

            INIT_LIST_HEAD(&new_node->list);
            list_add(&new_node->list, &process_list);

            printk(KERN_INFO "Added PID: %d | Soft: %d | Hard: %d\n",
                   new_node->data.pid,
                   new_node->data.soft_limit,
                   new_node->data.hard_limit);

            break;
        }

        default:
            return -EINVAL;
    }

    return 0;
}

// ===================== FILE OPS =====================

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .unlocked_ioctl = dev_ioctl,
};

// ===================== MONITOR THREAD =====================

static int monitor_fn(void *data) {

    while (!kthread_should_stop()) {

        struct process_node *entry;

        list_for_each_entry(entry, &process_list, list) {

            struct task_struct *container_task;
            struct task_struct *p;

            container_task = pid_task(find_vpid(entry->data.pid), PIDTYPE_PID);
            if (!container_task)
                continue;

            // 🔥 iterate all processes
            for_each_process(p) {

                // Check if process belongs to container (same parent)
                if (p == container_task || p->real_parent == container_task) {

                    if (!p->mm)
                        continue;

                    unsigned long rss_pages = get_mm_rss(p->mm);
                    unsigned long rss_mb = (rss_pages * PAGE_SIZE) / (1024 * 1024);

                    // Soft limit
                    if (rss_mb > entry->data.soft_limit) {
                        printk(KERN_WARNING "PID %d exceeded SOFT (%lu MB)\n",
                               p->pid, rss_mb);
                    }

                    // Hard limit
                    if (rss_mb > entry->data.hard_limit) {

                        printk(KERN_ALERT "PID %d exceeded HARD (%lu MB) → KILLING\n",
                               p->pid, rss_mb);

                        send_sig(SIGKILL, p, 0);
                    }
                }
            }
        }

        msleep(500);
    }

    return 0;
}

// ===================== INIT =====================

static int __init monitor_init(void) {

    printk(KERN_INFO "FINAL: Module Loaded\n");

    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0)
        return major;

    cls = class_create(DEVICE_NAME);
    if (IS_ERR(cls))
        return PTR_ERR(cls);

    dev = device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(dev))
        return PTR_ERR(dev);

    monitor_thread = kthread_run(monitor_fn, NULL, "monitor_thread");

    printk(KERN_INFO "Monitoring thread started\n");
    return 0;
}

// ===================== EXIT =====================

static void __exit monitor_exit(void) {

    struct process_node *entry, *tmp;

    if (monitor_thread)
        kthread_stop(monitor_thread);

    list_for_each_entry_safe(entry, tmp, &process_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "FINAL: Module Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
