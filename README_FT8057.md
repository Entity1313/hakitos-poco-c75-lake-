# FT8057M backend для `hakios.tcm.c` (POCO C75 4G / MT6768)

Файлы:

- `ft8057_tcm.h` — конфигурация платы, структуры отчёта, HAL-контракт.
- `ft8057_tcm.c` — сброс, инициализация, разбор FocalTech-пакета, обработка
  прерывания, suspend/resume.

## Что заменено

| Infinix (OmniVision) | POCO C75 (FT8057M) |
|---|---|
| чтение по SPI-командам OV | чтение регистров FocalTech `0x00..0x3E` |
| IRQ через EINT | GPIO 1 / EINT1, active low |
| reset через GPIO | GPIO 92, active low, 20 мс низкий + 200 мс загрузка |
| матрица Infinix | 720×1640, X 0..719, Y 0..1639 |

## Формат отчёта FocalTech (как в `fts_ts`)

Один burst-read с регистра `0x00`, длина `3 + 6*10 = 63` байта:

```
[0x00] : заголовок
[0x01] : заголовок
[0x02] : TD_STATUS, младший полубайт = число точек (0..10)
[0x03] : точка 0
[0x09] : точка 1
...
```

Точка — 6 байт:

| Байт | Биты | Поле |
|---|---|---|
| 0 | 7:6 | событие: 0 = down, 1 = up, 2 = contact, 3 = reserved |
| 0 | 3:0 | X[11:8] |
| 1 | 7:0 | X[7:0] |
| 2 | 7:4 | tracking id (0..9) |
| 2 | 3:0 | Y[11:8] |
| 3 | 7:0 | Y[7:0] |
| 4 | 7:0 | pressure |
| 5 | 7:4 | area / touch-major |

Важно: **event лежит в X-high, а id — в Y-high** (в некоторых даташитах
рисуют иначе). Именно так парсит вендорский `fts_ts`
(`FTS_TOUCH_EVENT_POS = 3`, `FTS_TOUCH_ID_POS = 5`).

## Прошивка SPI

Вендорская реализация SPI у FocalTech не стандартизована. В шапке
`ft8057_tcm.h` есть три макроса:

```c
#define FT8057_SPI_READ_FLAG      0x00u
#define FT8057_SPI_DUMMY_BYTES    1u
#define FT8057_SPI_WRITE_DUMMY    0u
```

Варианты кадра:

```
A) [reg]        [data ...]           READ_FLAG=0x00, DUMMY=0
B) [reg]        [dummy] [data ...]   READ_FLAG=0x00, DUMMY=1   <-- по умолчанию
C) [reg|0x80]   [dummy] [data ...]   READ_FLAG=0x80, DUMMY=1
```

Вариант C годится только если все адреса < 0x80; у FocalTech chip id — `0xA3`,
поэтому для `fts_ts` обычно A или B. **Снимите логику анализатором со стокового
Android-драйвера** и поправьте макросы, если чип не отвечает. Парсер от этого
не зависит.

## Device Tree

```dts
&spi0 {
    status = "okay";
    #address-cells = <1>;
    #size-cells = <0>;

    touchscreen@0 {
        compatible = "focaltech,fts";
        reg = <0>;
        spi-max-frequency = <9600000>;

        interrupt-parent = <&pio>;
        interrupts = <1 2>;                 /* EINT1, falling edge */
        reset-gpios = <&pio 92 1>;          /* GPIO_ACTIVE_LOW */

        focaltech,display-coords = <0 0 719 1639>;
        focaltech,max-touch-number = <10>;
        focaltech,irq-gpio = <&pio 1 1>;
        focaltech,reset-gpio = <&pio 92 1>;
    };
};
```

`interrupts = <1 2>` — для MTK pinctrl второй ячейкой идёт тип
(`IRQ_TYPE_EDGE_FALLING = 2`). Если в вашем дереве тип задаётся иначе,
используйте `IRQ_TYPE_LEVEL_LOW` (см. ниже).

## Интеграция в XNU/IOKit

1. **SPI из первичного ISR нельзя.** `ft8057_isr()` только выставляет флаг и
   вызывает `hakios_defer()`. В HakiOS это должен быть action на
   `IOCommandGate`/workloop или отдельный `IOThread`. Вся работа с SPI —
   в `ft8057_worker()`.
2. `hakios_spi0_xfer()` должен сериализоваться (gate/mutex), потому что
   `ft8057_poll()` может вызываться и из probe, и из worker.
3. Если SPI-контроллер делает DMA из буфера вызывающего, буферы 65 байт
   должны быть физически непрерывными и cache-coherent
   (`IOBufferMemoryDescriptor` + `prepare()`), либо используйте PIO-режим.
4. Пины: GPIO1 переключить в функцию EINT1, GPIO92 — в GPIO output,
   GPIO1 подтянуть к 3.3 В (IRQ open-drain, active low).
5. Прерывание: по умолчанию falling edge, как в `fts_ts`. Если после чтения
   линия остаётся низкой и прерывания «сыпятся» — включите `LEVEL_LOW`;
   чтение блока с `0x00` снимает запрос у FT8xxx.

## Порядок bring-up

1. Подать питание, проверить, что GPIO92 реально уходит в 0 и возвращается в 1.
2. Прочитать `0xA3` (chip id). Ждём значение, отличное от `0x00`/`0xFF`.
   Если `0xFF` — нет MISO/CS/тактов или неверный кадр SPI.
3. Прочитать `0xA6` (fw) и `0xA8` (vendor) — просто логируются.
4. Коснуться экрана: в логе должны появиться отчёты, координаты в пределах
   719/1639.
5. Если оси зеркальны — `FT8057_CFG_INVERT_X/Y`; если поменяны местами —
   `FT8057_CFG_SWAP_XY`; если прошивка отдаёт 0..16383 — задать
   `FT8057_CFG_RAW_XMAX/RAW_YMAX`.

## Оговорка по железу

FT8057M в большинстве телефонов сидит на **I2C**, а `compatible =
"focaltech,fts"` встречается и у I2C-, и у SPI-вариантов. Если по SPI чип
не отвечает вообще (id читается как `0x00`/`0xFF` при живом reset), проверьте
анализатором, не I2C ли это: вся логика разбора и обработки прерывания
останется той же, заменить нужно только `ft_bus_read()`/`ft_bus_write()`.
