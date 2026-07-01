#ifndef FACTORY_CONFIG_H
#define FACTORY_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "config_parser.h"

// Optional build-time factory config embed (scripts/factory_config_gen.py).
#define FACTORY_CFG_MAGIC       0xFAC70A5A

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t len;
    uint8_t data[MAX_CONFIG_SIZE];
} factory_flash_cfg_t;

#ifdef FACTORY_HAS_EMBED
extern const factory_flash_cfg_t g_factory_embed;
#endif

bool tryProvisionFactoryEmbed(void);

#endif
