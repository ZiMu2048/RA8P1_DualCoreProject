#ifndef IPC_YOLO_SAFETY_RUNTIME_H_
#define IPC_YOLO_SAFETY_RUNTIME_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct st_yolo_safety_result
{
    uint8_t frame_sequence;
    bool detected;
} yolo_safety_result_t;

/* 只允许Vehicle Thread调用，非阻塞读取下一条YOLO推理结果。 */
bool yolo_safety_result_take(yolo_safety_result_t * p_result);

#endif /* IPC_YOLO_SAFETY_RUNTIME_H_ */
