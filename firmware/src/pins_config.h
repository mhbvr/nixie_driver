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
 * Current board: 10 cathode transistors (MPSA42, digits 0..9) on GP6..GP15
 * and one anode optocoupler on GP18.
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
    { "K0",    6,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 0 */
    { "K1",    7,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 1 */
    { "K2",    8,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 2 */
    { "K3",    9,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 3 */
    { "K4",   10,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 4 */
    { "K5",   11,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 5 */
    { "K6",   12,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 6 */
    { "K7",   13,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 7 */
    { "K8",   14,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 8 */
    { "K9",   15,   PIN_GROUP_CATHODE,  false },   /* MPSA42 base, digit 9 */

    { "A0",   18,   PIN_GROUP_ANODE,    false },   /* anode optocoupler */
};

#define PIN_COUNT ((int)(sizeof(PIN_TABLE) / sizeof(PIN_TABLE[0])))

#endif /* PINS_CONFIG_H */
