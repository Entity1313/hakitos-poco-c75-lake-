/*
 * ft8057_tcm.h - FocalTech FT8057M touch backend for hakios.tcm.c
 *
 * Target board: POCO C75 4G (MediaTek MT6768 / Helio G81 Ultra)
 *   SPI0 @ 0x1100A000, chip select 0 ("spi0.0"), 9.6 MHz
 *   IRQ  : GPIO 1  (EINT1, active low)
 *   RESET: GPIO 92 (active low)
 *   Panel: 720 x 1640 (X 0..719, Y 0..1639)
 *
 * This file is a drop-in replacement for the OmniVision/SPI glue that
 * hakios.tcm.c used on the Infinix port.  The protocol layer below is
 * FocalTech's "fts_ts" report format; only the bus framing is SPI.
 */

#ifndef HAKIOS_FT8057_TCM_H
#define HAKIOS_FT8057_TCM_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* Board configuration                                                       */
/* ------------------------------------------------------------------------- */

#define FT8057_SPI_BUS_NAME       "spi0.0"
#define FT8057_SPI_BASE           0x1100A000u
#define FT8057_SPI_MAX_HZ         9600000u

#define FT8057_GPIO_IRQ           1u    /* EINT1 */
#define FT8057_GPIO_RESET         92u

#define FT8057_PANEL_XMAX         719u
#define FT8057_PANEL_YMAX         1639u

#define FT8057_MAX_POINTS         10u
/* report may additionally carry synthetic "up" events for lost contacts */
#define FT8057_MAX_REPORT_POINTS  (FT8057_MAX_POINTS * 2u)

/* ------------------------------------------------------------------------- */
/* Orientation / calibration (defaults match a stock 720x1640 portrait panel) */
/* ------------------------------------------------------------------------- */

#define FT8057_CFG_SWAP_XY        0
#define FT8057_CFG_INVERT_X       0
#define FT8057_CFG_INVERT_Y       0

/*
 * Set these only if the firmware reports a different raw range than the
 * panel resolution (for example 0..16383).  0 means "no rescale".
 */
#define FT8057_CFG_RAW_XMAX       0u
#define FT8057_CFG_RAW_YMAX       0u

/*
 * Chip id at register 0xA3.  FT8xxx firmwares are configured per panel and
 * do not all report the same value, so enforcement is off by default: the
 * driver logs what it reads and continues.
 */
#define FT8057_CFG_CHIP_ID        0x54u
#define FT8057_CFG_ENFORCE_ID     0

/* ------------------------------------------------------------------------- */
/* SPI framing                                                               */
/* ------------------------------------------------------------------------- */
/*
 * FocalTech SPI framing is vendor specific.  Three layouts are common:
 *
 *   A) [reg]            [data ...]              flag 0x00, dummy 0
 *   B) [reg]            [dummy] [data ...]      flag 0x00, dummy 1  (default)
 *   C) [reg | 0x80]     [dummy] [data ...]      flag 0x80, dummy 1
 *
 * Layout C only works if the register map stays below 0x80; FocalTech uses
 * addresses such as 0xA3 (chip id), so A/B are the usual choice for fts_ts.
 * Sniff the stock Android driver with a logic analyzer once and adjust these
 * three macros if the chip does not answer.  The report parser does not
 * depend on them.
 */
#define FT8057_SPI_READ_FLAG      0x00u
#define FT8057_SPI_DUMMY_BYTES    1u
#define FT8057_SPI_WRITE_DUMMY    0u

/* ------------------------------------------------------------------------- */
/* Protocol types                                                            */
/* ------------------------------------------------------------------------- */

#define FT8057_EV_DOWN            0u
#define FT8057_EV_UP              1u
#define FT8057_EV_CONTACT         2u

struct ft8057_point {
    uint8_t  id;        /* tracking id, 0..9                       */
    uint8_t  event;     /* FT8057_EV_*                              */
    uint16_t x;         /* mapped panel coordinate, 0..719          */
    uint16_t y;         /* mapped panel coordinate, 0..1639         */
    uint8_t  pressure;  /* raw pressure byte from the report        */
    uint8_t  area;      /* touch-major / area, 1..15                */
};

struct ft8057_report {
    uint64_t timestamp_ns;
    uint32_t count;
    struct ft8057_point points[FT8057_MAX_REPORT_POINTS];
};

typedef void (*ft8057_report_fn)(const struct ft8057_report *report);

/* ------------------------------------------------------------------------- */
/* Hardware abstraction - implemented by the HakiOS/IOKit port               */
/* ------------------------------------------------------------------------- */
/*
 * hakios_spi0_xfer(): full duplex transfer on SPI0/CS0, clock <= 9.6 MHz.
 *                     CS is asserted for the whole call.  Must be callable
 *                     from a workloop/thread context (may block).
 * hakios_gpio_*():    MTK global GPIO numbers (GPIO1, GPIO92...).
 * hakios_defer():     schedule fn() in a sleepable context (workloop /
 *                     IOCommandGate action).  Never call SPI from the
 *                     primary interrupt handler.
 */
int      hakios_spi0_xfer(const void *tx, void *rx, size_t len);
void     hakios_gpio_output(unsigned gpio);
void     hakios_gpio_input(unsigned gpio);
void     hakios_gpio_write(unsigned gpio, int level);
int      hakios_gpio_read(unsigned gpio);
void     hakios_delay_ms(unsigned ms);
void     hakios_defer(void (*fn)(void));
uint64_t hakios_uptime_ns(void);
void     hakios_log(const char *fmt, ...);

#define HAKIOS_IRQ_FALLING  0x1u
#define HAKIOS_IRQ_LEVEL_LOW 0x2u

int  hakios_gpio_irq_request(unsigned gpio, unsigned flags, void (*isr)(void));
void hakios_gpio_irq_enable(unsigned gpio, int enable);

/* ------------------------------------------------------------------------- */
/* Driver entry points                                                       */
/* ------------------------------------------------------------------------- */

/*
 * ft8057_probe() - hard reset, wait for firmware, disable gestures, drain
 * stale report and hook the EINT1 interrupt.
 * Returns 0 on success, negative on failure.  Sleepable context only.
 */
int  ft8057_probe(ft8057_report_fn sink);

/* optional explicit bus test, useful during bring-up */
int  ft8057_read_ids(uint8_t *chip_id, uint8_t *fw_ver, uint8_t *vendor_id);

/* read + parse one report; also usable for polling/debug */
int  ft8057_poll(struct ft8057_report *out);

/* interrupt halves */
void ft8057_isr(void);      /* primary handler: only defers work          */
void ft8057_worker(void);   /* deferred context: performs SPI and reports */

void ft8057_suspend(void);
int  ft8057_resume(void);
void ft8057_shutdown(void);

#endif /* HAKIOS_FT8057_TCM_H */
