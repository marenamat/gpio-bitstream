/*
 *	GPIO Bitstream
 *
 *	(c) 2022 Maria Matejka <mq@jmq.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/timekeeping.h>

struct gpio_bitstream_device {
	struct gpio_desc *gpio;
	struct device *dev;
	struct cdev cdev;
	dev_t chrdev_id;
	struct mutex lock;
	wait_queue_head_t write_queue;
	struct hrtimer timer;
	uint64_t delay_ns;
	uint64_t next_ns;
	uint32_t byte_begin;
	uint32_t byte_end;
	uint32_t byte_max;
	uint32_t flags;
	char *data;
};

#define GPIO_BITSTREAM_ACTIVE	0x200
#define GPIO_BITSTREAM_CORKED	0x100
#define GPIO_BITSTREAM_LASTBIT	0x0ff

static enum hrtimer_restart gpio_bitstream_send_bit(struct hrtimer *timer)
{
	struct gpio_bitstream_device *bdev = container_of(timer, struct gpio_bitstream_device, timer);

	ktime_t now = ktime_get();
	uint16_t bit;
	int overrun;

	mutex_lock(&bdev->lock);
       
	bit = ((bdev->flags << 1) & GPIO_BITSTREAM_LASTBIT) ?: 1;

	bdev->flags &= ~GPIO_BITSTREAM_LASTBIT;
	bdev->flags |= bit;

	gpiod_set_value(bdev->gpio, !!(bdev->data[bdev->byte_begin] & bit));

	if (bit == 0x80) {
		bdev->byte_begin++;
		bdev->byte_begin %= bdev->byte_max;
		if ((bdev->flags & GPIO_BITSTREAM_CORKED) && ((bdev->byte_end + bdev->byte_max - bdev->byte_begin) % bdev->byte_max < bdev->byte_max / 4)) {
			bdev->flags &= ~GPIO_BITSTREAM_CORKED;
			wake_up(&bdev->write_queue);
		} else if (bdev->byte_begin == bdev->byte_end) {
			mutex_unlock(&bdev->lock);
			return HRTIMER_NORESTART;
		}
	}

	overrun = hrtimer_forward(&bdev->timer, now, ns_to_ktime(bdev->delay_ns));
	if (overrun != 1)
		dev_err(bdev->dev, "overrun is not one but %d\n", overrun);

	bdev->next_ns += overrun * bdev->delay_ns;

	mutex_unlock(&bdev->lock);
	return HRTIMER_RESTART;
}

static ssize_t gpio_bitstream_write(struct file *fp, const char __user *buf, size_t sz, loff_t *off)
{
	struct gpio_bitstream_device *bdev = fp->private_data;
	uint64_t bs;
	void *pos;

	if (mutex_lock_interruptible(&bdev->lock))
		return -ERESTARTSYS;

	/* Bitrate not set */
	if (!bdev->delay_ns) {
		mutex_unlock(&bdev->lock);
		return -EINVAL;
	}

	/* Not enough space */
	if (bdev->flags & GPIO_BITSTREAM_CORKED) {
		mutex_unlock(&bdev->lock);

		/* Nonblocking. Call us again */
		if (fp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* Block until something happens */
		return wait_event_interruptible(bdev->write_queue,
					(fp->private_data != bdev) ||
					((bdev->flags & GPIO_BITSTREAM_CORKED) == 0))
			? -ERESTARTSYS : gpio_bitstream_write(fp, buf, sz, off);
	}

	/* Fill the buffer */
	if (bdev->byte_end < bdev->byte_max) {
		bs = bdev->byte_max - bdev->byte_end;
		pos = bdev->data + bdev->byte_end;
	} else {
		bs = bdev->byte_begin;
		pos = bdev->data;
	}

	/* Do not copy more than given to us */
	if (bs > sz)
		bs = sz;

	if (!copy_from_user(pos, buf, bs)) {
		mutex_unlock(&bdev->lock);
		return -EFAULT;
	}

	bdev->byte_end += bs;
	bdev->byte_end %= bdev->byte_max;
	
	if (bdev->byte_end == bdev->byte_begin)
		bdev->flags |= GPIO_BITSTREAM_CORKED;

	if (bdev->flags & GPIO_BITSTREAM_ACTIVE) {
		mutex_unlock(&bdev->lock);
		return bs;
	}

	bdev->next_ns = ktime_get_ns() + bdev->delay_ns;

	hrtimer_init(&bdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	bdev->timer.function = gpio_bitstream_send_bit;
	hrtimer_start(&bdev->timer, ns_to_ktime(bdev->next_ns), HRTIMER_MODE_ABS);

	mutex_unlock(&bdev->lock);
	return bs;
}

static __poll_t gpio_bitstream_poll(struct file *fp, struct poll_table_struct *wait)
{
	struct gpio_bitstream_device *bdev = fp->private_data;
	__poll_t mask;
	
	if (mutex_lock_interruptible(&bdev->lock))
		return -ERESTARTSYS;

	poll_wait(fp, &bdev->write_queue, wait);

	if (bdev->flags & GPIO_BITSTREAM_CORKED)
		mask = 0;
	else
		mask = POLLIN | POLLRDNORM;

	mutex_unlock(&bdev->lock);
	return mask;
}

static int gpio_bitstream_open(struct inode *inode, struct file *fp)
{
	struct gpio_bitstream_device *bdev = container_of(inode->i_cdev, struct gpio_bitstream_device, cdev);
	
	if (!try_module_get(THIS_MODULE))
		return -EIO;

	fp->private_data = bdev;
	return 0;
}

static int gpio_bitstream_release(struct inode *inode, struct file *fp)
{
	if (!fp->private_data)
		return -EINVAL;

	module_put(THIS_MODULE);
	return 0;
}

static struct file_operations gpio_bitstream_ops = {
	.owner = THIS_MODULE,
	.write = gpio_bitstream_write,
	.poll = gpio_bitstream_poll,
	.open = gpio_bitstream_open,
	.release = gpio_bitstream_release,
};

static int gpio_bitstream_set_delay(struct gpio_bitstream_device *bdev, uint64_t delay_ns)
{
	if (mutex_lock_interruptible(&bdev->lock))
		return -ERESTARTSYS;

	bdev->delay_ns = delay_ns;
	mutex_unlock(&bdev->lock);
	return 0;
}

static ssize_t bps_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	uint64_t bps;

	if (mutex_lock_interruptible(&bdev->lock))
		return -ERESTARTSYS;

	bps = 1000 * 1000 * 1000ULL + bdev->delay_ns / 2;
	bps /= bdev->delay_ns;
	
	mutex_unlock(&bdev->lock);
	return sprintf(buf, "%Lu\n", bps);
}

static ssize_t bps_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	uint64_t bps, delay_ns;

	if (!sscanf(buf, "%Lu", &bps))
		return -EINVAL;

	if (!bps)
		return -EINVAL;

	delay_ns = 1000 * 1000 * 1000ULL + bps/2;
	delay_ns /= bps;

	if (gpio_bitstream_set_delay(bdev, delay_ns))
		return -ERESTARTSYS;
	
	return 0;
}
static DEVICE_ATTR_RW(bps);

static ssize_t bpy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	uint64_t bpy;

	if (mutex_lock_interruptible(&bdev->lock))
		return -ERESTARTSYS;

	bpy = 365ULL * 86400 * 1000 * 1000 * 1000ULL + bdev->delay_ns / 2;
	bpy /= bdev->delay_ns;
	
	mutex_unlock(&bdev->lock);
	return sprintf(buf, "%Lu\n", bpy);
}

static ssize_t bpy_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	uint64_t bpy, delay_ns;

	if (!sscanf(buf, "%Lu", &bpy))
		return -EINVAL;

	if (!bpy)
		return -EINVAL;

	delay_ns = 365ULL * 86400 * 1000 * 1000 * 1000ULL + bpy/2;
	delay_ns /= bpy;

	if (gpio_bitstream_set_delay(bdev, delay_ns))
		return -ERESTARTSYS;
	
	return 0;
}
static DEVICE_ATTR_RW(bpy);

static ssize_t delay_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	uint64_t delay_ns;

	if (mutex_lock_interruptible(&bdev->lock))
		return -ERESTARTSYS;

	delay_ns = bdev->delay_ns;
	
	mutex_unlock(&bdev->lock);

	return sprintf(buf, "%Lu\n", delay_ns);
}
static ssize_t delay_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	uint64_t delay_ns;

	if (!sscanf(buf, "%Lu", &delay_ns))
		return -EINVAL;

	if (!delay_ns)
		return -EINVAL;

	if (gpio_bitstream_set_delay(bdev, delay_ns))
		return -ERESTARTSYS;
	
	return 0;
}
static DEVICE_ATTR_RW(delay);

static struct attribute *gpio_bitstream_attrs[] = {
	&dev_attr_bps.attr,
	&dev_attr_bpy.attr,
	&dev_attr_delay.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gpio_bitstream);

static struct device_type gpio_bitstream_type = {
	.groups = gpio_bitstream_groups,
};

static int gpio_bitstream_probe(struct platform_device *pdev)
{
	int e;
	struct gpio_bitstream_device *bdev;

	pdev->dev.type = &gpio_bitstream_type;
       
	bdev = devm_kzalloc(&pdev->dev, sizeof(*bdev), GFP_KERNEL);
	if (!bdev)
		return -ENOMEM;

	bdev->data = (void *)__get_free_page(GFP_KERNEL);
	if (!bdev->data)
		return -ENOMEM;

	bdev->dev = &pdev->dev;
	mutex_init(&bdev->lock);
	init_waitqueue_head(&bdev->write_queue);

	bdev->gpio = devm_gpiod_get_index(bdev->dev, NULL, 0, GPIOD_OUT_LOW);
	if (IS_ERR(bdev->gpio)) {
		free_page((long) bdev->data);
		return PTR_ERR(bdev->gpio);
	}

	cdev_init(&bdev->cdev, &gpio_bitstream_ops);
	bdev->cdev.owner = THIS_MODULE;

	e = alloc_chrdev_region(&bdev->chrdev_id, 0, 1, "gpio_bitstream");
	if (e) {
		free_page((long) bdev->data);
		return e;
	}

	e = cdev_add(&bdev->cdev, bdev->chrdev_id, 1);
	if (e) {
		free_page((long) bdev->data);
		unregister_chrdev_region(bdev->chrdev_id, 1);
		return e;
	}

	platform_set_drvdata(pdev, bdev);

	return 0;
}

static void gpio_bitstream_shutdown(struct platform_device *pdev)
{
	struct gpio_bitstream_device *bdev = platform_get_drvdata(pdev);

	cdev_del(&bdev->cdev);
	unregister_chrdev_region(bdev->chrdev_id, 1);
	free_page((long) bdev->data);
}

static const struct of_device_id of_gpio_bitstream_match[] = {
	{ .compatible = "gpio-bitstream", },
	{},
};
MODULE_DEVICE_TABLE(of, of_gpio_bitstream_match);

static struct platform_driver gpio_bitstream_driver = {
	.probe = gpio_bitstream_probe,
	.shutdown = gpio_bitstream_shutdown,
	.driver = {
		.name = "gpio-bitstream",
		.of_match_table = of_match_ptr(of_gpio_bitstream_match),
	},
};

int __init gpio_bitstream_init(void)
{
	int result;

	result = platform_driver_register(&gpio_bitstream_driver);
	if (result)
		return result;

	return 0;
}

void __exit gpio_bitstream_exit(void)
{
	platform_driver_unregister(&gpio_bitstream_driver);
}

module_init(gpio_bitstream_init);
module_exit(gpio_bitstream_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maria Matejka <mq@jmq.cz>");
MODULE_DESCRIPTION("gpio-bitstream");

/* vim: set ts=8 sw=8 sts=8: */
