#include "navigation_thread.h"
#include "ipc_thread.h"
#include "IPC/navigation_ipc_protocol.h"
#include "Navigation/navigation_runtime.h"
#include "SEGGER_RTT/bsp_print.h"

/*
 * 防跌落模型可调参数。
 * NAV_MODEL_STOP_THRESHOLD_Q 越小越容易触发STOP，修改后必须重新做离线回放验证。
 */
#define NAV_GRAY_WIDTH                         (200U)
#define NAV_GRAY_HEIGHT                        (112U)
#define NAV_ROI_HEIGHT                         (24U)
#define NAV_GRAY_THRESHOLD                     (128U)
#define NAV_MODEL_FEATURE_COUNT                (24U)
#define NAV_MODEL_INPUT_FRACTION_BITS          (4U)
#define NAV_MODEL_SCORE_FRACTION_BITS          (18U)
#define NAV_MODEL_INTERCEPT_Q                  INT64_C(1624267)
#define NAV_MODEL_STOP_THRESHOLD_Q             INT64_C(-237271)
#define NAV_STOP_CONFIRM_FRAMES                (1U)
#define NAV_ALLOW_CONFIRM_FRAMES               (3U)

/*
 * 自主脱险参数。
 * 当前1350 ms按100% PWM下约180度转向估算，必须通过架空和低速实车试验重新标定。
 * 摄像头在车体中心前方15.75 cm，恢复前必须停车复核，不能把转动中的单帧安全当成整车安全。
 */
#define NAV_ESCAPE_TURN_TOWARD_DARK_SUPPORT    (1U)
#define NAV_ESCAPE_SUPPORT_DELTA_Q              (80)       /* F4格式，80约等于5个百分点。 */
#define NAV_ESCAPE_SAFE_MARGIN_Q                INT64_C(131072) /* 仅用于转动中提前停轮，F18格式约0.5 logit。 */
#define NAV_ESCAPE_YOLO_WAIT_TIME_MS             (5000U)    /* 首次转向前保持STOP，让YOLO完成破损3/6仲裁。 */
#define NAV_ESCAPE_PRETURN_STOP_FRAMES          (1U)
#define NAV_ESCAPE_MIN_TURN_TIME_MS             (250U)
#define NAV_ESCAPE_MAX_TURN_TIME_MS             (1350U)
#define NAV_ESCAPE_MOVING_SAFE_FRAMES           (2U)
#define NAV_ESCAPE_STOP_SETTLE_TIME_MS           (120U)
#define NAV_ESCAPE_STOP_SAFE_FRAMES             (3U)
#define NAV_ESCAPE_VERIFY_TIMEOUT_MS            (600U)
#define NAV_ESCAPE_MAX_DIRECTION_ATTEMPTS       (2U)
#define NAV_ESCAPE_RANDOM_SEED                   (UINT32_C(0x6D2B79F5))

/* 置1后把热点代码和工作区放入ITCM/DTCM，工程链接脚本已提供对应段。 */
#define NAV_MODEL_TCM_ENABLE                   (1U)

/*
 * 决策RTT输出：默认关闭，关闭时相关字符串和格式化调用均在编译期移除。
 * 打开后输出模型原始判断、滤波后的实际IPC动作、定点分数和门限。
 */
#define NAV_DECISION_RTT_ENABLE                (0U)
#define NAV_DECISION_RTT_INTERVAL_FRAMES       (10U)

/*
 * 二值ROI校准输出：
 *   1：输出底部200x24 ROI，字符1表示灰度低于NAV_GRAY_THRESHOLD；
 *   0：编译期移除快照和输出代码，不占用运行时间及快照内存。
 * 每帧只发送少量行，避免单次输出超过RTT上行缓冲区。
 */
#define NAV_BINARY_RTT_DUMP_ENABLE             (0U)
#define NAV_BINARY_RTT_ROWS_PER_FRAME          (2U)

#if (NAV_STOP_CONFIRM_FRAMES == 0U) || (NAV_ALLOW_CONFIRM_FRAMES == 0U)
#error "Navigation confirmation frame counts must be greater than zero"
#endif

#if (NAV_ESCAPE_PRETURN_STOP_FRAMES == 0U) || \
    (NAV_ESCAPE_MOVING_SAFE_FRAMES == 0U) || \
    (NAV_ESCAPE_STOP_SAFE_FRAMES == 0U) || \
    (NAV_ESCAPE_MAX_DIRECTION_ATTEMPTS != 2U)
#error "Navigation escape frame counts are invalid"
#endif

#if (NAV_ESCAPE_MIN_TURN_TIME_MS >= NAV_ESCAPE_MAX_TURN_TIME_MS) || \
    (NAV_ESCAPE_STOP_SETTLE_TIME_MS >= NAV_ESCAPE_VERIFY_TIMEOUT_MS)
#error "Navigation escape turn time range is invalid"
#endif

#if NAV_DECISION_RTT_ENABLE && (NAV_DECISION_RTT_INTERVAL_FRAMES == 0U)
#error "NAV_DECISION_RTT_INTERVAL_FRAMES must be greater than zero"
#endif

#if NAV_BINARY_RTT_DUMP_ENABLE && (NAV_BINARY_RTT_ROWS_PER_FRAME == 0U)
#error "NAV_BINARY_RTT_ROWS_PER_FRAME must be greater than zero"
#endif

#if NAV_MODEL_TCM_ENABLE
#define NAV_MODEL_CODE_TCM BSP_PLACE_IN_SECTION(".itcm_code_from_flash")
#define NAV_MODEL_DATA_TCM BSP_PLACE_IN_SECTION(".dtcm")
#define NAV_MODEL_CONST_TCM BSP_PLACE_IN_SECTION(".dtcm_from_flash")
#else
#define NAV_MODEL_CODE_TCM
#define NAV_MODEL_DATA_TCM
#define NAV_MODEL_CONST_TCM
#endif

typedef enum e_nav_model_feature
{
    NAV_F_CENTER_COLUMN_DARK_MIN = 0,
    NAV_F_GRID_DARK_R3_C1,
    NAV_F_LEFT_EDGE_VALID_PERCENT,
    NAV_F_GRID_DARK_R1_C5,
    NAV_F_DARK_LEFT_EDGE,
    NAV_F_GRID_DARK_R1_C1,
    NAV_F_RIGHT_EDGE_VALID_PERCENT,
    NAV_F_ROW_DARK_MAX,
    NAV_F_DARK_RIGHT_EDGE,
    NAV_F_GRID_DARK_R3_C2,
    NAV_F_DARK_LEFT,
    NAV_F_COLUMN_DARK_MIN,
    NAV_F_WHITE_RIGHT_BORDER_MAX,
    NAV_F_DARK_BOTTOM_HALF,
    NAV_F_LONGEST_WHITE_RUN_MEAN,
    NAV_F_GRID_DARK_R4_C1,
    NAV_F_GRID_DARK_R2_C3,
    NAV_F_GRID_DARK_R1_C4,
    NAV_F_DARK_ALL,
    NAV_F_DARK_RIGHT,
    NAV_F_CENTER_ROW_DARK_MAX,
    NAV_F_WHITE_TOP_BORDER_MEAN,
    NAV_F_WHITE_RIGHT_BORDER_MEAN,
    NAV_F_ROW_TRANSITION_MAX,
    NAV_F_COUNT
} nav_model_feature_t;

typedef struct st_nav_model_workspace
{
    uint16_t column_dark[NAV_GRAY_WIDTH];
    uint8_t first_dark_row[NAV_GRAY_WIDTH];
    uint16_t grid_dark[4][5];
    int32_t features_q[NAV_MODEL_FEATURE_COUNT];
} nav_model_workspace_t;

typedef struct st_nav_decision_filter
{
    uint32_t stop_frames;
    uint32_t allow_frames;
    bool stop_applied;
} nav_decision_filter_t;

typedef enum e_nav_escape_state
{
    NAV_ESCAPE_MONITOR = 0,
    NAV_ESCAPE_WAIT_YOLO,
    NAV_ESCAPE_STOP_BEFORE_TURN,
    NAV_ESCAPE_TURNING,
    NAV_ESCAPE_STOP_VERIFY,
    NAV_ESCAPE_MANUAL_LATCHED,
} nav_escape_state_t;

typedef struct st_nav_escape_controller
{
    nav_escape_state_t state;
    nav_ipc_action_t turn_action;
    uint32_t direction_attempts;
    uint32_t preturn_stop_frames;
    uint32_t moving_safe_frames;
    uint32_t stopped_safe_frames;
    TickType_t yolo_wait_start_tick;
    TickType_t turn_start_tick;
    TickType_t verify_start_tick;
    uint32_t random_state;
} nav_escape_controller_t;

/* MATLAB定点导出的F14系数，顺序必须与nav_model_feature_t完全一致。 */
static int32_t const g_navigation_coefficients_q[NAV_MODEL_FEATURE_COUNT]
    NAV_MODEL_CONST_TCM =
{
     262,  -90,  113,  -87, -104,  -87,  132, -160,
    -110,  -63,  -90,   57,  106, -150,  137,  -98,
     -43,  -75, -124, -111, -115,  101,  133, -482
};

/* 工作区只由Navigation Thread访问，放入DTCM后无需锁和缓存维护。 */
static nav_model_workspace_t g_navigation_model_workspace
    BSP_ALIGN_VARIABLE(32) NAV_MODEL_DATA_TCM;

static const uint8_t * volatile gp_navigation_gray;
static volatile uint32_t g_navigation_frame_sequence;
static volatile bool g_navigation_auto_rearm_pending;
extern TaskHandle_t navigation_thread;

static nav_decision_filter_t g_navigation_decision_filter =
{
    .stop_frames = 0U,
    .allow_frames = 0U,
    .stop_applied = true,
};

static nav_escape_controller_t g_navigation_escape =
{
    .state = NAV_ESCAPE_MONITOR,
    .turn_action = NAV_IPC_ACTION_TURN_LEFT,
    .random_state = NAV_ESCAPE_RANDOM_SEED,
};

#if NAV_BINARY_RTT_DUMP_ENABLE
static char g_navigation_binary_rows[NAV_ROI_HEIGHT][NAV_GRAY_WIDTH + 2U];
static uint32_t g_navigation_binary_frame_sequence;
static uint32_t g_navigation_binary_next_row = NAV_ROI_HEIGHT;
#endif

static int64_t navigation_model_evaluate(const uint8_t * p_gray)
    NAV_MODEL_CODE_TCM;
static int32_t navigation_percent_f4(uint32_t count, uint32_t total)
    NAV_MODEL_CODE_TCM;

void navigation_frame_submit(const uint8_t * p_gray, uint32_t frame_sequence)
{
    if(NULL == p_gray)
    {
        return;
    }

    taskENTER_CRITICAL();
    gp_navigation_gray = p_gray;
    g_navigation_frame_sequence = frame_sequence;
    __DMB();
    taskEXIT_CRITICAL();
    xTaskNotifyGive(navigation_thread);
}

void navigation_auto_rearm_from_isr(void)
{
    g_navigation_auto_rearm_pending = true;
    __DMB();
}

static bool navigation_auto_rearm_take(void)
{
    bool pending;

    taskENTER_CRITICAL();
    pending = g_navigation_auto_rearm_pending;
    g_navigation_auto_rearm_pending = false;
    taskEXIT_CRITICAL();
    return pending;
}

static int32_t navigation_percent_f4(uint32_t count, uint32_t total)
{
    uint64_t const numerator = (uint64_t) count * UINT64_C(1600);
    return (int32_t) ((numerator + (total / 2U)) / total);
}

/*
 * 直接扫描Gray8底部ROI并同时完成二值化和24个特征统计。
 * 不生成中间二值图，输出分数为F18定点格式。
 */
static int64_t navigation_model_evaluate(const uint8_t * p_gray)
{
    nav_model_workspace_t * const p_work = &g_navigation_model_workspace;
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
    uint32_t const first_y = NAV_GRAY_HEIGHT - NAV_ROI_HEIGHT;

    for(uint32_t column = 0U; column < NAV_GRAY_WIDTH; column++)
    {
        p_work->column_dark[column] = 0U;
        p_work->first_dark_row[column] = NAV_ROI_HEIGHT;
    }
    for(uint32_t grid_row = 0U; grid_row < 4U; grid_row++)
    {
        for(uint32_t grid_column = 0U; grid_column < 5U; grid_column++)
        {
            p_work->grid_dark[grid_row][grid_column] = 0U;
        }
    }

    for(uint32_t row = 0U; row < NAV_ROI_HEIGHT; row++)
    {
        uint16_t row_dark = 0U;
        uint16_t center_row_dark = 0U;
        uint16_t first_dark = NAV_GRAY_WIDTH;
        int32_t last_dark = -1;
        uint16_t longest_white = 0U;
        uint16_t current_white = 0U;
        uint16_t transitions = 0U;
        uint32_t const row_offset = (first_y + row) * NAV_GRAY_WIDTH;
        uint8_t previous =
            (p_gray[row_offset] < NAV_GRAY_THRESHOLD) ? 1U : 0U;

        for(uint32_t column = 0U; column < NAV_GRAY_WIDTH; column++)
        {
            uint8_t const value =
                (p_gray[row_offset + column] < NAV_GRAY_THRESHOLD) ? 1U : 0U;
            if((column > 0U) && (value != previous))
            {
                transitions++;
            }
            previous = value;

            if(value != 0U)
            {
                row_dark++;
                dark_all++;
                p_work->column_dark[column]++;
                p_work->grid_dark[row / 6U][column / 40U]++;
                if(first_dark == NAV_GRAY_WIDTH)
                {
                    first_dark = (uint16_t) column;
                }
                last_dark = (int32_t) column;
                if(p_work->first_dark_row[column] == NAV_ROI_HEIGHT)
                {
                    p_work->first_dark_row[column] = (uint8_t) row;
                }
                current_white = 0U;
                if(column < 67U)
                {
                    dark_left++;
                }
                else if(column >= 134U)
                {
                    dark_right++;
                }
                if(column < 20U)
                {
                    dark_left_edge++;
                }
                if(column >= 180U)
                {
                    dark_right_edge++;
                }
                if(row >= 12U)
                {
                    dark_bottom_half++;
                }
                if((column >= 67U) && (column < 134U))
                {
                    center_row_dark++;
                }
            }
            else
            {
                current_white++;
                if(current_white > longest_white)
                {
                    longest_white = current_white;
                }
            }
        }

        uint16_t const right_white = (last_dark < 0) ? NAV_GRAY_WIDTH :
            (uint16_t) ((NAV_GRAY_WIDTH - 1U) - (uint32_t) last_dark);
        if((first_dark > 0U) && (first_dark < NAV_GRAY_WIDTH))
        {
            left_edge_valid++;
        }
        if((right_white > 0U) && (right_white < NAV_GRAY_WIDTH))
        {
            right_edge_valid++;
        }
        if(row_dark > row_dark_max)
        {
            row_dark_max = row_dark;
        }
        if(center_row_dark > center_row_dark_max)
        {
            center_row_dark_max = center_row_dark;
        }
        if(right_white > right_white_max)
        {
            right_white_max = right_white;
        }
        if(transitions > row_transition_max)
        {
            row_transition_max = transitions;
        }
        sum_longest_white += longest_white;
        sum_right_white += right_white;
    }

    uint16_t center_column_dark_min = NAV_ROI_HEIGHT;
    uint16_t column_dark_min = NAV_ROI_HEIGHT;
    for(uint32_t column = 0U; column < NAV_GRAY_WIDTH; column++)
    {
        if(p_work->column_dark[column] < column_dark_min)
        {
            column_dark_min = p_work->column_dark[column];
        }
        if((column >= 67U) && (column < 134U) &&
           (p_work->column_dark[column] < center_column_dark_min))
        {
            center_column_dark_min = p_work->column_dark[column];
        }
        sum_top_white += p_work->first_dark_row[column];
    }

    p_work->features_q[NAV_F_CENTER_COLUMN_DARK_MIN] = navigation_percent_f4(
        center_column_dark_min, NAV_ROI_HEIGHT);
    p_work->features_q[NAV_F_GRID_DARK_R3_C1] = navigation_percent_f4(
        p_work->grid_dark[2][0], 240U);
    p_work->features_q[NAV_F_LEFT_EDGE_VALID_PERCENT] = navigation_percent_f4(
        left_edge_valid, NAV_ROI_HEIGHT);
    p_work->features_q[NAV_F_GRID_DARK_R1_C5] = navigation_percent_f4(
        p_work->grid_dark[0][4], 240U);
    p_work->features_q[NAV_F_DARK_LEFT_EDGE] = navigation_percent_f4(
        dark_left_edge, NAV_ROI_HEIGHT * 20U);
    p_work->features_q[NAV_F_GRID_DARK_R1_C1] = navigation_percent_f4(
        p_work->grid_dark[0][0], 240U);
    p_work->features_q[NAV_F_RIGHT_EDGE_VALID_PERCENT] = navigation_percent_f4(
        right_edge_valid, NAV_ROI_HEIGHT);
    p_work->features_q[NAV_F_ROW_DARK_MAX] = navigation_percent_f4(
        row_dark_max, NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_DARK_RIGHT_EDGE] = navigation_percent_f4(
        dark_right_edge, NAV_ROI_HEIGHT * 20U);
    p_work->features_q[NAV_F_GRID_DARK_R3_C2] = navigation_percent_f4(
        p_work->grid_dark[2][1], 240U);
    p_work->features_q[NAV_F_DARK_LEFT] = navigation_percent_f4(
        dark_left, NAV_ROI_HEIGHT * 67U);
    p_work->features_q[NAV_F_COLUMN_DARK_MIN] = navigation_percent_f4(
        column_dark_min, NAV_ROI_HEIGHT);
    p_work->features_q[NAV_F_WHITE_RIGHT_BORDER_MAX] = navigation_percent_f4(
        right_white_max, NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_DARK_BOTTOM_HALF] = navigation_percent_f4(
        dark_bottom_half, 12U * NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_LONGEST_WHITE_RUN_MEAN] = navigation_percent_f4(
        sum_longest_white, NAV_ROI_HEIGHT * NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_GRID_DARK_R4_C1] = navigation_percent_f4(
        p_work->grid_dark[3][0], 240U);
    p_work->features_q[NAV_F_GRID_DARK_R2_C3] = navigation_percent_f4(
        p_work->grid_dark[1][2], 240U);
    p_work->features_q[NAV_F_GRID_DARK_R1_C4] = navigation_percent_f4(
        p_work->grid_dark[0][3], 240U);
    p_work->features_q[NAV_F_DARK_ALL] = navigation_percent_f4(
        dark_all, NAV_ROI_HEIGHT * NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_DARK_RIGHT] = navigation_percent_f4(
        dark_right, NAV_ROI_HEIGHT * 66U);
    p_work->features_q[NAV_F_CENTER_ROW_DARK_MAX] = navigation_percent_f4(
        center_row_dark_max, 67U);
    p_work->features_q[NAV_F_WHITE_TOP_BORDER_MEAN] = navigation_percent_f4(
        sum_top_white, NAV_ROI_HEIGHT * NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_WHITE_RIGHT_BORDER_MEAN] = navigation_percent_f4(
        sum_right_white, NAV_ROI_HEIGHT * NAV_GRAY_WIDTH);
    p_work->features_q[NAV_F_ROW_TRANSITION_MAX] =
        (int32_t) row_transition_max << NAV_MODEL_INPUT_FRACTION_BITS;

    int64_t score_q = NAV_MODEL_INTERCEPT_Q;
    for(uint32_t index = 0U; index < NAV_MODEL_FEATURE_COUNT; index++)
    {
        score_q += (int64_t) p_work->features_q[index] *
            g_navigation_coefficients_q[index];
    }
    return score_q;
}

/* 危险立即生效，恢复运动需要连续安全帧，避免门限附近反复启停。 */
static bool navigation_decision_filter_update(bool raw_stop_required)
{
    nav_decision_filter_t * const p_filter = &g_navigation_decision_filter;

    if(raw_stop_required)
    {
        p_filter->allow_frames = 0U;
        if(p_filter->stop_frames < NAV_STOP_CONFIRM_FRAMES)
        {
            p_filter->stop_frames++;
        }
        if(p_filter->stop_frames >= NAV_STOP_CONFIRM_FRAMES)
        {
            p_filter->stop_applied = true;
        }
    }
    else
    {
        p_filter->stop_frames = 0U;
        if(p_filter->stop_applied)
        {
            if(p_filter->allow_frames < NAV_ALLOW_CONFIRM_FRAMES)
            {
                p_filter->allow_frames++;
            }
            if(p_filter->allow_frames >= NAV_ALLOW_CONFIRM_FRAMES)
            {
                p_filter->allow_frames = 0U;
                p_filter->stop_applied = false;
            }
        }
        else
        {
            p_filter->allow_frames = 0U;
        }
    }
    return p_filter->stop_applied;
}

static void navigation_decision_filter_reset(bool stop_applied)
{
    g_navigation_decision_filter.stop_frames = 0U;
    g_navigation_decision_filter.allow_frames = 0U;
    g_navigation_decision_filter.stop_applied = stop_applied;
}

static nav_ipc_action_t navigation_escape_reverse(nav_ipc_action_t action)
{
    return (NAV_IPC_ACTION_TURN_LEFT == action) ?
           NAV_IPC_ACTION_TURN_RIGHT : NAV_IPC_ACTION_TURN_LEFT;
}

/* 暗像素代表太阳能板支撑；支撑差不明显时使用图像、帧号和时基混合的伪随机位。 */
static nav_ipc_action_t navigation_escape_direction_choose(uint32_t frame_sequence,
                                                           TickType_t now)
{
    int32_t const left_support_q =
        g_navigation_model_workspace.features_q[NAV_F_DARK_LEFT];
    int32_t const right_support_q =
        g_navigation_model_workspace.features_q[NAV_F_DARK_RIGHT];
    int32_t const support_delta_q = left_support_q - right_support_q;
    bool turn_left;

    if(support_delta_q >= NAV_ESCAPE_SUPPORT_DELTA_Q)
    {
        turn_left = (0U != NAV_ESCAPE_TURN_TOWARD_DARK_SUPPORT);
    }
    else if(support_delta_q <= -NAV_ESCAPE_SUPPORT_DELTA_Q)
    {
        turn_left = (0U == NAV_ESCAPE_TURN_TOWARD_DARK_SUPPORT);
    }
    else
    {
        uint32_t random_value = g_navigation_escape.random_state ^
                                frame_sequence ^ (uint32_t) now ^
                                ((uint32_t) (uint16_t) left_support_q << 16U) ^
                                (uint32_t) (uint16_t) right_support_q;
        random_value ^= random_value << 13U;
        random_value ^= random_value >> 17U;
        random_value ^= random_value << 5U;
        if(0U == random_value)
        {
            random_value = NAV_ESCAPE_RANDOM_SEED;
        }
        g_navigation_escape.random_state = random_value;
        turn_left = (0U != (random_value & 1U));
    }

    return turn_left ? NAV_IPC_ACTION_TURN_LEFT : NAV_IPC_ACTION_TURN_RIGHT;
}

static void navigation_escape_start(uint32_t frame_sequence, TickType_t now)
{
    g_navigation_escape.state = NAV_ESCAPE_WAIT_YOLO;
    g_navigation_escape.turn_action =
        navigation_escape_direction_choose(frame_sequence, now);
    g_navigation_escape.direction_attempts = 1U;
    g_navigation_escape.preturn_stop_frames = 0U;
    g_navigation_escape.moving_safe_frames = 0U;
    g_navigation_escape.stopped_safe_frames = 0U;
    g_navigation_escape.yolo_wait_start_tick = now;
    navigation_decision_filter_reset(true);
    g_printf("[NAV][SAFE] suspected edge; holding STOP %u ms for YOLO damage vote.\r\n",
             (unsigned int) NAV_ESCAPE_YOLO_WAIT_TIME_MS);
}

static void navigation_escape_rearm(void)
{
    g_navigation_escape.state = NAV_ESCAPE_MONITOR;
    g_navigation_escape.direction_attempts = 0U;
    g_navigation_escape.preturn_stop_frames = 0U;
    g_navigation_escape.moving_safe_frames = 0U;
    g_navigation_escape.stopped_safe_frames = 0U;
    g_navigation_escape.yolo_wait_start_tick = 0U;
    navigation_decision_filter_reset(true);
}

static nav_ipc_action_t navigation_escape_action_update(int64_t score_q,
                                                        bool raw_stop_required,
                                                        uint32_t frame_sequence,
                                                        TickType_t now)
{
    bool const confidently_safe =
        score_q < (NAV_MODEL_STOP_THRESHOLD_Q - NAV_ESCAPE_SAFE_MARGIN_Q);

    switch(g_navigation_escape.state)
    {
        case NAV_ESCAPE_MONITOR:
        {
            bool const stop_confirmed =
                navigation_decision_filter_update(raw_stop_required);
            if(raw_stop_required)
            {
                if(stop_confirmed)
                {
                    navigation_escape_start(frame_sequence, now);
                }
                return NAV_IPC_ACTION_STOP;
            }
            return stop_confirmed ?
                   NAV_IPC_ACTION_STOP : NAV_IPC_ACTION_FORWARD;
        }

        case NAV_ESCAPE_WAIT_YOLO:
            if((now - g_navigation_escape.yolo_wait_start_tick) <
               pdMS_TO_TICKS(NAV_ESCAPE_YOLO_WAIT_TIME_MS))
            {
                return NAV_IPC_ACTION_STOP;
            }
            g_navigation_escape.state = NAV_ESCAPE_STOP_BEFORE_TURN;
            g_navigation_escape.preturn_stop_frames = 0U;
            g_printf("[NAV][SAFE] YOLO damage-vote wait complete; escape turn enabled.\r\n");
            return NAV_IPC_ACTION_STOP;

        case NAV_ESCAPE_STOP_BEFORE_TURN:
            if(g_navigation_escape.preturn_stop_frames <
               NAV_ESCAPE_PRETURN_STOP_FRAMES)
            {
                g_navigation_escape.preturn_stop_frames++;
                return NAV_IPC_ACTION_STOP;
            }
            g_navigation_escape.state = NAV_ESCAPE_TURNING;
            g_navigation_escape.turn_start_tick = now;
            g_navigation_escape.moving_safe_frames = 0U;
            return g_navigation_escape.turn_action;

        case NAV_ESCAPE_TURNING:
        {
            TickType_t const turn_elapsed = now - g_navigation_escape.turn_start_tick;
            if((turn_elapsed >= pdMS_TO_TICKS(NAV_ESCAPE_MIN_TURN_TIME_MS)) &&
               confidently_safe)
            {
                if(g_navigation_escape.moving_safe_frames <
                   NAV_ESCAPE_MOVING_SAFE_FRAMES)
                {
                    g_navigation_escape.moving_safe_frames++;
                }
            }
            else
            {
                g_navigation_escape.moving_safe_frames = 0U;
            }

            if((g_navigation_escape.moving_safe_frames >=
                NAV_ESCAPE_MOVING_SAFE_FRAMES) ||
               (turn_elapsed >= pdMS_TO_TICKS(NAV_ESCAPE_MAX_TURN_TIME_MS)))
            {
                g_navigation_escape.state = NAV_ESCAPE_STOP_VERIFY;
                g_navigation_escape.verify_start_tick = now;
                g_navigation_escape.stopped_safe_frames = 0U;
                return NAV_IPC_ACTION_STOP;
            }
            return g_navigation_escape.turn_action;
        }

        case NAV_ESCAPE_STOP_VERIFY:
            if((now - g_navigation_escape.verify_start_tick) >=
               pdMS_TO_TICKS(NAV_ESCAPE_STOP_SETTLE_TIME_MS))
            {
                /* 停稳后按正常SAFE门限连续复核，避免额外裕量导致已安全仍反向。 */
                if(!raw_stop_required)
                {
                    if(g_navigation_escape.stopped_safe_frames <
                       NAV_ESCAPE_STOP_SAFE_FRAMES)
                    {
                        g_navigation_escape.stopped_safe_frames++;
                    }
                }
                else
                {
                    g_navigation_escape.stopped_safe_frames = 0U;
                }
            }

            if(g_navigation_escape.stopped_safe_frames >=
               NAV_ESCAPE_STOP_SAFE_FRAMES)
            {
                g_navigation_escape.state = NAV_ESCAPE_MONITOR;
                navigation_decision_filter_reset(false);
                return NAV_IPC_ACTION_FORWARD;
            }

            if((now - g_navigation_escape.verify_start_tick) >=
               pdMS_TO_TICKS(NAV_ESCAPE_VERIFY_TIMEOUT_MS))
            {
                if(g_navigation_escape.direction_attempts <
                   NAV_ESCAPE_MAX_DIRECTION_ATTEMPTS)
                {
                    g_navigation_escape.direction_attempts++;
                    g_navigation_escape.turn_action =
                        navigation_escape_reverse(g_navigation_escape.turn_action);
                    g_navigation_escape.preturn_stop_frames = 1U;
                    g_navigation_escape.moving_safe_frames = 0U;
                    g_navigation_escape.stopped_safe_frames = 0U;
                    g_navigation_escape.state = NAV_ESCAPE_STOP_BEFORE_TURN;
                    return NAV_IPC_ACTION_STOP;
                }
                g_navigation_escape.state = NAV_ESCAPE_MANUAL_LATCHED;
                return NAV_IPC_ACTION_MANUAL_LATCH;
            }
            return NAV_IPC_ACTION_STOP;

        case NAV_ESCAPE_MANUAL_LATCHED:
        default:
            return NAV_IPC_ACTION_MANUAL_LATCH;
    }
}

#if NAV_DECISION_RTT_ENABLE
static const char * navigation_raw_decision_name(bool raw_stop_required)
{
    return raw_stop_required ? "STOP" : "ALLOW";
}

static const char * navigation_action_name(nav_ipc_action_t action)
{
    switch(action)
    {
        case NAV_IPC_ACTION_FORWARD:      return "FORWARD";
        case NAV_IPC_ACTION_TURN_LEFT:    return "TURN_LEFT";
        case NAV_IPC_ACTION_TURN_RIGHT:   return "TURN_RIGHT";
        case NAV_IPC_ACTION_MANUAL_LATCH: return "MANUAL_LATCH";
        case NAV_IPC_ACTION_AUTO_REARMED_STOP: return "AUTO_REARMED_STOP";
        case NAV_IPC_ACTION_STOP:
        default:                          return "STOP";
    }
}
#endif

#if NAV_BINARY_RTT_DUMP_ENABLE
static void navigation_binary_rtt_dump_service(const uint8_t * p_gray,
                                               uint32_t frame_sequence)
{
    uint32_t const first_y = NAV_GRAY_HEIGHT - NAV_ROI_HEIGHT;
    unsigned const row_size = (unsigned) sizeof(g_navigation_binary_rows[0]);

    if(g_navigation_binary_next_row >= NAV_ROI_HEIGHT)
    {
        for(uint32_t roi_y = 0U; roi_y < NAV_ROI_HEIGHT; roi_y++)
        {
            uint32_t const row_offset = (first_y + roi_y) * NAV_GRAY_WIDTH;
            for(uint32_t x = 0U; x < NAV_GRAY_WIDTH; x++)
            {
                g_navigation_binary_rows[roi_y][x] =
                    (p_gray[row_offset + x] < NAV_GRAY_THRESHOLD) ? '1' : '0';
            }
            g_navigation_binary_rows[roi_y][NAV_GRAY_WIDTH] = '\r';
            g_navigation_binary_rows[roi_y][NAV_GRAY_WIDTH + 1U] = '\n';
        }

        g_navigation_binary_frame_sequence = frame_sequence;
        g_navigation_binary_next_row = 0U;
        g_printf("[NAV][BIN] BEGIN frame=%u roi=%ux%u y=%u..%u threshold=%u one=dark.\r\n",
                 (unsigned int) frame_sequence,
                 (unsigned int) NAV_GRAY_WIDTH,
                 (unsigned int) NAV_ROI_HEIGHT,
                 (unsigned int) first_y,
                 (unsigned int) (NAV_GRAY_HEIGHT - 1U),
                 (unsigned int) NAV_GRAY_THRESHOLD);
    }

    uint32_t rows_sent = 0U;
    while((g_navigation_binary_next_row < NAV_ROI_HEIGHT) &&
          (rows_sent < NAV_BINARY_RTT_ROWS_PER_FRAME))
    {
        unsigned const written = SEGGER_RTT_Write(
            BUFFER_INDEX,
            g_navigation_binary_rows[g_navigation_binary_next_row],
            row_size);
        if(written != row_size)
        {
            break;
        }

        g_navigation_binary_next_row++;
        rows_sent++;
    }

    if(g_navigation_binary_next_row >= NAV_ROI_HEIGHT)
    {
        g_printf("[NAV][BIN] END frame=%u.\r\n",
                 (unsigned int) g_navigation_binary_frame_sequence);
    }
}
#endif

void navigation_thread_entry(void * pvParameters)
{
    uint32_t last_frame_sequence = 0U;
    bool auto_rearm_ack_pending = false;

#if NAV_DECISION_RTT_ENABLE
    uint32_t log_frame_counter = 0U;
    nav_ipc_action_t last_logged_action = NAV_IPC_ACTION_COUNT;
    bool last_logged_raw_stop = false;
    bool ipc_error_logged = false;
#endif

    FSP_PARAMETER_NOT_USED(pvParameters);

#if NAV_DECISION_RTT_ENABLE
    g_printf("[NAV] 24-feature fixed-point fall-prevention model ready.\r\n");
#endif

    for(;;)
    {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const uint8_t * p_gray;
        uint32_t frame_sequence;
        taskENTER_CRITICAL();
        __DMB();
        p_gray = gp_navigation_gray;
        frame_sequence = g_navigation_frame_sequence;
        taskEXIT_CRITICAL();

        if((NULL == p_gray) || (frame_sequence == last_frame_sequence))
        {
            continue;
        }
        last_frame_sequence = frame_sequence;

        bool const auto_rearmed = navigation_auto_rearm_take();
        if(auto_rearmed)
        {
            navigation_escape_rearm();
            auto_rearm_ack_pending = true;
        }

        int64_t const score_q = navigation_model_evaluate(p_gray);
        bool const raw_stop_required = score_q >= NAV_MODEL_STOP_THRESHOLD_Q;
        nav_ipc_action_t const action = auto_rearm_ack_pending ?
            NAV_IPC_ACTION_AUTO_REARMED_STOP :
            navigation_escape_action_update(score_q,
                                            raw_stop_required,
                                            frame_sequence,
                                            xTaskGetTickCount());
        uint32_t const message =
            nav_ipc_message_encode(action, (uint8_t) frame_sequence);
        fsp_err_t const send_result =
            g_ipc0.p_api->messageSend(g_ipc0.p_ctrl, message);
        if(auto_rearm_ack_pending && (FSP_SUCCESS == send_result))
        {
            /* 回执成功前只重复STOP，不推进脱险状态机。 */
            auto_rearm_ack_pending = false;
        }

#if NAV_DECISION_RTT_ENABLE
        if(FSP_SUCCESS != send_result)
        {
            if(!ipc_error_logged)
            {
                g_printf("[NAV][ERR] IPC send=%u; next frame will retry.\r\n",
                         (unsigned int) send_result);
                ipc_error_logged = true;
            }
        }
        else
        {
            ipc_error_logged = false;
        }

        log_frame_counter++;
        bool const decision_changed =
            (action != last_logged_action) ||
            (raw_stop_required != last_logged_raw_stop);
        if(decision_changed ||
           (log_frame_counter >= NAV_DECISION_RTT_INTERVAL_FRAMES))
        {
            g_printf("[NAV] frame=%u raw=%s action=%s score_q=%d threshold_q=%d.\r\n",
                     (unsigned int) frame_sequence,
                     navigation_raw_decision_name(raw_stop_required),
                     navigation_action_name(action),
                     (int32_t) score_q,
                     (int32_t) NAV_MODEL_STOP_THRESHOLD_Q);
            last_logged_action = action;
            last_logged_raw_stop = raw_stop_required;
            log_frame_counter = 0U;
        }
#else
        FSP_PARAMETER_NOT_USED(send_result);
#endif

#if NAV_BINARY_RTT_DUMP_ENABLE
        navigation_binary_rtt_dump_service(p_gray, frame_sequence);
#endif
    }
}
