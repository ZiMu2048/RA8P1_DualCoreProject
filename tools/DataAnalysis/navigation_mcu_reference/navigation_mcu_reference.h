#ifndef NAVIGATION_MCU_REFERENCE_H
#define NAVIGATION_MCU_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#define NAVIGATION_ROI_WIDTH          (200U)
#define NAVIGATION_ROI_HEIGHT         (24U)
#define NAVIGATION_FEATURE_COUNT      (24U)
#define NAVIGATION_INPUT_FRACTION_BITS (4U)
#define NAVIGATION_SCORE_FRACTION_BITS (18U)

typedef struct st_navigation_result
{
    int32_t features_q[NAVIGATION_FEATURE_COUNT];
    int64_t score_q;
    bool stop_required;
} navigation_result_t;

void navigation_extract_features_q(const uint8_t * dark, int32_t * features_q);
int64_t navigation_score_q(const int32_t * features_q);
void navigation_evaluate(const uint8_t * dark, navigation_result_t * result);

#endif
