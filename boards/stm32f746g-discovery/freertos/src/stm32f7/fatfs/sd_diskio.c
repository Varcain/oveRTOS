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

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* Private function prototypes -----------------------------------------------*/
DSTATUS SD_initialize (BYTE);
DSTATUS SD_status (BYTE);
DRESULT SD_read (BYTE, BYTE*, DWORD, UINT);
#if _USE_WRITE == 1
  DRESULT SD_write (BYTE, const BYTE*, DWORD, UINT);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
  DRESULT SD_ioctl (BYTE, BYTE, void*);
#endif  /* _USE_IOCTL == 1 */
  
const Diskio_drvTypeDef  SD_Driver =
{
  SD_initialize,
  SD_status,
  SD_read, 
#if  _USE_WRITE == 1
  SD_write,
#endif /* _USE_WRITE == 1 */
  
#if  _USE_IOCTL == 1
  SD_ioctl,
#endif /* _USE_IOCTL == 1 */
};

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
  if(BSP_SD_Init() == MSD_OK)
  {
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

  if(BSP_SD_GetCardState() == MSD_OK)
  {
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
  uint8_t sd_res;

  for (i = 0; i < count; i++)
  {
    uint32_t timeout = 100000;

    /* Mask interrupts during SDMMC FIFO polling to prevent overflow.
       A single 512-byte sector takes ~50 us at 25 MHz / 4-bit — well
       within the audio DMA deadline (~2.9 ms at 44.1 kHz / 128 samples).
       taskENTER_CRITICAL raises BASEPRI to configMAX_SYSCALL_INTERRUPT_
       PRIORITY (5), masking audio DMA (6) and SysTick (15). */
    taskENTER_CRITICAL();
    sd_res = BSP_SD_ReadBlocks((uint32_t*)(buff + i * _MAX_SS),
                               (uint32_t)(sector + i),
                               1, SD_DATATIMEOUT);
    taskEXIT_CRITICAL();

    if (sd_res != MSD_OK)
    {
      res = RES_ERROR;
      break;
    }

    /* Poll for card ready with interrupts enabled */
    while (BSP_SD_GetCardState() != MSD_OK)
    {
      if (timeout-- == 0)
      {
        res = RES_ERROR;
        break;
      }
    }
    if (res != RES_OK) break;
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
  uint8_t sd_res;

  for (i = 0; i < count; i++)
  {
    uint32_t timeout = 100000;

    taskENTER_CRITICAL();
    sd_res = BSP_SD_WriteBlocks((uint32_t*)(buff + i * _MAX_SS),
                                (uint32_t)(sector + i),
                                1, SD_DATATIMEOUT);
    taskEXIT_CRITICAL();

    if (sd_res != MSD_OK)
    {
      res = RES_ERROR;
      break;
    }

    while (BSP_SD_GetCardState() != MSD_OK)
    {
      if (timeout-- == 0)
      {
        res = RES_ERROR;
        break;
      }
    }
    if (res != RES_OK) break;
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
  
  if (Stat & STA_NOINIT) return RES_NOTRDY;
  
  switch (cmd)
  {
  /* Make sure that no pending write process */
  case CTRL_SYNC :
    res = RES_OK;
    break;
  
  /* Get number of sectors on the disk (DWORD) */
  case GET_SECTOR_COUNT :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockNbr;
    res = RES_OK;
    break;
  
  /* Get R/W sector size (WORD) */
  case GET_SECTOR_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(WORD*)buff = CardInfo.LogBlockSize;
    res = RES_OK;
    break;
  
  /* Get erase block size in unit of sector (DWORD) */
  case GET_BLOCK_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockSize;
    break;
  
  default:
    res = RES_PARERR;
  }
  
  return res;
}
#endif /* _USE_IOCTL == 1 */
  
/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

