/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2C

/*
 * Single transfers and 1- or 2-byte register reads use the HAL's polled
 * calls.  A write of more than two bytes followed by a repeated-start read
 * has no polled HAL call, so it runs on the peripheral's interrupts: the
 * vectors below hand them to the HAL, and the caller sleeps on a task
 * notification until the HAL reports the transfer done.  The asynchronous
 * calls use the same interrupts.  A blocking call and an asynchronous
 * transfer never overlap: whichever comes second is refused with
 * OVE_ERR_BUS_BUSY.  A transfer that fails or times out resets the
 * peripheral, which releases SCL and SDA, so the bus is never left held.
 */

#include "ove/hal/hal_i2c.h"
#include "ove_backend_common.h"
#include "stm32f7_init.h"
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

#define I2C_INSTANCES 4

/* The open handle of each peripheral, for its interrupt vectors. */
static struct ove_i2c *s_i2c[I2C_INSTANCES];

static I2C_TypeDef *instance_to_periph(unsigned int instance)
{
	switch (instance) {
	case 0:
		return I2C1;
	case 1:
		return I2C2;
	case 2:
		return I2C3;
#ifdef I2C4
	case 3:
		return I2C4;
#endif
	default:
		return NULL;
	}
}

static IRQn_Type instance_ev_irq(unsigned int instance)
{
	switch (instance) {
	case 0:
		return I2C1_EV_IRQn;
	case 1:
		return I2C2_EV_IRQn;
	case 2:
		return I2C3_EV_IRQn;
#ifdef I2C4
	case 3:
		return I2C4_EV_IRQn;
#endif
	default:
		return (IRQn_Type)-1;
	}
}

static IRQn_Type instance_er_irq(unsigned int instance)
{
	switch (instance) {
	case 0:
		return I2C1_ER_IRQn;
	case 1:
		return I2C2_ER_IRQn;
	case 2:
		return I2C3_ER_IRQn;
#ifdef I2C4
	case 3:
		return I2C4_ER_IRQn;
#endif
	default:
		return (IRQn_Type)-1;
	}
}

static void irq_enable(unsigned int instance, int on)
{
	IRQn_Type irqs[2] = {instance_ev_irq(instance), instance_er_irq(instance)};
	for (int i = 0; i < 2; i++) {
		if (on) {
			/* Low enough to call the FreeRTOS FromISR API. */
			HAL_NVIC_SetPriority(irqs[i], configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
					     0);
			HAL_NVIC_EnableIRQ(irqs[i]);
		} else {
			HAL_NVIC_DisableIRQ(irqs[i]);
		}
	}
}

static uint32_t speed_to_timing(uint32_t speed_hz)
{
	/* STM32F7 I2C timing values (assumes 216 MHz system clock).
	 * These are typical values — production boards may need
	 * CubeMX-generated timing for exact specs. */
	if (speed_hz >= 1000000)
		return 0x00200404; /* Fast-mode Plus ~1 MHz */
	if (speed_hz >= 400000)
		return 0x00B01A4B; /* Fast-mode 400 kHz */
	return 0x10C0ECFF;	   /* Standard-mode 100 kHz */
}

/* Translate a HAL status into an ove code.  The F7 I2C HAL never returns
 * HAL_TIMEOUT: a timeout is HAL_ERROR with HAL_I2C_ERROR_TIMEOUT in
 * ErrorCode, so the cause is read from there.  AF is checked first because a
 * NACKed transfer whose STOP never arrives reports TIMEOUT as well.  Reads
 * the handle only, so the error callback can use it in interrupt context. */
static int hal_i2c_to_ove(const I2C_HandleTypeDef *hi2c, HAL_StatusTypeDef status)
{
	if (status == HAL_OK)
		return OVE_OK;
	if (status == HAL_BUSY)
		return OVE_ERR_BUS_BUSY;
	if (hi2c->ErrorCode & HAL_I2C_ERROR_AF)
		return OVE_ERR_BUS_NACK;
	if (hi2c->ErrorCode & HAL_I2C_ERROR_TIMEOUT)
		return OVE_ERR_TIMEOUT;
	return OVE_ERR_BUS_ERROR;
}

/* The blocking HAL calls wait for a free bus before they reset ErrorCode,
 * and only OR into it, so if that wait times out on a wedged bus the AF of
 * an earlier NACK would still be set and the timeout would read as a NACK.
 * Clear it first, unless a transfer is in flight: the HAL call then returns
 * HAL_BUSY without touching it, and the running transfer still owns it. */
static void clear_stale_error(I2C_HandleTypeDef *hi2c)
{
	if (hi2c->State == HAL_I2C_STATE_READY)
		hi2c->ErrorCode = HAL_I2C_ERROR_NONE;
}

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	I2C_TypeDef *periph = instance_to_periph(cfg->instance);
	HAL_StatusTypeDef hal_ret;

	if (periph == NULL)
		return OVE_ERR_INVALID_PARAM;

	memset(&i2c->hal_handle, 0, sizeof(i2c->hal_handle));
	i2c->hal_handle.Instance = periph;
	i2c->hal_handle.Init.Timing = speed_to_timing(i2c->speed_hz);
	i2c->hal_handle.Init.OwnAddress1 = 0;
	i2c->hal_handle.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	i2c->hal_handle.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	i2c->hal_handle.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	i2c->hal_handle.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

	hal_ret = HAL_I2C_Init(&i2c->hal_handle);
	if (hal_ret != HAL_OK)
		return OVE_ERR_IO;

	i2c->waiter = NULL;
	s_i2c[cfg->instance] = i2c;
	irq_enable(cfg->instance, 1);
	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	if (i2c->instance < I2C_INSTANCES && s_i2c[i2c->instance] == i2c) {
		irq_enable(i2c->instance, 0);
		s_i2c[i2c->instance] = NULL;
	}
	HAL_I2C_DeInit(&i2c->hal_handle);
}

/* Abandons a transfer and resets the peripheral: clearing PE releases SCL
 * and SDA and drops the transfer in hardware, and the HAL handle goes back
 * to ready, keeping its ErrorCode. */
static void i2c_reset(struct ove_i2c *i2c)
{
	I2C_HandleTypeDef *h = &i2c->hal_handle;

	irq_enable(i2c->instance, 0);
	__HAL_I2C_DISABLE_IT(h, I2C_IT_ERRI | I2C_IT_TCI | I2C_IT_STOPI | I2C_IT_NACKI |
					I2C_IT_ADDRI | I2C_IT_RXI | I2C_IT_TXI);
	__HAL_I2C_DISABLE(h);
	(void)READ_REG(h->Instance->CR1); /* PE must stay low for three APB clocks */
	(void)READ_REG(h->Instance->CR1);
	(void)READ_REG(h->Instance->CR1);
	__HAL_I2C_ENABLE(h);
	h->State = HAL_I2C_STATE_READY;
	h->PreviousState = 0;
	h->Mode = HAL_I2C_MODE_NONE;
	h->XferISR = NULL;
	__HAL_UNLOCK(h);
	irq_enable(i2c->instance, 1);
}

/* Before the scheduler starts there is no task to claim the peripheral
 * for or to notify, and FreeRTOS keeps interrupts masked from its first
 * object on (the bus mutex is one), so only the polled transfers can run;
 * the caller is then the only thread there is. */
static int scheduler_running(void)
{
	return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

/* Claims the peripheral for a blocking call, or refuses while an
 * asynchronous transfer is in flight.  Publishing the caller as the waiter
 * is the claim, and also routes the completion interrupts to it.  The check
 * and the claim are one critical section, as is the asynchronous side's
 * (which may run in an interrupt), so the two cannot both win. */
static int sync_claim(struct ove_i2c *i2c)
{
	int busy = 0;
	int running = scheduler_running();
	UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
#ifdef CONFIG_OVE_ASYNC
	busy = i2c->async_busy;
#endif
	if (!busy && running)
		i2c->waiter = xTaskGetCurrentTaskHandle();
	taskEXIT_CRITICAL_FROM_ISR(saved);
	if (busy)
		return OVE_ERR_BUS_BUSY;
	if (running)
		(void)ulTaskNotifyTake(pdTRUE, 0); /* drop a stale notification */
	return OVE_OK;
}

/* Ends a blocking call's claim.  A completion interrupt can come before the
 * caller starts waiting, which leaves its notification pending; it is
 * dropped here, or it would end some later wait of this task (a thread
 * join, say).  The interrupt reads the waiter and notifies in one go, so
 * nothing arrives once the waiter is cleared. */
static void sync_release(struct ove_i2c *i2c)
{
	if (i2c->waiter == NULL)
		return; /* claimed before the scheduler started */
	i2c->waiter = NULL;
	(void)ulTaskNotifyTake(pdTRUE, 0);
}

/* Finishes a polled transfer: a failed or timed-out one may have left the
 * controller holding the bus, so it resets the peripheral. */
static int polled_done(struct ove_i2c *i2c, HAL_StatusTypeDef ret)
{
	int rc = hal_i2c_to_ove(&i2c->hal_handle, ret);
	if (ret == HAL_ERROR || ret == HAL_TIMEOUT)
		i2c_reset(i2c);
	sync_release(i2c);
	return rc;
}

/* Sleeps until the interrupt-driven transfer the HAL just started is done,
 * or the deadline passes; the HAL is ready again either way. */
static int i2c_await(struct ove_i2c *i2c, TimeOut_t *start, TickType_t *ticks)
{
	I2C_HandleTypeDef *h = &i2c->hal_handle;

	while (h->State != HAL_I2C_STATE_READY) {
		if (xTaskCheckForTimeOut(start, ticks) != pdFALSE) {
			h->ErrorCode |= HAL_I2C_ERROR_TIMEOUT;
			i2c_reset(i2c);
			return OVE_ERR_TIMEOUT;
		}
		(void)ulTaskNotifyTake(pdTRUE, *ticks);
	}
	if (h->ErrorCode == HAL_I2C_ERROR_NONE)
		return OVE_OK;
	int rc = hal_i2c_to_ove(h, HAL_ERROR);
	i2c_reset(i2c); /* do not leave a NACKed or faulted transfer on the bus */
	return rc;
}

/* Writes @p tx without a STOP, then reads @p rx after a repeated start. */
static int i2c_write_then_read(struct ove_i2c *i2c, uint16_t addr, const void *tx, size_t tx_len,
			       void *rx, size_t rx_len, uint64_t timeout_ns)
{
	I2C_HandleTypeDef *h = &i2c->hal_handle;
	TimeOut_t start;
	TickType_t ticks = ove_timeout_is_forever(timeout_ns)
				   ? portMAX_DELAY
				   : pdMS_TO_TICKS(stm32f7_ns_to_hal_ms(timeout_ns));

	if (!scheduler_running())
		return OVE_ERR_NOT_SUPPORTED; /* its interrupts are masked until then */
	int rc = sync_claim(i2c);
	if (rc != OVE_OK)
		return rc;
	clear_stale_error(h);
	vTaskSetTimeOutState(&start);

	HAL_StatusTypeDef ret = HAL_I2C_Master_Seq_Transmit_IT(
		h, (uint16_t)(addr << 1), (uint8_t *)tx, (uint16_t)tx_len, I2C_FIRST_FRAME);
	rc = ret == HAL_OK ? i2c_await(i2c, &start, &ticks) : hal_i2c_to_ove(h, ret);
	if (rc == OVE_OK) {
		ret = HAL_I2C_Master_Seq_Receive_IT(h, (uint16_t)(addr << 1), rx, (uint16_t)rx_len,
						    I2C_LAST_FRAME);
		rc = ret == HAL_OK ? i2c_await(i2c, &start, &ticks) : hal_i2c_to_ove(h, ret);
	}
	sync_release(i2c);
	return rc;
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	int rc = sync_claim(i2c);
	if (rc != OVE_OK)
		return rc;
	clear_stale_error(&i2c->hal_handle);
	HAL_StatusTypeDef ret = HAL_I2C_Master_Transmit(&i2c->hal_handle, (uint16_t)(addr << 1),
							(uint8_t *)data, (uint16_t)len,
							stm32f7_ns_to_hal_ms(timeout_ns));
	return polled_done(i2c, ret);
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	int rc = sync_claim(i2c);
	if (rc != OVE_OK)
		return rc;
	clear_stale_error(&i2c->hal_handle);
	HAL_StatusTypeDef ret = HAL_I2C_Master_Receive(&i2c->hal_handle, (uint16_t)(addr << 1), buf,
						       (uint16_t)len,
						       stm32f7_ns_to_hal_ms(timeout_ns));
	return polled_done(i2c, ret);
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	if (tx_len == 0)
		return ove_hal_i2c_read(i2c, addr, rx, rx_len, timeout_ns);
	if (tx_len > 2)
		return i2c_write_then_read(i2c, addr, tx, tx_len, rx, rx_len, timeout_ns);

	/* A 1- or 2-byte register address: the HAL's polled register read. */
	uint16_t mem_addr;
	uint16_t mem_size;
	if (tx_len == 1) {
		mem_addr = ((const uint8_t *)tx)[0];
		mem_size = I2C_MEMADD_SIZE_8BIT;
	} else {
		mem_addr = (uint16_t)(((const uint8_t *)tx)[0] << 8 | ((const uint8_t *)tx)[1]);
		mem_size = I2C_MEMADD_SIZE_16BIT;
	}
	int rc = sync_claim(i2c);
	if (rc != OVE_OK)
		return rc;
	clear_stale_error(&i2c->hal_handle);
	HAL_StatusTypeDef ret = HAL_I2C_Mem_Read(&i2c->hal_handle, (uint16_t)(addr << 1), mem_addr,
						 mem_size, rx, (uint16_t)rx_len,
						 stm32f7_ns_to_hal_ms(timeout_ns));
	return polled_done(i2c, ret);
}

/* ---------- Interrupts ---------- */

static void i2c_ev_irq(unsigned int instance)
{
	struct ove_i2c *i2c = s_i2c[instance];
	if (i2c != NULL)
		HAL_I2C_EV_IRQHandler(&i2c->hal_handle);
}

static void i2c_er_irq(unsigned int instance)
{
	struct ove_i2c *i2c = s_i2c[instance];
	if (i2c != NULL)
		HAL_I2C_ER_IRQHandler(&i2c->hal_handle);
}

void I2C1_EV_IRQHandler(void)
{
	i2c_ev_irq(0);
}

void I2C1_ER_IRQHandler(void)
{
	i2c_er_irq(0);
}

void I2C2_EV_IRQHandler(void)
{
	i2c_ev_irq(1);
}

void I2C2_ER_IRQHandler(void)
{
	i2c_er_irq(1);
}

void I2C3_EV_IRQHandler(void)
{
	i2c_ev_irq(2);
}

void I2C3_ER_IRQHandler(void)
{
	i2c_er_irq(2);
}

#ifdef I2C4
void I2C4_EV_IRQHandler(void)
{
	i2c_ev_irq(3);
}

void I2C4_ER_IRQHandler(void)
{
	i2c_er_irq(3);
}
#endif

static struct ove_i2c *handle_owner(I2C_HandleTypeDef *hi2c)
{
	return (struct ove_i2c *)((char *)hi2c - offsetof(struct ove_i2c, hal_handle));
}

#ifdef CONFIG_OVE_ASYNC

int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len,
				 void *rx, size_t rx_len)
{
	I2C_HandleTypeDef *h = &i2c->hal_handle;
	HAL_StatusTypeDef ret;

	/* The common layer has marked the transfer busy, which keeps blocking
	 * calls off from here on; refuse if one already holds the peripheral
	 * (see sync_claim). */
	UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
	int busy = i2c->waiter != NULL;
	taskEXIT_CRITICAL_FROM_ISR(saved);
	if (busy)
		return OVE_ERR_BUS_BUSY;

	clear_stale_error(h);
	i2c->async_rx = NULL;
	i2c->async_rx_len = 0;
	if (rx_len == 0) {
		ret = HAL_I2C_Master_Transmit_IT(h, (uint16_t)(addr << 1), (uint8_t *)tx,
						 (uint16_t)tx_len);
	} else if (tx_len == 0) {
		ret = HAL_I2C_Master_Receive_IT(h, (uint16_t)(addr << 1), rx, (uint16_t)rx_len);
	} else {
		/* Write without a STOP; its completion starts the repeated-start
		 * read (i2c_done): the blocking path's sequence, for a write of
		 * any length. */
		i2c->async_rx = rx;
		i2c->async_rx_len = (uint16_t)rx_len;
		i2c->async_addr = addr;
		ret = HAL_I2C_Master_Seq_Transmit_IT(h, (uint16_t)(addr << 1), (uint8_t *)tx,
						     (uint16_t)tx_len, I2C_FIRST_FRAME);
	}
	return hal_i2c_to_ove(h, ret);
}

#endif /* CONFIG_OVE_ASYNC */

/* Interrupt context: wakes a waiting caller, or carries an asynchronous
 * transfer on and reports it. */
static void i2c_done(I2C_HandleTypeDef *hi2c, int result)
{
	struct ove_i2c *i2c = handle_owner(hi2c);
	TaskHandle_t waiter = i2c->waiter;

	if (waiter != NULL) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(waiter, &woken);
		portYIELD_FROM_ISR(woken);
		return;
	}
#ifdef CONFIG_OVE_ASYNC
	if (!i2c->async_busy)
		return;
	if (result == OVE_OK && i2c->async_rx != NULL) {
		void *rx = i2c->async_rx;
		i2c->async_rx = NULL;
		HAL_StatusTypeDef ret =
			HAL_I2C_Master_Seq_Receive_IT(hi2c, (uint16_t)(i2c->async_addr << 1), rx,
						      i2c->async_rx_len, I2C_LAST_FRAME);
		if (ret == HAL_OK)
			return;
		result = hal_i2c_to_ove(hi2c, ret);
	}
	i2c->async_rx = NULL;
	if (result != OVE_OK)
		i2c_reset(i2c); /* as in i2c_await: release the bus */
	ove_i2c_async_complete(i2c, result);
#else
	(void)result;
#endif
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
	i2c_done(hi2c, OVE_OK);
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
	i2c_done(hi2c, OVE_OK);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
	i2c_done(hi2c, hal_i2c_to_ove(hi2c, HAL_ERROR));
}

#endif /* CONFIG_OVE_I2C */
