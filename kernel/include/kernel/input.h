#pragma once
/* Input core + evdev (/dev/input/eventN, Linux input_event ABI). */
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

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

#define BITS_TO_U64(n) (((n) + 64) / 64)
struct input_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
struct input_id { uint16_t bustype, vendor, product, version; };

struct input_dev {
    char name[80], phys[32];
    struct input_id id;
    uint64_t evbit[BITS_TO_U64(EV_MAX)], keybit[BITS_TO_U64(KEY_MAX)], relbit[BITS_TO_U64(REL_MAX)],
             absbit[BITS_TO_U64(ABS_MAX)], mscbit[BITS_TO_U64(MSC_MAX)], ledbit[BITS_TO_U64(LED_MAX)],
             propbit[BITS_TO_U64(INPUT_PROP_MAX)];
    struct input_absinfo abs[ABS_MAX + 1];
    uint64_t keystate[BITS_TO_U64(KEY_MAX)], ledstate[BITS_TO_U64(LED_MAX)];
    int rep[2];                     /* delay, period (ms) */
    bool console_keys;              /* feed key presses to the console tty */
    /* private */
    int index;
    spinlock_t lock;
    struct list_node clients;
    void *grab;
    bool shift, ctrl, alt, caps;
};

static inline void input_set_bit(uint64_t *bm, unsigned bit) { bm[bit / 64] |= 1ull << (bit % 64); }
static inline bool input_test_bit(const uint64_t *bm, unsigned bit) { return bm[bit / 64] >> (bit % 64) & 1; }

/* registers and creates /dev/input/eventN; dev must stay allocated */
void input_register(struct input_dev *dev);
/* report one event; may be called from interrupt context. EV_SYN/SYN_REPORT wakes readers. */
void input_event(struct input_dev *dev, unsigned type, unsigned code, int value);
static inline void input_sync(struct input_dev *dev) { input_event(dev, EV_SYN, SYN_REPORT, 0); }
int input_device_count(void);
/* console keyboard mode (KDSKBMODE): K_OFF stops key → tty translation */
extern int console_kbmode;
