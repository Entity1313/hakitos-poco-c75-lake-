/*
 * ft8057_tcm.c - FocalTech FT8057M touch backend for hakios.tcm.c
 *
 * Bus    : MTK SPI0 (0x1100A000), CS0, 9.6 MHz
 * IRQ    : GPIO1 / EINT1, active low
 * RESET  : GPIO92, active low
 * Panel  : 720 x 1640
 *
 * FocalTech report format (same as the fts_ts family):
 *
 *   reg 0x00..0x02 : header, [0x02] = TD_STATUS (touch count, low nibble)
 *   reg 0x03 + 6*i : point i
 *
 *   byte 0 : E[7:6] | X[11:8]
 *   byte 1 : X[7:0]
 *   byte 2 : ID[7:4] | Y[11:8]
 *   byte 3 : Y[7:0]
 *   byte 4 : pressure
 *   byte 5 : area[7:4] | reserved[3:0]
 *
 * Note: event and touch-id are two separate fields: the event lives in the
 * X-high byte, the tracking id lives in the Y-high byte.  This matches the
 * vendor fts_ts parser (FTS_TOUCH_EVENT_POS = 3, FTS_TOUCH_ID_POS = 5).
 */

#include "ft8057_tcm.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* FocalTech registers                                                       */
/* ------------------------------------------------------------------------- */

#define FT_REG_TOUCH_DATA     0x00u
#define FT_REG_TD_STATUS      0x02u
#define FT_REG_CHIP_ID        0xA3u
#define FT_REG_POWER_MODE     0xA5u
#define FT_REG_FW_VER         0xA6u
#define FT_REG_VENDOR_ID      0xA8u
#define FT_REG_GESTURE_EN     0xD0u

#define FT_PM_ACTIVE          0x00u
#define FT_PM_SLEEP           0x03u

/* report geometry */
#define FT_POINT_LEN          6u
#define FT_REPORT_LEN         (3u + FT_POINT_LEN * FT8057_MAX_POINTS) /* 63 */
#define FT_READ_HDR_LEN       (1u + FT8057_SPI_DUMMY_BYTES)

/* timing */
#define FT_RESET_LOW_MS       20u
#define FT_RESET_BOOT_MS      200u
#define FT_PROBE_STEP_MS      20u
#define FT_PROBE_TIMEOUT_MS   300u

/* ------------------------------------------------------------------------- */
/* Driver state                                                              */
/* ------------------------------------------------------------------------- */

static ft8057_report_fn s_sink;
static uint8_t  s_active_mask;                       /* bit per tracking id */
static uint16_t s_last_x[FT8057_MAX_POINTS];
static uint16_t s_last_y[FT8057_MAX_POINTS];
static volatile unsigned s_pending;

/* ------------------------------------------------------------------------- */
/* SPI transport                                                             */
/* ------------------------------------------------------------------------- */

static int ft_bus_read(uint8_t reg, void *dst, size_t len)
{
    uint8_t tx[FT_READ_HDR_LEN + FT_REPORT_LEN];
    uint8_t rx[FT_READ_HDR_LEN + FT_REPORT_LEN];

    if (len > FT_REPORT_LEN)
        return -1;

    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));

    tx[0] = (uint8_t)(reg | FT8057_SPI_READ_FLAG);

    if (hakios_spi0_xfer(tx, rx, FT_READ_HDR_LEN + len) != 0)
        return -1;

    memcpy(dst, rx + FT_READ_HDR_LEN, len);
    return 0;
}

static int ft_bus_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[1u + FT8057_SPI_WRITE_DUMMY + 1u];
    uint8_t rx[1u + FT8057_SPI_WRITE_DUMMY + 1u];

    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));

    tx[0] = reg;
    tx[1u + FT8057_SPI_WRITE_DUMMY] = value;

    return (hakios_spi0_xfer(tx, rx, sizeof(tx)) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Reset / probe                                                             */
/* ------------------------------------------------------------------------- */

static void ft_hw_reset(void)
{
    hakios_gpio_output(FT8057_GPIO_RESET);
    hakios_gpio_write(FT8057_GPIO_RESET, 0);
    hakios_delay_ms(FT_RESET_LOW_MS);
    hakios_gpio_write(FT8057_GPIO_RESET, 1);
    hakios_delay_ms(FT_RESET_BOOT_MS);
}

/* Poll the chip-id register until firmware is up (mirrors fts_wait_tp_to_valid). */
static int ft_wait_ready(uint8_t *chip_id)
{
    unsigned waited = 0;

    while (waited < FT_PROBE_TIMEOUT_MS) {
        uint8_t id = 0;

        if (ft_bus_read(FT_REG_CHIP_ID, &id, 1) == 0 &&
            id != 0x00 && id != 0xFF) {
            if (chip_id)
                *chip_id = id;
            return 0;
        }
        hakios_delay_ms(FT_PROBE_STEP_MS);
        waited += FT_PROBE_STEP_MS;
    }
    return -1;
}

int ft8057_read_ids(uint8_t *chip_id, uint8_t *fw_ver, uint8_t *vendor_id)
{
    uint8_t id = 0, fw = 0, vendor = 0;

    if (ft_bus_read(FT_REG_CHIP_ID, &id, 1) != 0)
        return -1;
    if (ft_bus_read(FT_REG_FW_VER, &fw, 1) != 0)
        return -1;
    if (ft_bus_read(FT_REG_VENDOR_ID, &vendor, 1) != 0)
        return -1;

    if (chip_id)
        *chip_id = id;
    if (fw_ver)
        *fw_ver = fw;
    if (vendor_id)
        *vendor_id = vendor;
    return 0;
}

int ft8057_probe(ft8057_report_fn sink)
{
    uint8_t chip_id = 0, fw = 0, vendor = 0;
    struct ft8057_report drain;
    int rc;

    s_sink = sink;
    s_active_mask = 0;
    s_pending = 0;
    memset(s_last_x, 0, sizeof(s_last_x));
    memset(s_last_y, 0, sizeof(s_last_y));

    /* EINT1 as input; pinmux must select EINT/GPIO function for GPIO1. */
    hakios_gpio_input(FT8057_GPIO_IRQ);

    /* 1. Hard reset through GPIO92. */
    ft_hw_reset();

    /* 2. Wait for the firmware to answer on SPI. */
    if (ft_wait_ready(&chip_id) != 0) {
        hakios_log("ft8057: no response on %s (reset GPIO%u)\n",
                   FT8057_SPI_BUS_NAME, (unsigned)FT8057_GPIO_RESET);
        return -2;
    }

    if (ft8057_read_ids(&chip_id, &fw, &vendor) == 0)
        hakios_log("ft8057: chip 0x%02x fw 0x%02x vendor 0x%02x\n",
                   chip_id, fw, vendor);

#if FT8057_CFG_ENFORCE_ID
    if (chip_id != FT8057_CFG_CHIP_ID) {
        hakios_log("ft8057: unexpected chip id 0x%02x (want 0x%02x)\n",
                   chip_id, (unsigned)FT8057_CFG_CHIP_ID);
        return -3;
    }
#endif

    /* 3. Normal scan mode, no gesture wake-up. */
    (void)ft_bus_write(FT_REG_POWER_MODE, FT_PM_ACTIVE);
    (void)ft_bus_write(FT_REG_GESTURE_EN, 0x00);
    hakios_delay_ms(5);

    /* 4. Drop a stale report so the interrupt line is deasserted. */
    (void)ft8057_poll(&drain);

    /* 5. Hook EINT1.  Level-low also works; falling edge matches fts_ts. */
    rc = hakios_gpio_irq_request(FT8057_GPIO_IRQ, HAKIOS_IRQ_FALLING,
                                 ft8057_isr);
    if (rc != 0) {
        hakios_log("ft8057: cannot hook EINT%u (rc=%d)\n",
                   (unsigned)FT8057_GPIO_IRQ, rc);
        return -4;
    }

    hakios_gpio_irq_enable(FT8057_GPIO_IRQ, 1);
    hakios_log("ft8057: ready, %ux%u, %s @ %u Hz\n",
               (unsigned)FT8057_PANEL_XMAX + 1u,
               (unsigned)FT8057_PANEL_YMAX + 1u,
               FT8057_SPI_BUS_NAME, (unsigned)FT8057_SPI_MAX_HZ);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Coordinate mapping                                                        */
/* ------------------------------------------------------------------------- */

static void ft_map_point(struct ft8057_point *pt)
{
    uint32_t x = pt->x;
    uint32_t y = pt->y;

#if FT8057_CFG_SWAP_XY
    {
        uint32_t t = x;
        x = y;
        y = t;
    }
#endif

#if (FT8057_CFG_RAW_XMAX > 0)
    x = (x * FT8057_PANEL_XMAX + FT8057_CFG_RAW_XMAX / 2u) /
        FT8057_CFG_RAW_XMAX;
#endif
#if (FT8057_CFG_RAW_YMAX > 0)
    y = (y * FT8057_PANEL_YMAX + FT8057_CFG_RAW_YMAX / 2u) /
        FT8057_CFG_RAW_YMAX;
#endif

    if (x > FT8057_PANEL_XMAX)
        x = FT8057_PANEL_XMAX;
    if (y > FT8057_PANEL_YMAX)
        y = FT8057_PANEL_YMAX;

#if FT8057_CFG_INVERT_X
    x = FT8057_PANEL_XMAX - x;
#endif
#if FT8057_CFG_INVERT_Y
    y = FT8057_PANEL_YMAX - y;
#endif

    pt->x = (uint16_t)x;
    pt->y = (uint16_t)y;
}

/* ------------------------------------------------------------------------- */
/* Report parsing                                                            */
/* ------------------------------------------------------------------------- */

static void ft_append_up(struct ft8057_report *out, uint8_t id)
{
    struct ft8057_point *pt;

    if (out->count >= FT8057_MAX_REPORT_POINTS || id >= FT8057_MAX_POINTS)
        return;

    pt = &out->points[out->count++];
    memset(pt, 0, sizeof(*pt));
    pt->id = id;
    pt->event = FT8057_EV_UP;
    pt->x = s_last_x[id];
    pt->y = s_last_y[id];
    pt->area = 1;
}

int ft8057_poll(struct ft8057_report *out)
{
    uint8_t buf[FT_REPORT_LEN];
    uint8_t seen = 0;
    uint8_t down = 0;
    uint32_t n, i;

    if (!out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->timestamp_ns = hakios_uptime_ns();

    if (ft_bus_read(FT_REG_TOUCH_DATA, buf, sizeof(buf)) != 0)
        return -1;

    n = buf[FT_REG_TD_STATUS] & 0x0Fu;
    if (n > FT8057_MAX_POINTS)
        n = FT8057_MAX_POINTS;

    for (i = 0; i < n; i++) {
        const uint8_t *raw = &buf[3u + i * FT_POINT_LEN];
        struct ft8057_point *pt = &out->points[out->count];
        uint8_t id = (uint8_t)(raw[2] >> 4);
        uint8_t ev = (uint8_t)(raw[0] >> 6);
        uint16_t rx, ry;

        if (id >= FT8057_MAX_POINTS)
            continue;

        rx = (uint16_t)(((uint16_t)(raw[0] & 0x0Fu) << 8) | raw[1]);
        ry = (uint16_t)(((uint16_t)(raw[2] & 0x0Fu) << 8) | raw[3]);

        memset(pt, 0, sizeof(*pt));
        pt->id = id;
        pt->event = ev;
        pt->pressure = raw[4];
        pt->area = (uint8_t)(raw[5] >> 4);
        if (pt->area == 0)
            pt->area = 1;

        if (ev == FT8057_EV_UP && rx == 0 && ry == 0 &&
            (s_active_mask & (1u << id))) {
            /* Release packets sometimes carry no coordinates - reuse last. */
            pt->x = s_last_x[id];
            pt->y = s_last_y[id];
        } else {
            pt->x = rx;
            pt->y = ry;
            ft_map_point(pt);
        }

        seen |= (uint8_t)(1u << id);

        if (ev == FT8057_EV_DOWN || ev == FT8057_EV_CONTACT) {
            s_last_x[id] = pt->x;
            s_last_y[id] = pt->y;
            down |= (uint8_t)(1u << id);
        } else {
            down &= (uint8_t)~(1u << id);
        }

        out->count++;
    }

    /*
     * The controller occasionally drops a contact without an explicit UP.
     * Synthesize releases for ids that were down but are absent from this
     * report.  Explicit UP entries are already in "seen", so no duplicates.
     */
    for (i = 0; i < FT8057_MAX_POINTS; i++) {
        uint8_t bit = (uint8_t)(1u << i);

        if ((s_active_mask & bit) && !(seen & bit))
            ft_append_up(out, (uint8_t)i);
    }

    s_active_mask = down;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Interrupt path                                                            */
/* ------------------------------------------------------------------------- */

void ft8057_isr(void)
{
    /*
     * XNU primary interrupt context: no sleeping, no SPI.  The MTK SPI
     * driver needs to map/DMA buffers and may block, so hand the work to
     * the driver workloop (hakios_defer -> IOCommandGate / IOThread).
     */
    s_pending = 1;
    hakios_defer(ft8057_worker);
}

void ft8057_worker(void)
{
    struct ft8057_report report;

    if (!s_pending)
        return;
    s_pending = 0;

    if (ft8057_poll(&report) != 0)
        return;

    if (report.count > 0 && s_sink)
        s_sink(&report);
}

/* ------------------------------------------------------------------------- */
/* Power management                                                          */
/* ------------------------------------------------------------------------- */

void ft8057_suspend(void)
{
    hakios_gpio_irq_enable(FT8057_GPIO_IRQ, 0);
    (void)ft_bus_write(FT_REG_GESTURE_EN, 0x00);
    (void)ft_bus_write(FT_REG_POWER_MODE, FT_PM_SLEEP);
    s_pending = 0;
    s_active_mask = 0;
}

int ft8057_resume(void)
{
    uint8_t chip_id = 0;

    hakios_gpio_irq_enable(FT8057_GPIO_IRQ, 0);

    ft_hw_reset();

    if (ft_wait_ready(&chip_id) != 0) {
        hakios_log("ft8057: resume failed, chip id 0x%02x\n", chip_id);
        return -1;
    }

    (void)ft_bus_write(FT_REG_POWER_MODE, FT_PM_ACTIVE);
    (void)ft_bus_write(FT_REG_GESTURE_EN, 0x00);
    hakios_delay_ms(5);

    /* flush one stale report before re-enabling interrupts */
    {
        struct ft8057_report drain;
        (void)ft8057_poll(&drain);
    }

    s_pending = 0;
    hakios_gpio_irq_enable(FT8057_GPIO_IRQ, 1);
    return 0;
}

void ft8057_shutdown(void)
{
    struct ft8057_report release;

    hakios_gpio_irq_enable(FT8057_GPIO_IRQ, 0);
    (void)ft_bus_write(FT_REG_GESTURE_EN, 0x00);
    (void)ft_bus_write(FT_REG_POWER_MODE, FT_PM_SLEEP);

    /* tell the input stack that every finger is gone */
    memset(&release, 0, sizeof(release));
    release.timestamp_ns = hakios_uptime_ns();
    {
        uint32_t i;
        for (i = 0; i < FT8057_MAX_POINTS; i++) {
            if (s_active_mask & (1u << i))
                ft_append_up(&release, (uint8_t)i);
        }
    }
    if (release.count > 0 && s_sink)
        s_sink(&release);

    s_active_mask = 0;

    hakios_gpio_output(FT8057_GPIO_RESET);
    hakios_gpio_write(FT8057_GPIO_RESET, 0);
    s_sink = NULL;
}
