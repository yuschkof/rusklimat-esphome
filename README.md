# rusklimat-esphome

Внешний компонент ESPHome для управления кондиционером Rusklimat по UART (ESP32 вместо штатного Wi-Fi-модуля).

## Подключение

```yaml
external_components:
  - source: github://yuschkof/rusklimat-esphome@main
    components: [rusklimat]

uart:
  id: ac_uart
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 9600

climate:
  - platform: rusklimat
    id: main_ac
    name: "Кондиционер"
    uart_id: ac_uart

switch:
  - platform: rusklimat
    rusklimat_id: main_ac
    display:
      name: "Дисплей"
    eco:
      name: "Эко"
    ifeel:
      name: "iFeel"
```

Полный пример — [rusklimat-ac.yaml](rusklimat-ac.yaml). Все переключатели необязательные.

Поддерживается: питание, режимы (auto/cool/dry/heat/fan), уставка 16–32 °C, температура в комнате,
вентилятор (auto/low/medium/high), жалюзи (вертикаль/горизонталь/обе), пресеты «Турбо» (boost) и «Сон» (sleep), дисплей.
Эко и iFeel — экспериментальные: биты ещё не подтверждены по логам.

## Протокол

9600 8N1. Пакет: `AA <len> <type> ... <crc>`, `len` — число байт после него без CRC,
CRC — сумма всех предыдущих байт по модулю 256.

Запрос статуса (каждые `update_interval`, по умолчанию 2 с): `AA 02 01 AD`.

Статус (type `0x01`, от кондиционера) и команда (type `0x02`, от ESP) — по 22 байта:

| Байт | Значение |
|---|---|
| 3 | питание: `00` выкл, `01` вкл |
| 5 | вертикальные жалюзи: `00` качание, `06` стоп |
| 6 | `0x02` горизонтальные жалюзи, `0x10` дисплей, `0x08` базовый флаг |
| 7 | режим: `00` auto, `01` cool, `02` dry, `03` heat, `04` fan |
| 8 | `0x08` турбо, `0x01` сон, `0x04` iFeel (?), базовый `0x10` |
| 9 | вентилятор: `00` auto, `01`–`03` low–high |
| 10 | уставка, °C |
| 11 | температура в комнате, °C (в команде `00`) |
| 12 | эко (?) — `0x02` |
| 21 | CRC |

## Отладка

`logger: level: DEBUG` — в логе видны отправляемые команды, `VERBOSE` — ещё и каждый пакет статуса.
