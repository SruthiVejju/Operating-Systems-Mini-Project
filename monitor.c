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
#include <linux/mutex.h>
#include <linux/pid.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container Memory Monitor with Soft/Hard Limits");

/* ===================== GLOBALS ===================== */
static int major;
static struct class  *cls = NULL;
static struct device *dev = NULL;
static struct task_struct *monitor_thread;

/* ===================== LINKED LIST ===================== */
struct process_node {
    struct process_info data;
    int soft_warned;
    struct list_head list;
};

static LIST_HEAD(process_list);
static DEFINE_MUTEX(list_mutex);

/* ===================== DEVICE ===================== */
static int dev_open(struct inode *inodep, struct file *filep) { return 0; }
static int dev_release(struct inode *inodep, struct file *filep) { return 0; }

/* ===================== IOCTL ===================== */
static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct process_info info;

    if (copy_from_user(&info, (struct process_info __user *)arg, sizeof(info)))
        return -EFAULT;

    switch (cmd) {

    case IOCTL_ADD_PROCESS: {
        struct process_node *node = kmalloc(sizeof(*node), GFP_KERNEL);
        if (!node) return -ENOMEM;

        node->data = info;
        node->soft_warned = 0;
        INIT_LIST_HEAD(&node->list);

        mutex_lock(&list_mutex);
        list_add_tail(&node->list, &process_list);
        mutex_unlock(&list_mutex);

        printk(KERN_INFO "monitor: ADD PID=%d soft=%dMB hard=%dMB\n",
               info.pid, info.soft_limit, info.hard_limit);
        break;
    }

    case IOCTL_REMOVE_PROCESS: {
        struct process_node *entry, *tmp;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &process_list, list) {
            if (entry->data.pid == info.pid) {
                list_del(&entry->list);
                kfree(entry);
                printk(KERN_INFO "monitor: REMOVE PID=%d\n", info.pid);
                break;
            }
        }
        mutex_unlock(&list_mutex);
        break;
    }

    default:
        return -EINVAL;
    }

    return 0;
}

/* ===================== FILE OPS ===================== */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = dev_open,
    .release        = dev_release,
    .unlocked_ioctl = dev_ioctl,
};

/* ===================== MONITOR THREAD ===================== */
static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {

        struct process_node *entry, *tmp;

        mutex_lock(&list_mutex);

        list_for_each_entry_safe(entry, tmp, &process_list, list) {

            struct pid *pid_struct;
            struct task_struct *task;
            unsigned long rss_mb;

            pid_struct = find_get_pid(entry->data.pid);
            task = get_pid_task(pid_struct, PIDTYPE_PID);
            put_pid(pid_struct);  // FIX: prevent leak

            if (!task) {
                printk(KERN_INFO "monitor: PID %d gone\n", entry->data.pid);
                list_del(&entry->list);
                kfree(entry);
                continue;
            }

            if (!task->mm) {
                put_task_struct(task);
                continue;
            }

            rss_mb = (get_mm_rss(task->mm) * PAGE_SIZE) >> 20;

            printk(KERN_INFO "monitor: PID=%d RSS=%lu MB\n",
                   entry->data.pid, rss_mb);

            /* SOFT LIMIT */
            if (rss_mb > entry->data.soft_limit && !entry->soft_warned) {
                printk(KERN_WARNING
                       "monitor: SOFT LIMIT PID=%d (%luMB > %dMB)\n",
                       entry->data.pid, rss_mb, entry->data.soft_limit);
                entry->soft_warned = 1;
            }

            /* HARD LIMIT */
            if (rss_mb > entry->data.hard_limit) {
                printk(KERN_ALERT
                       "monitor: HARD LIMIT PID=%d (%luMB > %dMB) KILLING\n",
                       entry->data.pid, rss_mb, entry->data.hard_limit);

                send_sig(SIGKILL, task, 0);

                list_del(&entry->list);
                kfree(entry);

                put_task_struct(task);
                continue;
            }

            put_task_struct(task);
        }

        mutex_unlock(&list_mutex);

        ssleep(2);  // faster checking
    }

    return 0;
}

/* ===================== INIT ===================== */
static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) return major;

    cls = class_create(DEVICE_NAME);
    if (IS_ERR(cls)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(cls);
    }

    dev = device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        class_destroy(cls);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(dev);
    }

    monitor_thread = kthread_run(monitor_fn, NULL, "container_monitor");
    if (IS_ERR(monitor_thread)) {
        device_destroy(cls, MKDEV(major, 0));
        class_destroy(cls);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(monitor_thread);
    }

    printk(KERN_INFO "monitor: LOADED\n");
    return 0;
}

/* ===================== EXIT ===================== */
static void __exit monitor_exit(void)
{
    struct process_node *entry, *tmp;

    kthread_stop(monitor_thread);

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &process_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "monitor: UNLOADED\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
