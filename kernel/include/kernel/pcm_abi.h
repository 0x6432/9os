#pragma once
/* Linux ALSA PCM ABI, 64-bit little-endian targets. No mmap/capture/mixer advertised.
 * Kept libc-independent so the guest regression can share the wire structures. */
struct pcm_mask { unsigned bits[8]; };
struct pcm_interval { unsigned min, max, flags; }; /* openmin, openmax, integer, empty */
struct pcm_hw_params {
    unsigned flags;
    struct pcm_mask masks[3], mres[5];
    struct pcm_interval intervals[12], ires[9];
    unsigned rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char sync[16], reserved[48];
};
struct pcm_sw_params {
    int tstamp_mode;
    unsigned period_step, sleep_min;
    unsigned long avail_min, xfer_align, start_threshold, stop_threshold,
                  silence_threshold, silence_size, boundary;
    unsigned proto, tstamp_type;
    unsigned char reserved[56];
};
struct pcm_info {
    unsigned device, subdevice;
    int stream, card;
    unsigned char id[64], name[80], subname[32];
    int dev_class, dev_subclass;
    unsigned subdevices_count, subdevices_avail;
    unsigned char pad[16], reserved[64];
};
struct pcm_time { long sec, nsec; };
struct pcm_status {
    int state, pad;
    struct pcm_time trigger_tstamp, tstamp;
    unsigned long appl_ptr, hw_ptr;
    long delay;
    unsigned long avail, avail_max, overrange;
    int suspended_state;
    unsigned audio_tstamp_data;
    struct pcm_time audio_tstamp, driver_tstamp;
    unsigned audio_tstamp_accuracy;
    unsigned char reserved[20];
};
struct pcm_sync_ptr {
    unsigned flags, pad;
    union { struct { int state, pad; unsigned long hw_ptr; struct pcm_time tstamp;
                    int suspended_state, pad2; struct pcm_time audio_tstamp; } status;
            unsigned char reserved[64]; } s;
    union { struct { unsigned long appl_ptr, avail_min; } control;
            unsigned char reserved[64]; } c;
};
struct pcm_xferi { long result; void *buf; unsigned long frames; };
#define PCM_VERSION 0x020010
#define PCM_IO_VERSION 0x80044100u
#define PCM_IO_INFO 0x81204101u
#define PCM_IO_HW_REFINE 0xc2604110u
#define PCM_IO_HW_PARAMS 0xc2604111u
#define PCM_IO_HW_FREE 0x4112u
#define PCM_IO_SW_PARAMS 0xc0884113u
#define PCM_IO_STATUS 0x80984120u
#define PCM_IO_DELAY 0x80084121u
#define PCM_IO_HWSYNC 0x4122u
#define PCM_IO_SYNC_PTR 0xc0884123u
#define PCM_IO_PREPARE 0x4140u
#define PCM_IO_RESET 0x4141u
#define PCM_IO_START 0x4142u
#define PCM_IO_DROP 0x4143u
#define PCM_IO_DRAIN 0x4144u
#define PCM_IO_WRITEI 0x40184150u
#define PCM_OPEN 0
#define PCM_SETUP 1
#define PCM_PREPARED 2
#define PCM_RUNNING 3
#define PCM_XRUN 4
#define PCM_DISCONNECTED 8
#define PCM_RATE 48000u
#define PCM_PERIOD 256u
#define PCM_PERIODS 8u
#define PCM_BUFFER (PCM_PERIOD * PCM_PERIODS)
#define PCM_FRAME_BYTES 4u
_Static_assert(sizeof(unsigned long) == 8, "64-bit PCM ABI");
_Static_assert(sizeof(struct pcm_info) == 288, "pcm info");
_Static_assert(sizeof(struct pcm_hw_params) == 608, "pcm hw params");
_Static_assert(sizeof(struct pcm_sw_params) == 136, "pcm sw params");
_Static_assert(sizeof(struct pcm_status) == 152, "pcm status");
_Static_assert(sizeof(struct pcm_sync_ptr) == 136, "pcm sync ptr");
_Static_assert(sizeof(struct pcm_xferi) == 24, "pcm xferi");
