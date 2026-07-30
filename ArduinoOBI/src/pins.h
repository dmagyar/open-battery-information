#ifndef OBI_PINS_H
#define OBI_PINS_H

#ifdef ESP_BUILD
#define ONEWIRE_PIN ESP_OW_PIN
#define ENABLE_PIN ESP_EN_PIN
#else
#define ONEWIRE_PIN 6
#define ENABLE_PIN 8
#endif

#endif // OBI_PINS_H
