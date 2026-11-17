/*
 * Nixie driver board tester - Raspberry Pi Pico (RP2040 / RP2350)
 *
 * Simple command line on the UART (GP0 = TX, GP1 = RX, 115200 8N1) to switch
 * the cathode transistors (MPSA42) and anode optocouplers on/off, drive them
 * with hardware PWM, or cycle through them in round robin.
 *
 * Pin map: pins_config.h. Type "help" in the terminal for the commands.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "outputs.h"
#include "pico/stdlib.h"
#include "pins_config.h"

#define LINE_MAX_LEN  128
#define MAX_ARGS      16
#define PROMPT        "nixie> "

/* ======================================================================== */
/* Argument parsing                                                         */
/* ======================================================================== */

/* Parse a GPIO number written as "5", "gp5" or "GP5". */
static bool parse_gpio_number(const char *s, int *gpio)
{
    if (strncasecmp(s, "gp", 2) == 0)
        s += 2;
    if (!isdigit((unsigned char)*s))
        return false;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 47)
        return false;
    *gpio = (int)v;
    return true;
}

static int find_by_name(const char *s)
{
    for (int i = 0; i < PIN_COUNT; i++)
        if (strcasecmp(s, PIN_TABLE[i].name) == 0)
            return i;
    return -1;
}

static int find_by_gpio(int gpio)
{
    for (int i = 0; i < PIN_COUNT; i++)
        if (PIN_TABLE[i].gpio == gpio)
            return i;
    return -1;
}

/* A single pin: name from the table first, then GPIO number. */
static int find_pin(const char *s)
{
    int idx = find_by_name(s);
    if (idx >= 0)
        return idx;
    int gpio;
    if (parse_gpio_number(s, &gpio))
        return find_by_gpio(gpio);
    return -1;
}

static char g_err[96];

static const char *push(int *out, int max, int *n, int idx)
{
    if (*n >= max)
        return "too many pins in the list";
    out[(*n)++] = idx;
    return NULL;
}

/*
 * Parse a pin list such as
 *     K3            single pin by name
 *     5 / GP5       single pin by GPIO number
 *     K0,K5,A1      comma separated
 *     K0-K9         range in table order (K9-K0 runs backwards)
 *     2-11          range of GPIO numbers (GPIOs not in the table are skipped)
 *     cathodes / anodes / all
 * Items can be combined: "K0-K4,DP,anodes".
 */
static const char *parse_pin_list(const char *arg, int *out, int max, int *n)
{
    char buf[LINE_MAX_LEN];
    strncpy(buf, arg, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    for (char *save = NULL, *item = strtok_r(buf, ",", &save); item;
         item = strtok_r(NULL, ",", &save)) {
        const char *err = NULL;

        if (strcasecmp(item, "all") == 0 || strcasecmp(item, "cathodes") == 0 ||
            strcasecmp(item, "anodes") == 0) {
            bool all = strcasecmp(item, "all") == 0;
            pin_group_t grp = strcasecmp(item, "anodes") == 0 ? PIN_GROUP_ANODE
                                                               : PIN_GROUP_CATHODE;
            for (int i = 0; i < PIN_COUNT && !err; i++)
                if (all || PIN_TABLE[i].group == grp)
                    err = push(out, max, n, i);
        } else if (strchr(item, '-')) {
            char *dash = strchr(item, '-');
            *dash = '\0';
            const char *a = item, *b = dash + 1;
            int ga, gb;
            if (parse_gpio_number(a, &ga) && parse_gpio_number(b, &gb) &&
                find_by_name(a) < 0 && find_by_name(b) < 0) {
                int step = ga <= gb ? 1 : -1, added = 0;
                for (int g = ga; !err; g += step) {
                    int idx = find_by_gpio(g);
                    if (idx >= 0) {
                        err = push(out, max, n, idx);
                        added++;
                    }
                    if (g == gb)
                        break;
                }
                if (!err && added == 0) {
                    snprintf(g_err, sizeof g_err, "no configured pins in GPIO range %s-%s", a, b);
                    return g_err;
                }
            } else {
                int ia = find_pin(a), ib = find_pin(b);
                if (ia < 0 || ib < 0) {
                    snprintf(g_err, sizeof g_err, "unknown pin '%s'", ia < 0 ? a : b);
                    return g_err;
                }
                int step = ia <= ib ? 1 : -1;
                for (int i = ia; !err; i += step) {
                    err = push(out, max, n, i);
                    if (i == ib)
                        break;
                }
            }
        } else {
            int idx = find_pin(item);
            if (idx < 0) {
                snprintf(g_err, sizeof g_err, "unknown pin '%s' (see 'pins')", item);
                return g_err;
            }
            err = push(out, max, n, idx);
        }
        if (err)
            return err;
    }
    if (*n == 0)
        return "empty pin list";
    return NULL;
}

/* Number followed by an optional suffix. Returns the suffix or NULL. */
static const char *parse_number(const char *s, double *v)
{
    char *end;
    *v = strtod(s, &end);
    if (end == s)
        return NULL;
    return end;
}

/* Duration: "500" (ms), "500ms", "200us", "1.5s". Result in microseconds. */
static bool parse_duration_us(const char *s, uint32_t *us, bool allow_zero)
{
    double v;
    const char *suf = parse_number(s, &v);
    if (!suf)
        return false;
    double mul;
    if (*suf == '\0' || strcasecmp(suf, "ms") == 0)
        mul = 1000.0;
    else if (strcasecmp(suf, "us") == 0)
        mul = 1.0;
    else if (strcasecmp(suf, "s") == 0)
        mul = 1e6;
    else
        return false;
    double r = v * mul;
    if (r < 0.0 || r > 4.0e9 || (!allow_zero && r < 1.0))
        return false;
    *us = (uint32_t)(r + 0.5);
    return true;
}

/* Frequency: "1000", "1000hz", "2.5k", "2.5khz", "1m", "1mhz". */
static bool parse_freq(const char *s, double *hz)
{
    double v;
    const char *suf = parse_number(s, &v);
    if (!suf)
        return false;
    if (*suf == '\0' || strcasecmp(suf, "hz") == 0)
        *hz = v;
    else if (strcasecmp(suf, "k") == 0 || strcasecmp(suf, "khz") == 0)
        *hz = v * 1e3;
    else if (strcasecmp(suf, "m") == 0 || strcasecmp(suf, "mhz") == 0)
        *hz = v * 1e6;
    else
        return false;
    return *hz > 0.0;
}

/* Duty: "25", "25%", "12.5". */
static bool parse_duty(const char *s, double *pct)
{
    const char *suf = parse_number(s, pct);
    if (!suf || (*suf != '\0' && strcmp(suf, "%") != 0))
        return false;
    return *pct >= 0.0 && *pct <= 100.0;
}

static bool parse_uint(const char *s, uint32_t *v)
{
    char *end;
    if (!isdigit((unsigned char)*s))
        return false;
    unsigned long r = strtoul(s, &end, 10);
    if (*end != '\0')
        return false;
    *v = (uint32_t)r;
    return true;
}

static void print_duration(uint32_t us)
{
    if (us >= 1000000u && us % 1000u == 0)
        printf("%.3f s", us / 1e6);
    else if (us >= 1000u && us % 1000u == 0)
        printf("%lu ms", (unsigned long)(us / 1000u));
    else
        printf("%lu us", (unsigned long)us);
}

/* ======================================================================== */
/* Commands                                                                 */
/* ======================================================================== */

typedef void (*cmd_fn_t)(int argc, char **argv);

static void cmd_help(int argc, char **argv);

static void report(const char *name, const char *err)
{
    if (err)
        printf("error: %s: %s\n", name, err);
}

/* on / off / toggle <pins> [<pins> ...] */
static void cmd_switch(int argc, char **argv, int what /* 0 off, 1 on, 2 toggle */)
{
    if (argc < 2) {
        printf("usage: %s <pins> [<pins> ...]\n", argv[0]);
        return;
    }
    int idx[RR_MAX_STEPS], n = 0;
    for (int a = 1; a < argc; a++) {
        const char *err = parse_pin_list(argv[a], idx, RR_MAX_STEPS, &n);
        if (err) {
            printf("error: %s\n", err);
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        const char *err = what == 2 ? out_toggle(idx[i]) : out_set(idx[i], what == 1);
        report(PIN_TABLE[idx[i]].name, err);
    }
    printf("ok\n");
}

static void cmd_on(int argc, char **argv)     { cmd_switch(argc, argv, 1); }
static void cmd_off(int argc, char **argv)    { cmd_switch(argc, argv, 0); }
static void cmd_toggle(int argc, char **argv) { cmd_switch(argc, argv, 2); }

static void cmd_alloff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    out_all_off();
    printf("all outputs off\n");
}

/* pwm <pins> <freq> <duty>   |   pwm <pins> off */
static void cmd_pwm(int argc, char **argv)
{
    if (argc == 3 && strcasecmp(argv[2], "off") == 0) {
        char *v[] = { "off", argv[1] };
        cmd_off(2, v);
        return;
    }
    if (argc != 4) {
        printf("usage: pwm <pins> <freq> <duty>   e.g. pwm K3 1k 25\n"
               "       pwm <pins> off\n");
        return;
    }
    int idx[RR_MAX_STEPS], n = 0;
    const char *err = parse_pin_list(argv[1], idx, RR_MAX_STEPS, &n);
    if (err) {
        printf("error: %s\n", err);
        return;
    }
    double freq, duty;
    if (!parse_freq(argv[2], &freq)) {
        printf("error: bad frequency '%s' (e.g. 200, 1k, 2.5khz)\n", argv[2]);
        return;
    }
    if (!parse_duty(argv[3], &duty)) {
        printf("error: bad duty '%s' (0..100, e.g. 25 or 12.5%%)\n", argv[3]);
        return;
    }
    for (int i = 0; i < n; i++) {
        double f, d;
        err = out_pwm(idx[i], freq, duty, &f, &d);
        if (err)
            report(PIN_TABLE[idx[i]].name, err);
        else
            printf("%s: PWM %.3f Hz, duty %.3f %%\n", PIN_TABLE[idx[i]].name, f, d);
    }
}

/* rr <pins> <on> [off] [cycles]   |   rr stop */
static void cmd_rr(int argc, char **argv)
{
    if (argc == 2 && strcasecmp(argv[1], "stop") == 0) {
        printf(out_rr_stop() ? "round robin stopped\n" : "round robin not running\n");
        return;
    }
    if (argc < 3 || argc > 5) {
        printf("usage: rr <pins> <on_time> [off_time] [cycles]   e.g. rr K0-K9 500 100\n"
               "       rr stop\n");
        return;
    }
    int idx[RR_MAX_STEPS], n = 0;
    const char *err = parse_pin_list(argv[1], idx, RR_MAX_STEPS, &n);
    if (err) {
        printf("error: %s\n", err);
        return;
    }
    uint32_t on_us, off_us = 0, cycles = 0;
    if (!parse_duration_us(argv[2], &on_us, false)) {
        printf("error: bad on time '%s' (e.g. 500, 500ms, 200us, 1s)\n", argv[2]);
        return;
    }
    if (argc >= 4 && !parse_duration_us(argv[3], &off_us, true)) {
        printf("error: bad off time '%s' (e.g. 0, 100, 100ms, 50us)\n", argv[3]);
        return;
    }
    if (argc >= 5 && !parse_uint(argv[4], &cycles)) {
        printf("error: bad cycle count '%s' (0 = forever)\n", argv[4]);
        return;
    }
    err = out_rr_start(idx, n, on_us, off_us, cycles);
    if (err) {
        printf("error: %s\n", err);
        return;
    }
    printf("round robin:");
    for (int i = 0; i < n; i++)
        printf(" %s", PIN_TABLE[idx[i]].name);
    printf("\n  on ");
    print_duration(on_us);
    printf(", off ");
    print_duration(off_us);
    if (cycles)
        printf(", %lu cycle(s)\n", (unsigned long)cycles);
    else
        printf(", forever ('rr stop' or 'alloff' to end)\n");
}

static void cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    out_print_status();
}

static void cmd_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    out_print_pins();
}

typedef struct {
    const char *name;
    const char *alias;
    cmd_fn_t    fn;
    const char *usage;
    const char *help;
} cmd_t;

static const cmd_t COMMANDS[] = {
    { "help",   "?",    cmd_help,   "",                               "show this help" },
    { "pins",   NULL,   cmd_pins,   "",                               "list configured pins" },
    { "status", "st",   cmd_status, "",                               "show state of every pin" },
    { "on",     NULL,   cmd_on,     "<pins> [<pins>...]",             "switch pin(s) on" },
    { "off",    NULL,   cmd_off,    "<pins> [<pins>...]",             "switch pin(s) off" },
    { "toggle", "t",    cmd_toggle, "<pins> [<pins>...]",             "toggle pin(s)" },
    { "alloff", "x",    cmd_alloff, "",                               "everything off (also Ctrl-C)" },
    { "pwm",    NULL,   cmd_pwm,    "<pins> <freq> <duty> | <pins> off", "hardware PWM" },
    { "rr",     NULL,   cmd_rr,     "<pins> <on> [off] [cycles] | stop", "round robin" },
};

#define CMD_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("commands:\n");
    for (int i = 0; i < CMD_COUNT; i++) {
        char left[64];
        snprintf(left, sizeof left, "%s %s", COMMANDS[i].name, COMMANDS[i].usage);
        printf("  %-42s %s", left, COMMANDS[i].help);
        if (COMMANDS[i].alias)
            printf(" [alias: %s]", COMMANDS[i].alias);
        printf("\n");
    }
    printf("\n"
           "pins:      K3 | GP5 | 5 | K0,K5,A1 | K0-K9 | K9-K0 | 2-11 | cathodes | anodes | all\n"
           "times:     500 = 500ms | 200us | 1.5s      (min 20us; off time may be 0)\n"
           "freq:      200 | 1k | 2.5khz               duty: 0..100 (%%)\n"
           "cycles:    0 or omitted = run forever\n"
           "\n"
           "examples:\n"
           "  on A0              anode of tube 0 on\n"
           "  rr K0-K9 1s 200    count 0..9 on the tube, 1 s each, 200 ms dark gap\n"
           "  rr A0-A5 2ms 100us multiplex anodes (2 ms each, 100 us blanking)\n"
           "  rr K5 500 500      blink K5 at 1 Hz\n"
           "  pwm A0 1k 30       anode 0 dimmed: 1 kHz, 30 %%\n"
           "  alloff             everything off\n");
}

/* ======================================================================== */
/* Line input                                                               */
/* ======================================================================== */

static void execute(char *line)
{
    char *argv[MAX_ARGS];
    int argc = 0;
    for (char *save = NULL, *tok = strtok_r(line, " \t", &save); tok && argc < MAX_ARGS;
         tok = strtok_r(NULL, " \t", &save))
        argv[argc++] = tok;
    if (argc == 0)
        return;

    for (int i = 0; i < CMD_COUNT; i++) {
        if (strcasecmp(argv[0], COMMANDS[i].name) == 0 ||
            (COMMANDS[i].alias && strcasecmp(argv[0], COMMANDS[i].alias) == 0)) {
            COMMANDS[i].fn(argc, argv);
            return;
        }
    }
    printf("unknown command '%s', type 'help'\n", argv[0]);
}

static void print_banner(void)
{
    printf("\n\n=== Nixie driver board tester ===\n"
           "All outputs are OFF. Type 'help' for commands, Ctrl-C = all off.\n\n");
    out_print_pins();
    printf("\n");
}

int main(void)
{
    /* Outputs to a safe state before anything else (stdio init can block). */
    out_init();

    stdio_init_all();
    sleep_ms(100);
    print_banner();

    char line[LINE_MAX_LEN];
    char last[LINE_MAX_LEN] = "";
    int  len = 0;
    int  esc = 0;           /* 0 none, 1 got ESC, 2 got ESC [ */
    bool prev_cr = false;

    printf(PROMPT);
    for (;;) {
        if (out_poll()) {
            printf("\r\n[round robin finished, pins off]\n" PROMPT "%.*s", len, line);
        }

        int c = getchar_timeout_us(10000);
        if (c == PICO_ERROR_TIMEOUT)
            continue;

        /* ANSI escape sequences: up arrow recalls the last command. */
        if (esc == 1) {
            esc = (c == '[') ? 2 : 0;
            continue;
        }
        if (esc == 2) {
            esc = 0;
            if (c == 'A' && last[0]) {
                while (len--)
                    printf("\b \b");
                len = (int)strlen(last);
                memcpy(line, last, (size_t)len);
                printf("%.*s", len, line);
            }
            continue;
        }

        if (c == '\n' && prev_cr) {          /* CR LF -> single enter */
            prev_cr = false;
            continue;
        }
        prev_cr = (c == '\r');

        switch (c) {
        case 0x1b:                           /* ESC */
            esc = 1;
            break;
        case 0x03:                           /* Ctrl-C: emergency all off */
            out_all_off();
            len = 0;
            printf("^C\nall outputs off\n" PROMPT);
            break;
        case '\r':
        case '\n':
            printf("\n");
            line[len] = '\0';
            if (len > 0) {
                strcpy(last, line);
                execute(line);
            }
            len = 0;
            printf(PROMPT);
            break;
        case '\b':
        case 0x7f:
            if (len > 0) {
                len--;
                printf("\b \b");
            }
            break;
        default:
            if (c >= 0x20 && c < 0x7f && len < LINE_MAX_LEN - 1) {
                line[len++] = (char)c;
                putchar(c);
            }
            break;
        }
    }
}
