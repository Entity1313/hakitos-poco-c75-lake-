/*
 * board.h - the one part of mtkloader that is the phone's and not the SoC's.
 *
 * MT6768 is shared by many boards, but the touch controller, its pins and the command line a run
 * wants are not. Build with -DBOARD_LAKE=1 (the default of this repository, a POCO C75 4G /
 * Redmi 14C) or -DBOARD_X6826B=1 (the Infinix Hot 20 this loader was first written on).
 *
 * The addresses below were taken from the phone's own Android device tree and from the runs
 * recorded in docs/; nothing here is guessed silently: an unknown pin is marked as such and the
 * piece that would use it stays off.
 */
#ifndef HAKIOS_MTKLOADER_BOARD_H
#define HAKIOS_MTKLOADER_BOARD_H

#if !defined(BOARD_LAKE) && !defined(BOARD_X6826B)
#define BOARD_LAKE 1
#endif

#if defined(BOARD_LAKE) && defined(BOARD_X6826B)
#error "BOARD_LAKE and BOARD_X6826B are both set: pick one board"
#endif

/*
 * MT6768's DRAM begins here. It is not XNU's base: the loader is entered at 0x40080000 and XNU is
 * given the free window above the loader, LK's framebuffer, ramoops and the TEE (XNU_PHYS_BASE in
 * mtkloader.c). Keeping the physical base named lets the two be checked against each other.
 */
#define DRAM_BASE               0x40000000ULL

#if defined(BOARD_LAKE)

#define BOARD_NAME              "POCO C75 4G / Redmi 14C (lake)"
#define BOARD_TOUCH_NAME        "FocalTech FT8057M"

/* Four Cortex-A55s; XNU_EXTRA_ARGS must say the same "cpus=" (see mtkloader.c). */
#define BOARD_XNU_CORES         4
#define BOARD_XNU_EXTRA_ARGS    "cpus=4 touchscreen=1"

/*
 * The FT8057M runs its firmware from its own flash. There is no TCM ROM-bootloader handover and
 * no hdl_firmware.img: the loader only releases reset and lets the chip boot by itself.
 */
#define TOUCH_NEEDS_FIRMWARE    0
#define TOUCH_IRQ_PIN           1       /* EINT1, active low */
#define TOUCH_RESET_PIN         92      /* active low */
#define TOUCH_RESET_LOW_MS      15      /* LOW ... 15 ms ... HIGH, before XNU */
#define TOUCH_RESET_BOOT_MS     200     /* time the controller needs after reset */

/* SPI0, chip select 0. The bus itself is brought up by the kernel driver (ft8057_tcm.c). */
#define TOUCH_SPI_BASE          0x1100A000ULL
#define TOUCH_SPI_LOADER_PINS   0       /* the loader does not mux SPI0 on this board */

#else /* BOARD_X6826B */

#define BOARD_NAME              "Infinix Hot 20 (X6826B)"
#define BOARD_TOUCH_NAME        "OmniVision TD4160"

#define BOARD_XNU_CORES         4
#define BOARD_XNU_EXTRA_ARGS    "cpus=1 touchscreen=2"

/* The TD4160 keeps its firmware in RAM and is fed by the loader over SPI2. */
#define TOUCH_NEEDS_FIRMWARE    1
#define TOUCH_IRQ_PIN           1
#define TOUCH_RESET_PIN         0
#define TOUCH_RESET_LOW_MS      20
#define TOUCH_RESET_BOOT_MS     200

#define TOUCH_SPI_BASE          0x11012000ULL
#define TOUCH_SPI_LOADER_PINS   1
#define TOUCH_SPI_PIN_FUNC      2
#define TOUCH_SPI_PIN_CS        25
#define TOUCH_SPI_PIN_MO        26
#define TOUCH_SPI_PIN_CLK       27
#define TOUCH_SPI_PIN_MI        28

#endif /* BOARD_* */

#endif /* HAKIOS_MTKLOADER_BOARD_H */
