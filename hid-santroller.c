// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Santroller game controllers
 *
 * Santroller is an open source firmware for DIY and modded game
 * controllers and instruments: https://github.com/Santroller/Santroller
 *
 * Copyright (c) 2026 Sanjay Govind <sanjay.govind9@gmail.com>
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/idr.h>
#include <linux/input.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <dt-bindings/leds/common.h>

#include "hid-ids.h"

#define SANTROLLER_REPORT_ID_OUTPUT	0x01
#define SANTROLLER_REPORT_ID_FEATURES	0x10

/* Only devices exposing this usage support the features report */
#define SANTROLLER_FEATURES_USAGE	(HID_UP_MSVENDOR | 0x2882)

#define SANTROLLER_OUTPUT_REPORT_SIZE	11
#define SANTROLLER_FEATURES_REPORT_SIZE	3

#define SANTROLLER_FEATURES_RETRIES	8
#define SANTROLLER_FEATURES_TIMEOUT	msecs_to_jiffies(100)
/* Over Bluetooth, the host may still be enabling notifications for the reply */
#define SANTROLLER_FEATURES_TIMEOUT_BT	msecs_to_jiffies(250)

#define SANTROLLER_CMD_RUMBLE		0x5a
#define SANTROLLER_CMD_PLAYER_LED	0x5c
#define SANTROLLER_CMD_RGB_LED		0x5d

#define SANTROLLER_PLAYER_LED_OFF	0xff
#define SANTROLLER_NUM_PLAYER_LEDS	4

/* Capability bits from the features report */
#define SANTROLLER_CAP_PLAYER_LEDS	BIT(0)
#define SANTROLLER_CAP_RGB_LED		BIT(1)
#define SANTROLLER_CAP_RUMBLE		BIT(3)
#define SANTROLLER_CAP_EUPHORIA_LED	BIT(6)

static DEFINE_IDA(santroller_player_ids);

struct santroller {
	struct hid_device *hdev;
	struct input_dev *input;

	struct completion features_done;
	const char *name;
	u8 subtype;
	u8 caps;

	int player_id;
	struct led_classdev player_leds[SANTROLLER_NUM_PLAYER_LEDS];
	struct led_classdev_mc rgb_led;
	struct mc_subled rgb_subleds[3];
	struct led_classdev euphoria_led;

	spinlock_t lock;		/* protects the output state below */
	struct work_struct output_work;
	bool output_enabled;
	bool rumble_dirty;
	bool player_dirty;
	bool rgb_dirty;
	u8 rumble[2];
	u8 player;
	u8 rgb[3];
	u8 *output_buf;
};

static int santroller_send_report(struct hid_device *hdev, u8 *buf, size_t len)
{
	int ret;

	ret = hid_hw_output_report(hdev, buf, len);
	if (ret == -ENOSYS)
		ret = hid_hw_raw_request(hdev, buf[0], buf, len,
					 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);

	return ret;
}

/* Must be called with sc->lock held */
static void santroller_schedule_output(struct santroller *sc)
{
	if (sc->output_enabled)
		schedule_work(&sc->output_work);
}

/*
 * The firmware handles a single command per output report, so build the
 * next pending one into buf. Returns false if nothing is pending.
 */
static bool santroller_next_command(struct santroller *sc, u8 *buf)
{
	guard(spinlock_irqsave)(&sc->lock);

	memset(buf, 0, SANTROLLER_OUTPUT_REPORT_SIZE);
	buf[0] = SANTROLLER_REPORT_ID_OUTPUT;

	if (sc->rumble_dirty) {
		sc->rumble_dirty = false;
		buf[1] = SANTROLLER_CMD_RUMBLE;
		buf[2] = sc->rumble[0];
		buf[3] = sc->rumble[1];
		return true;
	}

	if (sc->player_dirty) {
		sc->player_dirty = false;
		buf[1] = SANTROLLER_CMD_PLAYER_LED;
		buf[2] = sc->player;
		return true;
	}

	if (sc->rgb_dirty) {
		sc->rgb_dirty = false;
		buf[1] = SANTROLLER_CMD_RGB_LED;
		memcpy(&buf[2], sc->rgb, sizeof(sc->rgb));
		return true;
	}

	return false;
}

static void santroller_output_work(struct work_struct *work)
{
	struct santroller *sc = container_of(work, struct santroller, output_work);
	int ret;

	while (santroller_next_command(sc, sc->output_buf)) {
		ret = santroller_send_report(sc->hdev, sc->output_buf,
					     SANTROLLER_OUTPUT_REPORT_SIZE);
		if (ret < 0)
			hid_warn_ratelimited(sc->hdev,
					     "failed to send output report: %d\n", ret);
	}
}

static int santroller_play_effect(struct input_dev *input, void *data,
				  struct ff_effect *effect)
{
	struct hid_device *hdev = input_get_drvdata(input);
	struct santroller *sc = hid_get_drvdata(hdev);

	if (effect->type != FF_RUMBLE)
		return 0;

	guard(spinlock_irqsave)(&sc->lock);

	sc->rumble[0] = effect->u.rumble.strong_magnitude >> 8;
	sc->rumble[1] = effect->u.rumble.weak_magnitude >> 8;
	sc->rumble_dirty = true;
	santroller_schedule_output(sc);

	return 0;
}

/*
 * The firmware shows a single player number, so the lowest lit player LED
 * wins. Turning all of them off turns the indicator off.
 *
 * Different player LEDs can be set concurrently, so look at all of them
 * under the lock, where the last caller is sure to see every update.
 */
static void santroller_player_led_set(struct led_classdev *led,
				      enum led_brightness value)
{
	struct hid_device *hdev = to_hid_device(led->dev->parent);
	struct santroller *sc = hid_get_drvdata(hdev);
	int i;

	guard(spinlock_irqsave)(&sc->lock);

	sc->player = SANTROLLER_PLAYER_LED_OFF;
	for (i = 0; i < SANTROLLER_NUM_PLAYER_LEDS; i++) {
		if (sc->player_leds[i].brightness) {
			sc->player = i;
			break;
		}
	}
	sc->player_dirty = true;
	santroller_schedule_output(sc);
}

static void santroller_rgb_led_set(struct led_classdev *led,
				   enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(led);
	struct santroller *sc = container_of(mc, struct santroller, rgb_led);
	int i;

	led_mc_calc_color_components(mc, brightness);

	guard(spinlock_irqsave)(&sc->lock);

	for (i = 0; i < ARRAY_SIZE(sc->rgb); i++)
		sc->rgb[i] = mc->subled_info[i].brightness;
	sc->rgb_dirty = true;
	santroller_schedule_output(sc);
}

static int santroller_register_player_leds(struct santroller *sc)
{
	static const char * const functions[] = {
		LED_FUNCTION_PLAYER1, LED_FUNCTION_PLAYER2,
		LED_FUNCTION_PLAYER3, LED_FUNCTION_PLAYER4,
	};
	struct hid_device *hdev = sc->hdev;
	struct led_classdev *led;
	u8 player = sc->player_id % SANTROLLER_NUM_PLAYER_LEDS;
	int i, ret;

	for (i = 0; i < SANTROLLER_NUM_PLAYER_LEDS; i++) {
		led = &sc->player_leds[i];
		led->name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "%s::%s",
					   dev_name(&sc->input->dev), functions[i]);
		if (!led->name)
			return -ENOMEM;

		led->brightness = i == player;
		led->max_brightness = 1;
		led->brightness_set = santroller_player_led_set;

		ret = devm_led_classdev_register(&hdev->dev, led);
		if (ret)
			return ret;
	}

	/* Show the assigned player number until userspace says otherwise */
	guard(spinlock_irqsave)(&sc->lock);

	sc->player = player;
	sc->player_dirty = true;
	santroller_schedule_output(sc);

	return 0;
}

/* The firmware takes the euphoria LED as a rumble command with equal values */
static void santroller_euphoria_led_set(struct led_classdev *led,
					enum led_brightness brightness)
{
	struct santroller *sc = container_of(led, struct santroller, euphoria_led);

	guard(spinlock_irqsave)(&sc->lock);

	sc->rumble[0] = brightness;
	sc->rumble[1] = brightness;
	sc->rumble_dirty = true;
	santroller_schedule_output(sc);
}

static int santroller_register_euphoria_led(struct santroller *sc)
{
	struct hid_device *hdev = sc->hdev;
	struct led_classdev *led = &sc->euphoria_led;

	led->name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "%s::%s",
				   dev_name(&sc->input->dev),
				   LED_FUNCTION_INDICATOR);
	if (!led->name)
		return -ENOMEM;

	led->max_brightness = 255;
	led->brightness_set = santroller_euphoria_led_set;

	return devm_led_classdev_register(&hdev->dev, led);
}

static int santroller_register_rgb_led(struct santroller *sc)
{
	struct hid_device *hdev = sc->hdev;
	struct led_classdev *led = &sc->rgb_led.led_cdev;

	sc->rgb_subleds[0].color_index = LED_COLOR_ID_RED;
	sc->rgb_subleds[1].color_index = LED_COLOR_ID_GREEN;
	sc->rgb_subleds[2].color_index = LED_COLOR_ID_BLUE;
	sc->rgb_led.subled_info = sc->rgb_subleds;
	sc->rgb_led.num_colors = ARRAY_SIZE(sc->rgb_subleds);

	led->name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "%s:rgb:indicator",
				   dev_name(&sc->input->dev));
	if (!led->name)
		return -ENOMEM;

	led->brightness = 255;
	led->max_brightness = 255;
	led->brightness_set = santroller_rgb_led_set;

	return devm_led_classdev_multicolor_register(&hdev->dev, &sc->rgb_led);
}

/* Name the gamepad input device and add rumble before it is registered */
static int santroller_input_configured(struct hid_device *hdev,
				       struct hid_input *hi)
{
	struct santroller *sc = hid_get_drvdata(hdev);
	struct input_dev *input = hi->input;
	int ret;

	if (!sc || sc->input)
		return 0;

	if (hi->application != HID_GD_GAMEPAD &&
	    hi->application != HID_GD_JOYSTICK)
		return 0;

	if (sc->name)
		input->name = sc->name;

	if (sc->caps & SANTROLLER_CAP_RUMBLE) {
		input_set_capability(input, EV_FF, FF_RUMBLE);
		ret = input_ff_create_memless(input, NULL, santroller_play_effect);
		if (ret)
			return ret;
	}

	sc->input = input;
	return 0;
}

static int santroller_raw_event(struct hid_device *hdev,
				struct hid_report *report, u8 *data, int size)
{
	struct santroller *sc = hid_get_drvdata(hdev);

	if (sc && data[0] == SANTROLLER_REPORT_ID_FEATURES &&
	    size >= SANTROLLER_FEATURES_REPORT_SIZE &&
	    !completion_done(&sc->features_done)) {
		sc->subtype = data[1];
		sc->caps = data[2];
		complete(&sc->features_done);
	}

	return 0;
}

/* The device answers the features command with an input report */
static int santroller_get_features(struct santroller *sc)
{
	u8 *buf __free(kfree) = kzalloc(2, GFP_KERNEL);
	unsigned long timeout = SANTROLLER_FEATURES_TIMEOUT;
	int i, ret = -ETIMEDOUT;

	if (!buf)
		return -ENOMEM;

	if (sc->hdev->bus == BUS_BLUETOOTH)
		timeout = SANTROLLER_FEATURES_TIMEOUT_BT;

	for (i = 0; i < SANTROLLER_FEATURES_RETRIES; i++) {
		reinit_completion(&sc->features_done);

		buf[0] = SANTROLLER_REPORT_ID_FEATURES;
		buf[1] = 0;
		ret = santroller_send_report(sc->hdev, buf, 2);
		if (ret < 0)
			continue;

		if (wait_for_completion_timeout(&sc->features_done, timeout))
			return 0;

		ret = -ETIMEDOUT;
	}

	return ret;
}

static bool santroller_has_features_report(struct hid_device *hdev)
{
	struct hid_report *report;

	report = hdev->report_enum[HID_OUTPUT_REPORT].report_id_hash[SANTROLLER_REPORT_ID_FEATURES];
	if (!report || !report->maxfield || !report->field[0]->maxusage)
		return false;

	return report->field[0]->usage[0].hid == SANTROLLER_FEATURES_USAGE;
}

/*
 * Each profile on a USB device gets its own interface, named after the
 * profile through the interface string.
 */
static const char *santroller_profile_name(struct hid_device *hdev)
{
	struct usb_interface *intf;
	const char *name;

	if (!hid_is_usb(hdev))
		return NULL;

	intf = to_usb_interface(hdev->dev.parent);
	name = READ_ONCE(intf->cur_altsetting->string);
	if (!name || !*name)
		return NULL;

	return devm_kstrdup(&hdev->dev, name, GFP_KERNEL);
}

static ssize_t subtype_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct santroller *sc = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%u\n", sc->subtype);
}
static DEVICE_ATTR_RO(subtype);

static struct attribute *santroller_attrs[] = {
	&dev_attr_subtype.attr,
	NULL
};

/* Interfaces left to the generic HID handling have no subtype */
static umode_t santroller_attr_is_visible(struct kobject *kobj,
					  struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);

	return hid_get_drvdata(to_hid_device(dev)) ? attr->mode : 0;
}

static const struct attribute_group santroller_group = {
	.attrs = santroller_attrs,
	.is_visible = santroller_attr_is_visible,
};
__ATTRIBUTE_GROUPS(santroller);

static void santroller_start_output(struct santroller *sc)
{
	guard(spinlock_irqsave)(&sc->lock);

	sc->output_enabled = true;
}

static void santroller_stop_output(struct santroller *sc)
{
	scoped_guard(spinlock_irqsave, &sc->lock)
		sc->output_enabled = false;

	cancel_work_sync(&sc->output_work);
}

static int santroller_probe(struct hid_device *hdev,
			    const struct hid_device_id *id)
{
	struct santroller *sc;
	int ret;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	/*
	 * Older firmware and the configuration interface don't support the
	 * features report, leave those to the generic HID handling.
	 */
	if (!santroller_has_features_report(hdev))
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);

	sc = devm_kzalloc(&hdev->dev, sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	sc->output_buf = devm_kzalloc(&hdev->dev, SANTROLLER_OUTPUT_REPORT_SIZE,
				      GFP_KERNEL);
	if (!sc->output_buf)
		return -ENOMEM;

	sc->hdev = hdev;
	sc->name = santroller_profile_name(hdev);
	init_completion(&sc->features_done);
	spin_lock_init(&sc->lock);
	INIT_WORK(&sc->output_work, santroller_output_work);

	ret = ida_alloc(&santroller_player_ids, GFP_KERNEL);
	if (ret < 0)
		return ret;
	sc->player_id = ret;

	hid_set_drvdata(hdev, sc);

	/*
	 * Start without connecting hid-input or hidraw, so the features
	 * report can tell santroller_input_configured() what the device is.
	 */
	ret = hid_hw_start(hdev, 0);
	if (ret) {
		hid_err(hdev, "failed to start HID device: %d\n", ret);
		goto err_free_id;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "failed to open HID device: %d\n", ret);
		goto err_stop;
	}

	hid_device_io_start(hdev);

	/*
	 * Older firmware never answers over Bluetooth, so without the
	 * features, still bind as a plain gamepad.
	 */
	ret = santroller_get_features(sc);
	if (ret == -ENOMEM)
		goto err_close;
	if (ret) {
		hid_warn(hdev, "failed to get device features: %d\n", ret);
		/* Ignore a late reply, the caps are settled from here on */
		complete(&sc->features_done);
	}

	hid_device_io_stop(hdev);

	santroller_start_output(sc);

	ret = hid_connect(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "failed to connect HID device: %d\n", ret);
		goto err_stop_output;
	}

	/* hid-input may have unwound after santroller_input_configured() */
	if (!(hdev->claimed & HID_CLAIMED_INPUT))
		sc->input = NULL;

	/* The LEDs are named after the gamepad input device */
	if (!sc->input)
		goto out;

	if (sc->caps & SANTROLLER_CAP_PLAYER_LEDS) {
		ret = santroller_register_player_leds(sc);
		if (ret) {
			hid_err(hdev, "failed to register player LEDs: %d\n", ret);
			goto err_stop_output;
		}
	}

	if (sc->caps & SANTROLLER_CAP_RGB_LED) {
		ret = santroller_register_rgb_led(sc);
		if (ret) {
			hid_err(hdev, "failed to register RGB LED: %d\n", ret);
			goto err_stop_output;
		}
	}

	if (sc->caps & SANTROLLER_CAP_EUPHORIA_LED) {
		ret = santroller_register_euphoria_led(sc);
		if (ret) {
			hid_err(hdev, "failed to register euphoria LED: %d\n", ret);
			goto err_stop_output;
		}
	}

out:
	/* hid-input and hidraw open the device themselves from here on */
	hid_hw_close(hdev);
	return 0;

err_stop_output:
	santroller_stop_output(sc);
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
err_free_id:
	ida_free(&santroller_player_ids, sc->player_id);
	return ret;
}

static void santroller_remove(struct hid_device *hdev)
{
	struct santroller *sc = hid_get_drvdata(hdev);

	if (!sc) {
		hid_hw_stop(hdev);
		return;
	}

	santroller_stop_output(sc);
	hid_hw_stop(hdev);
	ida_free(&santroller_player_ids, sc->player_id);
}

static int santroller_resume(struct hid_device *hdev)
{
	struct santroller *sc = hid_get_drvdata(hdev);

	if (!sc)
		return 0;

	/* The device may have lost its LED state, so send it again */
	guard(spinlock_irqsave)(&sc->lock);

	sc->player_dirty = sc->caps & SANTROLLER_CAP_PLAYER_LEDS;
	sc->rgb_dirty = sc->caps & SANTROLLER_CAP_RGB_LED;
	sc->rumble_dirty = sc->caps & SANTROLLER_CAP_EUPHORIA_LED;
	santroller_schedule_output(sc);

	return 0;
}

static const struct hid_device_id santroller_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_PID_CODES, USB_DEVICE_ID_SANTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PID_CODES, USB_DEVICE_ID_SANTROLLER_V2) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_PID_CODES, USB_DEVICE_ID_SANTROLLER) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_PID_CODES, USB_DEVICE_ID_SANTROLLER_V2) },
	{ }
};
MODULE_DEVICE_TABLE(hid, santroller_devices);

static struct hid_driver santroller_driver = {
	.name = "santroller",
	.id_table = santroller_devices,
	.probe = santroller_probe,
	.remove = santroller_remove,
	.raw_event = santroller_raw_event,
	.input_configured = santroller_input_configured,
	.resume = pm_ptr(santroller_resume),
	.reset_resume = pm_ptr(santroller_resume),
	.driver.dev_groups = santroller_groups,
};
module_hid_driver(santroller_driver);

MODULE_AUTHOR("Sanjay Govind <sanjay.govind9@gmail.com>");
MODULE_DESCRIPTION("HID driver for Santroller game controllers");
MODULE_LICENSE("GPL");
