#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 186

/* MAX_BUF_SIZE is set to 500 because fib(500) with 106 digits
 */
#define MAX_BUF_SIZE 106

typedef unsigned __int128 u128_t;

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);

static ktime_t kt;
static u_int64_t kt_ns;

static struct kobject *logger;

static ssize_t kt_show(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       char *buf)
{
    kt_ns = ktime_to_ns(kt);
    return snprintf(buf, 16, "%lld", kt_ns);
}

static ssize_t kt_store(struct kobject *kobj,
                        struct kobj_attribute *attr,
                        const char *buf,
                        size_t count)
{
    return 0;  // do nothing
}

static struct kobj_attribute profiler = __ATTR(kt_ns, 0664, kt_show, kt_store);
static struct attribute *attrs[] = {
    &profiler.attr,
    NULL,  // need to NULL terminate the list of attributes
};
static struct attribute_group attr_group = {
    .attrs = attrs,
};

static inline char *strrev(char *str)
{
    char *p1, *p2;

    if (!str || !*str)
        return str;
    for (p1 = str, p2 = str + strlen(str) - 1; p2 > p1; ++p1, --p2) {
        *p1 ^= *p2;
        *p2 ^= *p1;
        *p1 ^= *p2;
    }
    return str;
}

static char *to_string(u128_t n)
{
    char *ret = kmalloc(MAX_BUF_SIZE, GFP_USER), *ret_ptr = ret;

    do {
        *(ret_ptr++) = n % 10 + '0';
        n /= 10;
    } while (n);

    return strrev(ret);
}

static char *fib_sequence(long long k)
{
    u128_t *f = kmalloc(sizeof(u128_t) * (k + 2), GFP_USER);

    f[0] = 0;
    f[1] = 1;

    for (int i = 2; i <= k; i++) {
        f[i] = f[i - 1] + f[i - 2];
    }

    u128_t ret = f[k];
    kfree(f);

    return to_string(ret);
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    ssize_t bytes_read = 0;
    kt = ktime_get();
    char *msg = fib_sequence(*offset), *msg_ptr = msg;
    kt = ktime_sub(ktime_get(), kt);

    while (size && *msg_ptr) {
        put_user(*(msg_ptr++), buf++);
        bytes_read++;
        size--;
    }
    put_user('\0', buf);

    kfree(msg);
    return bytes_read;
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }

    logger = kobject_create_and_add("fib_logger", kernel_kobj);
    if (!logger) {
        printk(KERN_ALERT "Failed to create logger");
        rc = -ENOMEM;
        goto failed_logger_create;
    }

    if ((rc = sysfs_create_group(logger, &attr_group))) {
        printk(KERN_ALERT "Failed to create sysfs");
        goto failed_create_sysfs;
    }

    return rc;
failed_create_sysfs:
    kobject_put(logger);
failed_logger_create:
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
    kobject_put(logger);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
