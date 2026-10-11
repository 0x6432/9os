#pragma once
/* Input core + evdev (/dev/input/eventN, Linux input_event ABI). */
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/device.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_LED 0x11
#define EV_REP 0x14
#define EV_MAX 0x1f
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define KEY_MAX 0x2ff
#define REL_MAX 0x0f
#define ABS_MAX 0x3f
#define MSC_MAX 0x07
#define LED_MAX 0x0f
#define INPUT_PROP_MAX 0x1f
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_TOUCH 0x14a
#define BUS_I8042 0x11
#define BUS_VIRTUAL 0x06
#define BUS_USB 0x03
#define EV_SW 0x05
#define EV_SND 0x12
#define EV_FF 0x15
#define EV_UINPUT 0x0101
#define SW_MAX 0x10
#define SND_MAX 0x07
#define FF_MAX 0x7f
#define FF_GAIN 0x60
#define FF_AUTOCENTER 0x61
#define FF_SLOTS 64
#define FF_EFFECT_SIZE 48             /* struct ff_effect (LP64) */
#define REL_CNT (REL_MAX + 1)

#define BITS_TO_U64(n) (((n) + 64) / 64)
struct input_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
struct input_id { uint16_t bustype, vendor, product, version; };

struct input_dev {
    char name[80], phys[32];
    struct input_id id;
    uint64_t evbit[BITS_TO_U64(EV_MAX)], keybit[BITS_TO_U64(KEY_MAX)], relbit[BITS_TO_U64(REL_MAX)],
             absbit[BITS_TO_U64(ABS_MAX)], mscbit[BITS_TO_U64(MSC_MAX)], ledbit[BITS_TO_U64(LED_MAX)],
             propbit[BITS_TO_U64(INPUT_PROP_MAX)], swbit[BITS_TO_U64(SW_MAX)], sndbit[BITS_TO_U64(SND_MAX)],
             ffbit[BITS_TO_U64(FF_MAX)];
    struct input_absinfo abs[ABS_MAX + 1];
    uint64_t keystate[BITS_TO_U64(KEY_MAX)], ledstate[BITS_TO_U64(LED_MAX)], swstate[BITS_TO_U64(SW_MAX)],
             sndstate[BITS_TO_U64(SND_MAX)];
    /* M36 force feedback: ff_max effect slots (EVIOCGEFFECTS); upload/erase may sleep (uinput
     * asks its owner), to_device gets LED/SND/FF events written by evdev clients */
    int ff_max;
    int (*ff_upload)(struct input_dev *dev, const uint8_t *effect, const uint8_t *old);
    int (*ff_erase)(struct input_dev *dev, int id);
    void (*to_device)(struct input_dev *dev, unsigned type, unsigned code, int value);
    void *drv;                      /* driver private (uinput) */
    void *ff_owner[FF_SLOTS];       /* evdev client that uploaded each effect */
    uint8_t ff_effect[FF_SLOTS][FF_EFFECT_SIZE];
    int rep[2];                     /* delay, period (ms) */
    bool console_keys;              /* feed key presses to the console tty */
    struct device *parent;          /* M34: sysfs parent (virtio device), nullptr: virtual */
    struct device idev, edev;       /* /sys/class/input/inputN and its eventN */
    void (*free)(struct input_dev *dev);   /* dynamic devices (uinput): called after the last reference */
    /* private */
    int index, num;                 /* eventN slot (lowest free), inputN (never reused) */
    unsigned npending;              /* events since the last SYN_REPORT */
    bool gone;                      /* unregistered: readers get ENODEV, poll HUP */
    spinlock_t lock;
    struct list_node clients;
    void *grab;
    bool shift, ctrl, alt, caps;
};

static inline void input_set_bit(uint64_t *bm, unsigned bit) { bm[bit / 64] |= 1ull << (bit % 64); }
static inline bool input_test_bit(const uint64_t *bm, unsigned bit) { return bm[bit / 64] >> (bit % 64) & 1; }

/* registers and creates /dev/input/eventN (0, or -ENOSPC). Static devices stay allocated;
 * dynamic ones set ->free, which runs once the device is unregistered and unreferenced. */
int input_register(struct input_dev *dev);
void input_unregister(struct input_dev *dev);
/* report one event; may be called from interrupt context. EV_SYN/SYN_REPORT wakes readers. */
void input_event(struct input_dev *dev, unsigned type, unsigned code, int value);
static inline void input_sync(struct input_dev *dev) { input_event(dev, EV_SYN, SYN_REPORT, 0); }
int input_device_count(void);
/* console keyboard mode (KDSKBMODE): K_OFF stops key → tty translation */
extern int console_kbmode;
