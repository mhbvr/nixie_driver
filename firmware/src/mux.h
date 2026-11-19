/*
 * Two-tube multiplexing: the tubes share the cathodes, each has its own anode.
 *
 * One multiplexing period (1 / freq) is split into four segments:
 *
 *   | tube 0 on      | gap   | tube 1 on      | gap (rest of period)  |
 *   | duty1 * period | delay | duty2 * period | >= delay              |
 *
 * At the start of a "tube on" segment that tube's anode is switched on first,
 * then the cathodes selected for it. At the start of a gap all cathodes are
 * switched off first, then the anode. So no cathode is ever on while no anode
 * is on, and during each gap (delay) every cathode and anode is off.
 * Validity: duty1 + duty2 + 2 * delay / period <= 100 %.
 *
 * Each tube shows either a static set of cathodes (mx on / mx off) or its
 * own round robin of cathodes (mxrr). The two tubes are independent.
 */
#ifndef MUX_H
#define MUX_H

#include <stdbool.h>
#include <stdint.h>

#define MX_TUBES 2

/* Find the anodes/cathodes in the pin table. Call once after out_init(). */
void mux_init(void);

/* Tube index for an anode pin index, or -1. */
int  mux_tube_from_pin(int idx);
int  mux_anode_pin(int tube);
bool mux_is_cathode(int idx);

/* All functions below return NULL on success or a static error string. */

/* Start (or retune, if already running) the anode multiplexing. */
const char *mux_start(double freq_hz, double duty1_pct, double duty2_pct,
                      uint32_t delay_us);
bool mux_stop(void);              /* true if it was running; selections kept */
bool mux_running(void);

/* Static cathode selection of one tube (mx on / mx off). */
const char *mux_set(int tube, const int *cath, int n, bool on);
void        mux_clear(int tube);  /* nothing selected, mxrr stopped */

/* Independent round robin of cathodes on one tube (mxrr). */
const char *mux_rr_start(int tube, const int *cath, int n,
                         uint32_t on_us, uint32_t off_us, uint32_t cycles);
bool mux_rr_stop(int tube);       /* true if it was running */

/* Stop everything and forget all selections (used by alloff). */
void mux_all_off(void);

/* Call from the main loop: bit t set = mxrr on tube t has finished. */
unsigned mux_poll(void);

void mux_print_timing(void);
void mux_print_status(void);

#endif /* MUX_H */
