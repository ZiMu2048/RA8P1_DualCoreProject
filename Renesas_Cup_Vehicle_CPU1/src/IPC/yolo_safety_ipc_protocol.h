#ifndef IPC_YOLO_SAFETY_IPC_PROTOCOL_H_
#define IPC_YOLO_SAFETY_IPC_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 32位短消息格式：魔数8位、源帧序号8位、阳性状态8位、校验8位。 */
#define YOLO_SAFETY_IPC_MAGIC              (0xA9U)
#define YOLO_SAFETY_IPC_CHECK_XOR          (0xD6U)
#define YOLO_SAFETY_IPC_DETECTED_MASK      (0x80U)

typedef struct st_yolo_safety_ipc_result
{
    uint8_t frame_sequence;
    bool detected;
} yolo_safety_ipc_result_t;

static inline uint32_t yolo_safety_ipc_message_encode(uint8_t frame_sequence,
                                                       bool detected)
{
    uint8_t const status =
        detected ? YOLO_SAFETY_IPC_DETECTED_MASK : 0U;
    uint8_t const checksum = (uint8_t)
        (YOLO_SAFETY_IPC_MAGIC ^ frame_sequence ^ status ^
         YOLO_SAFETY_IPC_CHECK_XOR);

    return ((uint32_t) YOLO_SAFETY_IPC_MAGIC << 24U) |
           ((uint32_t) frame_sequence << 16U) |
           ((uint32_t) status << 8U) |
           checksum;
}

static inline bool yolo_safety_ipc_message_decode(
    uint32_t message,
    yolo_safety_ipc_result_t * p_result)
{
    uint8_t const magic = (uint8_t) (message >> 24U);
    uint8_t const sequence = (uint8_t) (message >> 16U);
    uint8_t const status = (uint8_t) (message >> 8U);
    uint8_t const checksum = (uint8_t) message;

    if((NULL == p_result) ||
       (YOLO_SAFETY_IPC_MAGIC != magic) ||
       (checksum != (uint8_t) (magic ^ sequence ^ status ^
                               YOLO_SAFETY_IPC_CHECK_XOR)))
    {
        return false;
    }

    p_result->frame_sequence = sequence;
    p_result->detected = 0U != (status & YOLO_SAFETY_IPC_DETECTED_MASK);
    return true;
}

#endif /* IPC_YOLO_SAFETY_IPC_PROTOCOL_H_ */
