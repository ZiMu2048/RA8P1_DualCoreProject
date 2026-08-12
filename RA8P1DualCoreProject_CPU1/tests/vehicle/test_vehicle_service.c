#include "Vehicle/application/vehicle_service.h"

#include <string.h>

#define TEST_CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

typedef struct st_fake_hardware
{
    float left;
    float right;
    float suction;
} fake_hardware_t;

static bool fake_i2c_write(void * context,
                           uint8_t address,
                           uint8_t reg,
                           uint8_t const * data,
                           size_t length)
{
    (void) context;
    (void) address;
    (void) reg;
    (void) data;
    return length > 0U;
}

static bool fake_i2c_read(void * context,
                          uint8_t address,
                          uint8_t reg,
                          uint8_t * data,
                          size_t length)
{
    (void) context;
    (void) address;
    memset(data, 0, length);
    if ((0x75U == reg) && (length > 0U)) data[0] = 0x68U;
    return true;
}

static void fake_delay(void * context, uint32_t delay_ms)
{
    (void) context;
    (void) delay_ms;
}

static bool fake_actuator_init(void * context)
{
    (void) context;
    return true;
}

static bool fake_wheels_write(void * context, float left, float right)
{
    fake_hardware_t * hardware = context;
    hardware->left = left;
    hardware->right = right;
    return true;
}

static bool fake_suction_write(void * context, float duty)
{
    fake_hardware_t * hardware = context;
    hardware->suction = duty;
    return true;
}

int main(void)
{
    fake_hardware_t hardware = {0};
    vehicle_status_t status;
    vehicle_dependencies_t dependencies =
    {
        .imu_i2c = {NULL, fake_i2c_write, fake_i2c_read, fake_delay},
        .actuators =
        {
            &hardware,
            fake_actuator_init,
            fake_wheels_write,
            fake_suction_write,
        },
    };

    TEST_CHECK(VEHICLE_RESULT_OK == vehicle_service_init(&dependencies));
    vehicle_service_status_get(&status);
    TEST_CHECK(status.initialized);
    TEST_CHECK(!status.suction_ready);
    TEST_CHECK((hardware.suction > 0.799F) && (hardware.suction < 0.801F));

    /* 吸附建立前，任何前进命令都必须被拒绝。 */
    TEST_CHECK(VEHICLE_RESULT_NOT_READY ==
               vehicle_service_manual_command(VEHICLE_MANUAL_FORWARD, 40U));
    TEST_CHECK(0.0F == hardware.left);
    TEST_CHECK(0.0F == hardware.right);

    for (uint32_t i = 0U; i < 200U; ++i)
    {
        TEST_CHECK(VEHICLE_RESULT_OK == vehicle_service_step(0.01F));
    }
    TEST_CHECK(VEHICLE_RESULT_OK ==
               vehicle_service_manual_command(VEHICLE_MANUAL_FORWARD, 40U));
    TEST_CHECK(hardware.left > 0.39F);
    TEST_CHECK(hardware.right > 0.39F);

    vehicle_service_emergency_stop();
    TEST_CHECK(0.0F == hardware.left);
    TEST_CHECK(0.0F == hardware.right);
    TEST_CHECK(0.0F == hardware.suction);

    return 0;
}
