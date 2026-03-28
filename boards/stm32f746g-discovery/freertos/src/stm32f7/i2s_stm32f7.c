/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "i2s_stm32f7.h"
#include "stm32746g_discovery_audio.h"
#include "stm32f7xx_hal.h"
#include "serial_wrapper.h"
#include "board_desc.h"

/* ========================================================================= */
/* CONFIGURATION & CONSTANTS                                                 */
/* ========================================================================= */

/** Half-buffer size matches DSP processing block size */
#define HALF_BUFFER_SIZE (OVE_AUDIO_I2S_BUFFER_SAMPLES)

/** Total buffer size for double-buffering (ping-pong) */
#define FULL_BUFFER_SIZE (HALF_BUFFER_SIZE * 2)

/** Audio sample rate (Hz) - board default, overridable via i2s_stm32f7_set_sample_rate() */
static unsigned int g_sample_rate = OVE_AUDIO_I2S_SAMPLE_RATE;

/** Input device selection — set before i2s_stm32f7_init() via
 *  i2s_stm32f7_set_input_device(). Default: line-in. */
static int g_use_dmic;

/* ========================================================================= */
/* DMA BUFFERS - PLACED IN DTCM (NON-CACHEABLE MEMORY)                       */
/* ========================================================================= */

/**
 * DMA buffers must be in non-cacheable memory to avoid cache coherency issues
 * with the Cortex-M7 D-Cache. DTCM (0x20000000-0x2000FFFF) is not cached.
 *
 * The .RxBUF and .TxBUF sections are defined in the linker script and placed
 * in DTCM memory regions (Memory3 and Memory4).
 */
static uint16_t g_rx_buffer[FULL_BUFFER_SIZE] __attribute__((section(".RxBUF"), aligned(32)));
static uint16_t g_tx_buffer[FULL_BUFFER_SIZE] __attribute__((section(".TxBUF"), aligned(32)));

/* ========================================================================= */
/* DRIVER STATE STRUCTURE                                                    */
/* ========================================================================= */

/**
 * @brief Encapsulated driver state for real-time audio processing
 *
 * All mutable state is isolated here and marked volatile where accessed
 * from both ISR and task context. This structure ensures clear ownership
 * and makes thread-safety requirements explicit.
 */
typedef struct {
    /* Hardware handles - configured once, read-only after init */
    SAI_HandleTypeDef sai_tx;           /**< SAI transmit handle */
    SAI_HandleTypeDef sai_rx;           /**< SAI receive handle */
    DMA_HandleTypeDef dma_tx;           /**< DMA transmit handle */
    DMA_HandleTypeDef dma_rx;           /**< DMA receive handle */
    AUDIO_DrvTypeDef *codec_driver;     /**< WM8994 codec driver */

    /* DMA buffers - pointers to DTCM (non-cacheable) memory */
    uint16_t *rx_buffer;                /**< RX buffer in DTCM */
    uint16_t *tx_buffer;                /**< TX buffer in DTCM */

    /* Buffer state - modified in ISR, read in task context */
    volatile uint8_t rx_completed_buffer_half;    /**< Current RX half: 0=first, 1=second */
    volatile uint8_t tx_completed_buffer_half;    /**< Current TX half: 0=first, 1=second */
    volatile bool rx_buffer_ready;      /**< Flag: RX half-buffer ready for processing */

    /* Callbacks for completion notifications */
    i2s_driver_rxCompleteCb rx_complete_callback;
    i2s_driver_rxCompleteCb tx_complete_callback;  /* Reuse same callback type */

    /* Driver state */
    bool initialized;                   /**< Hardware initialization complete */
    bool rx_streaming;                     /**< DMA streaming active */
    bool tx_streaming;                     /**< DMA streaming active */

    /* Debug counters */
    volatile uint32_t rx_half_count;    /**< RX half-complete interrupt count */
    volatile uint32_t rx_full_count;    /**< RX full-complete interrupt count */
    volatile uint32_t tx_half_count;    /**< TX half-complete interrupt count */
    volatile uint32_t tx_full_count;    /**< TX full-complete interrupt count */
    volatile uint32_t rx_callback_invoked; /**< Times user callback was called */
    volatile uint32_t tx_callback_invoked; /**< Times user callback was called */
    volatile uint32_t phase_misalign;   /**< Times RX/TX phase mismatch detected */
} i2s_driver_state_t;

/* ========================================================================= */
/* PRIVATE STATE                                                             */
/* ========================================================================= */

/** Single driver instance - static allocation for real-time predictability */
static i2s_driver_state_t g_driver_state = {0};

/* ========================================================================= */
/* FORWARD DECLARATIONS                                                      */
/* ========================================================================= */

static void hardware_init(void);
static void gpio_init(void);
static void dma_init(void);
static void sai_init(void);
static void codec_init(void);
static inline void set_rx_buffer_ready(uint8_t half_index);
static inline void set_tx_buffer_half(uint8_t half_index);

/* ========================================================================= */
/* DRIVER OPERATIONS TABLE                                                   */
/* ========================================================================= */

/** Driver operations structure for abstraction layer */
static struct i2s_drv_ops driver_ops = {
    .init = i2s_stm32f7_init,
    .getRxBuffer = i2s_stm32f7_getRxBuffer,
    .getTxBuffer = i2s_stm32f7_getTxBuffer,
    .rxBufferRdy = i2s_stm32f7_rxBufferRdy,
    .xferCnt = i2s_stm32f7_rx_xferCnt,
    .setRxCompleteCb = i2s_stm32f7_setRxCompleteCb,
    .startStream = i2s_stm32f7_startStream,
    .pauseStream = i2s_stm32f7_pauseStream,
    .resumeStream = i2s_stm32f7_resumeStream,
};

/* ========================================================================= */
/* PUBLIC API IMPLEMENTATION                                                 */
/* ========================================================================= */

/**
 * @brief Initialize I2S hardware and driver state
 *
 * Configures:
 * - GPIO pins for SAI peripheral
 * - DMA controllers for zero-copy transfers
 * - SAI blocks in master/slave configuration
 * - WM8994 codec for line input and headphone output
 */
void i2s_stm32f7_set_input_device(int use_dmic)
{
    g_use_dmic = use_dmic;
}

void i2s_stm32f7_set_sample_rate(unsigned int rate)
{
    if (rate > 0)
        g_sample_rate = rate;
}

void i2s_stm32f7_init(void)
{
    if (g_driver_state.initialized) {
        return; /* Already initialized */
    }
    
    /* Save callbacks before clearing state - may have been set before init */
    i2s_driver_rxCompleteCb saved_rx_callback = g_driver_state.rx_complete_callback;
    i2s_driver_rxCompleteCb saved_tx_callback = g_driver_state.tx_complete_callback;
    
    /* Clear state structure */
    memset(&g_driver_state, 0, sizeof(g_driver_state));

    /* Restore callbacks */
    g_driver_state.rx_complete_callback = saved_rx_callback;
    g_driver_state.tx_complete_callback = saved_tx_callback;

    /* Set buffer pointers to DTCM (non-cacheable) memory */
    g_driver_state.rx_buffer = g_rx_buffer;
    g_driver_state.tx_buffer = g_tx_buffer;

    /* Initialize hardware subsystems in dependency order */
    hardware_init();
    
    g_driver_state.initialized = true;
}

/**
 * @brief Get pointer to current completed RX buffer half
 * @return Pointer to RX buffer half that was just filled by DMA
 *
 * Returns the buffer half indicated by the most recent DMA half/complete
 * interrupt.
 */
unsigned long i2s_stm32f7_getRxBuffer(void)
{
    uint16_t *buffer_ptr;
    
    /* Read volatile half index once to ensure consistency */
    uint8_t half = g_driver_state.rx_completed_buffer_half;
    
    if (half == 0) {
        /* First half was just completed, return pointer to it */
        buffer_ptr = &g_driver_state.rx_buffer[0];
    } else {
        /* Second half was just completed */
        buffer_ptr = &g_driver_state.rx_buffer[HALF_BUFFER_SIZE];
    }
    
    return (unsigned long)buffer_ptr;
}

/**
 * @brief Get pointer to TX buffer half that is safe to write
 * @return Pointer to TX buffer half NOT currently being read by DMA
 *
 * Returns the TX buffer half that DMA is not currently transmitting from.
 * Uses tx_completed_buffer_half which indicates which half just completed:
 * - If tx_completed_buffer_half = 0 (first half done), write to first half
 * - If tx_completed_buffer_half = 1 (second half done), write to second half
 */
unsigned long i2s_stm32f7_getTxBuffer(void)
{
    uint16_t *buffer_ptr;

    uint8_t half = g_driver_state.tx_completed_buffer_half;

    if (half == 0) {
        /* First half just transmitted, DMA now on second half, write to first */
        buffer_ptr = &g_driver_state.tx_buffer[0];
    } else {
        /* Second half just transmitted, DMA now on first half, write to second */
        buffer_ptr = &g_driver_state.tx_buffer[HALF_BUFFER_SIZE];
    }

    return (unsigned long)buffer_ptr;
}

/**
 * @brief Get which RX buffer half just completed
 * @return 0 for first half, 1 for second half
 * 
 * Returns the half-buffer index that was most recently completed by DMA.
 * Used by audio task to determine which buffer is safe to process.
 * Value is set atomically in DMA ISR.
 */
uint8_t i2s_stm32f7_getRxCompletedBufferHalf(void)
{
    return g_driver_state.rx_completed_buffer_half;
}

uint8_t i2s_stm32f7_getTxCompletedBufferHalf(void)
{
    return g_driver_state.tx_completed_buffer_half;
}

/**
 * @brief Get pointer to first half of RX buffer
 * @return Pointer to start of first half
 */
unsigned long i2s_stm32f7_getRxBufferFirstHalf(void)
{
    return (unsigned long)(&g_driver_state.rx_buffer[0]);
}

/**
 * @brief Get pointer to second half of RX buffer
 * @return Pointer to start of second half
 */
unsigned long i2s_stm32f7_getRxBufferSecondHalf(void)
{
    return (unsigned long)(&g_driver_state.rx_buffer[HALF_BUFFER_SIZE]);
}

/**
 * @brief Get pointer to first half of TX buffer
 * @return Pointer to start of first half
 */
unsigned long i2s_stm32f7_getTxBufferFirstHalf(void)
{
    return (unsigned long)(&g_driver_state.tx_buffer[0]);
}

/**
 * @brief Get pointer to second half of TX buffer
 * @return Pointer to start of second half
 */
unsigned long i2s_stm32f7_getTxBufferSecondHalf(void)
{
    return (unsigned long)(&g_driver_state.tx_buffer[HALF_BUFFER_SIZE]);
}

/**
 * @brief Check if RX buffer is ready for processing
 * @return 1 if new RX data available, 0 otherwise
 *
 * This is a consume-once flag - returns 1 only once per DMA interrupt
 * then clears the ready flag. Thread-safe for single consumer.
 */
int i2s_stm32f7_rxBufferRdy(void)
{
    if (g_driver_state.rx_buffer_ready) {
        g_driver_state.rx_buffer_ready = false;
        return 1;
    }
    return 0;
}

/**
 * @brief Get DMA transfer counter
 * @return Number of data items remaining in current DMA transfer
 *
 * Useful for debugging and timing analysis. Decrements as DMA progresses.
 */
unsigned int i2s_stm32f7_rx_xferCnt(void)
{
    return g_driver_state.dma_rx.Instance->NDTR;
}

unsigned int i2s_stm32f7_tx_xferCnt(void)
{
	return g_driver_state.dma_tx.Instance->NDTR;
}
/**
 * @brief Set callback for RX completion events
 * @param cb Function to call from ISR when RX buffer is ready
 *
 * Callback is invoked from interrupt context - must be ISR-safe.
 * Typically used to resume a FreeRTOS task for audio processing.
 */
void i2s_stm32f7_setRxCompleteCb(i2s_driver_rxCompleteCb cb)
{
    g_driver_state.rx_complete_callback = cb;
}

/**
 * @brief Set callback for TX completion events
 * @param cb Function to call from ISR when TX buffer needs refilling
 *
 * Callback is invoked from interrupt context - must be ISR-safe.
 * Used in test modes where audio generation doesn't depend on RX input.
 */
void i2s_stm32f7_setTxCompleteCb(i2s_driver_rxCompleteCb cb)
{
    g_driver_state.tx_complete_callback = cb;
}

/**
 * @brief Start audio streaming
 *
 * Begins DMA transfers for both RX and TX. TX must start first (or simultaneously)
 * because RX is a synchronous slave that depends on TX master for clocks.
 */
void i2s_stm32f7_startStream(void)
{
    if (g_driver_state.rx_streaming) {
        return; /* Already streaming */
    }

    /* Pre-fill TX buffer with silence to avoid startup glitch */
    memset(g_driver_state.tx_buffer, 0, FULL_BUFFER_SIZE * sizeof(uint16_t));
    memset(g_driver_state.rx_buffer, 0, FULL_BUFFER_SIZE * sizeof(uint16_t));

    /* Initialize buffer half indicators to ensure proper initial state */
    g_driver_state.rx_completed_buffer_half = 0;
    g_driver_state.tx_completed_buffer_half = 0;

    /* Start TX DMA first - master must generate clocks for slave RX */
    HAL_SAI_Transmit_DMA(&g_driver_state.sai_tx,
                         (uint8_t *)g_driver_state.tx_buffer,
                         FULL_BUFFER_SIZE);
    g_driver_state.tx_streaming = true;

    /* Start RX DMA - slave synchronized to TX master */
    HAL_SAI_Receive_DMA(&g_driver_state.sai_rx,
                        (uint8_t *)g_driver_state.rx_buffer,
                        FULL_BUFFER_SIZE);
    g_driver_state.rx_streaming = true;
}

void i2s_stm32f7_stopStream(void)
{
    g_driver_state.rx_streaming = false;
    HAL_SAI_DMAStop(&g_driver_state.sai_rx);
    HAL_SAI_DMAStop(&g_driver_state.sai_tx);
}

/**
 * @brief Pause audio streaming
 *
 * Pauses DMA transfers without stopping them. Can be resumed without
 * reconfiguration. Useful for temporary suspension.
 */
void i2s_stm32f7_pauseStream(void)
{
    HAL_SAI_DMAPause(&g_driver_state.sai_rx);
    HAL_SAI_DMAPause(&g_driver_state.sai_tx);
}

/**
 * @brief Resume audio streaming after pause
 */
void i2s_stm32f7_resumeStream(void)
{
    HAL_SAI_DMAResume(&g_driver_state.sai_tx);    
    HAL_SAI_DMAResume(&g_driver_state.sai_rx);
}

/**
 * @brief Get driver operations table for abstraction layer
 * @return Pointer to driver operations structure
 */
struct i2s_drv_ops* i2s_stm32f7_get(void)
{
    return &driver_ops;
}

/**
 * @brief Get debug counters for DMA interrupt monitoring
 * @param half_count Pointer to store RX half-complete count
 * @param full_count Pointer to store RX full-complete count
 */
void i2s_stm32f7_getRxDebugCounters(uint32_t *half_count, uint32_t *full_count)
{
    if (half_count != NULL) {
        *half_count = g_driver_state.rx_half_count;
    }
    if (full_count != NULL) {
        *full_count = g_driver_state.rx_full_count;
    }
}

void i2s_stm32f7_getTxDebugCounters(uint32_t *half_count, uint32_t *full_count)
{
    if (half_count != NULL) {
        *half_count = g_driver_state.tx_half_count;
    }
    if (full_count != NULL) {
        *full_count = g_driver_state.tx_full_count;
    }
}

uint32_t i2s_stm32f7_getRxCallbackCount(void)
{
    return g_driver_state.rx_callback_invoked;
}

uint32_t i2s_stm32f7_getTxCallbackCount(void)
{
    return g_driver_state.tx_callback_invoked;
}

/**
 * @brief Get phase alignment statistics
 * @return Number of times RX/TX were out of phase
 */
uint32_t i2s_stm32f7_getPhaseAlignment(void)
{
    return g_driver_state.phase_misalign;
}

/* ========================================================================= */
/* HAL INTERRUPT CALLBACKS                                                   */
/* ========================================================================= */

/**
 * @brief SAI RX half-transfer complete callback
 * @param hsai SAI handle (unused, we use global state)
 *
 * Called by HAL when first half of RX buffer is filled.
 * Minimal processing - just set flag and invoke user callback.
 */
void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai; /* Unused parameter */

    g_driver_state.rx_half_count++;
    
    set_rx_buffer_ready(0); /* First half (index 0) is ready */
    
    /* Check RX/TX phase alignment - TX should also be at half-complete */
    /* When RX half-complete fires, TX should have just finished transmitting first half */
    /* We expect TX to be at the same phase as RX */
    uint8_t expected_tx_half = g_driver_state.rx_completed_buffer_half;
    if (g_driver_state.tx_completed_buffer_half != expected_tx_half) {
        g_driver_state.phase_misalign++;
    }
    
    if (g_driver_state.rx_complete_callback != NULL) {
        g_driver_state.rx_callback_invoked++;
        g_driver_state.rx_complete_callback();
    }
}

/**
 * @brief SAI RX full-transfer complete callback
 * @param hsai SAI handle (unused, we use global state)
 *
 * Called by HAL when second half of RX buffer is filled.
 */
void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    g_driver_state.rx_full_count++;
    
    (void)hsai; /* Unused parameter */
    
    set_rx_buffer_ready(1); /* Second half (index 1) is ready */
    
    /* Check RX/TX phase alignment */
    uint8_t expected_tx_half = g_driver_state.rx_completed_buffer_half;
    if (g_driver_state.tx_completed_buffer_half != expected_tx_half) {
        g_driver_state.phase_misalign++;
    }
    
    if (g_driver_state.rx_complete_callback != NULL) {
        g_driver_state.rx_callback_invoked++;
        g_driver_state.rx_complete_callback();
    }
}

/**
 * @brief SAI TX half-transfer complete callback
 * @param hsai SAI handle (unused)
 *
 * Called when first half of TX buffer has been transmitted.
 * Track TX buffer state for phase alignment verification.
 */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai; /* Unused parameter */
    
    g_driver_state.tx_half_count++;
    
    /* Update TX buffer half - first half (index 0) was just transmitted */
    set_tx_buffer_half(0); /* Now transmitting second half */
	//serial_safe_write("set_tx_buffer_half(1)", 22);
    
    /* Invoke TX callback if registered (for output-only test modes) */
    if (g_driver_state.tx_complete_callback != NULL) {
        g_driver_state.tx_callback_invoked++;
        g_driver_state.tx_complete_callback();
    }
}

/**
 * @brief SAI TX full-transfer complete callback
 * @param hsai SAI handle (unused)
 *
 * Called when second half of TX buffer has been transmitted.
 * Track TX buffer state for phase alignment verification.
 */
void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai; /* Unused parameter */
    
    g_driver_state.tx_full_count++;
    
    /* Update TX buffer half - second half (index 1) was just transmitted */
    set_tx_buffer_half(1); /* Now transmitting first half (wrapped) */
	//serial_safe_write("set_tx_buffer_half(0)", 22);
    
    /* Invoke TX callback if registered (for output-only test modes) */
    if (g_driver_state.tx_complete_callback != NULL) {
        g_driver_state.tx_callback_invoked++;
        g_driver_state.tx_complete_callback();
    }
}

/* ========================================================================= */
/* DMA INTERRUPT HANDLERS                                                    */
/* ========================================================================= */

/**
 * @brief TX DMA interrupt handler
 *
 * Installed in NVIC, forwards to HAL DMA handler which calls SAI callbacks.
 */
void AUDIO_OUT_SAIx_DMAx_IRQHandler(void)
{
    HAL_DMA_IRQHandler(g_driver_state.sai_tx.hdmatx);
}

/**
 * @brief RX DMA interrupt handler
 *
 * Installed in NVIC, forwards to HAL DMA handler which calls SAI callbacks.
 */
void AUDIO_IN_SAIx_DMAx_IRQHandler(void)
{
    HAL_DMA_IRQHandler(g_driver_state.sai_rx.hdmarx);
}

/* ========================================================================= */
/* PRIVATE HELPER FUNCTIONS                                                  */
/* ========================================================================= */

/**
 * @brief Set RX buffer ready flag with half-buffer index
 * @param half_index Which half was completed (0 or 1)
 *
 * Inline helper to update volatile state atomically from ISR.
 */
static inline void set_rx_buffer_ready(uint8_t half_index)
{
    g_driver_state.rx_completed_buffer_half = half_index;
    g_driver_state.rx_buffer_ready = true;
}

/**
 * @brief Set TX buffer half index atomically
 * @param half_index Which half DMA is now transmitting from (0 or 1)
 *
 * Inline helper to update volatile state atomically from ISR.
 * Unlike RX, TX doesn't need a "ready" flag since it's producer-driven
 * (audio task writes whenever ready, not ISR-triggered).
 */
static inline void set_tx_buffer_half(uint8_t half_index)
{
    g_driver_state.tx_completed_buffer_half = half_index;
}

/* ========================================================================= */
/* HARDWARE INITIALIZATION                                                   */
/* ========================================================================= */

/**
 * @brief Configure PLL for audio clock generation
 *
 * Clock tree for 44.1 kHz audio:
 * PLLI2S_VCO = HSE (25 MHz) * PLLI2SN = 25 * 429 = 10725 MHz
 * I2S_CLK = PLLI2S_VCO / PLLI2SQ = 10725 / 2 = 5362.5 MHz
 * SAI_CLK = I2S_CLK / PLLI2SDIVQ = 5362.5 / 19 ≈ 282.24 MHz
 * 
 * This generates precise 44.1 kHz sample rate with minimal jitter.
 */
static void clock_init(void)
{
    RCC_PeriphCLKInitTypeDef rcc_clk_cfg = {0};
    
    HAL_RCCEx_GetPeriphCLKConfig(&rcc_clk_cfg);
    
    rcc_clk_cfg.PeriphClockSelection = RCC_PERIPHCLK_SAI2;
    rcc_clk_cfg.Sai2ClockSelection = RCC_SAI2CLKSOURCE_PLLI2S;
    rcc_clk_cfg.PLLI2S.PLLI2SN = 429;
    rcc_clk_cfg.PLLI2S.PLLI2SQ = 2;
    rcc_clk_cfg.PLLI2SDivQ = 19;
    
    HAL_RCCEx_PeriphCLKConfig(&rcc_clk_cfg);
}

/**
 * @brief Configure GPIO pins for SAI peripheral
 *
 * SAI2 Block A (Master TX): FS, SCK, MCLK, SD pins
 * SAI2 Block B (Slave RX): SD pin (clock shared with Block A)
 */
static void gpio_init(void)
{
    GPIO_InitTypeDef gpio_cfg = {0};
    
    /* Enable SAI peripheral clock */
    AUDIO_OUT_SAIx_CLK_ENABLE();
    AUDIO_IN_SAIx_CLK_ENABLE();
    
    /* Enable GPIO clocks */
    AUDIO_OUT_SAIx_MCLK_ENABLE();
    AUDIO_OUT_SAIx_SCK_SD_ENABLE();
    AUDIO_OUT_SAIx_FS_ENABLE();
    AUDIO_IN_SAIx_SD_ENABLE();
    AUDIO_IN_INT_GPIO_ENABLE();
    
    /* Configure TX pins: Frame Sync (FS) */
    gpio_cfg.Pin = AUDIO_OUT_SAIx_FS_PIN;
    gpio_cfg.Mode = GPIO_MODE_AF_PP;
    gpio_cfg.Pull = GPIO_NOPULL;
    gpio_cfg.Speed = GPIO_SPEED_HIGH;
    gpio_cfg.Alternate = AUDIO_OUT_SAIx_FS_SD_MCLK_AF;
    HAL_GPIO_Init(AUDIO_OUT_SAIx_FS_GPIO_PORT, &gpio_cfg);
    
    /* Configure TX pins: Serial Clock (SCK) */
    gpio_cfg.Pin = AUDIO_OUT_SAIx_SCK_PIN;
    gpio_cfg.Alternate = AUDIO_OUT_SAIx_SCK_AF;
    HAL_GPIO_Init(AUDIO_OUT_SAIx_SCK_SD_GPIO_PORT, &gpio_cfg);
    
    /* Configure TX pins: Serial Data (SD) */
    gpio_cfg.Pin = AUDIO_OUT_SAIx_SD_PIN;
    gpio_cfg.Alternate = AUDIO_OUT_SAIx_FS_SD_MCLK_AF;
    HAL_GPIO_Init(AUDIO_OUT_SAIx_SCK_SD_GPIO_PORT, &gpio_cfg);
    
    /* Configure TX pins: Master Clock (MCLK) */
    gpio_cfg.Pin = AUDIO_OUT_SAIx_MCLK_PIN;
    gpio_cfg.Alternate = AUDIO_OUT_SAIx_FS_SD_MCLK_AF;
    HAL_GPIO_Init(AUDIO_OUT_SAIx_MCLK_GPIO_PORT, &gpio_cfg);
    
    /* Configure RX pin: Serial Data (SD) */
    gpio_cfg.Pin = AUDIO_IN_SAIx_SD_PIN;
    gpio_cfg.Speed = GPIO_SPEED_FAST;
    gpio_cfg.Alternate = AUDIO_IN_SAIx_SD_AF;
    HAL_GPIO_Init(AUDIO_IN_SAIx_SD_GPIO_PORT, &gpio_cfg);
    
    /* Configure audio codec interrupt pin */
    gpio_cfg.Pin = AUDIO_IN_INT_GPIO_PIN;
    gpio_cfg.Mode = GPIO_MODE_INPUT;
    gpio_cfg.Pull = GPIO_NOPULL;
    gpio_cfg.Speed = GPIO_SPEED_FAST;
    HAL_GPIO_Init(AUDIO_IN_INT_GPIO_PORT, &gpio_cfg);
}

/**
 * @brief Configure DMA controllers for TX and RX
 *
 * TX DMA: Memory-to-peripheral, circular mode with FIFO
 * RX DMA: Peripheral-to-memory, circular mode without FIFO
 * 
 * Both use high priority and generate half-transfer + transfer-complete
 * interrupts for double-buffering.
 */
static void dma_init(void)
{
    /* Enable DMA clocks */
    AUDIO_OUT_SAIx_DMAx_CLK_ENABLE();
    AUDIO_IN_SAIx_DMAx_CLK_ENABLE();
    
    /* === Configure TX DMA === */
    g_driver_state.dma_tx.Instance = AUDIO_OUT_SAIx_DMAx_STREAM;
    g_driver_state.dma_tx.Init.Channel = AUDIO_OUT_SAIx_DMAx_CHANNEL;
    g_driver_state.dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    g_driver_state.dma_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    g_driver_state.dma_tx.Init.MemInc = DMA_MINC_ENABLE;
    g_driver_state.dma_tx.Init.PeriphDataAlignment = AUDIO_OUT_SAIx_DMAx_PERIPH_DATA_SIZE;
    g_driver_state.dma_tx.Init.MemDataAlignment = AUDIO_OUT_SAIx_DMAx_MEM_DATA_SIZE;
    g_driver_state.dma_tx.Init.Mode = DMA_CIRCULAR;
    g_driver_state.dma_tx.Init.Priority = DMA_PRIORITY_HIGH;
    g_driver_state.dma_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    g_driver_state.dma_tx.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
    g_driver_state.dma_tx.Init.MemBurst = DMA_MBURST_SINGLE;
    g_driver_state.dma_tx.Init.PeriphBurst = DMA_PBURST_SINGLE;
    
    __HAL_LINKDMA(&g_driver_state.sai_tx, hdmatx, g_driver_state.dma_tx);
    
    HAL_DMA_DeInit(&g_driver_state.dma_tx);
    HAL_DMA_Init(&g_driver_state.dma_tx);
    
    /* Configure TX DMA interrupt — must be higher priority than PendSV
     * (configKERNEL_INTERRUPT_PRIORITY = 15) so that both TX and RX ISRs
     * complete before PendSV fires a context switch.  Otherwise PendSV
     * preempts the RX ISR, the audio task wakes with only the TX phase
     * updated, skips processing, and audio stops after the first buffer. */
    HAL_NVIC_SetPriority(AUDIO_OUT_SAIx_DMAx_IRQ, 6, 0);
    HAL_NVIC_EnableIRQ(AUDIO_OUT_SAIx_DMAx_IRQ);
    
    /* === Configure RX DMA === */
    g_driver_state.dma_rx.Instance = AUDIO_IN_SAIx_DMAx_STREAM;
    g_driver_state.dma_rx.Init.Channel = AUDIO_IN_SAIx_DMAx_CHANNEL;
    g_driver_state.dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    g_driver_state.dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    g_driver_state.dma_rx.Init.MemInc = DMA_MINC_ENABLE;
    g_driver_state.dma_rx.Init.PeriphDataAlignment = AUDIO_IN_SAIx_DMAx_PERIPH_DATA_SIZE;
    g_driver_state.dma_rx.Init.MemDataAlignment = AUDIO_IN_SAIx_DMAx_MEM_DATA_SIZE;
    g_driver_state.dma_rx.Init.Mode = DMA_CIRCULAR;
    g_driver_state.dma_rx.Init.Priority = DMA_PRIORITY_HIGH;
    g_driver_state.dma_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE; /* Lower latency for RX */
    g_driver_state.dma_rx.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
    g_driver_state.dma_rx.Init.MemBurst = DMA_MBURST_SINGLE;
    g_driver_state.dma_rx.Init.PeriphBurst = DMA_MBURST_SINGLE;
    
    __HAL_LINKDMA(&g_driver_state.sai_rx, hdmarx, g_driver_state.dma_rx);
    
    HAL_DMA_DeInit(&g_driver_state.dma_rx);
    HAL_DMA_Init(&g_driver_state.dma_rx);
    
    /* Configure RX DMA interrupt — same priority as TX so both complete
     * before PendSV (priority 15) fires.  Must be >= configLIBRARY_MAX_
     * SYSCALL_INTERRUPT_PRIORITY (5) since ISRs call FreeRTOS APIs. */
    HAL_NVIC_SetPriority(AUDIO_IN_SAIx_DMAx_IRQ, 6, 0);
    HAL_NVIC_EnableIRQ(AUDIO_IN_SAIx_DMAx_IRQ);

    /* Configure audio interrupt (codec events) */
    HAL_NVIC_SetPriority(AUDIO_IN_INT_IRQ, 6, 0);
    HAL_NVIC_EnableIRQ(AUDIO_IN_INT_IRQ);
}

/**
 * @brief Configure SAI peripheral blocks
 *
 * SAI2 Block A: Master transmitter (audio output)
 *  - Generates MCLK, SCK, FS for codec
 *  - 16-bit data, 44.1 kHz sample rate
 *  - Frame: 64 bits (32 bits per channel, I2S protocol)
 *
 * SAI2 Block B: Slave receiver (audio input)
 *  - Synchronous to Block A (shares clock)
 *  - 16-bit data, matches TX configuration
 */
static void sai_init(void)
{
    /*
     * Two configurations depending on input device:
     *
     * Line-in (default):
     *   Block A = Master TX, Block B = Slave RX
     *   2 slots, 32-bit frame, falling edge clock
     *
     * DMIC (g_use_dmic):
     *   Block A = Master RX, Block B = Slave RX (matches BSP SAIx_In_Init)
     *   4 slots, 64-bit frame, rising edge clock, slots 1+3 active
     */
    uint32_t slot_active;
    uint32_t slot_number;
    uint32_t frame_length;
    uint32_t active_frame_length;
    uint32_t master_mode;
    uint32_t clock_strobing_tx;
    uint32_t fifo_threshold;

    if (g_use_dmic) {
        /*
         * DMIC mode: 4 slots, all active, 64-bit frame.
         * DMIC data arrives in slots 1+3 (Timeslot 1).
         * TX output goes to slots 0+1 (Timeslot 0, headphone DAC).
         * All 4 slots active so both timeslots are transferred via DMA.
         */
        slot_active = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1 |
                      SAI_SLOTACTIVE_2 | SAI_SLOTACTIVE_3;
        slot_number = 4;
        frame_length = 64;
        active_frame_length = 32;
        master_mode = SAI_MODEMASTER_TX;
        clock_strobing_tx = SAI_CLOCKSTROBING_RISINGEDGE;
        fifo_threshold = SAI_FIFOTHRESHOLD_1QF;
    } else {
        slot_active = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1;
        slot_number = 2;
        frame_length = 32;
        active_frame_length = 16;
        master_mode = SAI_MODEMASTER_TX;
        clock_strobing_tx = SAI_CLOCKSTROBING_FALLINGEDGE;
        fifo_threshold = SAI_FIFOTHRESHOLD_FULL;
    }

    /* === Configure SAI Block A (Master) === */
    g_driver_state.sai_tx.Instance = AUDIO_OUT_SAIx;

    __HAL_SAI_DISABLE(&g_driver_state.sai_tx);

    g_driver_state.sai_tx.Init.AudioFrequency = g_sample_rate;
    g_driver_state.sai_tx.Init.AudioMode = master_mode;
    g_driver_state.sai_tx.Init.NoDivider = SAI_MASTERDIVIDER_ENABLED;
    g_driver_state.sai_tx.Init.Protocol = SAI_FREE_PROTOCOL;
    g_driver_state.sai_tx.Init.DataSize = SAI_DATASIZE_16;
    g_driver_state.sai_tx.Init.FirstBit = SAI_FIRSTBIT_MSB;
    g_driver_state.sai_tx.Init.ClockStrobing = clock_strobing_tx;
    g_driver_state.sai_tx.Init.Synchro = SAI_ASYNCHRONOUS;
    g_driver_state.sai_tx.Init.OutputDrive = SAI_OUTPUTDRIVE_ENABLED;
    g_driver_state.sai_tx.Init.FIFOThreshold = fifo_threshold;
    if (!g_use_dmic)
        g_driver_state.sai_tx.Init.MonoStereoMode = SAI_MONOMODE;

    g_driver_state.sai_tx.FrameInit.FrameLength = frame_length;
    g_driver_state.sai_tx.FrameInit.ActiveFrameLength = active_frame_length;
    g_driver_state.sai_tx.FrameInit.FSDefinition = SAI_FS_CHANNEL_IDENTIFICATION;
    g_driver_state.sai_tx.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
    g_driver_state.sai_tx.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;

    g_driver_state.sai_tx.SlotInit.FirstBitOffset = 0;
    g_driver_state.sai_tx.SlotInit.SlotSize = SAI_SLOTSIZE_DATASIZE;
    g_driver_state.sai_tx.SlotInit.SlotNumber = slot_number;
    g_driver_state.sai_tx.SlotInit.SlotActive = slot_active;

    HAL_SAI_Init(&g_driver_state.sai_tx);

    /* === Configure SAI Block B (Slave RX) === */
    g_driver_state.sai_rx.Instance = AUDIO_IN_SAIx;

    __HAL_SAI_DISABLE(&g_driver_state.sai_rx);

    g_driver_state.sai_rx.Init.AudioFrequency = g_sample_rate;
    g_driver_state.sai_rx.Init.AudioMode = SAI_MODESLAVE_RX;
    g_driver_state.sai_rx.Init.NoDivider = SAI_MASTERDIVIDER_ENABLED;
    g_driver_state.sai_rx.Init.Protocol = SAI_FREE_PROTOCOL;
    g_driver_state.sai_rx.Init.DataSize = SAI_DATASIZE_16;
    g_driver_state.sai_rx.Init.FirstBit = SAI_FIRSTBIT_MSB;
    g_driver_state.sai_rx.Init.ClockStrobing = SAI_CLOCKSTROBING_RISINGEDGE;
    g_driver_state.sai_rx.Init.Synchro = SAI_SYNCHRONOUS;
    g_driver_state.sai_rx.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLED;
    g_driver_state.sai_rx.Init.FIFOThreshold = fifo_threshold;
    if (!g_use_dmic)
        g_driver_state.sai_rx.Init.MonoStereoMode = SAI_MONOMODE;

    g_driver_state.sai_rx.FrameInit.FrameLength = frame_length;
    g_driver_state.sai_rx.FrameInit.ActiveFrameLength = active_frame_length;
    g_driver_state.sai_rx.FrameInit.FSDefinition = SAI_FS_CHANNEL_IDENTIFICATION;
    g_driver_state.sai_rx.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
    g_driver_state.sai_rx.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;

    g_driver_state.sai_rx.SlotInit.FirstBitOffset = 0;
    g_driver_state.sai_rx.SlotInit.SlotSize = SAI_SLOTSIZE_DATASIZE;
    g_driver_state.sai_rx.SlotInit.SlotNumber = slot_number;
    g_driver_state.sai_rx.SlotInit.SlotActive = slot_active;

    HAL_SAI_Init(&g_driver_state.sai_rx);

    /* Enable SAI peripherals to generate clocks */
    __HAL_SAI_ENABLE(&g_driver_state.sai_tx);
    __HAL_SAI_ENABLE(&g_driver_state.sai_rx);
}

/**
 * @brief Initialize WM8994 audio codec
 *
 * Configures codec for:
 * - Input: Line In 1 (for guitar/instrument input)
 * - Output: Headphones
 * - Volume: 50%
 * - Sample rate: 44.1 kHz
 */
static void codec_init(void)
{
    uint32_t codec_id;
    
    /* Read codec ID to verify I2C communication */
    codec_id = wm8994_drv.ReadID(OVE_AUDIO_CODEC_I2C_ADDR);
    
    if (codec_id != WM8994_ID) {
        /* Codec not detected - error condition */
        /* In production code, should handle this gracefully */
        return;
    }
    
    /* Reset codec to known state */
    wm8994_drv.Reset(OVE_AUDIO_CODEC_I2C_ADDR);
    
    /* Register codec driver */
    g_driver_state.codec_driver = &wm8994_drv;
    
    /* Select input device based on configuration */
    uint16_t input_dev = g_use_dmic ? INPUT_DEVICE_DIGITAL_MICROPHONE_2
                                    : INPUT_DEVICE_INPUT_LINE_1;

    /* Initialize codec with application-specific configuration */
    if (g_driver_state.codec_driver->Init(
        OVE_AUDIO_CODEC_I2C_ADDR,
        input_dev | OUTPUT_DEVICE_HEADPHONE,
        70,  /* Volume: 0-100% */
        g_sample_rate
    ) != 0) {
		/* Codec initialization failed */
		printf("Codec init failed!\n");
		return;
	}

    /* Common output path overrides — applied for both line-in and DMIC */

    /* Power Management 1: disable speaker output amps */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x01, 0x0313);

    /* Power Management 3: disable speaker mixer amps */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x03, 0x0030);

    /* Speaker Mixer: disconnect DAC2 */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x36, 0x0000);

    /* Output Mixer 1 & 2: route DAC1 to headphone output */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x2D, 0x0100);
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x2E, 0x0100);

    /* HP volume: +1dB */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x1C, 0x01FA);
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x1D, 0x01FA);

    /* ADC digital volume: 0dB */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x400, 0x01C0);
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x401, 0x01C0);

    if (!g_use_dmic) {
        /* Line-in specific: remove feedback and +30dB boost */
        AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x29, 0x0020);
        AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x2A, 0x0020);
    } else {
        /*
         * DMIC mode with 4-slot SAI (all slots active):
         *   RX: DMIC data in slots 1+3 (Timeslot 1) — BSP default
         *   TX: Headphone DAC reads from Timeslot 0 (slots 0+2)
         */

        /* Unmute Timeslot 0 DAC for headphone output */
        AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x420, 0x0000);

        /* AIF1ADC2 digital volume (DMIC path): +6dB boost
         * Register 0x404 = Left, 0x405 = Right
         * Range: 0x01 = -71.625dB, 0xC0 = 0dB, 0xEF = +17.625dB
         * 0xD8 = ~+6dB */
        AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x404, 0x01D8);
        AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x405, 0x01D8);
    }

    /* Enable oversampling for better SNR */
    AUDIO_IO_Write(OVE_AUDIO_CODEC_I2C_ADDR, 0x620, 0x0002);

}

/**
 * @brief Master hardware initialization sequence
 *
 * Initializes all hardware subsystems in correct dependency order:
 * 1. Clock configuration (PLL for audio)
 * 2. GPIO pins (SAI peripheral connections)
 * 3. DMA controllers (data transfer)
 * 4. SAI peripheral (audio interface)
 * 5. Codec chip (WM8994 configuration)
 */
static void hardware_init(void)
{
    clock_init();
    gpio_init();
    dma_init();
    sai_init();
    codec_init();
}
