/*
 * camera_capture.h
 *
 *  Created on: 2026年8月11日
 *      Author: lingk
 */

#ifndef CAMERA_CAMERA_CAPTURE_H_
#define CAMERA_CAMERA_CAPTURE_H_

#include "hal_data.h"
#include "common/common.h"

fsp_err_t camera_capture_open(void);
camera_capture_start(void);
camera_capture_stop(void);
uint8_t * camera_completed_frame_get(uint32_t * p_sequence);
void vin_callback(capture_callback_args_t * p_args);
void mipi_csi0_callback(mipi_csi_callback_args_t * p_args);

#endif /* CAMERA_CAMERA_CAPTURE_H_ */
