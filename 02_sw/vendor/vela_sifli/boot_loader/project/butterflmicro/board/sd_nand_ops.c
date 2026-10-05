#include <stdio.h>
#include "sd_nand_drv.h"
#include "boot_hw.h"

static uint8_t  wire_mode = 1;  //0 for 1-wire mode, 1 for 4-wire mode
static uint8_t  sdsc = 1; //0 for sdhc/sdxc, 1 for sdsc
static uint64_t g_sd_card_bytes;
static uint8_t  g_sd_up;
static uint8_t  g_dat0_poll;

static uint8_t mmcsd_parse_csd(uint32_t *resp);

uint8_t sdmmc1_sdnand()
{
    uint8_t test_result = 1;
    uint8_t  rsp_idx;
    uint32_t rsp_arg[4];
    uint8_t  cmd_result;
    uint8_t  ccs;
    uint16_t rca;
    uint32_t cmd_arg;
    uint32_t cid[4];

    //debug_print("SDMMC1 sd case start!\n");

    //initialize sdmmc host
    sd1_init();
    /* 2SFBL stays on HRC48 (~48 MHz HCLK); not the 144 MHz DLL path. */
    hwp_sdmmc1->CLKCR = 119 << SD_CLKCR_DIV_Pos; //48M/120=400k, stop_clk = 0
    hwp_sdmmc1->CLKCR |= SD_CLKCR_VOID_FIFO_ERROR;
    hwp_sdmmc1->IER = 0; //mask sdmmc interrupt
    hwp_sdmmc1->TOR = 0x00100000; // set timeout for 400K about 2.6s

    // add a delay after clock set, at least 74 SD clock
    // need wait more than 200ms for 400khz
    HAL_Delay_us(500);

    rca = 0x0;

    //initialize sd card
    cmd_result = sd1_send_cmd(0, 0); //CMD0

    //set sd_req and wait for sd_busy before access sd in normal mode
    hwp_sdmmc1->CASR = SD_CASR_SD_REQ;
    while ((hwp_sdmmc1->CASR & SD_CASR_SD_BUSY) == 0);

    //start card identification
    //CMD8
    HAL_Delay_us(20);
    cmd_arg = 0x000001aa; //VHS=1
    cmd_result = sd1_send_cmd(8, cmd_arg); //CMD8
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD8 TIMEOUT!\n");
        test_result = '8';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD8 CRC ERR!\n");
        test_result = '8';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    if ((rsp_idx != 0x8) || (rsp_arg[0] != 0x1aa))
    {
        //debug_print("CMD8 RSP ERR!\n");
        test_result = '8';
        //HAL_ASSERT(0);
        return test_result;
    }

    //ACMD41
    cmd_arg = 0x40ff8000;
    while (1) //wait for card busy status
    {
        boot_hw_wdt_pet();
        HAL_Delay_us(20);
        cmd_result = sd1_send_acmd(41, cmd_arg, rca); //CMD55+ACMD41
        if (cmd_result == SD_TIMEOUT)
        {
            //debug_print("ACMD41 TIMEOUT!\n");
            test_result = '4';
            //HAL_ASSERT(0);
            return test_result; //CMD 41
        }
        sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
        if ((rsp_arg[0] & 0x80000000) != 0)
        {
            break; //card power up done
        }
        HAL_Delay_us(2); //add some delay
    }
    ccs = (rsp_arg[0] >> 30) & 0x1;

    //CMD2
    HAL_Delay_us(20);
    cmd_arg = 0x0;
    cmd_result = sd1_send_cmd(2, cmd_arg); //CMD2
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD2 TIMEOUT!\n");
        test_result = '2';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD2 CRC ERR!\n");
        test_result = '2';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &cid[3], &cid[2], &cid[1], &cid[0]);

    //CMD3
    HAL_Delay_us(20);
    cmd_arg = 0x0;
    cmd_result = sd1_send_cmd(3, cmd_arg); //CMD3
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD3 TIMEOUT!\n");
        test_result = '3';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD3 CRC ERR!\n");
        test_result = '3';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    rca = rsp_arg[0] >> 16;
    if (rsp_idx != 0x3)
    {
        //debug_print("CMD3 RSP ERR!\n");
        test_result = '3';
        //HAL_ASSERT(0);
        return test_result;
    }

    HAL_Delay_us(20);
    cmd_arg = rca << 16;
    cmd_result = sd1_send_cmd(9, cmd_arg); //CMD9
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD9 TIMEOUT!\n");
        test_result = '9';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD9 CRC ERR!\n");
        test_result = '9';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    {
        // FOR R2, it need 128 bits response, high/low words should switch.
        // least 8 bit has been removed, so need fill 8 bits at least bits
        uint32_t temp;
        // switch for [0] as highest
        temp = rsp_arg[0];
        rsp_arg[0] = rsp_arg[3];
        rsp_arg[3] = temp;
        temp = rsp_arg[1];
        rsp_arg[1] = rsp_arg[2];
        rsp_arg[2] = temp;

        // << 8
        rsp_arg[0] = (rsp_arg[0] << 8) | (rsp_arg[1] >> 24);
        rsp_arg[1] = (rsp_arg[1] << 8) | (rsp_arg[2] >> 24);
        rsp_arg[2] = (rsp_arg[2] << 8) | (rsp_arg[3] >> 24);
        rsp_arg[3] = (rsp_arg[3] << 8);
    }

    uint8_t csd_struct = mmcsd_parse_csd(&rsp_arg[0]);
    if (csd_struct == 0)
    {
        //debug_print("SDSC card!\n");
        sdsc = 1;
    }
    else if (csd_struct == 1)
    {
        //debug_print("SDHC card!\n");
        sdsc = 0;
    }
    else
    {
        //debug_print("SD card invalid csd structure !\n");
        test_result = 'T';
        //HAL_ASSERT(0);
        return test_result; // structure Type fail
    }
    //debug_print("SD card identification done!\n");

    hwp_sdmmc1->CLKCR = 1 << SD_CLKCR_DIV_Pos; //48M/2=24M
    hwp_sdmmc1->CLKCR |= SD_CLKCR_VOID_FIFO_ERROR;
    hwp_sdmmc1->TOR = 0x02000000; // set timeout for 24M about 1.4s
    hwp_sdmmc1->CDR = SD_CDR_ITIMING_SEL | (0 << SD_CDR_ITIMING_Pos);

    //start card transfer
    //CMD7 (SELECT_CARD)
    HAL_Delay_us(20);
    cmd_arg = (uint32_t)rca << 16;
    cmd_result = sd1_send_cmd(7, cmd_arg);
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD7 TIMEOUT!\n");
        test_result = '7';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD7 CRC ERR!\n");
        test_result = '7';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    if (rsp_idx != 7)
    {
        //debug_print("CMD7 RSP ERR!\n");
        test_result = '7';
        //HAL_ASSERT(0);
        return test_result;
    }

    //ACMD6
    HAL_Delay_us(20);
    cmd_arg = wire_mode ? 2 : 0; //select 4-wire mode or 1-wire mode
    cmd_result = sd1_send_acmd(6, cmd_arg, rca); //CMD55+ACMD6
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("ACMD6 TIMEOUT!\n");
        test_result = '6';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("ACMD6 CRC ERR!\n");
        test_result = '6';
        //HAL_ASSERT(0);
        return test_result;
    }

    sd1_read(wire_mode, 1); //4 wire mode,8 blocks

    //CMD17 (READ_SINGLE_BLOCK)
    HAL_Delay_us(20);
    cmd_arg = 0; //start data address
    cmd_result = sd1_send_cmd(17, cmd_arg);
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD17 TIMEOUT!\n");
        test_result = 'R';
        //HAL_ASSERT(0);
        return test_result; // Read command
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD17 CRC ERR!\n");
        test_result = 'R';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    if (rsp_idx != 17)
    {
        //debug_print("CMD17 RSP ERR!\n");
        test_result = 'R';
        //HAL_ASSERT(0);
        return test_result;
    }

    //wait read data
    hwp_sdmmc1->SR = 0xffffffff; //clear sdmmc interrupts
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
    cmd_result = sd1_wait_read();  //wait sdmmc interrupt
    if (hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT)
    {
        //debug_print("DATA READ TIMEOUT!\n");
        test_result = 'O';
        //HAL_ASSERT(0);
        return test_result; // read time Out
    }
    if (hwp_sdmmc1->SR & SD_SR_DATA_CRC)
    {
        //debug_print("READ CRC ERR!\n");
        test_result = 'D';
        //HAL_ASSERT(0);
        return test_result; // Data error
    }

    /* Idle DAT0 must read high.  If DSR[0] is stuck, skip later polls. */
    g_dat0_poll = (hwp_sdmmc1->DSR & 1u) != 0;
    g_sd_up = (test_result == TEST_PASS) ? 1 : 0;
    return test_result;
}

static void sd_prepare(void)
{
    hwp_sdmmc1->DCR = 0;
    hwp_sdmmc1->IER = 0;
    sd_wait_idle();
}

int sd_read_data(uint64_t addr, uint8_t *data, uint32_t len)
{
    uint8_t  rsp_idx;
    uint8_t test_result = TEST_PASS;
    int i;
    uint32_t cmd_result;
    uint32_t cmd_arg;
    uint32_t *buf = (uint32_t *)data;
    uint32_t rsp_arg1, rsp_arg2, rsp_arg3, rsp_arg4;

    sd_prepare();
    sd1_read(wire_mode, 1);

    //CMD17 (READ_SINGLE_BLOCK)
    HAL_Delay_us(20);
    cmd_arg = sdsc ? (uint32_t)addr : (uint32_t)(addr >> 9); //start data address
    cmd_result = sd1_send_cmd(17, cmd_arg);
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD17 TIMEOUT!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD17 CRC ERR!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg1, &rsp_arg2, &rsp_arg3, &rsp_arg4);
    if (rsp_idx != 17)
    {
        //debug_print("CMD17 RSP ERR!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }

    //wait for dma read data
    hwp_sdmmc1->SR = 0xffffffff; //clear sdmmc interrupts
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
    cmd_result = sd1_wait_read();  //wait sdmmc interrupt
    if (hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT)
    {
        //debug_print("DATA READ TIMEOUT!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }
    if (hwp_sdmmc1->SR & SD_SR_DATA_CRC)
    {
        //debug_print("READ CRC ERR!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }

    for (i = 0; i < len / 4; i++)
    {
        *buf = hwp_sdmmc1->FIFO;
        buf++;
    }

    if (test_result == TEST_FAIL)
    {
        //debug_print("Read page fail!\n");
        return 0;
    }

    return len;
}

static __inline uint32_t GET_BITS(uint32_t *resp, uint32_t  start, uint32_t  size)
{
    const int32_t __size = size;
    const uint32_t __mask = (__size < 32 ? 1 << __size : 0) - 1;
    const int32_t __off = 3 - ((start) / 32);
    const int32_t __shft = (start) & 31;
    uint32_t __res;

    __res = resp[__off] >> __shft;
    if (__size + __shft > 32)
        __res |= resp[__off - 1] << ((32 - __shft) % 32);

    return __res & __mask;
}

static uint8_t mmcsd_parse_csd(uint32_t *resp)
{
    uint32_t csd_structure = GET_BITS(resp, 126, 2);
    uint32_t c_size;
    uint32_t read_bl_len;
    uint32_t c_mult;

    if (csd_structure == 0)
    {
        read_bl_len = GET_BITS(resp, 80, 4);
        c_size = GET_BITS(resp, 62, 12);
        c_mult = GET_BITS(resp, 47, 3);
        g_sd_card_bytes = ((uint64_t)c_size + 1ull) << (c_mult + 2 + read_bl_len);
    }
    else
    {
        c_size = GET_BITS(resp, 48, csd_structure >= 2 ? 28 : 22);
        g_sd_card_bytes = ((uint64_t)c_size + 1ull) * 512ull * 1024ull;
    }
    return (uint8_t)csd_structure;
}

uint64_t sd_card_bytes(void)
{
    return g_sd_card_bytes;
}

int sd_write_data(uint64_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t rsp_idx;
    uint32_t cmd_result;
    uint32_t cmd_arg;
    uint32_t rsp_arg1, rsp_arg2, rsp_arg3, rsp_arg4;
    const uint32_t *buf = (const uint32_t *)data;
    int i;

    if (len != SD_BLOCK_SIZE)
    {
        return 0;
    }

    sd_prepare();
    HAL_Delay_us(20);
    cmd_arg = sdsc ? (uint32_t)addr : (uint32_t)(addr >> 9);
    cmd_result = sd1_send_cmd(24, cmd_arg);
    if (cmd_result != SD_SUCCESS)
    {
        return 0;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg1, &rsp_arg2, &rsp_arg3, &rsp_arg4);
    if (rsp_idx != 24)
    {
        return 0;
    }

    sd1_write(wire_mode, 1);
    hwp_sdmmc1->SR = 0xffffffff;
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
    for (i = 0; i < (int)(SD_BLOCK_SIZE / 4); i++)
    {
        hwp_sdmmc1->FIFO = buf[i];
    }
    if (sd1_wait_write() != SD_SUCCESS)
    {
        return 0;
    }
    /* DATA_DONE is the host TX engine.  The card still holds DAT0 low while
     * it programs; reading OVNX on another region before that finishes can
     * interrupt the KV LittleFS commit (especially /fw on KV_REGION).
     */
    sd_wait_idle();
    return (int)len;
}

void sd_wait_idle(void)
{
    unsigned n;

    if (!g_sd_up)
    {
        return;
    }

    /* Host data engine (TXACT). */
    for (n = 0; n < 200000u; n++)
    {
        if ((hwp_sdmmc1->SR & SD_SR_DATA_BUSY) == 0)
        {
            break;
        }
        boot_hw_wdt_pet();
        HAL_Delay_us(10);
    }

    if (!g_dat0_poll)
    {
        return;
    }
    /* Card programming busy on DAT0.  2 s @ 10 us. */
    for (n = 0; n < 200000u; n++)
    {
        if ((hwp_sdmmc1->DSR & 1u) != 0)
        {
            return;
        }
        boot_hw_wdt_pet();
        HAL_Delay_us(10);
    }
}

void sd_release(void)
{
    if (!g_sd_up)
    {
        return;
    }

    hwp_sdmmc1->DCR = 0;
    hwp_sdmmc1->IER = 0;
    hwp_sdmmc1->SR = 0xffffffff;
    sd_wait_idle();
    (void)sd1_send_cmd(0, 0);
    HAL_Delay_us(1000);
    hwp_sdmmc1->CLKCR = 0;
    hwp_hpsys_rcc->ENR2 &= ~HPSYS_RCC_ENR2_SDMMC1;
    g_sd_up = 0;
}

