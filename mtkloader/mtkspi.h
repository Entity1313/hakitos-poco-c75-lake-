/*
 * The SPI controller the touch screen sits on. On the Infinix Hot 20 that is SPI2 at
 * 0x11012000 (a Synaptics/OmniVision TCM controller, Android's omnivision_tcm_spi); on the POCO
 * C75 it is SPI0 at 0x1100a000 (a FocalTech FT8057M, Android's fts_ts). The address and whether
 * the loader has to mux the pins are the board's (board.h); the register layout is the SoC's and
 * follows Linux's drivers/spi/spi-mt65xx.c for "mediatek,mt6765-spi".
 *
 * Polled, FIFO only, no DMA and no interrupt -- the same shape as the eMMC reader beside it.
 * Device memory with the MMU off: 32-bit accesses, nothing clever.
 */
#ifndef MTKSPI_H
#define MTKSPI_H

#include <stdint.h>
#include "board.h"

#define TOUCH_SPI_BASE_ADDR  ((uint64_t)TOUCH_SPI_BASE)
#define INFRACFG_BASE        0x10001000ULL
#define TOPCKGEN_BASE        0x10000000ULL

/*
 * Where the bit clock comes from (Linux clk-mt6768.c): the "spi_sel" mux is two bits at 24 in
 * CLK_CFG_2, with a power-down bit at 31, and each of these registers has a companion that clears
 * whatever bits you write to it. Source 0 is the 26 MHz crystal, which needs no PLL to be running
 * and is the one thing certain to be there this early.
 */
#define CLK_CFG_2_CLR        0x68
#define CLK_CFG_2_SET        0x64
#define CLK_CFG_2            0x60
#define SPI_SEL_FIELD        (3u << 24)
#define SPI_SEL_PDN          (1u << 31)

#define SPI_CFG0             0x00
#define SPI_CFG1             0x04
#define SPI_TX_SRC           0x08
#define SPI_RX_DST           0x0c
#define SPI_TX_DATA          0x10
#define SPI_RX_DATA          0x14
#define SPI_CMD              0x18
#define SPI_STATUS0          0x1c
#define SPI_STATUS1          0x20
#define SPI_PAD_SEL          0x24
#define SPI_CFG2             0x28

/*
 * The timing registers, in this chip's layout. Linux calls it "enhance_timing" and mt6765 (and so
 * mt6768) has it: the clock's high and low halves go in CFG2, sixteen bits each, while CFG0 holds
 * only the chip-select setup and hold. The older parts put all four in CFG0, eight bits each, and
 * using that layout here leaves the clock with no length at all -- the transfer still reports
 * itself done, and reads come back as nothing but the pull-ups.
 */
#define SPI_CFG0_CS_HOLD_SHIFT    0
#define SPI_CFG0_CS_SETUP_SHIFT   16
#define SPI_CFG2_SCK_HIGH_SHIFT   0
#define SPI_CFG2_SCK_LOW_SHIFT    16

#define SPI_CFG1_CS_IDLE_SHIFT    0
#define SPI_CFG1_PACKET_LOOP_SHIFT 8
#define SPI_CFG1_PACKET_LEN_SHIFT 16

#define SPI_CMD_ACT          (1u << 0)
#define SPI_CMD_RESUME       (1u << 1)
#define SPI_CMD_RST          (1u << 2)
#define SPI_CMD_PAUSE_EN     (1u << 4)
#define SPI_CMD_DEASSERT     (1u << 5)
#define SPI_CMD_SAMPLE_SEL   (1u << 6)
#define SPI_CMD_CS_POL       (1u << 7)
#define SPI_CMD_CPHA         (1u << 8)
#define SPI_CMD_CPOL         (1u << 9)
#define SPI_CMD_TXMSBF       (1u << 12)
#define SPI_CMD_RXMSBF       (1u << 13)
#define SPI_CMD_RX_ENDIAN    (1u << 14)
#define SPI_CMD_TX_ENDIAN    (1u << 15)

#define SPI_STATUS0_DONE     (1u << 0)

#define SPI_FIFO_BYTES       32

/*
 * The pins. An MT6768 GPIO is one of eight in a function register and one of 32 in a bank:
 *
 *   function  GPIO_MODE<n>  0x300 + 0x10 * (pin / 8), four bits a pin
 *   direction 0x000 + 0x10 * (pin / 32), the 32-bit bank, set at +4 and clear at +8
 *   output    0x100 + 0x10 * (pin / 32), the same set/clear pair
 *   input     0x200 + 0x10 * (pin / 32)
 *
 * So GPIO92, the FT8057M's reset, is bit 28 of bank 2 (0x120 / 0x124 / 0x128) and bits 16..19
 * of GPIO_MODE11 (0x3b0). The old code only knew bank 0 and wrote the whole direction register,
 * which is why GPIO92 could not be driven at all.
 */
#define GPIO_BASE            0x10005000ULL
#define GPIO_BANK(pin)       ((uint32_t)(pin) / 32u)
#define GPIO_BIT(pin)        (1u << ((uint32_t)(pin) % 32u))

static inline void pin_set(uint32_t pin, int high)
{
	uint64_t bank = GPIO_BASE + 0x100u + 0x10u * GPIO_BANK(pin);

	wr32(bank + (high ? 0x04u : 0x08u), GPIO_BIT(pin));
}

static inline int pin_get(uint32_t pin)
{
	return (rd32(GPIO_BASE + 0x200u + 0x10u * GPIO_BANK(pin)) >> (pin % 32u)) & 1u;
}

static inline void pin_output(uint32_t pin)
{
	wr32(GPIO_BASE + 0x10u * GPIO_BANK(pin) + 0x04u, GPIO_BIT(pin));
}

static inline void pin_input(uint32_t pin)
{
	wr32(GPIO_BASE + 0x10u * GPIO_BANK(pin) + 0x08u, GPIO_BIT(pin));
}

static inline void pin_mode_gpio(uint32_t pin)
{
	uint64_t reg = GPIO_BASE + 0x300u + 0x10u * ((uint32_t)pin / 8u);
	uint32_t shift = ((uint32_t)pin % 8u) * 4u;

	wr32(reg, rd32(reg) & ~(0xfu << shift));
	__asm__ volatile("dsb sy");
}

/* Long enough for the pad to settle; a register read alone is already most of a microsecond. */
static inline void pin_settle(void)
{
	(void)rd32(GPIO_BASE + 0x200u);
	(void)rd32(GPIO_BASE + 0x200u);
}

/*
 * The touch controller's reset, active low: drive it high first (so the pulse has a clean edge),
 * hold it low for TOUCH_RESET_LOW_MS, release it, and give the chip TOUCH_RESET_BOOT_MS to run
 * its own boot code. On the POCO C75 that is GPIO92, 15 ms low; on the Infinix Hot 20 it is
 * GPIO0, 20 ms low. The pin is put in GPIO function 0 and made an output; the preloader may
 * have left it as an EINT.
 */
__attribute__((unused)) static void touch_reset(void)
{
	pin_mode_gpio(TOUCH_RESET_PIN);
	pin_output(TOUCH_RESET_PIN);
	pin_set(TOUCH_RESET_PIN, 1);
	__asm__ volatile("dsb sy");
	dbg_delay_ms(1);

	pin_set(TOUCH_RESET_PIN, 0);                    /* LOW ... */
	__asm__ volatile("dsb sy");
	dbg_delay_ms(TOUCH_RESET_LOW_MS);

	pin_set(TOUCH_RESET_PIN, 1);                    /* ... HIGH */
	__asm__ volatile("dsb sy");
	dbg_delay_ms(TOUCH_RESET_BOOT_MS);
}

#if TOUCH_SPI_LOADER_PINS
/*
 * The bus pins. On the Infinix Hot 20 SPI2 is GPIO25 (CSB), 26 (MO), 27 (CLK) and 28 (MI), each
 * at function 2 -- from the MT6768 pin table, not the MT6765 one it is so often confused with,
 * where those functions sit on GPIO17-20 instead.
 *
 * The preloader already sets them, which is why the phone's device tree carries no pinctrl state
 * for them -- Linux never has to. We set them anyway: it costs nothing and says what we rely on.
 */
static void spi_pins_to_spi2(void)
{
	for (uint32_t pin = TOUCH_SPI_PIN_CS; pin <= TOUCH_SPI_PIN_MI; pin++) {
		uint64_t reg = GPIO_BASE + 0x300u + 0x10u * (pin / 8u);
		uint32_t shift = (pin % 8u) * 4u;

		wr32(reg, (rd32(reg) & ~(0xfu << shift)) | ((uint32_t)TOUCH_SPI_PIN_FUNC << shift));
	}
	__asm__ volatile("dsb sy");
}

/*
 * The bus itself. Each field counts ticks of the source clock and is stored one less than it
 * counts. Ten ticks a half-period divides the source by twenty, which keeps us well under the
 * 8 MHz the chip's own device tree asks for.
 */
#define SPI_TICKS 2                             /* 26 MHz over four: 6.5, under the 8 the chip allows */

static void spi_setup(void)
{
	wr32(TOPCKGEN_BASE + CLK_CFG_2_CLR, SPI_SEL_FIELD | SPI_SEL_PDN);
	__asm__ volatile("dsb sy");

	wr32(TOUCH_SPI_BASE_ADDR + SPI_CMD, SPI_CMD_RST);       /* whatever LK left behind */
	wr32(TOUCH_SPI_BASE_ADDR + SPI_CMD, 0);
	wr32(TOUCH_SPI_BASE_ADDR + SPI_CFG2, ((SPI_TICKS - 1) << SPI_CFG2_SCK_HIGH_SHIFT) |
	    ((SPI_TICKS - 1) << SPI_CFG2_SCK_LOW_SHIFT));
	wr32(TOUCH_SPI_BASE_ADDR + SPI_CFG0, ((SPI_TICKS - 1) << SPI_CFG0_CS_HOLD_SHIFT) |
	    ((SPI_TICKS - 1) << SPI_CFG0_CS_SETUP_SHIFT));
	wr32(TOUCH_SPI_BASE_ADDR + SPI_PAD_SEL, 0);             /* the device tree says pad group 0 */
}

/*
 * The same four pins driven by hand. The SPI controller has many ways to be set up wrong -- the
 * clock mux, the sampling edge, the FIFO's byte order -- and this has none of them: it is the
 * wire protocol and nothing else, slow but beyond doubt. If the chip answers here and not through
 * the controller, the controller is misconfigured; if it answers neither way, the chip is not
 * listening at all.
 */
static void spi_pins_to_gpio(void)
{
	for (uint32_t pin = TOUCH_SPI_PIN_CS; pin <= TOUCH_SPI_PIN_MI; pin++) {
		uint64_t reg = GPIO_BASE + 0x300u + 0x10u * (pin / 8u);
		uint32_t shift = (pin % 8u) * 4u;

		wr32(reg, rd32(reg) & ~(0xfu << shift));
	}
	/* three of them we drive, the chip drives the fourth */
	pin_output(TOUCH_SPI_PIN_MO);
	pin_output(TOUCH_SPI_PIN_CLK);
	pin_output(TOUCH_SPI_PIN_CS);
	pin_input(TOUCH_SPI_PIN_MI);
	pin_set(TOUCH_SPI_PIN_CS, 1);                   /* chip select idles high */
	pin_set(TOUCH_SPI_PIN_CLK, 1);                  /* and in mode 3 so does the clock */
	__asm__ volatile("dsb sy");
	pin_settle();
}

/*
 * `len` bytes each way, MSB first, in mode 3: the clock idles high, the data changes on the
 * falling edge and is read on the rising one.
 *
 * `byte_gap_us` is a pause left after each byte. The chip's ROM bootloader asks for twenty
 * microseconds there (omnivision,ubl-byte-delay-us in the phone's device tree) and will not
 * follow a stream sent any faster -- which is why the firmware goes out this way and not through
 * the SPI controller, whose bytes come back to back with nothing between them.
 */
static void spi_bitbang_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t byte_gap_us)
{
	int idle = 1;

	pin_set(TOUCH_SPI_PIN_CLK, idle);
	pin_set(TOUCH_SPI_PIN_CS, 0);
	pin_settle();

	for (uint32_t i = 0; i < len; i++) {
		uint8_t out_byte = tx ? tx[i] : 0xff, in_byte = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(TOUCH_SPI_PIN_MO, (out_byte >> bit) & 1);
			pin_set(TOUCH_SPI_PIN_CLK, !idle);      /* the edge that moves the data */
			pin_settle();
			pin_set(TOUCH_SPI_PIN_CLK, idle);       /* the edge that reads it */
			pin_settle();
			in_byte = (uint8_t)((in_byte << 1) | pin_get(TOUCH_SPI_PIN_MI));
		}
		if (rx) {
			rx[i] = in_byte;
		}
		if (byte_gap_us) {
			dbg_delay_us(byte_gap_us);
		}
	}
	pin_set(TOUCH_SPI_PIN_CS, 1);
	pin_settle();
}
#endif /* TOUCH_SPI_LOADER_PINS */

#if TOUCH_SPI_LOADER_PINS
/*
 * One transfer: `len` bytes out of tx (zeros where tx is NULL) and the same number in, into rx.
 * The chip select is asserted for the whole of it, which is what a TCM exchange wants. At most
 * SPI_FIFO_BYTES at a time -- the FIFO is all this uses.
 */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint32_t words = (len + 3) / 4;

	if (len == 0 || len > SPI_FIFO_BYTES) {
		return 0;
	}
	spi_setup();                                    /* the last transfer left its FIFO behind */
	/*
	 * Mode 3 (CPOL = CPHA = 1), MSB first, chip select active low. The endian bits stay clear:
	 * Linux sets them only on a big-endian host, and with them on the bytes of every word come
	 * back in the wrong order.
	 */
	wr32(TOUCH_SPI_BASE_ADDR + SPI_CMD, SPI_CMD_CPHA | SPI_CMD_CPOL | SPI_CMD_TXMSBF | SPI_CMD_RXMSBF);
	wr32(TOUCH_SPI_BASE_ADDR + SPI_CFG1, ((len - 1) << SPI_CFG1_PACKET_LEN_SHIFT) |
	    (0 << SPI_CFG1_PACKET_LOOP_SHIFT) | ((SPI_TICKS - 1) << SPI_CFG1_CS_IDLE_SHIFT));

	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = 0;

		for (uint32_t b = 0; b < 4; b++) {
			uint32_t i = w * 4 + b;
			v |= (uint32_t)(tx && i < len ? tx[i] : 0xff) << (8 * b);
		}
		wr32(TOUCH_SPI_BASE_ADDR + SPI_TX_DATA, v);
	}
	wr32(TOUCH_SPI_BASE_ADDR + SPI_CMD, rd32(TOUCH_SPI_BASE_ADDR + SPI_CMD) | SPI_CMD_ACT);

	for (uint32_t spin = 0; ; spin++) {
		if (rd32(TOUCH_SPI_BASE_ADDR + SPI_STATUS0) & SPI_STATUS0_DONE) {
			break;
		}
		if (spin > 2000000) {
			return 0;
		}
	}
	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = rd32(TOUCH_SPI_BASE_ADDR + SPI_RX_DATA);

		for (uint32_t b = 0; b < 4; b++) {
			uint32_t i = w * 4 + b;

			if (rx && i < len) {
				rx[i] = (uint8_t)(v >> (8 * b));
			}
		}
	}
	return 1;
}
#endif /* TOUCH_SPI_LOADER_PINS */

#endif /* MTKSPI_H */
