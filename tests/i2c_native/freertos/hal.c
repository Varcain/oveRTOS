/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ove/irq.h"
ove_irq_key_t ove_irq_lock(void)
{
	taskENTER_CRITICAL();
	return 0;
}
void ove_irq_unlock(ove_irq_key_t key)
{
	(void)key;
	taskEXIT_CRITICAL();
}
bool ove_is_in_isr(void)
{
	return false;
}

unsigned i2c_hal_calls;
uint16_t i2c_hal_address, i2c_hal_sizes[2];
uint32_t i2c_hal_frames[2];
static HAL_StatusTypeDef record(I2C_HandleTypeDef *handle, uint16_t address, uint16_t size,
				uint32_t frame)
{
	unsigned index = i2c_hal_calls++;
	if (index < 2) {
		i2c_hal_sizes[index] = size;
		i2c_hal_frames[index] = frame;
	}
	i2c_hal_address = address;
	handle->State = HAL_I2C_STATE_READY;
	handle->ErrorCode = HAL_I2C_ERROR_NONE;
	return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *handle)
{
	handle->State = HAL_I2C_STATE_READY;
	return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *handle)
{
	(void)handle;
	return HAL_OK;
}
uint32_t HAL_I2C_GetError(const I2C_HandleTypeDef *handle)
{
	return handle->ErrorCode;
}
uint32_t HAL_GetTick(void)
{
	return xTaskGetTickCount();
}
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *handle, uint16_t address,
					  uint8_t *data, uint16_t size, uint32_t timeout)
{
	(void)data;
	(void)timeout;
	return record(handle, address, size, 0);
}
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *handle, uint16_t address, uint8_t *data,
					 uint16_t size, uint32_t timeout)
{
	return HAL_I2C_Master_Transmit(handle, address, data, size, timeout);
}
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *handle, uint16_t address,
				   uint16_t memory_address, uint16_t memory_size, uint8_t *data,
				   uint16_t size, uint32_t timeout)
{
	(void)memory_address;
	(void)memory_size;
	return HAL_I2C_Master_Transmit(handle, address, data, size, timeout);
}
HAL_StatusTypeDef HAL_I2C_Master_Transmit_IT(I2C_HandleTypeDef *handle, uint16_t address,
					     uint8_t *data, uint16_t size)
{
	(void)data;
	return record(handle, address, size, 0);
}
HAL_StatusTypeDef HAL_I2C_Master_Receive_IT(I2C_HandleTypeDef *handle, uint16_t address,
					    uint8_t *data, uint16_t size)
{
	return HAL_I2C_Master_Transmit_IT(handle, address, data, size);
}
HAL_StatusTypeDef HAL_I2C_Master_Seq_Transmit_IT(I2C_HandleTypeDef *handle, uint16_t address,
						 uint8_t *data, uint16_t size, uint32_t frame)
{
	(void)data;
	return record(handle, address, size, frame);
}
HAL_StatusTypeDef HAL_I2C_Master_Seq_Receive_IT(I2C_HandleTypeDef *handle, uint16_t address,
						uint8_t *data, uint16_t size, uint32_t frame)
{
	return HAL_I2C_Master_Seq_Transmit_IT(handle, address, data, size, frame);
}
