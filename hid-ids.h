/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stand-in for drivers/hid/hid-ids.h, which isn't shipped with kernel
 * headers, so hid-santroller.c builds unmodified out of tree.
 */
#ifndef __HID_SANTROLLER_IDS_H
#define __HID_SANTROLLER_IDS_H

#include <linux/hid.h>

#define USB_VENDOR_ID_PID_CODES		0x1209
#define USB_DEVICE_ID_SANTROLLER	0x2882
#define USB_DEVICE_ID_SANTROLLER_V2	0x2883

/* Added in 6.17 */
#ifndef hid_warn_ratelimited
#define hid_warn_ratelimited(hid, fmt, ...) \
	dev_warn_ratelimited(&(hid)->dev, "%s: " fmt, (hid)->name, ##__VA_ARGS__)
#endif

#endif
