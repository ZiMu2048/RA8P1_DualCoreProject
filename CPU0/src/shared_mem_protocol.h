#ifndef SHARED_MEM_PROTOCOL_H
#define SHARED_MEM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define SHARED_MEM_MAGIC             (0x53484D31UL)
#define SHARED_MEM_VERSION           (1UL)
#define SHARED_MEM_PAYLOAD_SIZE      (32UL)

#define SHARED_MEM_STATE_FREE        (0UL)
#define SHARED_MEM_STATE_READY       (2UL)
#define SHARED_MEM_STATE_PROCESSING  (3UL)
#define SHARED_MEM_STATE_DONE        (4UL)
#define SHARED_MEM_STATE_ERROR       (5UL)

#define SHARED_MEM_OWNER_CPU0        (0UL)
#define SHARED_MEM_OWNER_CPU1        (1UL)

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t owner;
    uint32_t sequence;
    uint32_t data_length;
    uint32_t crc32;
    uint32_t error_code;
    uint8_t  payload[SHARED_MEM_PAYLOAD_SIZE];
} shared_mem_control_block_t;

_Static_assert(sizeof(shared_mem_control_block_t) == 64UL,
               "shared_mem_control_block_t must be 64 bytes");
_Static_assert(offsetof(shared_mem_control_block_t, payload) == 32UL,
               "shared payload offset must be 32 bytes");

#endif
