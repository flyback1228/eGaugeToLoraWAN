#include "my_config.h"
#include <string.h>

// Flash address and validation constant
#define CONFIG_FLASH_ADDR ((uint32_t)0x0803F800)
#define CONFIG_MAGIC      0xDEADBEEF
//#define FLASH_PAGE_SIZE 2048U

typedef struct
{
    my_config_t config;
    uint32_t magic;
} config_flash_block_t;

static config_flash_block_t current_config_block;

my_config_t* GetCurrentConfig(void)
{
    return &current_config_block.config;
}

HAL_StatusTypeDef LoadConfigFromFlash(void)
{
    const config_flash_block_t* flash_ptr = (const config_flash_block_t*)CONFIG_FLASH_ADDR;

    if (flash_ptr->magic == CONFIG_MAGIC)
    {
        memcpy(&current_config_block, flash_ptr, sizeof(config_flash_block_t));
        return HAL_OK;
    }
    else
    {
        memset(&current_config_block, 0, sizeof(config_flash_block_t));
        return HAL_ERROR;
    }
}

HAL_StatusTypeDef SaveConfigToFlash(const my_config_t* cfg)
{
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Page = (CONFIG_FLASH_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE,
        .NbPages = 1
    };
    uint32_t error;

    if (HAL_FLASHEx_Erase(&erase, &error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return HAL_ERROR;
    }

    config_flash_block_t block;
    block.config = *cfg;
    block.magic = CONFIG_MAGIC;

    const uint64_t* src64 = (const uint64_t*)&block;
    for (uint32_t i = 0; i < sizeof(block) / 8; i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, CONFIG_FLASH_ADDR + i * 8, src64[i]) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return HAL_ERROR;
        }
    }

    HAL_FLASH_Lock();

    memcpy(&current_config_block, &block, sizeof(block));
    return HAL_OK;
}
