/*
 * TC4x TMADC kernels (TMADC0..7) and the analog sources they convert.
 *
 * sysbus MMIO region 0: the registers of all kernels.  The device serves
 * the results (AW0_RES0..15) and MODSTAT and passes the other registers
 * to the SFR store.
 * gpio-out "sr"[TC4X_TMADC_COUNT * TC4X_TMADC_SR_COUNT]: the service
 * requests SRi of kernel k at index k * TC4X_TMADC_SR_COUNT + i.
 *
 * Each channel converts a voltage: the output of an analog source
 * connected with tc4x_tmadc_connect(), else its property
 * "volts[K][CH]" (0 to TC4X_TMADC_VREF, beyond that the input
 * saturates).  While a kernel runs, each service request line has a round
 * of conversions every "period-ns[K][SR]" ns of guest time (0: none, for
 * boards that trigger conversions themselves) and raises SRi when its
 * SRi_CFG.EVEN enables any result.
 *
 * Locking: results are read on vCPU threads without the BQL; everything
 * else runs under it.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_TMADC_H
#define HW_TRICORE_TC4X_TMADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/*
 * An object with analog outputs: volts() gives the voltage of output
 * @line.  It runs on a vCPU thread without the BQL, at every conversion.
 */
#define TYPE_TC4X_ANALOG_SOURCE "tc4x-analog-source"
typedef struct Tc4xAnalogSourceClass Tc4xAnalogSourceClass;
DECLARE_CLASS_CHECKERS(Tc4xAnalogSourceClass, TC4X_ANALOG_SOURCE,
                       TYPE_TC4X_ANALOG_SOURCE)
#define TC4X_ANALOG_SOURCE(obj) \
    INTERFACE_CHECK(Tc4xAnalogSource, (obj), TYPE_TC4X_ANALOG_SOURCE)

typedef struct Tc4xAnalogSource Tc4xAnalogSource;

struct Tc4xAnalogSourceClass {
    InterfaceClass parent_class;

    double (*volts)(Tc4xAnalogSource *src, unsigned line);
};

#define TYPE_TC4X_TMADC "tc4x-tmadc"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xTmadc, TC4X_TMADC)

#define TC4X_TMADC_COUNT        8u
#define TC4X_TMADC_CHANNELS     16u
#define TC4X_TMADC_SR_COUNT     6u
#define TC4X_TMADC_VREF         3.3     /* V */

/* Base and size of the register window of all kernels */
uint32_t tc4x_tmadc_base(void);
uint32_t tc4x_tmadc_window(void);

/*
 * Channel @ch of kernel @k converts output @line of @src (NULL: its
 * "volts" property again).  Machine init only.
 */
bool tc4x_tmadc_connect(Tc4xTmadc *adc, unsigned k, unsigned ch,
                        Tc4xAnalogSource *src, unsigned line, Error **errp);

/* The voltage channel @ch of kernel @k converts (any thread) */
double tc4x_tmadc_volts(Tc4xTmadc *adc, unsigned k, unsigned ch);

/* Set the "volts" property of channel @ch of kernel @k (any thread) */
void tc4x_tmadc_set_volts(Tc4xTmadc *adc, unsigned k, unsigned ch,
                          double volts);

/* Kernel @k running and its service request line @sr enabled (any thread) */
bool tc4x_tmadc_sr_enabled(Tc4xTmadc *adc, unsigned k, unsigned sr);

#endif
