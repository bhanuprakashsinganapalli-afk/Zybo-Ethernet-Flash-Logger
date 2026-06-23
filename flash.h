#ifndef FLASH_H
#define FLASH_H

#include "xil_types.h"

int Flash_Init(void);
int Flash_ReadJEDEC(void);
int Flash_WriteEnable(void);
int Flash_EraseSector(u32 Address);
int Flash_Write(u32 Address, u8 *Buffer, u32 Length);
int Flash_Read(u32 Address, u8 *Buffer, u32 Length);

#endif
