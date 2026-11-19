/*
 * Pin map of the nixie driver test board.
 *
 * >>> EDIT THIS TABLE TO MATCH THE SCHEMATIC <<<
 *
 * Every pin the firmware touches must be listed here. Nothing else is driven,
 * so all other GPIOs stay untouched.
 *
 *  name        - name used in the CLI (case-insensitive). Must not contain
 *                spaces, ',' or '-'.
 *  gpio        - RP2040/RP2350 GPIO number.
 *  group       - PIN_GROUP_CATHODE or PIN_GROUP_ANODE. The groups can be
 *                used in commands as "cathodes" / "anodes" / "all".
 *  active_low  - false: GPIO high = element ON (e.g. GPIO -> resistor ->
 *                MPSA42 base, or GPIO -> resistor -> optocoupler LED anode).
 *                true:  GPIO low = element ON (e.g. optocoupler LED wired
 *                from 3V3 to the GPIO).
 *
 * Current board: two tubes sharing 10 cathode transistors (MPSA42) on
 * GP6..GP15, wired to digits 0 9 8 1 2 3 4 5 6 7 in GPIO order. Each tube has
 * its own anode optocoupler: tube 0 = A0 on GP18, tube 1 = A1 on GP17.
 *
 * Multiplexing (mxa / mx / mxrr commands) uses the first two anode pins of the
 * table as tube 0 and tube 1, and all cathode pins as the shared cathodes.
 * In mx commands a bare number is a digit: "5" means CATHODE_DIGIT_PREFIX"5".
 */
#ifndef PINS_CONFIG_H
#define PINS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PIN_GROUP_CATHODE = 0,
    PIN_GROUP_ANODE   = 1,
} pin_group_t;

typedef struct {
    const char *name;
    uint8_t     gpio;
    pin_group_t group;
    bool        active_low;
} pin_def_t;

static const pin_def_t PIN_TABLE[] = {
    /* name   gpio  group               active_low */
    /* Kn = cathode of digit n. Listed in digit order so "K0-K9" counts
     * 0..9; the GPIO wiring order is 0 9 8 1 2 3 4 5 6 7 (GP6..GP15). */
    { "K0",    6,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 0 */
    { "K1",    9,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 1 */
    { "K2",   10,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 2 */
    { "K3",   11,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 3 */
    { "K4",   12,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 4 */
    { "K5",   13,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 5 */
    { "K6",   14,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 6 */
    { "K7",   15,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 7 */
    { "K8",    8,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 8 */
    { "K9",    7,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 9 */

    { "A0",   18,   PIN_GROUP_ANODE,    false },   /* anode optocoupler, tube 0 */
    { "A1",   17,   PIN_GROUP_ANODE,    false },   /* anode optocoupler, tube 1 */
};

#define PIN_COUNT ((int)(sizeof(PIN_TABLE) / sizeof(PIN_TABLE[0])))

/* Cathode name = prefix + digit, used to turn "5" into "K5" in mx commands. */
#define CATHODE_DIGIT_PREFIX "K"

#endif /* PINS_CONFIG_H */
