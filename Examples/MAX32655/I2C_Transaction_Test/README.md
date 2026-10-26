## Description

This example checks I2C controller transaction sequences against the on-board MAX9867 audio codec (address 0x18, whose revision register 0xFF reads 0x42).

It runs 14 test cases, each 1000 times back to back, covering blocking (`MXC_I2C_MasterTransaction`), interrupt-driven (`MXC_I2C_MasterTransactionAsync`) and DMA (`MXC_I2C_MasterTransactionDMA`) transactions, and sequences that mix them:

| # | Sequence |
|---|---|
| 1 | blocking read, then blocking read |
| 2 | blocking write ending in a repeated START (`restart = 1`), then a blocking read |
| 3 | blocking write to an address with no device (NACK), then a blocking read |
| 4 | blocking write with `restart = 1`, then a blocking write |
| 5 | blocking write with `restart = 1`, then a blocking write and read |
| 6 | async read, then a blocking read as soon as it completes |
| 7 | async read, then an async read |
| 8 | async write to an address with no device, then a blocking read |
| 9 | blocking write with `restart = 1`, then an async read |
| 10 | blocking write with `restart = 1`, then an async write |
| 11 | DMA read, then a blocking read as soon as it completes |
| 12 | DMA read, then a DMA read |
| 13 | DMA read from an address with no device, then a blocking read |
| 14 | blocking write with `restart = 1`, then a DMA read |

Each "read" writes the register address and reads one byte. A case passes only if every call returns the expected code, every read returns 0x42, and the bus is idle after each iteration. A case that hangs is caught by the watchdog: the device resets, reports the case as HANG, and continues with the next case. A case whose async or DMA completion callback does not arrive within 20 ms stops at that iteration and reports it. Cases 2 and 14 also report how long the read takes in I2C bit times, which shows a duplicated read.

None of the writes change a codec register: they only set the register address.

## Software

### Project Usage

Universal instructions on building, flashing, and debugging this project can be found in the **[MSDK User Guide](https://analogdevicesinc.github.io/msdk/USERGUIDE/)**.

### Project-Specific Build Notes

This project builds as a standard example. Optional build settings (pass them to `make`):

- `SCOPE_CASE=<1..14>`: capture build. After the same setup, it runs one iteration of that case every 20 ms, so a logic analyzer can trigger on it.
- `MSDK_LABEL=<text>`: prints a label for the MSDK under test in the console header.

## Required Connections

If using the Standard EV Kit (EvKit\_V1):
-   Connect a USB cable between the PC and the CN1 (USB/PWR) connector.
-   Connect pins JP4(RX_SEL) and JP5(TX_SEL) to RX0 and TX0  header.
-   Open a terminal application on the PC and connect to the EV kit's console UART at 115200, 8-N-1.
-   No other connections are needed: the MAX9867 is on I2C2 (SCL - P0.30, SDA - P0.31).

If using the Featherboard (FTHR\_Apps\_P1):
-   Connect a USB cable between the PC and the J4 (USB/PWR) connector.
-   Open a terminal application on the PC and connect to the board's console UART at 115200, 8-N-1.
-   No other connections are needed: the MAX9867 is on I2C1 (SCL - P0.16, SDA - P0.17).

## Expected Output

The Console UART of the device will output these messages (FTHR\_Apps\_P1 shown; EvKit\_V1 reports I2C2):

```
***** I2C Transaction Test *****
I2C1 at 100000 Hz, MAX9867 at 0x18, register 0xFF (expect 0x42)
1000 iterations per case

[ 1] blocking read, blocking read
  PASS  1000/1000 ok
[ 2] blocking restart, blocking read
  PASS  1000/1000 ok
        timed call: 20.1..20.2 bit times (one read is about 20; a duplicated read adds about 19)
[ 3] blocking NACK, blocking read
  PASS  1000/1000 ok
...
[ 8] async NACK, blocking read
  PASS  1000/1000 ok
        note: async NACK write completed with E_NO_ERROR (1000/1000)
...
[14] blocking restart, DMA read
  PASS  1000/1000 ok
        timed call: 20.1..20.2 bit times (one read is about 20; a duplicated read adds about 19)

Summary: 14 PASS, 0 FAIL, 0 HANG of 14
Example Succeeded
```

A failing case prints the first error, for example `FAIL  0/1000 ok, 1000 errors (first #0 read 2: E_BUSY (-6))`, and a case caught by the watchdog prints `HANG  at iteration #0 (watchdog reset)`. Case 8 notes, but does not fail on, an async write to a missing device completing with `E_NO_ERROR`.

## Test Results

These results come from this fork; they are not part of upstream MSDK. In every table:

- **main** is MSDK main at b75575ac10, unmodified.
- **main + fix** is main with this fork's changes to
  [`i2c_reva.c`](../../../Libraries/PeriphDrivers/Source/I2C/i2c_reva.c).
- All runs are on a MAX32655FTHR (FTHR\_Apps\_P1) at 100 kHz, 2026-10-26.

### Across MSDK releases

The test built against each whole MSDK release, with that release's own build files. Each column links to its full console log.

| # | Case | [v2023_06](results/logs/MAX32655FTHR-v2023_06.txt) | [v2023_10](results/logs/MAX32655FTHR-v2023_10.txt) | [v2024_02](results/logs/MAX32655FTHR-v2024_02.txt) | [v2024_10](results/logs/MAX32655FTHR-v2024_10.txt) | [CFS 2.1.0](results/logs/MAX32655FTHR-CFS-2.1.0.txt) | [CFS 2.3.0](results/logs/MAX32655FTHR-CFS-2.3.0.txt) | [main](results/logs/MAX32655FTHR-main.txt) | [main + fix](results/logs/MAX32655FTHR-main%2Bfix.txt) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | blocking read, blocking read | PASS | PASS | PASS | PASS | FAIL | FAIL | FAIL | PASS |
| 2 | blocking restart, blocking read | PASS | PASS | PASS | PASS | FAIL | FAIL | FAIL | PASS |
| 3 | blocking NACK, blocking read | PASS | PASS | PASS | HANG | FAIL | FAIL | FAIL | PASS |
| 4 | blocking restart, blocking write | PASS | PASS | PASS | PASS | FAIL | FAIL | FAIL | PASS |
| 5 | blocking restart, blocking write+read | PASS | PASS | PASS | PASS | FAIL | FAIL | FAIL | PASS |
| 6 | async read, blocking read | HANG | HANG | HANG | HANG | FAIL | FAIL | FAIL | PASS |
| 7 | async read, async read | FAIL | FAIL | FAIL | FAIL | PASS | PASS | PASS | PASS |
| 8 | async NACK, blocking read | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL | PASS |
| 9 | blocking restart, async read | FAIL | FAIL | FAIL | FAIL | HANG | HANG | HANG | PASS |
| 10 | blocking restart, async write | FAIL | FAIL | FAIL | FAIL | HANG | HANG | HANG | PASS |
| 11 | DMA read, blocking read | HANG | HANG | HANG | FAIL | FAIL | FAIL | FAIL | PASS |
| 12 | DMA read, DMA read | FAIL | FAIL | FAIL | HANG | HANG | HANG | HANG | PASS |
| 13 | DMA NACK, blocking read | FAIL | FAIL | FAIL | HANG | FAIL | FAIL | FAIL | PASS |
| 14 | blocking restart, DMA read | FAIL | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| | **PASS / FAIL / HANG** | **5 / 7 / 2** | **6 / 6 / 2** | **6 / 6 / 2** | **5 / 5 / 4** | **2 / 9 / 3** | **2 / 9 / 3** | **2 / 9 / 3** | **14 / 0 / 0** |

- Blocking cases 1-5 pass on every release up to v2024_02 and fail on main and both CodeFusion Studio bundles. The MAX32690EVKIT shows the same.
- The CodeFusion Studio 2.1.0 and 2.3.0 bundles match main case for case.
- Async and DMA (cases 6-14) differ from the MAX32690EVKIT on older releases. On v2024_10, async cases 6, 7 and 9 fail here but pass there. On main, case 12 (two DMA reads back to back) hangs here but passes there: the second transfer can start while the first one's STOP is still on the bus, and the outcome depends on timing.

There are no bus captures for this board. On the MAX32655FTHR the codec bus (I2C1, P0.16/P0.17) is not on any header; the Feather SDA/SCL pins are I2C2. The MAX32690EVKIT captures in [`Examples/MAX32690/I2C_Transaction_Test`](../../MAX32690/I2C_Transaction_Test/README.md#bus-captures) show the bus-level behaviour. The I2C peripheral and `i2c_reva.c` are the same on both parts.

The scripts in `tools/`, and how the runs were made, are the same as for the MAX32690. See [How the Results Were Produced](../../MAX32690/I2C_Transaction_Test/README.md#how-the-results-were-produced). The scripts pick the target, the board (FTHR\_Apps\_P1 by default), the OpenOCD target and the console port (`/dev/ttyACM0`) from this folder's location.
