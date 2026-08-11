/*
 * yolo_postprocess.c
 */

#include "AI/yolo_postprocess.h"

#include <stdbool.h>
#include <stddef.h>

const char * const g_yolo_class_names[YOLO_CLASS_COUNT] =
{
    "PhysicalDamage",
};

static float yolo_clampf(float value, float lower, float upper)
{
    if (value < lower)
    {
        return lower;
    }

    if (value > upper)
    {
        return upper;
    }

    return value;
}

static float yolo_iou(yolo_detection_t const * p_a,
                      yolo_detection_t const * p_b)
{
    float const x1 = (p_a->x1 > p_b->x1) ? p_a->x1 : p_b->x1;
    float const y1 = (p_a->y1 > p_b->y1) ? p_a->y1 : p_b->y1;
    float const x2 = (p_a->x2 < p_b->x2) ? p_a->x2 : p_b->x2;
    float const y2 = (p_a->y2 < p_b->y2) ? p_a->y2 : p_b->y2;
    float const intersection_width = x2 - x1;
    float const intersection_height = y2 - y1;

    if ((intersection_width <= 0.0f) ||
        (intersection_height <= 0.0f))
    {
        return 0.0f;
    }

    float const intersection = intersection_width * intersection_height;
    float const area_a = (p_a->x2 - p_a->x1) * (p_a->y2 - p_a->y1);
    float const area_b = (p_b->x2 - p_b->x1) * (p_b->y2 - p_b->y1);
    float const union_area = area_a + area_b - intersection;

    if (union_area <= 0.0f)
    {
        return 0.0f;
    }

    return intersection / union_area;
}

static void yolo_add_candidate(yolo_detection_t * p_detections,
                               int capacity,
                               int * p_count,
                               yolo_detection_t const * p_candidate)
{
    if (*p_count < capacity)
    {
        p_detections[*p_count] = *p_candidate;
        (*p_count)++;
        return;
    }

    int lowest_index = 0;

    for (int index = 1; index < capacity; index++)
    {
        if (p_detections[index].score < p_detections[lowest_index].score)
        {
            lowest_index = index;
        }
    }

    if (p_candidate->score > p_detections[lowest_index].score)
    {
        p_detections[lowest_index] = *p_candidate;
    }
}

int yolo_decode_int8_output(const int8_t * p_output,
                            yolo_detection_t * p_detections,
                            int capacity,
                            float confidence_threshold)
{
    /* Renesas_YOLO_INT8_PhyD_0260716.tflite output quantization. */
    float const output_scale = 0.0084315790f;
    int const output_zero_point = -58;
    int count = 0;

    if ((NULL == p_output) ||
        (NULL == p_detections) ||
        (capacity <= 0))
    {
        return 0;
    }

    confidence_threshold = yolo_clampf(confidence_threshold, 0.0f, 1.0f);

    for (int box_index = 0;
         box_index < YOLO_OUTPUT_BOX_COUNT;
         box_index++)
    {
        int8_t const * p_row =
            &p_output[box_index * YOLO_OUTPUT_ATTRS];

        float const x1 =
            (float) ((int) p_row[0] - output_zero_point) * output_scale;
        float const y1 =
            (float) ((int) p_row[1] - output_zero_point) * output_scale;
        float const x2 =
            (float) ((int) p_row[2] - output_zero_point) * output_scale;
        float const y2 =
            (float) ((int) p_row[3] - output_zero_point) * output_scale;
        float const score =
            (float) ((int) p_row[4] - output_zero_point) * output_scale;

        if (score < confidence_threshold)
        {
            continue;
        }

        yolo_detection_t candidate =
        {
            .x1 = x1 * (float) YOLO_INPUT_SIZE,
            .y1 = y1 * (float) YOLO_INPUT_SIZE,
            .x2 = x2 * (float) YOLO_INPUT_SIZE,
            .y2 = y2 * (float) YOLO_INPUT_SIZE,
            .score = score,
            .class_id = 0U,
        };

        candidate.x1 = yolo_clampf(candidate.x1,
                                   0.0f,
                                   (float) YOLO_INPUT_SIZE);
        candidate.y1 = yolo_clampf(candidate.y1,
                                   0.0f,
                                   (float) YOLO_INPUT_SIZE);
        candidate.x2 = yolo_clampf(candidate.x2,
                                   0.0f,
                                   (float) YOLO_INPUT_SIZE);
        candidate.y2 = yolo_clampf(candidate.y2,
                                   0.0f,
                                   (float) YOLO_INPUT_SIZE);

        if ((candidate.x2 <= candidate.x1) ||
            (candidate.y2 <= candidate.y1))
        {
            continue;
        }

        yolo_add_candidate(p_detections,
                           capacity,
                           &count,
                           &candidate);
    }

    return count;
}

int yolo_nms(yolo_detection_t * p_detections,
             int count,
             float iou_threshold)
{
    if ((NULL == p_detections) || (count <= 0))
    {
        return 0;
    }

    iou_threshold = yolo_clampf(iou_threshold, 0.0f, 1.0f);

    /* Selection sort is sufficient because count is capped at four. */
    for (int index = 0; index < (count - 1); index++)
    {
        int best_index = index;

        for (int candidate_index = index + 1;
             candidate_index < count;
             candidate_index++)
        {
            if (p_detections[candidate_index].score >
                p_detections[best_index].score)
            {
                best_index = candidate_index;
            }
        }

        if (best_index != index)
        {
            yolo_detection_t const temporary = p_detections[index];
            p_detections[index] = p_detections[best_index];
            p_detections[best_index] = temporary;
        }
    }

    int kept_count = 0;

    for (int index = 0; index < count; index++)
    {
        bool suppressed = false;

        for (int kept_index = 0;
             kept_index < kept_count;
             kept_index++)
        {
            if ((p_detections[index].class_id ==
                 p_detections[kept_index].class_id) &&
                (yolo_iou(&p_detections[index],
                          &p_detections[kept_index]) > iou_threshold))
            {
                suppressed = true;
                break;
            }
        }

        if (!suppressed)
        {
            p_detections[kept_count] = p_detections[index];
            kept_count++;
        }
    }

    return kept_count;
}
