#include "flash.h"
#include "xqspips.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"

#define QSPI_DEVICE_ID      0

#define WRITE_ENABLE_CMD    0x06
#define SECTOR_ERASE_CMD    0xD8
#define PAGE_PROGRAM_CMD    0x02
#define READ_STATUS_CMD     0x05
#define PAGE_SIZE           256

#define FLASH_READY_TIMEOUT 100000U

/* Function prototypes */
static int Flash_WaitForReady(void);   

XQspiPs Qspi;

static u8 WriteCmd[PAGE_SIZE + 4];

#define READ_CMD_BUF_SIZE  (PAGE_SIZE + 4)
static u8 ReadCmdBuf[READ_CMD_BUF_SIZE];

/* Flash Init  */
int Flash_Init(void)
{
    XQspiPs_Config *Config;
    int Status;

    Config = XQspiPs_LookupConfig(QSPI_DEVICE_ID);

    if (Config == NULL)
    {
        xil_printf("QSPI Config Error\r\n");
        return XST_FAILURE;
    }

    Status = XQspiPs_CfgInitialize(&Qspi, Config, Config->BaseAddress);

    if (Status != XST_SUCCESS)
    {
        xil_printf("QSPI Init Failed\r\n");
        return XST_FAILURE;
    }

    xil_printf("QSPI Base = 0x%08X\r\n", (unsigned int)Config->BaseAddress);

    XQspiPs_SetOptions(&Qspi, XQSPIPS_FORCE_SSELECT_OPTION);
    XQspiPs_SetClkPrescaler(&Qspi, XQSPIPS_CLK_PRESCALE_64);

    /* check return value of SetSlaveSelect */
    Status = XQspiPs_SetSlaveSelect(&Qspi);
    XQspiPs_Enable(&Qspi);

    xil_printf("QSPI Enabled\r\n");
    if (Status != XST_SUCCESS)
    {
        xil_printf("QSPI Slave Select Failed\r\n");
        return XST_FAILURE;
    }

    xil_printf("QSPI Initialized Successfully\r\n");

    return XST_SUCCESS;
}

/* Read JEDEC ID */
int Flash_ReadJEDEC(void)
{
    int Status;
    u8 SendBuffer[4] = {XQSPIPS_FLASH_OPCODE_RDID, 0x00, 0x00, 0x00};
    u8 RecvBuffer[4] = {0};

    Status = XQspiPs_PolledTransfer(&Qspi, SendBuffer, RecvBuffer, 4);
    
    if (Status != XST_SUCCESS)
    {
        xil_printf("JEDEC Read Failed\r\n");
        return XST_FAILURE;
    }

    xil_printf("\r\nFlash JEDEC ID\r\n");
    xil_printf("------------------\r\n");
    xil_printf("Manufacturer : 0x%02X\r\n", RecvBuffer[1]);
    xil_printf("Memory Type  : 0x%02X\r\n", RecvBuffer[2]);
    xil_printf("Capacity ID  : 0x%02X\r\n", RecvBuffer[3]);

    return XST_SUCCESS;
}

/* Write Enable */
int Flash_WriteEnable(void)
{
    int Status;
    u8 Cmd = WRITE_ENABLE_CMD;
    u8 StatusBuf[2];

    Status = XQspiPs_PolledTransfer(&Qspi, &Cmd, NULL, 1);

    if (Status != XST_SUCCESS)
    {
        xil_printf("Write Enable Failed\r\n");
        return XST_FAILURE;
    }
    StatusBuf[0] = READ_STATUS_CMD;
    StatusBuf[1] = 0x00;
    Status = XQspiPs_PolledTransfer(&Qspi, StatusBuf, StatusBuf, 2);
    if (Status != XST_SUCCESS || !(StatusBuf[1] & 0x02))
    {
        xil_printf("Write Enable Latch not set\r\n");
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/* Sector Erase */

int Flash_EraseSector(u32 Address)
{
    int Status;
    u8 Cmd[4];

    Status = Flash_WriteEnable();
    if (Status != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    Cmd[0] = SECTOR_ERASE_CMD;
    Cmd[1] = (Address >> 16) & 0xFF;
    Cmd[2] = (Address >> 8) & 0xFF;
    Cmd[3] = Address & 0xFF;

    Status = XQspiPs_PolledTransfer(&Qspi, Cmd, NULL, 4);

    if (Status != XST_SUCCESS)
    {
        xil_printf("Sector Erase Failed\r\n");
        return XST_FAILURE;
    }

    /* Flash_WaitForReady now returns a status */
    Status = Flash_WaitForReady();
    if (Status != XST_SUCCESS)
    {
        xil_printf("Sector Erase Timeout at 0x%06X\r\n", (unsigned int)Address);
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

/* Flash Write (single page, max 256 bytes)  */

int Flash_Write(u32 Address, u8 *Buffer, u32 Length)
{
    int Status;
    u32 i;

    /* Validate length */
    if (Length > PAGE_SIZE)
    {
        xil_printf("Write length exceeds page size\r\n");
        return XST_FAILURE;
    }

    /* Check page boundary crossing */
    if ((Address % PAGE_SIZE) + Length > PAGE_SIZE)
    {
        xil_printf("Write crosses page boundary\r\n");
        return XST_FAILURE;
    }

    Status = Flash_WriteEnable();
    if (Status != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    WriteCmd[0] = PAGE_PROGRAM_CMD;
    WriteCmd[1] = (Address >> 16) & 0xFF;
    WriteCmd[2] = (Address >> 8) & 0xFF;
    WriteCmd[3] = Address & 0xFF;

    for (i = 0; i < Length; i++)
    {
        WriteCmd[i + 4] = Buffer[i];
    }

    Status = XQspiPs_PolledTransfer(&Qspi, WriteCmd, NULL, Length + 4);

    if (Status != XST_SUCCESS)
    {
        xil_printf("Flash Write Failed\r\n");
        return XST_FAILURE;
    }
    /* check timeout return from Flash_WaitForReady */
    Status = Flash_WaitForReady();
    if (Status != XST_SUCCESS)
    {
        xil_printf("Flash Write Timeout at 0x%06X\r\n", (unsigned int)Address);
        return XST_FAILURE;
    }

    xil_printf("Flash Write Complete at 0x%06X\r\n", (unsigned int)Address);

    return XST_SUCCESS;
}

/* Flash Read  */
int Flash_Read(u32 Address, u8 *Buffer, u32 Length)
{
    int Status;
    u32 i;
    if (Length + 4 > READ_CMD_BUF_SIZE)
    {
        xil_printf("Flash Read: requested length %lu exceeds buffer\r\n",
                   (unsigned long)Length);
        return XST_FAILURE;
    }

    ReadCmdBuf[0] = 0x03;
    ReadCmdBuf[1] = (Address >> 16) & 0xFF;
    ReadCmdBuf[2] = (Address >> 8) & 0xFF;
    ReadCmdBuf[3] = Address & 0xFF;

    for (i = 4; i < Length + 4; i++)
    {
        ReadCmdBuf[i] = 0x00;
    }
    
    Status = XQspiPs_PolledTransfer(&Qspi, ReadCmdBuf, ReadCmdBuf, Length + 4);
    
    if (Status != XST_SUCCESS)
    {
        xil_printf("Flash Read Failed\r\n");
        return XST_FAILURE;
    }

    for (i = 0; i < Length; i++)
    {
        Buffer[i] = ReadCmdBuf[i + 4];
    }

    return XST_SUCCESS;
}

static int Flash_WaitForReady(void)
{
    u8 StatusBuf[2];
    u32 Timeout = FLASH_READY_TIMEOUT;

    do
    {
        StatusBuf[0] = READ_STATUS_CMD;
        StatusBuf[1] = 0x00;

        XQspiPs_PolledTransfer(&Qspi, StatusBuf, StatusBuf, 2);

        if (Timeout-- == 0)
        {
            xil_printf("Flash WaitForReady: timeout!\r\n");
            return XST_FAILURE;
        }

    } while (StatusBuf[1] & 0x01);

    return XST_SUCCESS;
}
