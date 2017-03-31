#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>

#define DEVICE_NAME "usb_mouse_interrupter"
#define USB_MOUSE_VENDOR_ID 0x0458
#define USB_MOUSE_DEVICE_ID 0x003a

static dev_t usb_mouse_dev;
static struct cdev usb_mouse_cdev;
static int l_cnt, r_cnt, m_cnt;
static int eof_flag;

struct my_usb_struct {
	char name[128];
	char phys[64];
	struct usb_device *udev;
	struct input_dev *idev;
	struct urb *irq;

	char *data;
};

// (VendorID, DeviceID)
static struct usb_device_id usb_ids[] = {
	{ USB_DEVICE(USB_MOUSE_VENDOR_ID, USB_MOUSE_DEVICE_ID), },
	{ 0, }
};
MODULE_DEVICE_TABLE(usb, usb_ids);

static int usb_open(struct input_dev *dev)
{
	struct my_usb_struct *mouse = input_get_drvdata(dev);

	mouse->irq->dev = mouse->udev;
	if (usb_submit_urb(mouse->irq, GFP_KERNEL))
		return -EIO;

	return 0;
}

static void usb_close(struct input_dev *dev)
{
	struct my_usb_struct *mouse = input_get_drvdata(dev);

	usb_kill_urb(mouse->irq);
}

static void usb_irq(struct urb *urb)
{
	struct my_usb_struct *mouse = urb->context;
	char *data = mouse->data;
	struct input_dev *dev = mouse->idev;
	int status;

	switch (urb->status) {
	case 0:			// все ОК
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	default:		//ошибка
		goto resubmit;
	}

	//разбор прерывания
	if (data[0] & 0x01) {
		pr_info("interrupt from mouse: left button\n");
		l_cnt++;
	}
	if (data[0] & 0x02) {
		pr_info("interrupt from mouse: right button\n");
		r_cnt++;
	}
	if (data[0] & 0x04) {
		pr_info("interrupt from mouse: middle button\n");
		m_cnt++;
	}

	//стандартный ввод мыши
	input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
	input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
	input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);

	input_report_rel(dev, REL_X,     data[1]);
	input_report_rel(dev, REL_Y,     data[2]);
	input_report_rel(dev, REL_WHEEL, data[3]);

	input_sync(dev);
resubmit:
	//пробуем зарегать urb еще раз
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status)
		dev_err(&mouse->udev->dev,
			"can't resubmit intr");
}

static int usb_probe(struct usb_interface *interface,
						const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct my_usb_struct *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct input_dev *input_dev;
	int pipe, maxp;

	l_cnt = 0;
	r_cnt = 0;
	m_cnt = 0;

	pr_info("USB_mouse_interr: in probe()\n");

	//проверяем устройство
	if (!udev) {
		pr_err("udev is NULL\n");
		return -ENODEV;
	}
	//находим endpoint, который входной
	//и interrupt
	//собираем информацию

	iface_desc = interface->cur_altsetting;

	if (iface_desc->desc.bNumEndpoints != 1) {
		pr_err("endpoints num != 1\n");
		return -ENODEV;
	}

	endpoint = &iface_desc->endpoint[0].desc;
	if (!usb_endpoint_is_int_in(endpoint)) {
		pr_err("endpoint isn't a int_in\n");
		return -ENODEV;
	}

	//труба для прерываний
	pipe = usb_rcvintpipe(udev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(udev, pipe, usb_pipeout(pipe));

	//выделяем память под данные
	dev = kzalloc(sizeof(struct my_usb_struct), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!dev || !input_dev) {
		input_free_device(input_dev);
		kfree(dev);
		pr_err("cannot allocate memory for struct my_usb_struct/input_dev");
		return -ENOMEM;
	}

	//выделяем data
	dev->data = kzalloc(8, GFP_ATOMIC);
	if (!dev->data) {
		input_free_device(input_dev);
		kfree(dev);
		pr_err("cannot allocate memory for data");
		return -ENOMEM;
	}

	//выделяем urb
	dev->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->irq) {
		kfree(dev->data);
		input_free_device(input_dev);
		kfree(dev);
		pr_err("cannot allocate memory for urb");
		return -ENOMEM;
	}

	//пишем в структуру
	dev->udev = udev;
	dev->idev = input_dev;

	//вытаскиваем данные из мышки
	if (udev->manufacturer)
		strlcpy(dev->name, udev->manufacturer, sizeof(dev->name));

	if (udev->product) {
		if (udev->manufacturer)
			strlcat(dev->name, " ", sizeof(dev->name));
		strlcat(dev->name, udev->product, sizeof(dev->name));
	}

	if (!strlen(dev->name))
		snprintf(dev->name, sizeof(dev->name),
				"USB HID Mouse %04x:%04x",
				le16_to_cpu(udev->descriptor.idVendor),
				le16_to_cpu(udev->descriptor.idProduct));

	usb_make_path(udev, dev->phys, sizeof(dev->phys));
	strlcat(dev->phys, "/input0", sizeof(dev->phys));

	//подготовка к регистрации
	input_dev->name = dev->name;
	input_dev->phys = dev->phys;
	usb_to_input_id(udev, &input_dev->id);
	input_dev->dev.parent = &interface->dev;

	//выборка срабатывания
	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
		BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
	input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] |= BIT_MASK(BTN_SIDE) |
		BIT_MASK(BTN_EXTRA);
	input_dev->relbit[0] |= BIT_MASK(REL_WHEEL);

	input_set_drvdata(input_dev, dev);

	//выбор функций для открытия/закрытия
	input_dev->open = usb_open;
	input_dev->close = usb_close;

	//включаем urb для прерываний
	usb_fill_int_urb(dev->irq, udev, pipe, dev->data,
		(maxp > 8 ? 8 : maxp),
		usb_irq, dev, endpoint->bInterval);

	//регистрация устройства
	if (input_register_device(dev->idev)) {
		usb_free_urb(dev->irq);
		kfree(dev->data);
		input_free_device(input_dev);
		kfree(dev);
		pr_err("cannot allocate memory for urb");
		return -ENOMEM;
	}

	//сохраняем данные в интерфейс
	usb_set_intfdata(interface, dev);

	return 0;
}

static void usb_disconnect(struct usb_interface *intf)
{
	struct my_usb_struct *mouse = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (mouse) {
		usb_kill_urb(mouse->irq);
		input_unregister_device(mouse->idev);
		usb_free_urb(mouse->irq);
		kfree(mouse->data);
		kfree(mouse);
	}
}

static struct usb_driver usb_driver = {
	.name		= DEVICE_NAME,
	.id_table	= usb_ids,
	.probe		= usb_probe,
	.disconnect	= usb_disconnect
};

static int usb_mouse_open(struct inode *inode, struct file *f)
{
	eof_flag = 0;
	return 0;
}

static int usb_mouse_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t usb_mouse_read(struct file *file,
				char *bf,
				size_t length,
				loff_t *offset)
{
	char *temp = kmalloc(length, GFP_KERNEL);

	if (temp == NULL)
		return -1;

	if (eof_flag) {
		eof_flag = 0;
		kfree(temp);
		return 0;
	}

	//красиво выводим MAC
	sprintf(temp, "Left clicks: %d\nRight clicks: %d\nMiddle clicks: %d\n",
			l_cnt, r_cnt, m_cnt);

	copy_to_user(bf, temp, strlen(temp));
	eof_flag = 1;
	kfree(temp);
	return (ssize_t)strlen(temp);
}

static const struct file_operations usb_ops = {
	.owner		= THIS_MODULE,
	.read		= usb_mouse_read,
	.open		= usb_mouse_open,
	.release	= usb_mouse_release
};

static int __init usb_device_init(void)
{
	int ret;

	pr_info("USB_mouse_int: init\n");

	//выделяем место для симв. у-ва
	ret = alloc_chrdev_region(&usb_mouse_dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_alert("USB_mouse_int: Failed to get a major number\n");
		return -1;
	}
	pr_info("USB_mouse_int: major %d and minor %d\n",
	MAJOR(usb_mouse_dev), MINOR(usb_mouse_dev));

	//инициализируем cdev
	cdev_init(&usb_mouse_cdev, &usb_ops);
	usb_mouse_cdev.owner = THIS_MODULE;
	ret = cdev_add(&usb_mouse_cdev, usb_mouse_dev, 1);
	if (ret < 0) {
		pr_alert("USB_mouse_int: Failed to register cdev\n");
		unregister_chrdev_region(usb_mouse_dev, 1);
		cdev_del(&usb_mouse_cdev);
		return -1;
	}

	//выделяем устройство
	ret = usb_register(&usb_driver);
	if (ret < 0) {
		pr_alert("USB_mouse_int: Failed to register usb\n");
		unregister_chrdev_region(usb_mouse_dev, 1);
		cdev_del(&usb_mouse_cdev);
		return -1;
	}

	return 0;
}

static void __exit usb_device_exit(void)
{
	pr_info("USB_mouse_int: exit\n");
	
	usb_deregister(&usb_driver);

	//освобождаем симв. у-во
	cdev_del(&usb_mouse_cdev);
	unregister_chrdev_region(usb_mouse_dev, 1);
	pr_info("USB_mouse_int: exit completed\n");
}

module_init(usb_device_init);
module_exit(usb_device_exit);

//module_usb_driver(usb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("USB mouse interrupt driver");
MODULE_AUTHOR("Sergey Samokhvalov/Ilya Vedmanov");


