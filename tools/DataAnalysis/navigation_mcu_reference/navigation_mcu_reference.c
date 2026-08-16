#include "navigation_mcu_reference.h"

#include <stddef.h>

enum
{
    F_CENTER_COLUMN_DARK_MIN = 0,
    F_GRID_DARK_R3_C1,
    F_LEFT_EDGE_VALID_PERCENT,
    F_GRID_DARK_R1_C5,
    F_DARK_LEFT_EDGE,
    F_GRID_DARK_R1_C1,
    F_RIGHT_EDGE_VALID_PERCENT,
    F_ROW_DARK_MAX,
    F_DARK_RIGHT_EDGE,
    F_GRID_DARK_R3_C2,
    F_DARK_LEFT,
    F_COLUMN_DARK_MIN,
    F_WHITE_RIGHT_BORDER_MAX,
    F_DARK_BOTTOM_HALF,
    F_LONGEST_WHITE_RUN_MEAN,
    F_GRID_DARK_R4_C1,
    F_GRID_DARK_R2_C3,
    F_GRID_DARK_R1_C4,
    F_DARK_ALL,
    F_DARK_RIGHT,
    F_CENTER_ROW_DARK_MAX,
    F_WHITE_TOP_BORDER_MEAN,
    F_WHITE_RIGHT_BORDER_MEAN,
    F_ROW_TRANSITION_MAX
};

static int32_t const g_coefficients_q[NAVIGATION_FEATURE_COUNT] =
{
     262,  -90,  113,  -87, -104,  -87,  132, -160,
    -110,  -63,  -90,   57,  106, -150,  137,  -98,
     -43,  -75, -124, -111, -115,  101,  133, -482
};

static int64_t const g_intercept_q = INT64_C(1624267);
static int64_t const g_stop_threshold_q = INT64_C(-237271);

static int32_t navigation_percent_f4(uint32_t count, uint32_t total)
{
    uint64_t numerator = (uint64_t) count * UINT64_C(1600);
    return (int32_t) ((numerator + (total / 2U)) / total);
}

void navigation_extract_features_q(const uint8_t * dark, int32_t * features_q)
{
    uint16_t column_dark[NAVIGATION_ROI_WIDTH] = {0};
    uint8_t first_dark_row[NAVIGATION_ROI_WIDTH];
    uint16_t grid_dark[4][5] = {{0}};
    uint32_t dark_all = 0U;
    uint32_t dark_left = 0U;
    uint32_t dark_right = 0U;
    uint32_t dark_left_edge = 0U;
    uint32_t dark_right_edge = 0U;
    uint32_t dark_bottom_half = 0U;
    uint32_t sum_longest_white = 0U;
    uint32_t sum_right_white = 0U;
    uint32_t sum_top_white = 0U;
    uint16_t row_dark_max = 0U;
    uint16_t center_row_dark_max = 0U;
    uint16_t right_white_max = 0U;
    uint16_t row_transition_max = 0U;
    uint16_t left_edge_valid = 0U;
    uint16_t right_edge_valid = 0U;

    for (uint32_t column = 0U; column < NAVIGATION_ROI_WIDTH; column++)
    {
        first_dark_row[column] = NAVIGATION_ROI_HEIGHT;
    }

    for (uint32_t row = 0U; row < NAVIGATION_ROI_HEIGHT; row++)
    {
        uint16_t row_dark = 0U;
        uint16_t center_row_dark = 0U;
        uint16_t first_dark = NAVIGATION_ROI_WIDTH;
        int32_t last_dark = -1;
        uint16_t longest_white = 0U;
        uint16_t current_white = 0U;
        uint16_t transitions = 0U;
        uint8_t previous = dark[row * NAVIGATION_ROI_WIDTH];

        for (uint32_t column = 0U; column < NAVIGATION_ROI_WIDTH; column++)
        {
            uint8_t value = dark[row * NAVIGATION_ROI_WIDTH + column] != 0U;
            if (column > 0U && value != previous)
            {
                transitions++;
            }
            previous = value;

            if (value != 0U)
            {
                row_dark++;
                dark_all++;
                column_dark[column]++;
                grid_dark[row / 6U][column / 40U]++;
                if (first_dark == NAVIGATION_ROI_WIDTH)
                {
                    first_dark = (uint16_t) column;
                }
                last_dark = (int32_t) column;
                if (first_dark_row[column] == NAVIGATION_ROI_HEIGHT)
                {
                    first_dark_row[column] = (uint8_t) row;
                }
                current_white = 0U;
                if (column < 67U)
                {
                    dark_left++;
                }
                else if (column >= 134U)
                {
                    dark_right++;
                }
                if (column < 20U)
                {
                    dark_left_edge++;
                }
                if (column >= 180U)
                {
                    dark_right_edge++;
                }
                if (row >= 12U)
                {
                    dark_bottom_half++;
                }
                if (column >= 67U && column < 134U)
                {
                    center_row_dark++;
                }
            }
            else
            {
                current_white++;
                if (current_white > longest_white)
                {
                    longest_white = current_white;
                }
            }
        }

        uint16_t right_white = (last_dark < 0) ? NAVIGATION_ROI_WIDTH :
            (uint16_t) ((NAVIGATION_ROI_WIDTH - 1U) - (uint32_t) last_dark);
        if (first_dark > 0U && first_dark < NAVIGATION_ROI_WIDTH)
        {
            left_edge_valid++;
        }
        if (right_white > 0U && right_white < NAVIGATION_ROI_WIDTH)
        {
            right_edge_valid++;
        }
        if (row_dark > row_dark_max)
        {
            row_dark_max = row_dark;
        }
        if (center_row_dark > center_row_dark_max)
        {
            center_row_dark_max = center_row_dark;
        }
        if (right_white > right_white_max)
        {
            right_white_max = right_white;
        }
        if (transitions > row_transition_max)
        {
            row_transition_max = transitions;
        }
        sum_longest_white += longest_white;
        sum_right_white += right_white;
    }

    uint16_t center_column_dark_min = NAVIGATION_ROI_HEIGHT;
    uint16_t column_dark_min = NAVIGATION_ROI_HEIGHT;
    for (uint32_t column = 0U; column < NAVIGATION_ROI_WIDTH; column++)
    {
        if (column_dark[column] < column_dark_min)
        {
            column_dark_min = column_dark[column];
        }
        if (column >= 67U && column < 134U &&
                column_dark[column] < center_column_dark_min)
        {
            center_column_dark_min = column_dark[column];
        }
        sum_top_white += first_dark_row[column];
    }

    features_q[F_CENTER_COLUMN_DARK_MIN] = navigation_percent_f4(
        center_column_dark_min, NAVIGATION_ROI_HEIGHT);
    features_q[F_GRID_DARK_R3_C1] = navigation_percent_f4(grid_dark[2][0], 240U);
    features_q[F_LEFT_EDGE_VALID_PERCENT] = navigation_percent_f4(
        left_edge_valid, NAVIGATION_ROI_HEIGHT);
    features_q[F_GRID_DARK_R1_C5] = navigation_percent_f4(grid_dark[0][4], 240U);
    features_q[F_DARK_LEFT_EDGE] = navigation_percent_f4(
        dark_left_edge, NAVIGATION_ROI_HEIGHT * 20U);
    features_q[F_GRID_DARK_R1_C1] = navigation_percent_f4(grid_dark[0][0], 240U);
    features_q[F_RIGHT_EDGE_VALID_PERCENT] = navigation_percent_f4(
        right_edge_valid, NAVIGATION_ROI_HEIGHT);
    features_q[F_ROW_DARK_MAX] = navigation_percent_f4(
        row_dark_max, NAVIGATION_ROI_WIDTH);
    features_q[F_DARK_RIGHT_EDGE] = navigation_percent_f4(
        dark_right_edge, NAVIGATION_ROI_HEIGHT * 20U);
    features_q[F_GRID_DARK_R3_C2] = navigation_percent_f4(grid_dark[2][1], 240U);
    features_q[F_DARK_LEFT] = navigation_percent_f4(
        dark_left, NAVIGATION_ROI_HEIGHT * 67U);
    features_q[F_COLUMN_DARK_MIN] = navigation_percent_f4(
        column_dark_min, NAVIGATION_ROI_HEIGHT);
    features_q[F_WHITE_RIGHT_BORDER_MAX] = navigation_percent_f4(
        right_white_max, NAVIGATION_ROI_WIDTH);
    features_q[F_DARK_BOTTOM_HALF] = navigation_percent_f4(
        dark_bottom_half, 12U * NAVIGATION_ROI_WIDTH);
    features_q[F_LONGEST_WHITE_RUN_MEAN] = navigation_percent_f4(
        sum_longest_white, NAVIGATION_ROI_HEIGHT * NAVIGATION_ROI_WIDTH);
    features_q[F_GRID_DARK_R4_C1] = navigation_percent_f4(grid_dark[3][0], 240U);
    features_q[F_GRID_DARK_R2_C3] = navigation_percent_f4(grid_dark[1][2], 240U);
    features_q[F_GRID_DARK_R1_C4] = navigation_percent_f4(grid_dark[0][3], 240U);
    features_q[F_DARK_ALL] = navigation_percent_f4(
        dark_all, NAVIGATION_ROI_HEIGHT * NAVIGATION_ROI_WIDTH);
    features_q[F_DARK_RIGHT] = navigation_percent_f4(
        dark_right, NAVIGATION_ROI_HEIGHT * 66U);
    features_q[F_CENTER_ROW_DARK_MAX] = navigation_percent_f4(
        center_row_dark_max, 67U);
    features_q[F_WHITE_TOP_BORDER_MEAN] = navigation_percent_f4(
        sum_top_white, NAVIGATION_ROI_HEIGHT * NAVIGATION_ROI_WIDTH);
    features_q[F_WHITE_RIGHT_BORDER_MEAN] = navigation_percent_f4(
        sum_right_white, NAVIGATION_ROI_HEIGHT * NAVIGATION_ROI_WIDTH);
    features_q[F_ROW_TRANSITION_MAX] = (int32_t) row_transition_max <<
        NAVIGATION_INPUT_FRACTION_BITS;
}

int64_t navigation_score_q(const int32_t * features_q)
{
    int64_t accumulator = g_intercept_q;
    for (uint32_t index = 0U; index < NAVIGATION_FEATURE_COUNT; index++)
    {
        accumulator += (int64_t) features_q[index] * g_coefficients_q[index];
    }
    return accumulator;
}

void navigation_evaluate(const uint8_t * dark, navigation_result_t * result)
{
    navigation_extract_features_q(dark, result->features_q);
    result->score_q = navigation_score_q(result->features_q);
    result->stop_required = result->score_q >= g_stop_threshold_q;
}
