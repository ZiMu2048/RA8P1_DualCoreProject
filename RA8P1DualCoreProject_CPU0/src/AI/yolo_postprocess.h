/*
 * yolo_postprocess.h
 *
 * YOLO INT8 output decoding and non-maximum suppression.
 * This module contains no drawing or RTOS code.
 */

#ifndef AI_YOLO_POSTPROCESS_H_
#define AI_YOLO_POSTPROCESS_H_

#include <stdint.h>

#define YOLO_INPUT_SIZE             (128)
#define YOLO_CLASS_COUNT            (1)
#define YOLO_OUTPUT_BOX_COUNT       (336)
#define YOLO_OUTPUT_ATTRS           (5)
#define YOLO_MAX_DETECTIONS         (4)

typedef struct st_yolo_detection
{
    float   x1;       /* Left edge in 128 x 128 model coordinates. */
    float   y1;       /* Top edge in 128 x 128 model coordinates. */
    float   x2;       /* Right edge in 128 x 128 model coordinates. */
    float   y2;       /* Bottom edge in 128 x 128 model coordinates. */
    float   score;    /* Confidence in the range 0.0 to 1.0. */
    uint8_t class_id;
} yolo_detection_t;

extern const char * const g_yolo_class_names[YOLO_CLASS_COUNT];

/*
 * Decode the model's INT8 [336, 5] output tensor.
 *
 * The function writes at most capacity detections.  If more candidates pass
 * the threshold, the candidates with the highest scores are retained.
 */
int yolo_decode_int8_output(const int8_t * p_output,
                            yolo_detection_t * p_detections,
                            int capacity,
                            float confidence_threshold);

/*
 * Sort and suppress detections in place.  The return value is the number of
 * valid elements remaining at the beginning of p_detections.
 */
int yolo_nms(yolo_detection_t * p_detections,
             int count,
             float iou_threshold);

#endif /* AI_YOLO_POSTPROCESS_H_ */
