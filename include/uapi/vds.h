/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com> */
#ifndef _UAPI_VDS_H
#define _UAPI_VDS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VDS_IOC_MAGIC 'V'
#define VDS_NAME_MAX 32
#define VDS_FRAME_MAX_PAYLOAD 4096

enum vds_identity {
	VDS_IDENTITY_DS5 = 0,
	VDS_IDENTITY_DSE = 1,
};

enum vds_frame_type {
	VDS_FRAME_STATUS = 0,
	VDS_FRAME_USB_HID_OUT = 1,
	VDS_FRAME_USB_FEATURE_GET = 2,
	VDS_FRAME_USB_FEATURE_SET = 3,
	VDS_FRAME_USB_AUDIO_OUT = 4,
	VDS_FRAME_USB_HID_IN = 5,
	VDS_FRAME_USB_FEATURE_REPLY = 6,
	VDS_FRAME_USB_INTERFACE = 7,
};

enum vds_status_flags {
	VDS_STATUS_CONNECTED = 1u << 0,
	VDS_STATUS_CONFIGURED = 1u << 1,
	VDS_STATUS_HID_ENABLED = 1u << 2,
	VDS_STATUS_AUDIO_ENABLED = 1u << 3,
};

enum vds_usb_interface_kind {
	VDS_USB_INTERFACE_HID = 0,
	VDS_USB_INTERFACE_AUDIO_OUT = 1,
	VDS_USB_INTERFACE_AUDIO_IN = 2,
};

struct vds_frame_header {
	__u16 type;
	__u16 flags;
	__u32 length;
	__u64 sequence;
};

struct vds_usb_interface_event {
	__u8 interface_number;
	__u8 altsetting;
	__u8 interface_kind;
	__u8 reserved;
};

struct vds_status {
	__u32 status_flags;
	__u32 identity;
	__u64 frames_to_user;
	__u64 frames_from_user;
	char backend[VDS_NAME_MAX];
};

struct vds_identity_config {
	__u32 identity;
	__u32 polling_rate_mode;
};

#ifdef __cplusplus
static_assert(sizeof(struct vds_frame_header) == 16,
	      "vds_frame_header is part of the /dev/vds0 ABI");
static_assert(sizeof(struct vds_usb_interface_event) == 4,
	      "vds_usb_interface_event is part of the /dev/vds0 ABI");
static_assert(sizeof(struct vds_status) == 56,
	      "vds_status is part of the /dev/vds0 ABI");
static_assert(sizeof(struct vds_identity_config) == 8,
	      "vds_identity_config is part of the /dev/vds0 ABI");
#endif

#define VDS_IOC_GET_STATUS _IOR(VDS_IOC_MAGIC, 0x01, struct vds_status)
#define VDS_IOC_SET_IDENTITY \
	_IOW(VDS_IOC_MAGIC, 0x02, struct vds_identity_config)
#define VDS_IOC_CONNECT _IO(VDS_IOC_MAGIC, 0x03)
#define VDS_IOC_DISCONNECT _IO(VDS_IOC_MAGIC, 0x04)

#endif /* _UAPI_VDS_H */
