#include "mux.h"

#include <math.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "outputs.h"
#include "pico/stdlib.h"
#include "pins_config.h"

#define MX_MAX_CATH    32
#define MX_MIN_SEG_US  RR_MIN_US      /* shortest non-zero segment          */
#define MX_MAX_PERIOD  10000000u      /* 10 s, i.e. freq >= 0.1 Hz          */
#define MX_MAX_HOLD_US 100000u        /* 100 ms                             */

/* ------------------------------------------------------------------------ */
/* Configuration (from the pin table)                                       */
/* ------------------------------------------------------------------------ */

static const char *g_init_err = "not initialised";
static int       g_anode[MX_TUBES];
static int       g_cath[MX_MAX_CATH];        /* pin index of cathode bit i  */
static int       g_ncath;
static uint32_t  g_cath_gpio_bit[MX_MAX_CATH];
static uint32_t  g_cath_gpio_mask;           /* all cathode GPIOs           */
static uint32_t  g_cath_idle;                /* their levels, all off       */

/* ------------------------------------------------------------------------ */
/* State shared with the alarm IRQ                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t mask;          /* static selection, bit i = g_cath[i] */
    volatile bool     rr_active;
    volatile bool     rr_finished;
    uint8_t           rr_steps[RR_MAX_STEPS];   /* cathode bit numbers      */
    int               rr_n;
    uint32_t          rr_on_us;
    uint32_t          rr_off_us;
    uint32_t          rr_cycles;     /* 0 = forever */
    uint64_t          rr_t0;
} tube_t;

static tube_t g_tube[MX_TUBES];

static struct {
    volatile bool running;
    alarm_id_t    alarm;
    uint32_t      dur[4];            /* on0, gap0, on1, gap1 in us       */
    uint32_t      seg_us[6];         /* on0, hold, idle0, on1, hold, idle1 */
    volatile int  seg;               /* segment to start next            */
    uint32_t      period_us;
    double        freq;              /* actual                           */
    double        freq_req;          /* as requested, for retuning       */
    double        duty[MX_TUBES];
    uint32_t      delay_us;
} mx;

/* Cathodes stay on this long after the anode is switched off. */
static uint32_t g_hold_us = MX_DEFAULT_HOLD_US;

/* ------------------------------------------------------------------------ */
/* Init / lookup                                                            */
/* ------------------------------------------------------------------------ */

void mux_init(void)
{
    int na = 0;
    g_ncath = 0;
    g_cath_gpio_mask = 0;
    g_cath_idle = 0;

    for (int i = 0; i < PIN_COUNT; i++) {
        const pin_def_t *p = &PIN_TABLE[i];
        if (p->group == PIN_GROUP_ANODE) {
            if (na < MX_TUBES)
                g_anode[na++] = i;
        } else {
            if (g_ncath >= MX_MAX_CATH || p->gpio >= 32) {
                g_init_err = "multiplexing supports up to 32 cathodes on GP0..GP31";
                return;
            }
            g_cath[g_ncath] = i;
            g_cath_gpio_bit[g_ncath] = 1u << p->gpio;
            g_cath_gpio_mask |= 1u << p->gpio;
            if (p->active_low)
                g_cath_idle |= 1u << p->gpio;
            g_ncath++;
        }
    }
    if (na < MX_TUBES) {
        g_init_err = "multiplexing needs two anode pins in pins_config.h";
        return;
    }
    if (g_ncath == 0) {
        g_init_err = "multiplexing needs cathode pins in pins_config.h";
        return;
    }
    g_init_err = NULL;
}

int mux_tube_from_pin(int idx)
{
    if (g_init_err)
        return -1;
    for (int t = 0; t < MX_TUBES; t++)
        if (g_anode[t] == idx)
            return t;
    return -1;
}

int mux_anode_pin(int tube)
{
    return (tube >= 0 && tube < MX_TUBES && !g_init_err) ? g_anode[tube] : -1;
}

static int cath_bit(int idx)
{
    for (int i = 0; i < g_ncath; i++)
        if (g_cath[i] == idx)
            return i;
    return -1;
}

bool mux_is_cathode(int idx)
{
    return cath_bit(idx) >= 0;
}

bool mux_running(void)
{
    return mx.running;
}

/* ------------------------------------------------------------------------ */
/* IRQ side                                                                 */
/* ------------------------------------------------------------------------ */

/* Cathode mask to show on a tube right now (static or round robin). */
static uint32_t tube_mask_now(tube_t *tb)
{
    if (!tb->rr_active)
        return tb->mask;

    uint64_t step = (uint64_t)tb->rr_on_us + tb->rr_off_us;
    uint64_t cyc  = step * (uint64_t)tb->rr_n;
    uint64_t e    = time_us_64() - tb->rr_t0;

    if (tb->rr_cycles && e >= cyc * tb->rr_cycles) {
        tb->rr_active   = false;
        tb->mask        = 0;
        tb->rr_finished = true;
        return 0;
    }
    uint64_t r   = e % cyc;
    uint32_t pos = (uint32_t)(r / step);
    return (r - (uint64_t)pos * step) < tb->rr_on_us ? (1u << tb->rr_steps[pos]) : 0u;
}

static void tube_light(int t)
{
    uint32_t m   = tube_mask_now(&g_tube[t]);
    uint32_t val = g_cath_idle;
    for (int i = 0; i < g_ncath; i++)
        if (m & (1u << i))
            val ^= g_cath_gpio_bit[i];          /* active level = !idle */
    gpio_put_masked(g_cath_gpio_mask, val);     /* cathodes (if any) first ... */
    out_drive(g_anode[t], true);                /* ... then the anode          */
}

/* Tube -> idle, step 1: anode off. The cathodes stay as they are for the
 * hold time, so the anode's residual charge drains through the same digit. */
static void tube_anode_off(int t)
{
    out_drive(g_anode[t], false);
}

/* Tube -> idle, step 2 (after the hold time): all cathodes off = idle. */
static void cathodes_off(void)
{
    gpio_put_masked(g_cath_gpio_mask, g_cath_idle);
}

/* Segment s starts now:
 *   0 = tube 0 on (cathodes, then anode)   3 = tube 1 on
 *   1 = tube 0 anode off, hold starts      4 = tube 1 anode off, hold starts
 *   2 = all cathodes off (idle)            5 = all cathodes off (idle)
 * Zero-length segments are applied and skipped in the same call. A negative
 * return value reschedules relative to the previous target (no drift). */
static int64_t mx_alarm_cb(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;

    if (!mx.running)
        return 0;

    for (int guard = 0; guard < 6; guard++) {
        int      s = mx.seg;
        uint32_t d = mx.seg_us[s];
        int      t = s / 3;

        switch (s % 3) {
        case 0:
            if (d)
                tube_light(t);
            break;
        case 1:
            tube_anode_off(t);
            break;
        default:
            cathodes_off();
            break;
        }
        mx.seg = s == 5 ? 0 : s + 1;
        if (d)
            return -(int64_t)d;
    }
    return -(int64_t)mx.period_us;   /* not reached: period > 0 */
}

/* ------------------------------------------------------------------------ */
/* Anode multiplexing                                                       */
/* ------------------------------------------------------------------------ */

static void mx_cancel(void)
{
    if (mx.running) {
        mx.running = false;
        cancel_alarm(mx.alarm);
    }
}

/* Everything off (start/retune/stop): anodes first, then the cathodes. */
static void all_dark(void)
{
    bool was_lit = false;
    for (int t = 0; t < MX_TUBES; t++) {
        was_lit |= out_get_mode(g_anode[t]) != OUT_MODE_OFF &&
                   gpio_get_out_level(PIN_TABLE[g_anode[t]].gpio) != PIN_TABLE[g_anode[t]].active_low;
        out_drive(g_anode[t], false);           /* anodes off ...             */
    }
    if (was_lit && g_hold_us)
        busy_wait_us_32(g_hold_us);             /* ... same hold as in the ISR */
    gpio_put_masked(g_cath_gpio_mask, g_cath_idle);   /* ... then cathodes  */
}

const char *mux_start(double freq_hz, double duty1_pct, double duty2_pct,
                      uint32_t delay_us)
{
    static char msg[112];

    if (g_init_err)
        return g_init_err;
    if (!(duty1_pct >= 0.0 && duty1_pct <= 100.0) ||
        !(duty2_pct >= 0.0 && duty2_pct <= 100.0))
        return "duty must be 0..100 %";

    double period = freq_hz > 0.0 ? 1e6 / freq_hz : 0.0;
    double fmax   = 1e6 / (4.0 * MX_MIN_SEG_US);
    if (!(period >= 4.0 * MX_MIN_SEG_US && period <= (double)MX_MAX_PERIOD)) {
        snprintf(msg, sizeof msg, "frequency must be 0.1 Hz .. %.0f Hz", fmax);
        return msg;
    }

    double total = duty1_pct + duty2_pct + 200.0 * (double)delay_us / period;
    if (total > 100.0 + 1e-9) {
        snprintf(msg, sizeof msg,
                 "duty1 + duty2 + 2*delay/period = %.2f + %.2f + %.2f = %.2f %% > 100 %%",
                 duty1_pct, duty2_pct, 200.0 * (double)delay_us / period, total);
        return msg;
    }

    uint32_t p   = (uint32_t)lround(period);
    uint32_t on0 = (uint32_t)lround(duty1_pct * p / 100.0);
    uint32_t on1 = (uint32_t)lround(duty2_pct * p / 100.0);
    /* Rounding to whole microseconds must not eat into the second gap. */
    int64_t room = (int64_t)p - on0 - 2 * (int64_t)delay_us;
    if (room < 0) {
        snprintf(msg, sizeof msg, "delay too long for this frequency");
        return msg;
    }
    if ((int64_t)on1 > room)
        on1 = (uint32_t)room;

    uint32_t dur[4];
    dur[0] = on0;
    dur[1] = delay_us;
    dur[2] = on1;
    dur[3] = p - on0 - delay_us - on1;          /* >= delay_us */

    if (g_hold_us > delay_us) {
        snprintf(msg, sizeof msg,
                 "cathode hold (%lu us) is longer than the delay (%lu us): "
                 "use a longer delay or a shorter 'mxhold'",
                 (unsigned long)g_hold_us, (unsigned long)delay_us);
        return msg;
    }
    uint32_t seg[6];
    seg[0] = dur[0];
    seg[1] = g_hold_us;
    seg[2] = dur[1] - g_hold_us;
    seg[3] = dur[2];
    seg[4] = g_hold_us;
    seg[5] = dur[3] - g_hold_us;               /* dur[3] >= delay >= hold */

    static const char *seg_name[6] = { "tube 0 on time", "cathode hold", "delay - hold",
                                       "tube 1 on time", "cathode hold",
                                       "second gap - hold" };
    for (int s = 0; s < 6; s++) {
        if (seg[s] != 0 && seg[s] < MX_MIN_SEG_US) {
            snprintf(msg, sizeof msg,
                     "%s is %lu us, min %d us (use 0, a longer time or a lower frequency)",
                     seg_name[s], (unsigned long)seg[s], MX_MIN_SEG_US);
            return msg;
        }
    }

    /* Take over the pins. A round robin using any of them is stopped. */
    bool rr_stopped = false;
    for (int t = 0; t < MX_TUBES && !rr_stopped; t++)
        rr_stopped = out_get_mode(g_anode[t]) == OUT_MODE_RR;
    for (int i = 0; i < g_ncath && !rr_stopped; i++)
        rr_stopped = out_get_mode(g_cath[i]) == OUT_MODE_RR;
    if (rr_stopped)
        out_rr_stop();

    /* Anodes off before cathodes, also when retuning or taking over pins
     * that were switched on by hand. */
    mx_cancel();
    all_dark();
    for (int t = 0; t < MX_TUBES; t++)
        out_claim(g_anode[t], OUT_MODE_MX);
    for (int i = 0; i < g_ncath; i++)
        out_claim(g_cath[i], OUT_MODE_MX);

    for (int s = 0; s < 4; s++)
        mx.dur[s] = dur[s];
    for (int s = 0; s < 6; s++)
        mx.seg_us[s] = seg[s];
    mx.period_us = p;
    mx.freq      = 1e6 / (double)p;
    mx.freq_req  = freq_hz;
    mx.duty[0]   = duty1_pct;
    mx.duty[1]   = duty2_pct;
    mx.delay_us  = delay_us;
    mx.seg       = 0;
    mx.running   = true;

    mx.alarm = add_alarm_in_us(MX_MIN_SEG_US, mx_alarm_cb, NULL, true);
    if (mx.alarm <= 0) {
        mx.running = false;
        for (int t = 0; t < MX_TUBES; t++)
            out_release(g_anode[t]);
        for (int i = 0; i < g_ncath; i++)
            out_release(g_cath[i]);
        return "no free hardware alarm";
    }
    if (rr_stopped)
        printf("note: round robin stopped, its pins are used by multiplexing now\n");
    return NULL;
}

uint32_t mux_get_hold(void)
{
    return g_hold_us;
}

const char *mux_set_hold(uint32_t hold_us)
{
    if (hold_us != 0 && hold_us < MX_MIN_SEG_US)
        return "hold must be 0 or at least 20 us";
    if (hold_us > MX_MAX_HOLD_US)
        return "hold must be at most 100 ms";
    uint32_t old = g_hold_us;
    g_hold_us = hold_us;
    if (mx.running) {
        const char *err = mux_start(mx.freq_req, mx.duty[0], mx.duty[1], mx.delay_us);
        if (err) {
            g_hold_us = old;            /* keep running with the old value */
            return err;
        }
    }
    return NULL;
}

bool mux_stop(void)
{
    if (!mx.running)
        return false;
    mx_cancel();
    all_dark();
    for (int t = 0; t < MX_TUBES; t++)
        out_release(g_anode[t]);
    for (int i = 0; i < g_ncath; i++)
        out_release(g_cath[i]);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Per-tube cathode selection                                               */
/* ------------------------------------------------------------------------ */

static bool valid_tube(int t)
{
    return t >= 0 && t < MX_TUBES;
}

const char *mux_set(int tube, const int *cath, int n, bool on)
{
    if (g_init_err)
        return g_init_err;
    if (!valid_tube(tube))
        return "invalid tube";
    if (g_tube[tube].rr_active)
        return "this tube runs mxrr (use 'mxrr <anode> stop' or 'mx off <anode>')";

    uint32_t bits = 0;
    for (int i = 0; i < n; i++) {
        int b = cath_bit(cath[i]);
        if (b < 0)
            return "not a cathode pin";
        bits |= 1u << b;
    }
    if (on)
        g_tube[tube].mask |= bits;
    else
        g_tube[tube].mask &= ~bits;
    return NULL;
}

void mux_clear(int tube)
{
    if (!valid_tube(tube))
        return;
    uint32_t s = save_and_disable_interrupts();
    g_tube[tube].rr_active   = false;
    g_tube[tube].rr_finished = false;
    g_tube[tube].mask        = 0;
    restore_interrupts(s);
}

const char *mux_rr_start(int tube, const int *cath, int n,
                         uint32_t on_us, uint32_t off_us, uint32_t cycles)
{
    if (g_init_err)
        return g_init_err;
    if (!valid_tube(tube))
        return "invalid tube";
    if (n < 1)
        return "empty pin list";
    if (n > RR_MAX_STEPS)
        return "too many pins in the list";
    if (on_us < RR_MIN_US)
        return "on time too short (min 20 us)";
    if (off_us != 0 && off_us < RR_MIN_US)
        return "off time too short (0 or min 20 us)";

    uint8_t steps[RR_MAX_STEPS];
    for (int i = 0; i < n; i++) {
        int b = cath_bit(cath[i]);
        if (b < 0)
            return "not a cathode pin";
        steps[i] = (uint8_t)b;
    }

    tube_t *tb = &g_tube[tube];
    uint32_t s = save_and_disable_interrupts();
    for (int i = 0; i < n; i++)
        tb->rr_steps[i] = steps[i];
    tb->rr_n        = n;
    tb->rr_on_us    = on_us;
    tb->rr_off_us   = off_us;
    tb->rr_cycles   = cycles;
    tb->rr_t0       = time_us_64();
    tb->mask        = 0;
    tb->rr_finished = false;
    tb->rr_active   = true;
    restore_interrupts(s);
    return NULL;
}

bool mux_rr_stop(int tube)
{
    if (!valid_tube(tube) || !g_tube[tube].rr_active)
        return false;
    mux_clear(tube);
    return true;
}

void mux_all_off(void)
{
    if (g_init_err)
        return;
    mux_stop();
    for (int t = 0; t < MX_TUBES; t++)
        mux_clear(t);
}

unsigned mux_poll(void)
{
    unsigned done = 0;
    for (int t = 0; t < MX_TUBES; t++) {
        if (g_tube[t].rr_finished) {
            g_tube[t].rr_finished = false;
            done |= 1u << t;
        }
    }
    return done;
}

/* ------------------------------------------------------------------------ */
/* Reporting                                                                */
/* ------------------------------------------------------------------------ */

void mux_print_timing(void)
{
    if (!mx.running) {
        printf("multiplexing: off (cathode hold %lu us)\n", (unsigned long)g_hold_us);
        return;
    }
    printf("multiplexing: %.3f Hz, period %lu us\n", mx.freq, (unsigned long)mx.period_us);
    printf("  %s on %lu us (%.2f %%) | gap %lu us | %s on %lu us (%.2f %%) | gap %lu us\n",
           PIN_TABLE[g_anode[0]].name, (unsigned long)mx.dur[0], mx.duty[0],
           (unsigned long)mx.dur[1],
           PIN_TABLE[g_anode[1]].name, (unsigned long)mx.dur[2], mx.duty[1],
           (unsigned long)mx.dur[3]);
    printf("  cathodes stay on %lu us after each anode off, then all off (mxhold)\n",
           (unsigned long)g_hold_us);
}

static void print_time(uint32_t us)
{
    if (us >= 1000000u && us % 1000u == 0)
        printf("%.3f s", us / 1e6);
    else if (us >= 1000u && us % 1000u == 0)
        printf("%lu ms", (unsigned long)(us / 1000u));
    else
        printf("%lu us", (unsigned long)us);
}

static void print_mask(uint32_t m)
{
    bool first = true;
    for (int i = 0; i < g_ncath; i++) {
        if (m & (1u << i)) {
            printf("%s%s", first ? "" : ",", PIN_TABLE[g_cath[i]].name);
            first = false;
        }
    }
    if (first)
        printf("none");
}

void mux_print_status(void)
{
    if (g_init_err) {
        printf("multiplexing: unavailable (%s)\n", g_init_err);
        return;
    }
    mux_print_timing();
    for (int t = 0; t < MX_TUBES; t++) {
        tube_t *tb = &g_tube[t];
        printf("  tube %d (%s): ", t, PIN_TABLE[g_anode[t]].name);
        if (tb->rr_active) {
            uint64_t cyc = ((uint64_t)tb->rr_on_us + tb->rr_off_us) * (uint64_t)tb->rr_n;
            uint64_t done = (time_us_64() - tb->rr_t0) / cyc;
            printf("mxrr");
            for (int i = 0; i < tb->rr_n; i++)
                printf("%s%s", i ? "," : " ", PIN_TABLE[g_cath[tb->rr_steps[i]]].name);
            printf(", on ");
            print_time(tb->rr_on_us);
            printf(", off ");
            print_time(tb->rr_off_us);
            printf(", cycle %llu/", (unsigned long long)done + 1);
            if (tb->rr_cycles)
                printf("%lu\n", (unsigned long)tb->rr_cycles);
            else
                printf("inf\n");
        } else {
            print_mask(tb->mask);
            printf("\n");
        }
    }
}
