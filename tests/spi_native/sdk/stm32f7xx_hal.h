/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_TEST_SPI_STM32_HAL_H
#define OVE_TEST_SPI_STM32_HAL_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct {
	unsigned int opaque;
} SPI_TypeDef;
extern SPI_TypeDef ove_test_spi_peripherals[5];
#define SPI1 (&ove_test_spi_peripherals[0])
#define SPI2 (&ove_test_spi_peripherals[1])
#define SPI3 (&ove_test_spi_peripherals[2])
#define SPI4 (&ove_test_spi_peripherals[3])
#define SPI5 (&ove_test_spi_peripherals[4])
#define SPI_MODE_MASTER 1
#define SPI_DIRECTION_2LINES 2
#define SPI_DATASIZE_8BIT 8
#define SPI_DATASIZE_16BIT 16
#define SPI_NSS_SOFT 1
#define SPI_NSS_PULSE_DISABLE 0
#define SPI_FIRSTBIT_LSB 1
#define SPI_FIRSTBIT_MSB 0
#define SPI_TIMODE_DISABLE 0
#define SPI_CRCCALCULATION_DISABLE 0
#define SPI_POLARITY_LOW 0
#define SPI_POLARITY_HIGH 1
#define SPI_PHASE_1EDGE 0
#define SPI_PHASE_2EDGE 1
#define SPI_BAUDRATEPRESCALER_16 16
#define HAL_MAX_DELAY UINT32_MAX
typedef struct {
	SPI_TypeDef *Instance;
	struct {
		uint32_t Mode, Direction, DataSize, NSS, NSSPMode, FirstBit;
		uint32_t TIMode, CRCCalculation, CLKPolarity, CLKPhase, BaudRatePrescaler;
	} Init;
} SPI_HandleTypeDef;
typedef struct {
	unsigned int opaque;
} IWDG_HandleTypeDef;
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *, uint8_t *, uint8_t *, uint16_t,
					  uint32_t);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_TransmitReceive_IT(SPI_HandleTypeDef *, uint8_t *, uint8_t *, uint16_t);
HAL_StatusTypeDef HAL_SPI_Transmit_IT(SPI_HandleTypeDef *, uint8_t *, uint16_t);
HAL_StatusTypeDef HAL_SPI_Receive_IT(SPI_HandleTypeDef *, uint8_t *, uint16_t);
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *);
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *);
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *);
uint32_t HAL_GetTick(void);
#endif
