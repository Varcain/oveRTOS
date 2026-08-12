/**
  ******************************************************************************
  * @file    sd_diskio.c
  * @author  MCD Application Team
  * @version V1.4.0
  * @date    09-September-2016
  * @brief   SD Disk I/O driver
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT 2016 STMicroelectronics</center></h2>
  *
  * Redistribution and use in source and binary forms, with or without 
  * modification, are permitted, provided that the following conditions are met:
  *
  * 1. Redistribution of source code must retain the above copyright notice, 
  *    this list of conditions and the following disclaimer.
  * 2. Redistributions in binary form must reproduce the above copyright notice,
  *    this list of conditions and the following disclaimer in the documentation
  *    and/or other materials provided with the distribution.
  * 3. Neither the name of STMicroelectronics nor the names of other 
  *    contributors to this software may be used to endorse or promote products 
  *    derived from this software without specific written permission.
  * 4. This software, including modifications and/or derivative works of this 
  *    software, must execute solely and exclusively on microcontroller or
  *    microprocessor devices manufactured by or for STMicroelectronics.
  * 5. Redistribution and use of this software other than as permitted under 
  *    this license is void and will automatically terminate your rights under 
  *    this license. 
  *
  * THIS SOFTWARE IS PROVIDED BY STMICROELECTRONICS AND CONTRIBUTORS "AS IS" 
  * AND ANY EXPRESS, IMPLIED OR STATUTORY WARRANTIES, INCLUDING, BUT NOT 
  * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
  * PARTICULAR PURPOSE AND NON-INFRINGEMENT OF THIRD PARTY INTELLECTUAL PROPERTY
  * RIGHTS ARE DISCLAIMED TO THE FULLEST EXTENT PERMITTED BY LAW. IN NO EVENT 
  * SHALL STMICROELECTRONICS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
  * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
  * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
  * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include "ff_gen_drv.h"
#include "FreeRTOS.h"
#include "task.h"

#define SD_DMA_BUFFER_SIZE 512u
#define SD_DMA_CACHE_LINE_SIZE 32u
#define SD_DMA_TIMEOUT_MS 5000u

_Static_assert(_MAX_SS == SD_DMA_BUFFER_SIZE, "STM32 SD DMA bounce buffer must hold one sector");
_Static_assert((SD_DMA_BUFFER_SIZE % SD_DMA_CACHE_LINE_SIZE) == 0u,
	       "SD DMA buffer must span complete M7 cache lines");

extern SD_HandleTypeDef uSdHandle;

enum sd_dma_result {
	SD_DMA_PENDING = 0,
	SD_DMA_COMPLETE,
	SD_DMA_ERROR,
};

/* FatFs passes both its internal sector window and caller-owned buffers to the
 * disk layer. Their cache-line alignment and neighbouring ownership are not
 * guaranteed, so cache maintenance on those addresses could discard unrelated
 * dirty data. Keep DMA ownership in one aligned, whole-cache-line bounce sector
 * and copy at the CPU boundary instead. The disk API is already serialized by
 * FatFs/the native FS server, hence one buffer and one waiter are sufficient. */
static uint32_t g_dma_buffer[SD_DMA_BUFFER_SIZE / sizeof(uint32_t)]
	__attribute__((aligned(SD_DMA_CACHE_LINE_SIZE)));
static volatile TaskHandle_t g_dma_waiter;
static volatile uint8_t g_dma_result;

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* Private function prototypes -----------------------------------------------*/
DSTATUS SD_initialize(BYTE);
DSTATUS SD_status(BYTE);
DRESULT SD_read(BYTE, BYTE *, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write(BYTE, const BYTE *, DWORD, UINT);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE, BYTE, void *);
#endif /* _USE_IOCTL == 1 */

const Diskio_drvTypeDef SD_Driver = {
	SD_initialize, SD_status, SD_read,
#if _USE_WRITE == 1
	SD_write,
#endif /* _USE_WRITE == 1 */

#if _USE_IOCTL == 1
	SD_ioctl,
#endif /* _USE_IOCTL == 1 */
};

static void sd_dma_complete_from_isr(uint8_t result)
{
	TaskHandle_t waiter = g_dma_waiter;
	BaseType_t wake = pdFALSE;

	g_dma_result = result;
	__DMB();
	if (waiter != NULL) {
		vTaskNotifyGiveFromISR(waiter, &wake);
		portYIELD_FROM_ISR(wake);
	}
}

void BSP_SD_ReadCpltCallback(void)
{
	sd_dma_complete_from_isr(SD_DMA_COMPLETE);
}

void BSP_SD_WriteCpltCallback(void)
{
	sd_dma_complete_from_isr(SD_DMA_COMPLETE);
}

void BSP_SD_AbortCallback(void)
{
	sd_dma_complete_from_isr(SD_DMA_ERROR);
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd)
{
	(void)hsd;
	sd_dma_complete_from_isr(SD_DMA_ERROR);
}

void SDMMC1_IRQHandler(void)
{
	HAL_SD_IRQHandler(&uSdHandle);
}

void DMA2_Stream3_IRQHandler(void)
{
	HAL_DMA_IRQHandler(uSdHandle.hdmarx);
}

void DMA2_Stream6_IRQHandler(void)
{
	HAL_DMA_IRQHandler(uSdHandle.hdmatx);
}

static void sd_dma_begin(void)
{
	/* A late notification from an aborted transfer must not satisfy the next
	 * request. Publish the waiter only after draining its notification slot. */
	(void)ulTaskNotifyTake(pdTRUE, 0u);
	g_dma_result = SD_DMA_PENDING;
	g_dma_waiter = xTaskGetCurrentTaskHandle();
	__DMB();
}

static DRESULT sd_dma_wait(void)
{
	if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SD_DMA_TIMEOUT_MS)) == 0u) {
		g_dma_waiter = NULL;
		__DMB();
		(void)HAL_SD_Abort(&uSdHandle);
		return RES_ERROR;
	}

	uint8_t result = g_dma_result;
	g_dma_waiter = NULL;
	__DMB();
	return result == SD_DMA_COMPLETE ? RES_OK : RES_ERROR;
}

static DRESULT sd_wait_card_ready(void)
{
	TickType_t start = xTaskGetTickCount();
	TickType_t timeout = pdMS_TO_TICKS(SD_DMA_TIMEOUT_MS);

	while (BSP_SD_GetCardState() != MSD_OK) {
		if (xTaskGetTickCount() - start >= timeout)
			return RES_ERROR;
		vTaskDelay(1u);
	}
	return RES_OK;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  lun : not used 
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_initialize(BYTE lun)
{
	Stat = STA_NOINIT;

	/* Configure the uSD device */
	if (BSP_SD_Init() == MSD_OK) {
		Stat &= ~STA_NOINIT;
	}

	return Stat;
}

/**
  * @brief  Gets Disk Status
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_status(BYTE lun)
{
	Stat = STA_NOINIT;

	if (BSP_SD_GetCardState() == MSD_OK) {
		Stat &= ~STA_NOINIT;
	}

	return Stat;
}

/**
  * @brief  Reads Sector(s)
  * @param  lun : not used
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
	UINT i;
	DRESULT res = RES_OK;

	for (i = 0; i < count; i++) {
		/* Clean first so no pre-DMA dirty line can later overwrite received
		 * bytes, then invalidate after completion before the CPU copies them. */
		SCB_CleanInvalidateDCache_by_Addr(g_dma_buffer, sizeof(g_dma_buffer));
		sd_dma_begin();
		if (BSP_SD_ReadBlocks_DMA(g_dma_buffer, (uint32_t)(sector + i), 1u) != MSD_OK) {
			g_dma_waiter = NULL;
			__DMB();
			res = RES_ERROR;
		} else {
			res = sd_dma_wait();
		}
		if (res == RES_OK) {
			SCB_InvalidateDCache_by_Addr(g_dma_buffer, sizeof(g_dma_buffer));
			memcpy(buff + i * _MAX_SS, g_dma_buffer, _MAX_SS);
			res = sd_wait_card_ready();
		}
		if (res != RES_OK)
			break;
	}

	return res;
}

/**
  * @brief  Writes Sector(s)
  * @param  lun : not used
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
	UINT i;
	DRESULT res = RES_OK;

	for (i = 0; i < count; i++) {
		memcpy(g_dma_buffer, buff + i * _MAX_SS, _MAX_SS);
		SCB_CleanDCache_by_Addr(g_dma_buffer, sizeof(g_dma_buffer));
		sd_dma_begin();
		if (BSP_SD_WriteBlocks_DMA(g_dma_buffer, (uint32_t)(sector + i), 1u) != MSD_OK) {
			g_dma_waiter = NULL;
			__DMB();
			res = RES_ERROR;
		} else {
			res = sd_dma_wait();
		}
		if (res == RES_OK)
			res = sd_wait_card_ready();
		if (res != RES_OK)
			break;
	}

	return res;
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  lun : not used
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
	DRESULT res = RES_ERROR;
	BSP_SD_CardInfo CardInfo;

	if (Stat & STA_NOINIT)
		return RES_NOTRDY;

	switch (cmd) {
	/* Make sure that no pending write process */
	case CTRL_SYNC:
		res = RES_OK;
		break;

	/* Get number of sectors on the disk (DWORD) */
	case GET_SECTOR_COUNT:
		BSP_SD_GetCardInfo(&CardInfo);
		*(DWORD *)buff = CardInfo.LogBlockNbr;
		res = RES_OK;
		break;

	/* Get R/W sector size (WORD) */
	case GET_SECTOR_SIZE:
		BSP_SD_GetCardInfo(&CardInfo);
		*(WORD *)buff = CardInfo.LogBlockSize;
		res = RES_OK;
		break;

	/* Get erase block size in unit of sector (DWORD) */
	case GET_BLOCK_SIZE:
		/* The HAL exposes no erase-group geometry. Report one logical sector,
		 * a conservative valid granularity, rather than the old byte count. */
		*(DWORD *)buff = 1u;
		res = RES_OK;
		break;

	default:
		res = RES_PARERR;
	}

	return res;
}
#endif /* _USE_IOCTL == 1 */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
