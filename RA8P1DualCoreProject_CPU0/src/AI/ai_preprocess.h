/*
 * ai_preprocess.h
 *
 * Helium-assisted camera-frame preprocessing for the 128 x 128 INT8 model.
 */

#ifndef AI_AI_PREPROCESS_H_
#define AI_AI_PREPROCESS_H_

#include <stddef.h>
#include <stdint.h>

#define AI_PREPROCESS_SOURCE_WIDTH          (1024U)
#define AI_PREPROCESS_SOURCE_HEIGHT         (600U)
#define AI_PREPROCESS_SOURCE_STRIDE_BYTES   (2048U)
#define AI_PREPROCESS_CROP_X                (212U)
#define AI_PREPROCESS_CROP_Y                (0U)
#define AI_PREPROCESS_CROP_WIDTH            (600U)
#define AI_PREPROCESS_CROP_HEIGHT           (600U)
#define AI_PREPROCESS_DESTINATION_WIDTH     (128U)
#define AI_PREPROCESS_DESTINATION_HEIGHT    (128U)
#define AI_PREPROCESS_CHANNEL_COUNT         (3U)

#define AI_PREPROCESS_SOURCE_BYTES                                      \
    ((AI_PREPROCESS_SOURCE_HEIGHT - 1U) *                               \
     AI_PREPROCESS_SOURCE_STRIDE_BYTES +                                \
     AI_PREPROCESS_SOURCE_WIDTH * sizeof(uint16_t))

#define AI_PREPROCESS_DESTINATION_BYTES                                 \
    (AI_PREPROCESS_DESTINATION_WIDTH *                                  \
     AI_PREPROCESS_DESTINATION_HEIGHT *                                 \
     AI_PREPROCESS_CHANNEL_COUNT)

#define AI_PREPROCESS_HORIZONTAL_MAP_LENGTH \
    (AI_PREPROCESS_DESTINATION_WIDTH)

typedef enum e_ai_preprocess_status
{
    AI_PREPROCESS_SUCCESS = 0,
    AI_PREPROCESS_INVALID_ARGUMENT,
    AI_PREPROCESS_SOURCE_TOO_SMALL,
    AI_PREPROCESS_OUTPUT_TOO_SMALL,
    AI_PREPROCESS_WORKSPACE_TOO_SMALL,
    AI_PREPROCESS_OVERLAPPING_BUFFERS
} ai_preprocess_status_t;

ai_preprocess_status_t ai_preprocess_rgb565_to_int8(
    uint8_t const * p_source,
    size_t source_size_bytes,
    int8_t * p_destination,
    size_t destination_size_bytes,
    uint16_t * p_horizontal_map,
    size_t horizontal_map_length);

#endif /* AI_AI_PREPROCESS_H_ */
