#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#define DEVICE_NAME "usb_mouse_interrupter"
#define USB_MOUSE_VENDOR_ID 0x0458
#define USB_MOUSE_DEVICE_ID 0x003a

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
	if (data[0] & 0x01)
		pr_info("interrupt from mouse: left button\n");
	if (data[0] & 0x02)
		pr_info("interrupt from mouse: right button\n");
	if (data[0] & 0x04)
		pr_info("interrupt from mouse: middle button\n");

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

module_usb_driver(usb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("USB mouse interrupt driver");
MODULE_AUTHOR("Sergey Samokhvalov/Ilya Vedmanov");


