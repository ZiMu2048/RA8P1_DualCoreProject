/*
 * i2c_control.h
 *
 *  Created on: 2026年8月10日
 *      Author: lingk
 */

#ifndef CAMERA_I2C_CONTROL_H_
#define CAMERA_I2C_CONTROL_H_

#include "camera_thread.h"

/* 仅在Camera Thread任务上下文调用 */
fsp_err_t i2c_control_init(void);
fsp_err_t write_reg_16bit(uint16_t address, uint8_t data);
fsp_err_t read_reg_16bit(uint16_t address, uint8_t * p_data);
fsp_err_t write_reg_8bit(uint8_t address, uint8_t data);
fsp_err_t read_reg_8bit(uint8_t address, uint8_t * p_data);

#endif /* CAMERA_I2C_CONTROL_H_ */
