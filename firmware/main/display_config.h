#pragma once

#include <stdint.h>

static const uint16_t MUSIC_DISPLAY_WIDTH = 400;
static const uint16_t MUSIC_DISPLAY_HEIGHT = 300;
static const uint32_t MUSIC_DISPLAY_BYTES =
    ((uint32_t)MUSIC_DISPLAY_WIDTH * (uint32_t)MUSIC_DISPLAY_HEIGHT) / 8U;
static const uint16_t MUSIC_DISPLAY_ROW_BYTES = MUSIC_DISPLAY_WIDTH / 8U;
