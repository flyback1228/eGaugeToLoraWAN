#ifndef __MY_CONFIG_H__
#define __MY_CONFIG_H__

#include <stdint.h>
#include "stm32wlxx_hal.h"

// Config structure sizes
#define LORAWAN_EUI_LENGTH 8
#define LORAWAN_KEY_LENGTH 16

// The user configuration structure
typedef struct
{
    uint32_t upload_interval;                   // Interval in seconds
    uint8_t devEui[LORAWAN_EUI_LENGTH];         // Device EUI
    uint8_t joinEui[LORAWAN_EUI_LENGTH];        // Join EUI (AppEUI)
    uint8_t appKey[LORAWAN_KEY_LENGTH];         // Application Key
} my_config_t;

// Public interface
my_config_t* GetCurrentConfig(void);
HAL_StatusTypeDef LoadConfigFromFlash(void);
HAL_StatusTypeDef SaveConfigToFlash(const my_config_t* cfg);

#endif /* __MY_CONFIG_H__ */
