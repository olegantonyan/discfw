#pragma once
#include <linux/ioctl.h>

#define CS43131_DEV_A "/dev/cs43131"
#define CS43131_DEV_C "/dev/cs43131c"

#define CS43131A_HEALTH_CHECK _IO('M', 0x28)
#define CS43131A_SET_BITSIZE _IOW('M', 0x29, unsigned char)
#define CS43131_MUTE_ALL _IO('M', 0x2d)
#define CS43131A_SET_VOL_A _IOW('M', 0x2d, unsigned char)
#define CS43131_UNMUTE_ALL _IO('M', 0x2e)
#define CS43131_LOW_POWER _IO('M', 0x2f)
#define CS43131_POWER_DOWN _IO('M', 0x30)
#define CS43131A_SET_SRATE _IOW('M', 0x34, unsigned char)
#define CS43131A_SET_MODE _IOW('M', 0x38, unsigned char)
#define CS43131A_SET_FILTER _IOW('M', 0x3e, unsigned char)
#define CS43131A_SET_VOL_B _IOW('M', 0x42, unsigned char)
#define CS43131_SET_DRE _IOW('M', 0x46, unsigned char)

#define CS43131C_HEALTH_CHECK _IO('M', 0x2b)
#define CS43131C_SET_VOL_A _IOW('M', 0x2f, unsigned char)
#define CS43131C_SET_BITSIZE _IOW('M', 0x32, unsigned char)
#define CS43131C_SET_SRATE _IOW('M', 0x36, unsigned char)
#define CS43131C_SET_MODE _IOW('M', 0x3c, unsigned char)
#define CS43131C_SET_FILTER _IOW('M', 0x40, unsigned char)
#define CS43131C_SET_VOL_B _IOW('M', 0x44, unsigned char)

enum cs43131_mode {
	CS43131_MODE_PCM = 0,
	CS43131_MODE_DOP = 1,
	CS43131_MODE_FORCE_REINIT = 3,
};

enum cs43131_filter {
	CS43131_FILTER_FAST_LL = 0,
	CS43131_FILTER_SLOW_LL = 1,
	CS43131_FILTER_SLOW_PC = 2,
	CS43131_FILTER_FAST_PC = 3,
	CS43131_FILTER_NOS = 4,
	CS43131_FILTER_WIDEBAND_FF = 5,
};
