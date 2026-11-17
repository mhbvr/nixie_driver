/*
 * Pin map of the nixie driver test board.
 *
 * >>> EDIT THIS TABLE TO MATCH THE SCHEMATIC <<<
 *
 * Every pin the firmware touches must be listed here. Nothing else is driven,
 * so the UART pins (GP0/GP1 by default) and other GPIOs stay untouched.
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
 * The values below are placeholders: 10 cathode transistors (digits 0..9),
 * one decimal-point transistor and 6 anode optocouplers (one per tube).
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
    { "K0",    2,   PIN_GROUP_CATHODE,  false },
    { "K1",    3,   PIN_GROUP_CATHODE,  false },
    { "K2",    4,   PIN_GROUP_CATHODE,  false },
    { "K3",    5,   PIN_GROUP_CATHODE,  false },
    { "K4",    6,   PIN_GROUP_CATHODE,  false },
    { "K5",    7,   PIN_GROUP_CATHODE,  false },
    { "K6",    8,   PIN_GROUP_CATHODE,  false },
    { "K7",    9,   PIN_GROUP_CATHODE,  false },
    { "K8",   10,   PIN_GROUP_CATHODE,  false },
    { "K9",   11,   PIN_GROUP_CATHODE,  false },
    { "DP",   12,   PIN_GROUP_CATHODE,  false },

    { "A0",   16,   PIN_GROUP_ANODE,    false },
    { "A1",   17,   PIN_GROUP_ANODE,    false },
    { "A2",   18,   PIN_GROUP_ANODE,    false },
    { "A3",   19,   PIN_GROUP_ANODE,    false },
    { "A4",   20,   PIN_GROUP_ANODE,    false },
    { "A5",   21,   PIN_GROUP_ANODE,    false },
};

#define PIN_COUNT ((int)(sizeof(PIN_TABLE) / sizeof(PIN_TABLE[0])))

#endif /* PINS_CONFIG_H */
