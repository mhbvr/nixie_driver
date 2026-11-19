#include "outputs.h"

#include <math.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "mux.h"
#include "pins_config.h"

/* ------------------------------------------------------------------------ */
/* Per-pin state (only touched from thread context)                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    out_mode_t mode;
    double     pwm_freq;      /* actual frequency, valid in OUT_MODE_PWM */
    double     pwm_duty;      /* requested duty %, valid in OUT_MODE_PWM */
} pin_state_t;

static pin_state_t g_state[PIN_COUNT];

/* ------------------------------------------------------------------------ */
/* Round robin state (shared with the alarm IRQ)                             */
/* ------------------------------------------------------------------------ */

static struct {
    int               steps[RR_MAX_STEPS];
    int               n;
    uint32_t          on_us;
    uint32_t          off_us;
    uint32_t          cycles;          /* 0 = forever             */
    volatile uint32_t cycles_done;
    volatile int      pos;             /* index into steps[]      */
    volatile bool     lit;             /* steps[pos] currently on */
    volatile bool     running;
    volatile bool     finished;        /* set by IRQ, read by out_poll() */
    alarm_id_t        alarm;
} rr;

/* ------------------------------------------------------------------------ */
/* Low level helpers                                                         */
/* ------------------------------------------------------------------------ */

static inline void drive(int idx, bool on)
{
    gpio_put(PIN_TABLE[idx].gpio, on != PIN_TABLE[idx].active_low);
}

static inline bool level_is_on(int idx)
{
    return gpio_get_out_level(PIN_TABLE[idx].gpio) != PIN_TABLE[idx].active_low;
}

static bool valid_idx(int idx)
{
    return idx >= 0 && idx < PIN_COUNT;
}

/* Return a pin currently in PWM mode back to plain GPIO, switched off. */
static void pwm_release(int idx)
{
    uint gpio  = PIN_TABLE[idx].gpio;
    uint slice = pwm_gpio_to_slice_num(gpio);

    drive(idx, false);                        /* SIO output = inactive level  */
    gpio_set_function(gpio, GPIO_FUNC_SIO);   /* SIO OE is still set (output) */
    pwm_set_chan_level(slice, pwm_gpio_to_channel(gpio), 0);
    g_state[idx].mode = OUT_MODE_OFF;

    /* Stop the slice if no other configured pin still uses it. */
    for (int i = 0; i < PIN_COUNT; i++) {
        if (g_state[i].mode == OUT_MODE_PWM &&
            pwm_gpio_to_slice_num(PIN_TABLE[i].gpio) == slice)
            return;
    }
    pwm_set_enabled(slice, false);
}

/* Bring a pin to the plain OFF state, whatever it was doing. Caller must make
 * sure the pin is not part of a running round robin. */
static void make_plain(int idx)
{
    if (g_state[idx].mode == OUT_MODE_PWM)
        pwm_release(idx);
    drive(idx, false);
    g_state[idx].mode = OUT_MODE_OFF;
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

void out_init(void)
{
    for (int i = 0; i < PIN_COUNT; i++) {
        uint gpio = PIN_TABLE[i].gpio;
        gpio_init(gpio);                      /* SIO, input, out latch = 0 */
        gpio_put(gpio, PIN_TABLE[i].active_low);  /* inactive level first   */
        gpio_set_dir(gpio, GPIO_OUT);
        g_state[i].mode = OUT_MODE_OFF;
    }
    rr.running = false;
    rr.finished = false;
}

/* ------------------------------------------------------------------------ */
/* Static on / off                                                           */
/* ------------------------------------------------------------------------ */

static const char *check_not_rr(int idx)
{
    if (g_state[idx].mode == OUT_MODE_RR)
        return "pin is used by the running round robin (use 'rr stop' or 'alloff')";
    if (g_state[idx].mode == OUT_MODE_MX)
        return "pin is used by multiplexing (use 'mxa off' or 'alloff')";
    return NULL;
}

const char *out_set(int idx, bool on)
{
    if (!valid_idx(idx))
        return "invalid pin";
    const char *err = check_not_rr(idx);
    if (err)
        return err;

    make_plain(idx);
    if (on) {
        drive(idx, true);
        g_state[idx].mode = OUT_MODE_ON;
    }
    return NULL;
}

const char *out_toggle(int idx)
{
    if (!valid_idx(idx))
        return "invalid pin";
    return out_set(idx, g_state[idx].mode != OUT_MODE_ON);
}

/* ------------------------------------------------------------------------ */
/* PWM                                                                       */
/* ------------------------------------------------------------------------ */

static uint16_t duty_to_level(double duty_pct, uint32_t top)
{
    double lvl = duty_pct / 100.0 * (double)(top + 1u);
    if (lvl < 0)
        lvl = 0;
    if (lvl > (double)(top + 1u))
        lvl = (double)(top + 1u);
    /* level = top + 1 = 65536 cannot be stored in 16 bits; it only happens
     * for top = 65535 and 100 % duty, where 65535 is indistinguishable. */
    uint32_t l = (uint32_t)lround(lvl);
    return (uint16_t)(l > 0xFFFFu ? 0xFFFFu : l);
}

const char *out_pwm(int idx, double freq_hz, double duty_pct,
                    double *actual_freq, double *actual_duty)
{
    if (!valid_idx(idx))
        return "invalid pin";
    const char *err = check_not_rr(idx);
    if (err)
        return err;
    if (!(duty_pct >= 0.0 && duty_pct <= 100.0))
        return "duty must be 0..100 %";

    const uint     gpio  = PIN_TABLE[idx].gpio;
    const uint     slice = pwm_gpio_to_slice_num(gpio);
    const uint     chan  = pwm_gpio_to_channel(gpio);
    const double   clk   = (double)clock_get_hz(clk_sys);
    const double   fmin  = clk * 16.0 / (4095.0 * 65536.0);
    const double   fmax  = clk / 2.0;

    if (!(freq_hz >= fmin && freq_hz <= fmax)) {
        static char msg[80];
        snprintf(msg, sizeof msg, "frequency must be %.2f Hz .. %.0f Hz", fmin, fmax);
        return msg;
    }

    /* Another configured pin on the very same slice+channel (GPIO n and n+16
     * on RP2040) would output the same waveform - refuse. Also find the
     * sibling channel that shares the counter (= the frequency). */
    int sibling = -1;
    for (int i = 0; i < PIN_COUNT; i++) {
        if (i == idx || g_state[i].mode != OUT_MODE_PWM)
            continue;
        uint g = PIN_TABLE[i].gpio;
        if (pwm_gpio_to_slice_num(g) != slice)
            continue;
        if (pwm_gpio_to_channel(g) == chan) {
            static char msg[80];
            snprintf(msg, sizeof msg, "PWM channel is already used by %s", PIN_TABLE[i].name);
            return msg;
        }
        sibling = i;
    }

    /* Divider in 1/16 steps (16 .. 4095 => 1.0 .. 255.9375), as small as
     * possible to get the best duty resolution, then the wrap value. */
    uint32_t div16 = (uint32_t)ceil(clk * 16.0 / (freq_hz * 65536.0));
    if (div16 < 16)
        div16 = 16;
    if (div16 > 4095)
        div16 = 4095;
    double periods = clk * 16.0 / ((double)div16 * freq_hz);  /* = top + 1 */
    uint32_t top1 = (uint32_t)lround(periods);
    if (top1 > 65536u)
        top1 = 65536u;
    if (top1 < 2u)
        top1 = 2u;
    uint32_t top = top1 - 1u;
    double   f_act = clk * 16.0 / ((double)div16 * (double)top1);

    if (g_state[idx].mode != OUT_MODE_PWM)
        make_plain(idx);

    uint16_t level = duty_to_level(duty_pct, top);

    pwm_hw->slice[slice].div = div16;    /* INT in [11:4], FRAC in [3:0] */
    pwm_set_wrap(slice, (uint16_t)top);
    pwm_set_chan_level(slice, chan, level);

    /* Output polarity of this channel only; keep the sibling's bit. */
    uint32_t inv_mask = chan == PWM_CHAN_A ? PWM_CH0_CSR_A_INV_BITS : PWM_CH0_CSR_B_INV_BITS;
    hw_write_masked(&pwm_hw->slice[slice].csr,
                    PIN_TABLE[idx].active_low ? inv_mask : 0u, inv_mask);
    pwm_set_enabled(slice, true);
    gpio_set_function(gpio, GPIO_FUNC_PWM);

    if (sibling >= 0) {
        /* Keep the sibling's duty, but its frequency is now ours. */
        pwm_set_chan_level(slice, pwm_gpio_to_channel(PIN_TABLE[sibling].gpio),
                           duty_to_level(g_state[sibling].pwm_duty, top));
        if (fabs(g_state[sibling].pwm_freq - f_act) > 1e-6 * f_act)
            printf("warning: %s shares PWM slice %u with %s, its frequency is now %.3f Hz too\n",
                   PIN_TABLE[sibling].name, slice, PIN_TABLE[idx].name, f_act);
        g_state[sibling].pwm_freq = f_act;
    }

    g_state[idx].mode     = OUT_MODE_PWM;
    g_state[idx].pwm_freq = f_act;
    g_state[idx].pwm_duty = duty_pct;

    if (actual_freq)
        *actual_freq = f_act;
    if (actual_duty)
        *actual_duty = 100.0 * (double)level / (double)top1;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Round robin                                                               */
/* ------------------------------------------------------------------------ */

/* Runs in IRQ context. A negative return value reschedules the alarm
 * relative to its previous target time, so the timing does not drift. */
static int64_t rr_alarm_cb(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;

    if (!rr.running)
        return 0;

    if (rr.lit) {
        drive(rr.steps[rr.pos], false);
        rr.lit = false;
        if (rr.off_us > 0)
            return -(int64_t)rr.off_us;
    }

    int next = rr.pos + 1;
    if (next >= rr.n) {
        next = 0;
        rr.cycles_done++;
        if (rr.cycles != 0 && rr.cycles_done >= rr.cycles) {
            rr.running  = false;
            rr.finished = true;
            return 0;                /* do not reschedule */
        }
    }
    rr.pos = next;
    drive(rr.steps[next], true);
    rr.lit = true;
    return -(int64_t)rr.on_us;
}

static void rr_release_pins(void)
{
    for (int i = 0; i < rr.n; i++) {
        drive(rr.steps[i], false);
        g_state[rr.steps[i]].mode = OUT_MODE_OFF;
    }
    rr.n = 0;
}

bool out_rr_stop(void)
{
    bool was_running = rr.running;
    if (rr.running) {
        cancel_alarm(rr.alarm);
        rr.running = false;
    }
    rr.finished = false;
    rr_release_pins();
    return was_running;
}

bool out_rr_running(void)
{
    return rr.running;
}

const char *out_rr_start(const int *idx, int n,
                         uint32_t on_us, uint32_t off_us, uint32_t cycles)
{
    if (n < 1)
        return "empty pin list";
    if (n > RR_MAX_STEPS)
        return "too many pins in the list";
    if (on_us < RR_MIN_US)
        return "on time too short (min 20 us)";
    if (off_us != 0 && off_us < RR_MIN_US)
        return "off time too short (0 or min 20 us)";
    for (int i = 0; i < n; i++)
        if (!valid_idx(idx[i]))
            return "invalid pin";
    for (int i = 0; i < n; i++)
        if (g_state[idx[i]].mode == OUT_MODE_MX)
            return "pin is used by multiplexing (use 'mxa off' or 'alloff')";

    out_rr_stop();

    for (int i = 0; i < n; i++) {
        make_plain(idx[i]);
        g_state[idx[i]].mode = OUT_MODE_RR;
        rr.steps[i] = idx[i];
    }
    rr.n           = n;
    rr.on_us       = on_us;
    rr.off_us      = off_us;
    rr.cycles      = cycles;
    rr.cycles_done = 0;
    rr.pos         = 0;
    rr.finished    = false;

    /* First step lights immediately. */
    rr.lit     = true;
    rr.running = true;
    drive(rr.steps[0], true);

    rr.alarm = add_alarm_in_us(on_us, rr_alarm_cb, NULL, true);
    if (rr.alarm <= 0) {
        rr.running = false;
        rr_release_pins();
        return "no free hardware alarm";
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Everything off                                                            */
/* ------------------------------------------------------------------------ */

void out_all_off(void)
{
    mux_all_off();

    if (rr.running) {
        cancel_alarm(rr.alarm);
        rr.running = false;
    }
    rr.finished = false;
    rr.n = 0;

    for (int i = 0; i < PIN_COUNT; i++) {
        uint gpio = PIN_TABLE[i].gpio;
        drive(i, false);
        gpio_set_function(gpio, GPIO_FUNC_SIO);
        gpio_set_dir(gpio, GPIO_OUT);
        pwm_set_enabled(pwm_gpio_to_slice_num(gpio), false);
        g_state[i].mode = OUT_MODE_OFF;
    }
}

bool out_poll(void)
{
    if (rr.finished) {
        rr.finished = false;
        rr_release_pins();
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------ */

static const char *group_name(pin_group_t g)
{
    return g == PIN_GROUP_ANODE ? "anode" : "cathode";
}

void out_print_pins(void)
{
    printf("  %-6s %-4s %-8s %s\n", "name", "gpio", "group", "polarity");
    for (int i = 0; i < PIN_COUNT; i++)
        printf("  %-6s GP%-2u %-8s %s\n", PIN_TABLE[i].name, PIN_TABLE[i].gpio,
               group_name(PIN_TABLE[i].group),
               PIN_TABLE[i].active_low ? "active-low" : "active-high");
}

void out_print_status(void)
{
    printf("  %-6s %-4s %-8s %-5s %s\n", "name", "gpio", "group", "mode", "details");
    for (int i = 0; i < PIN_COUNT; i++) {
        const pin_def_t *p = &PIN_TABLE[i];
        printf("  %-6s GP%-2u %-8s ", p->name, p->gpio, group_name(p->group));
        switch (g_state[i].mode) {
        case OUT_MODE_OFF:
            printf("off\n");
            break;
        case OUT_MODE_ON:
            printf("ON\n");
            break;
        case OUT_MODE_PWM:
            printf("PWM   %.3f Hz, %.2f %% (slice %u%c)\n", g_state[i].pwm_freq,
                   g_state[i].pwm_duty, pwm_gpio_to_slice_num(p->gpio),
                   pwm_gpio_to_channel(p->gpio) == PWM_CHAN_A ? 'A' : 'B');
            break;
        case OUT_MODE_RR:
            printf("RR    %s\n", level_is_on(i) ? "lit" : "dark");
            break;
        case OUT_MODE_MX:
            printf("MX    multiplexing\n");
            break;
        }
    }
    if (rr.running) {
        printf("round robin: %d steps, on %lu us, off %lu us, cycle %lu/",
               rr.n, (unsigned long)rr.on_us, (unsigned long)rr.off_us,
               (unsigned long)rr.cycles_done + 1);
        if (rr.cycles)
            printf("%lu\n", (unsigned long)rr.cycles);
        else
            printf("inf\n");
    }
}

/* ------------------------------------------------------------------------ */
/* Hooks for the multiplexer                                                 */
/* ------------------------------------------------------------------------ */

out_mode_t out_get_mode(int idx)
{
    return valid_idx(idx) ? g_state[idx].mode : OUT_MODE_OFF;
}

void out_claim(int idx, out_mode_t mode)
{
    if (!valid_idx(idx))
        return;
    make_plain(idx);
    g_state[idx].mode = mode;
}

void out_release(int idx)
{
    if (!valid_idx(idx))
        return;
    drive(idx, false);
    g_state[idx].mode = OUT_MODE_OFF;
}

void out_drive(int idx, bool on)
{
    drive(idx, on);
}
