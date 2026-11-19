/*
 * Control of the cathode transistors / anode optocouplers listed in
 * pins_config.h: static on/off, hardware PWM and round robin.
 */
#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <stdbool.h>
#include <stdint.h>

#define RR_MAX_STEPS   64     /* max number of entries in a round-robin list */
#define RR_MIN_US      20     /* shortest allowed on/off interval            */

typedef enum {
    OUT_MODE_OFF = 0,
    OUT_MODE_ON,
    OUT_MODE_PWM,
    OUT_MODE_RR,
    OUT_MODE_MX,          /* owned by the multiplexer (mux.c) */
} out_mode_t;

/* Drive every configured pin to its inactive level. Call first thing. */
void out_init(void);

/* All functions below return NULL on success or a static error string. */

const char *out_set(int idx, bool on);
const char *out_toggle(int idx);

/* freq in Hz, duty in percent (0..100). Actual values are written back. */
const char *out_pwm(int idx, double freq_hz, double duty_pct,
                    double *actual_freq, double *actual_duty);

/* Light idx[0], idx[1], ... in turn: each for on_us, followed by off_us of
 * everything dark. cycles = 0 means run forever. */
const char *out_rr_start(const int *idx, int n,
                         uint32_t on_us, uint32_t off_us, uint32_t cycles);
bool out_rr_stop(void);          /* returns true if it was running */
bool out_rr_running(void);

/* Stop round robin and PWM and switch every pin off. Safe to call anytime. */
void out_all_off(void);

/* Call from the main loop. Returns true (once) when a round robin with a
 * finite number of cycles has completed. */
bool out_poll(void);

void out_print_status(void);
void out_print_pins(void);

/* Hooks for the multiplexer (mux.c). */
out_mode_t out_get_mode(int idx);
void out_claim(int idx, out_mode_t mode);   /* stop PWM, switch off, set mode */
void out_release(int idx);                  /* switch off, mode = OFF         */
void out_drive(int idx, bool on);           /* raw level, IRQ safe            */

#endif /* OUTPUTS_H */
