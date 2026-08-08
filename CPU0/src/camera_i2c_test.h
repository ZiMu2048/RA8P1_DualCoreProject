#ifndef CAMERA_I2C_TEST_H_
#define CAMERA_I2C_TEST_H_

#include <stdbool.h>
#include "hal_data.h"

/* 测试函数：复位OV5640并通过IIC1读取芯片ID寄存器。 */
bool camera_i2c_id_test(void);

extern volatile uint8_t   g_camera_i2c_test_id_high;
extern volatile uint8_t   g_camera_i2c_test_id_low;
extern volatile uint16_t  g_camera_i2c_test_chip_id;
extern volatile fsp_err_t g_camera_i2c_test_last_error;
extern volatile uint32_t  g_camera_i2c_test_passed;

#endif /* CAMERA_I2C_TEST_H_ */
