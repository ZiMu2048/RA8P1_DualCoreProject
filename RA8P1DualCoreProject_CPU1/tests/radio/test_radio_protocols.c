#include "Radio/protocol/control_protocol.h"
#include "Radio/protocol/video_protocol.h"

#include <string.h>

#define TEST_CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static uint16_t read_u16(uint8_t const * p_data)
{
    return (uint16_t) p_data[0] | (uint16_t) ((uint16_t) p_data[1] << 8U);
}

int main(void)
{
    uint8_t control[CONTROL_PACKET_SIZE] =
    {
        CONTROL_PACKET_MAGIC,
        CONTROL_PACKET_VERSION,
        CONTROL_ID_DIRECTION,
        CONTROL_ACTION_PRESSED,
        CONTROL_DIRECTION_FORWARD,
        0U,
        7U,
        0U,
    };
    for (uint32_t i = 0U; i < CONTROL_PACKET_SIZE - 1U; i++)
    {
        control[CONTROL_PACKET_SIZE - 1U] ^= control[i];
    }

    control_packet_t decoded;
    uint16_t value = 0U;
    TEST_CHECK(ControlProtocol_Decode(control, sizeof(control), &decoded, &value));
    TEST_CHECK(CONTROL_DIRECTION_FORWARD == value);
    TEST_CHECK(7U == decoded.sequence);

    control[7] ^= 1U;
    TEST_CHECK(!ControlProtocol_Decode(control, sizeof(control), &decoded, &value));

    uint8_t jpeg[25];
    (void) memset(jpeg, 0xA5, sizeof(jpeg));
    video_frame_t frame =
    {
        .p_jpeg = jpeg,
        .jpeg_size = sizeof(jpeg),
        .crc32 = VideoProtocol_Crc32(jpeg, sizeof(jpeg)),
        .frame_id = 42U,
        .source_width = 240U,
        .source_height = 136U,
    };
    TEST_CHECK(2U == VideoProtocol_ChunkCountGet(frame.jpeg_size));

    uint8_t packet[VIDEO_PACKET_SIZE];
    VideoProtocol_StartPacketBuild(&frame, packet);
    TEST_CHECK(VIDEO_PACKET_MAGIC == packet[0]);
    TEST_CHECK(VIDEO_PACKET_TYPE_START == packet[1]);
    TEST_CHECK(42U == read_u16(&packet[2]));
    TEST_CHECK(2U == read_u16(&packet[18]));

    VideoProtocol_DataPacketBuild(&frame, 1U, packet);
    TEST_CHECK(VIDEO_PACKET_TYPE_DATA == packet[1]);
    TEST_CHECK(1U == packet[6]);
    TEST_CHECK(0xA5U == packet[7]);

    VideoProtocol_EndPacketBuild(&frame, packet);
    TEST_CHECK(VIDEO_PACKET_TYPE_END == packet[1]);
    TEST_CHECK(2U == read_u16(&packet[4]));
    return 0;
}
