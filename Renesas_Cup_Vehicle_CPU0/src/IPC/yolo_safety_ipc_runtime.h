#ifndef IPC_YOLO_SAFETY_IPC_RUNTIME_H_
#define IPC_YOLO_SAFETY_IPC_RUNTIME_H_

#include <stdbool.h>
#include <stdint.h>

/* AI Thread调用，短临界区写入静态环形缓冲，不直接访问IPC硬件。 */
bool yolo_safety_ipc_publish(uint32_t frame_sequence, bool detected);

#endif /* IPC_YOLO_SAFETY_IPC_RUNTIME_H_ */
