/******************************************************************************
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************************/

/**
 * @file    main.c
 * @brief   Self-checking test of I2C controller transaction sequences.
 * @details Runs blocking, interrupt-driven (async) and DMA transaction
 *          sequences back to back against the on-board MAX9867 audio codec
 *          (address 0x18, revision register 0xFF reads 0x42), and checks every
 *          return code, every value read, and that the bus is idle afterwards.
 *          A sequence that hangs is caught by the watchdog: the device resets,
 *          reports the case as HANG and continues with the next one.
 *
 *          Build options (project.mk):
 *          - SCOPE_CASE=<1..14>: capture build. Runs one iteration of that case
 *            every 20 ms after the same setup, for a logic analyzer.
 *          - MSDK_LABEL=<text>: printed in the header, to label the MSDK under
 *            test when building against several releases.
 */

/***** Includes *****/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "mxc_device.h"
#include "mxc_delay.h"
#include "board.h"
#include "dma.h"
#include "i2c.h"
#include "nvic_table.h"
#include "pwrseq_regs.h"
#include "uart.h"
#include "wdt.h"

/***** Definitions *****/
#if defined(BOARD_FTHR_APPS_P1)
// The MAX32655FTHR has the MAX9867 on I2C1 (SCL P0.16, SDA P0.17)
#define TEST_I2C MXC_I2C1
#define TEST_I2C_IRQn I2C1_IRQn
#else
// The MAX32655EVKIT has the MAX9867 on I2C2 (SCL P0.30, SDA P0.31)
#define TEST_I2C MXC_I2C2
#define TEST_I2C_IRQn I2C2_IRQn
#endif

#define I2C_FREQ 100000
#define CODEC_ADDR 0x18
#define CODEC_REG 0xFF
#define CODEC_REV 0x42
#define NO_DEVICE_ADDR 0x50 // No device at this address on the codec bus

#define ITERATIONS 1000

/***** Globals *****/
static volatile int xfer_done, xfer_err;
static bool cb_missed; // A completion callback never came: stop the case
static int dma_tx_ch = -1, dma_rx_ch = -1;
static uint32_t bit_cycles; // One I2C bit time in core clocks, measured

static uint8_t reg = CODEC_REG;
static uint8_t val;

// Write the register address, repeated START, read 1 byte, STOP
static mxc_i2c_req_t rd = { .addr = CODEC_ADDR,
                            .tx_buf = &reg,
                            .tx_len = 1,
                            .rx_buf = &val,
                            .rx_len = 1 };
// Write the register address, then a repeated START instead of a STOP
static mxc_i2c_req_t wr_rs = { .addr = CODEC_ADDR, .tx_buf = &reg, .tx_len = 1, .restart = 1 };
// Read 1 byte, STOP (no write phase)
static mxc_i2c_req_t rd_only = { .addr = CODEC_ADDR, .rx_buf = &val, .rx_len = 1 };
// Write the register address only (no data), STOP
static mxc_i2c_req_t wr = { .addr = CODEC_ADDR, .tx_buf = &reg, .tx_len = 1 };
// Transfers to an address with no device
static mxc_i2c_req_t nack_wr = { .addr = NO_DEVICE_ADDR, .tx_buf = &reg, .tx_len = 1 };
static mxc_i2c_req_t nack_rd = { .addr = NO_DEVICE_ADDR, .rx_buf = &val, .rx_len = 1 };

// Copies of the above that complete through xfer_cb(), for async and DMA
static mxc_i2c_req_t c_rd, c_rd_only, c_wr, c_nack_wr, c_nack_rd;

struct tally {
    uint32_t iters, ok, fails, bad_value, stuck, note_count;
    char first_fail[64];
    char note[64];
    uint32_t bits_min, bits_max; // Tenths of a bit time
};

/***** Functions *****/

/* ------------------------------------------------------------------------- */
/* Timing: SysTick as a free-running 24-bit down-counter at the core clock   */

#define CYC_MASK 0xFFFFFFU

static void cyc_init(void)
{
    SysTick->CTRL = 0;
    SysTick->LOAD = CYC_MASK;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

static inline uint32_t cyc(void)
{
    return CYC_MASK - SysTick->VAL;
}

static inline uint32_t cyc_since(uint32_t t0)
{
    return (cyc() - t0) & CYC_MASK;
}

static void console_flush(void)
{
    fflush(stdout);
    while (MXC_UART_ReadyForSleep(MXC_UART_GET_UART(CONSOLE_UART)) != E_NO_ERROR) {}
}

static const char *err_name(int err)
{
    switch (err) {
    case E_NO_ERROR:
        return "E_NO_ERROR";
    case E_BUSY:
        return "E_BUSY";
    case E_COMM_ERR:
        return "E_COMM_ERR";
    case E_TIME_OUT:
        return "E_TIME_OUT";
    default:
        return "other";
    }
}

/* ------------------------------------------------------------------------- */
/* I2C, async and DMA setup                                                  */

void TEST_I2C_IRQHandler(void)
{
    MXC_I2C_AsyncHandler(TEST_I2C);
}

void DMA_IRQHandler(void)
{
    MXC_DMA_Handler();
}

static void xfer_cb(mxc_i2c_req_t *req, int error)
{
    (void)req;
    xfer_err = error;
    xfer_done = 1;
}

// Wait up to 20 ms for the async/DMA completion callback: its result, or E_TIME_OUT
static int wait_cb(void)
{
    uint32_t t0 = cyc(), lim = SystemCoreClock / 50;

    while (!xfer_done) {
        if (cyc_since(t0) > lim) {
            cb_missed = true;
            return E_TIME_OUT;
        }
    }
    return xfer_err;
}

// Bounded wait (about 1 ms) for the bus to go idle
static bool wait_idle(void)
{
    uint32_t t0 = cyc(), lim = SystemCoreClock / 1000;

    while (TEST_I2C->status & MXC_F_I2C_STATUS_BUSY) {
        if (cyc_since(t0) > lim) {
            return false;
        }
    }
    return true;
}

/*
 * Fresh controller state for every case, so a case that leaves the bus held
 * cannot affect the next one, and the first iteration starts on an idle bus.
 */
static void i2c_reinit(void)
{
    // MXC_I2C_Init() forgets the DMA channels without releasing them
    if (dma_tx_ch >= 0) {
        MXC_DMA_ReleaseChannel(dma_tx_ch);
    }
    if (dma_rx_ch >= 0) {
        MXC_DMA_ReleaseChannel(dma_rx_ch);
    }
    dma_tx_ch = dma_rx_ch = -1;

    NVIC_DisableIRQ(TEST_I2C_IRQn);
    if (MXC_I2C_Init(TEST_I2C, 1, 0) != E_NO_ERROR) {
        printf("  ! MXC_I2C_Init failed\n");
    }
    MXC_I2C_SetFrequency(TEST_I2C, I2C_FREQ);
    MXC_NVIC_SetVector(TEST_I2C_IRQn, TEST_I2C_IRQHandler);
    NVIC_EnableIRQ(TEST_I2C_IRQn);

    // Bus recovery ends with a STOP, which may still be on the bus here
    if (!wait_idle()) {
        printf("  ! bus still busy after init\n");
    }
}

/*
 * MSDK releases before v2023_10 have no MXC_I2C_DMA_Init() or
 * MXC_I2C_DMA_Get*Channel(): their DMA transactions acquire and release
 * channels per transfer, so route every DMA channel's interrupt instead.
 */
extern int MXC_I2C_DMA_Init(mxc_i2c_regs_t *i2c, mxc_dma_regs_t *dma, bool use_dma_tx,
                            bool use_dma_rx) __attribute__((weak));
extern int MXC_I2C_DMA_GetTXChannel(mxc_i2c_regs_t *i2c) __attribute__((weak));
extern int MXC_I2C_DMA_GetRXChannel(mxc_i2c_regs_t *i2c) __attribute__((weak));

static bool dma_setup(void)
{
    if (!MXC_I2C_DMA_Init || !MXC_I2C_DMA_GetTXChannel || !MXC_I2C_DMA_GetRXChannel) {
        for (int ch = 0; ch < MXC_DMA_CHANNELS; ch++) {
            MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(ch), DMA_IRQHandler);
            NVIC_EnableIRQ(MXC_DMA_CH_GET_IRQ(ch));
        }
        return true;
    }

    if (MXC_I2C_DMA_Init(TEST_I2C, MXC_DMA, true, true) != E_NO_ERROR) {
        printf("  ! MXC_I2C_DMA_Init failed\n");
        return false;
    }

    dma_tx_ch = MXC_I2C_DMA_GetTXChannel(TEST_I2C);
    dma_rx_ch = MXC_I2C_DMA_GetRXChannel(TEST_I2C);
    if (dma_tx_ch < 0 || dma_rx_ch < 0) {
        printf("  ! no DMA channels (tx %d, rx %d)\n", dma_tx_ch, dma_rx_ch);
        return false;
    }

    MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(dma_tx_ch), DMA_IRQHandler);
    MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(dma_rx_ch), DMA_IRQHandler);
    NVIC_EnableIRQ(MXC_DMA_CH_GET_IRQ(dma_tx_ch));
    NVIC_EnableIRQ(MXC_DMA_CH_GET_IRQ(dma_rx_ch));
    return true;
}

static void requests_init(void)
{
    mxc_i2c_req_t *all[] = { &rd, &wr_rs, &rd_only, &wr, &nack_wr, &nack_rd };

    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        all[i]->i2c = TEST_I2C;
    }

    c_rd = rd;
    c_rd_only = rd_only;
    c_wr = wr;
    c_nack_wr = nack_wr;
    c_nack_rd = nack_rd;
    c_rd.callback = xfer_cb;
    c_rd_only.callback = xfer_cb;
    c_wr.callback = xfer_cb;
    c_nack_wr.callback = xfer_cb;
    c_nack_rd.callback = xfer_cb;
}

static int start_async(mxc_i2c_req_t *req)
{
    xfer_done = 0;
    return MXC_I2C_MasterTransactionAsync(req);
}

static int start_dma(mxc_i2c_req_t *req)
{
    xfer_done = 0;
    return MXC_I2C_MasterTransactionDMA(req);
}

/* ------------------------------------------------------------------------- */
/* Result bookkeeping                                                        */

static void tally_fail(struct tally *t, uint32_t iter, const char *step, int ret)
{
    if (!t->fails) {
        snprintf(t->first_fail, sizeof(t->first_fail), "#%lu %s: %s (%d)", (unsigned long)iter,
                 step, err_name(ret), ret);
    }
    t->fails++;
}

// Expect `ret == want`; returns true if so
static bool expect(struct tally *t, uint32_t iter, const char *step, int ret, int want)
{
    if (ret == want) {
        return true;
    }
    tally_fail(t, iter, step, ret);
    return false;
}

static bool expect_val(struct tally *t)
{
    bool good = (val == CODEC_REV);

    if (!good) {
        t->bad_value++;
    }
    val = 0;
    return good;
}

static void tally_bits(struct tally *t, uint32_t cycles)
{
    uint32_t tenths = bit_cycles ? (cycles * 10U + bit_cycles / 2) / bit_cycles : 0;

    if (tenths < t->bits_min) {
        t->bits_min = tenths;
    }
    if (tenths > t->bits_max) {
        t->bits_max = tenths;
    }
}

// End of an iteration: the bus must be idle again
static void end_iter(struct tally *t, bool good)
{
    if (!wait_idle()) {
        t->stuck++;
        good = false;
    }
    if (good) {
        t->ok++;
    }
}

/* ------------------------------------------------------------------------- */
/* Cases                                                                     */

// Blocking: two write+Sr+read+STOP reads back to back
static void case_blk_back_to_back(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "read 1", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) &&
                expect_val(t);

    good = expect(t, i, "read 2", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) && expect_val(t) &&
           good;
    end_iter(t, good);
}

// Blocking: write(restart=1), then a read-only call. Times the read call.
static void case_blk_restart_read(struct tally *t, uint32_t i)
{
    uint32_t t0;
    int ret;
    bool good = expect(t, i, "write(restart)", MXC_I2C_MasterTransaction(&wr_rs), E_NO_ERROR);

    t0 = cyc();
    ret = MXC_I2C_MasterTransaction(&rd_only);
    tally_bits(t, cyc_since(t0));
    good = expect(t, i, "read", ret, E_NO_ERROR) && expect_val(t) && good;
    end_iter(t, good);
}

// Blocking: write to a missing device (NACK), then a read
static void case_blk_nack_read(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "NACK write", MXC_I2C_MasterTransaction(&nack_wr), E_COMM_ERR);

    good = expect(t, i, "read", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) && expect_val(t) &&
           good;
    end_iter(t, good);
}

// Blocking: write(restart=1), then a write
static void case_blk_restart_write(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "write(restart)", MXC_I2C_MasterTransaction(&wr_rs), E_NO_ERROR);

    good = expect(t, i, "write", MXC_I2C_MasterTransaction(&wr), E_NO_ERROR) && good;
    end_iter(t, good);
}

// Blocking: write(restart=1), then write+Sr+read
static void case_blk_restart_write_read(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "write(restart)", MXC_I2C_MasterTransaction(&wr_rs), E_NO_ERROR);

    good = expect(t, i, "write+read", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) &&
           expect_val(t) && good;
    end_iter(t, good);
}

// Async read, then a blocking read as soon as it completes
static void case_async_then_blk(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "async start", start_async(&c_rd), E_NO_ERROR);

    good = expect(t, i, "async done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    good = expect(t, i, "blocking read", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) &&
           expect_val(t) && good;
    end_iter(t, good);
}

// Async read, then another async read
static void case_async_then_async(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "async 1 start", start_async(&c_rd), E_NO_ERROR);

    good = expect(t, i, "async 1 done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    good = expect(t, i, "async 2 start", start_async(&c_rd), E_NO_ERROR) && good;
    good = expect(t, i, "async 2 done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    end_iter(t, good);
}

/*
 * Async write to a missing device, then a blocking read. Only the blocking
 * read is checked: the async write is reported as a note if it does not
 * complete with E_COMM_ERR.
 */
static void case_async_nack_then_blk(struct tally *t, uint32_t i)
{
    int ret;
    bool good = expect(t, i, "async start", start_async(&c_nack_wr), E_NO_ERROR);

    ret = wait_cb();
    if (ret == E_TIME_OUT) {
        tally_fail(t, i, "async done", ret);
        good = false;
    } else if (ret != E_COMM_ERR) {
        if (!t->note_count) {
            snprintf(t->note, sizeof(t->note), "async NACK write completed with %s",
                     err_name(ret));
        }
        t->note_count++;
    }
    good = expect(t, i, "blocking read", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) &&
           expect_val(t) && good;
    end_iter(t, good);
}

// Blocking write(restart=1), then an async read
static void case_blk_restart_async_read(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "write(restart)", MXC_I2C_MasterTransaction(&wr_rs), E_NO_ERROR);

    good = expect(t, i, "async start", start_async(&c_rd_only), E_NO_ERROR) && good;
    good = expect(t, i, "async done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    end_iter(t, good);
}

// Blocking write(restart=1), then an async write
static void case_blk_restart_async_write(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "write(restart)", MXC_I2C_MasterTransaction(&wr_rs), E_NO_ERROR);

    good = expect(t, i, "async start", start_async(&c_wr), E_NO_ERROR) && good;
    good = expect(t, i, "async done", wait_cb(), E_NO_ERROR) && good;
    end_iter(t, good);
}

// DMA read, then a blocking read as soon as it completes
static void case_dma_then_blk(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "DMA start", start_dma(&c_rd), E_NO_ERROR);

    good = expect(t, i, "DMA done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    good = expect(t, i, "blocking read", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) &&
           expect_val(t) && good;
    end_iter(t, good);
}

// DMA read, then another DMA read
static void case_dma_then_dma(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "DMA 1 start", start_dma(&c_rd), E_NO_ERROR);

    good = expect(t, i, "DMA 1 done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    good = expect(t, i, "DMA 2 start", start_dma(&c_rd), E_NO_ERROR) && good;
    good = expect(t, i, "DMA 2 done", wait_cb(), E_NO_ERROR) && expect_val(t) && good;
    end_iter(t, good);
}

// DMA read from a missing device (NACK), then a blocking read
static void case_dma_nack_then_blk(struct tally *t, uint32_t i)
{
    bool good = expect(t, i, "DMA NACK read", start_dma(&c_nack_rd), E_COMM_ERR);

    val = 0;
    good = expect(t, i, "blocking read", MXC_I2C_MasterTransaction(&rd), E_NO_ERROR) &&
           expect_val(t) && good;
    end_iter(t, good);
}

// Blocking write(restart=1), then a DMA read. Times the DMA read.
static void case_blk_restart_dma_read(struct tally *t, uint32_t i)
{
    uint32_t t0;
    int ret;
    bool good = expect(t, i, "write(restart)", MXC_I2C_MasterTransaction(&wr_rs), E_NO_ERROR);

    t0 = cyc();
    ret = start_dma(&c_rd_only);
    good = expect(t, i, "DMA start", ret, E_NO_ERROR) && good;
    ret = wait_cb();
    tally_bits(t, cyc_since(t0));
    good = expect(t, i, "DMA done", ret, E_NO_ERROR) && expect_val(t) && good;
    end_iter(t, good);
}

struct test_case {
    const char *name;
    void (*fn)(struct tally *, uint32_t);
    bool dma;
    bool timed; // Print the timed call's length in bit times
};

static const struct test_case cases[] = {
    { "blocking read, blocking read", case_blk_back_to_back, false, false },
    { "blocking restart, blocking read", case_blk_restart_read, false, true },
    { "blocking NACK, blocking read", case_blk_nack_read, false, false },
    { "blocking restart, blocking write", case_blk_restart_write, false, false },
    { "blocking restart, blocking write+read", case_blk_restart_write_read, false, false },
    { "async read, blocking read", case_async_then_blk, false, false },
    { "async read, async read", case_async_then_async, false, false },
    { "async NACK, blocking read", case_async_nack_then_blk, false, false },
    { "blocking restart, async read", case_blk_restart_async_read, false, false },
    { "blocking restart, async write", case_blk_restart_async_write, false, false },
    { "DMA read, blocking read", case_dma_then_blk, true, false },
    { "DMA read, DMA read", case_dma_then_dma, true, false },
    { "DMA NACK, blocking read", case_dma_nack_then_blk, true, false },
    { "blocking restart, DMA read", case_blk_restart_dma_read, true, true },
};

#define NUM_CASES (sizeof(cases) / sizeof(cases[0]))

#ifndef SCOPE_CASE
/* ------------------------------------------------------------------------- */
/* Watchdog-backed sequencing (the full test; not used by the capture build) */

/*
 * PWRSEQ GP0 holds the running case and iteration, tagged with an ID unique
 * to this build, and GP1 the results (2 bits per case). Both survive the
 * watchdog reset.
 */
#define RES_PASS 1U
#define RES_FAIL 2U
#define RES_HANG 3U

static uint32_t build_id(void)
{
    const char *s = __DATE__ __TIME__;
    uint32_t h = 2166136261U;

    while (*s) {
        h = (h ^ (uint8_t)*s++) * 16777619U;
    }
    return (h & 0xFFFF0000U) | 0x8000U;
}

static void set_result(unsigned idx, uint32_t res)
{
    MXC_PWRSEQ->gp1 = (MXC_PWRSEQ->gp1 & ~(3U << (2 * idx))) | (res << (2 * idx));
}

static uint32_t get_result(unsigned idx)
{
    return (MXC_PWRSEQ->gp1 >> (2 * idx)) & 3U;
}

static void wdt_start(void)
{
    mxc_wdt_cfg_t cfg = { .mode = MXC_WDT_COMPATIBILITY, .upperResetPeriod = MXC_WDT_PERIOD_2_26 };

    MXC_WDT_Disable(MXC_WDT0);
    MXC_WDT_Init(MXC_WDT0, &cfg);
    MXC_WDT_SetResetPeriod(MXC_WDT0, &cfg);
    MXC_WDT_ResetTimer(MXC_WDT0);
    MXC_WDT_EnableReset(MXC_WDT0);
    MXC_WDT_Enable(MXC_WDT0);
}

// One I2C bit time, from a plain write+Sr+read+STOP read on an idle bus
static void calibrate(void)
{
    uint32_t best = UINT32_MAX;

    for (int k = 0; k < 8; k++) {
        uint32_t t0, d;

        wait_idle();
        t0 = cyc();
        if (MXC_I2C_MasterTransaction(&rd) != E_NO_ERROR) {
            continue;
        }
        d = cyc_since(t0);
        if (d < best) {
            best = d;
        }
    }
    // START, 4 x (8 bits + ACK/NACK), Sr, STOP: about 38.5 bit times
    bit_cycles = (best == UINT32_MAX) ? 0 : (best * 2U) / 77U;
}

static void run_case(unsigned idx)
{
    const struct test_case *c = &cases[idx];
    struct tally t = { .iters = ITERATIONS, .bits_min = UINT32_MAX };
    uint32_t res, ran = 0;

    i2c_reinit();
    if (c->dma && !dma_setup()) {
        set_result(idx, RES_FAIL);
        printf("  FAIL  (DMA setup)\n");
        return;
    }
    if (c->timed) {
        calibrate();
    }
    val = 0;

    // Start the first iteration on an idle bus, whatever ran before
    if (!wait_idle()) {
        printf("  ! bus still busy before the first iteration\n");
    }

    cb_missed = false;
    for (uint32_t i = 0; i < t.iters && !cb_missed; i++) {
        MXC_WDT_ResetTimer(MXC_WDT0);
        MXC_PWRSEQ->gp0 = build_id() | ((i & 0x7FFU) << 4) | idx;
        c->fn(&t, i);
        ran = i + 1;
    }

    res = (t.ok == t.iters) ? RES_PASS : RES_FAIL;
    set_result(idx, res);

    printf("  %s  %lu/%lu ok", res == RES_PASS ? "PASS" : "FAIL", (unsigned long)t.ok,
           (unsigned long)t.iters);
    if (t.fails) {
        printf(", %lu errors (first %s)", (unsigned long)t.fails, t.first_fail);
    }
    if (t.bad_value) {
        printf(", %lu wrong values", (unsigned long)t.bad_value);
    }
    if (t.stuck) {
        printf(", bus left busy %lu times", (unsigned long)t.stuck);
    }
    if (cb_missed) {
        printf(", stopped at #%lu: completion callback never came", (unsigned long)(ran - 1));
    }
    printf("\n");
    if (c->timed && t.bits_min != UINT32_MAX) {
        printf("        timed call: %lu.%lu..%lu.%lu bit times (one read is about 20;"
               " a duplicated read adds about 19)\n",
               (unsigned long)(t.bits_min / 10), (unsigned long)(t.bits_min % 10),
               (unsigned long)(t.bits_max / 10), (unsigned long)(t.bits_max % 10));
    }
    if (t.note_count) {
        printf("        note: %s (%lu/%lu)\n", t.note, (unsigned long)t.note_count,
               (unsigned long)t.iters);
    }
}

// *****************************************************************************
int main(void)
{
    unsigned start = 0;
    unsigned pass = 0, fail = 0, hang = 0;
    bool resumed = false;

    MXC_WDT_Disable(MXC_WDT0);
    cyc_init();
    requests_init();

    // A case was running when the device reset: the watchdog caught a hang
    if ((MXC_PWRSEQ->gp0 & 0xFFFF8000U) == build_id()) {
        unsigned idx = MXC_PWRSEQ->gp0 & 0xFU;
        unsigned iter = (MXC_PWRSEQ->gp0 >> 4) & 0x7FFU;

        if (idx < NUM_CASES) {
            set_result(idx, RES_HANG);
            printf("  HANG  at iteration #%u (watchdog reset)\n", iter);
            start = idx + 1;
            resumed = true;
        }
    }

    if (!resumed) {
        MXC_PWRSEQ->gp0 = 0;
        MXC_PWRSEQ->gp1 = 0;
        // Let resets issued by the debugger after programming happen before any case starts
        MXC_Delay(MXC_DELAY_MSEC(1500));
        printf("\n***** I2C Transaction Test *****\n");
#ifdef MSDK_LABEL
        printf("MSDK: %s\n", MSDK_LABEL);
#endif
        printf("I2C%d at %d Hz, MAX9867 at 0x%02X, register 0x%02X (expect 0x%02X)\n",
               MXC_I2C_GET_IDX(TEST_I2C), I2C_FREQ, CODEC_ADDR, CODEC_REG, CODEC_REV);
        printf("%d iterations per case\n\n", ITERATIONS);
    }

    wdt_start();

    for (unsigned idx = start; idx < NUM_CASES; idx++) {
        MXC_PWRSEQ->gp0 = build_id() | idx;
        printf("[%2u] %s\n", idx + 1, cases[idx].name);
        console_flush();
        run_case(idx);
    }

    MXC_WDT_Disable(MXC_WDT0);
    MXC_PWRSEQ->gp0 = 0;

    for (unsigned idx = 0; idx < NUM_CASES; idx++) {
        uint32_t r = get_result(idx);

        pass += (r == RES_PASS);
        fail += (r == RES_FAIL);
        hang += (r == RES_HANG);
    }
    printf("\nSummary: %u PASS, %u FAIL, %u HANG of %u\n", pass, fail, hang, (unsigned)NUM_CASES);
    if (pass == NUM_CASES) {
        printf("Example Succeeded\n");
    } else {
        printf("Example Failed\n");
    }

    while (1) {}
}
#else
/*
 * Capture build: one iteration of case SCOPE_CASE every 20 ms, after the same
 * controller setup the test does, so a logic analyzer can trigger on it.
 */
int main(void)
{
    const struct test_case *c = &cases[SCOPE_CASE - 1];
    struct tally t = { .iters = 1, .bits_min = UINT32_MAX };

    MXC_WDT_Disable(MXC_WDT0);
    cyc_init();
    requests_init();
    i2c_reinit();
    if (c->dma) {
        dma_setup();
    }
    printf("\nI2C Transaction Test capture build: case %d (%s)\n", SCOPE_CASE, c->name);
    console_flush();
    MXC_Delay(MXC_DELAY_MSEC(1500));

    for (uint32_t i = 0;; i++) {
        c->fn(&t, i);
        MXC_Delay(MXC_DELAY_MSEC(20));
    }
}
#endif /* SCOPE_CASE */
