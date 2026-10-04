/*********************************************************************************************************//**
 * @file    BMH08002_4_HT32/src/BMH08002_4.h
 * @version V1.0.1
 * @date    2024-09-01
 * @brief   The header file of BMH08002_4 driver.
 *************************************************************************************************************
 * @attention
 *
 * Firmware Disclaimer Information
 *
 * 1. The customer hereby acknowledges and agrees that the program technical documentation, including the
 *    code, which is supplied by Holtek Semiconductor Inc., (hereinafter referred to as "HOLTEK") is the
 *    proprietary and confidential intellectual property of HOLTEK, and is protected by copyright law and
 *    other intellectual property laws.
 *
 * 2. The customer hereby acknowledges and agrees that the program technical documentation, including the
 *    code, is confidential information belonging to HOLTEK, and must not be disclosed to any third parties
 *    other than HOLTEK and the customer.
 *
 * 3. The program technical documentation, including the code, is provided "as is" and for customer reference
 *    only. After delivery by HOLTEK, the customer shall use the program technical documentation, including
 *    the code, at their own risk. HOLTEK disclaims any expressed, implied or statutory warranties, including
 *    the warranties of merchantability, satisfactory quality and fitness for a particular purpose.
 *
 * <h2><center>Copyright (C) Holtek Semiconductor Inc. All rights reserved</center></h2>
 ************************************************************************************************************/
/* Define to prevent recursive inclusion -------------------------------------------------------------------*/
#ifndef __BMH08002_4_H
#define __BMH08002_4_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------------------------------------*/
#include "ht32.h"
#include "ht32_board_config.h"
#include "bmduino_check.h"

#include "middleware/uart_module.h"

/* Settings ------------------------------------------------------------------------------------------------*/
/* Exported typedef ----------------------------------------------------------------------------------------*/
typedef enum {
  BMH08002_4_SUCCESS,
  BMH08002_4_FAILURE
} BMH08002_4_selStatus;
/* Exported constants --------------------------------------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------------------------------------*/
#define BMH08002_4_UART_TXTIMEOUT          1000   //uint:baud clock

#define BMH08002_4_START_MEASURE   0x00
#define BMH08002_4_STOP_MEASURE    0x01
#define BMH08002_4_ENTER_HALT      0x02
#define BMH08002_4_CHECK_OK        0
#define BMH08002_4_CHECK_ERROR     1
#define BMH08002_4_TIMEOUT_ERROR   2

/* Exported variables --------------------------------------------------------------------------------------*/
/* Exported functions --------------------------------------------------------------------------------------*/
BMH08002_4_selStatus BMH08002_4_selSerial(uint32_t serial_number);
void BMH08002_4_Init(void);

uint8_t BMH08002_4_beginMeasure(void);
uint8_t BMH08002_4_endMeasure(void);
uint8_t BMH08002_4_sleep(void);
void BMH08002_4_powerDown(void);
uint8_t BMH08002_4_requestInfoPackage(uint8_t buff[]);
bool BMH08002_4_isInfoAvailable(void);
void BMH08002_4_readInfoPackage(uint8_t buff[]);
uint8_t BMH08002_4_updateWave(void);
uint8_t BMH08002_4_calSensitivity(void);
uint16_t BMH08002_4_getTimeInterval(void);
uint8_t BMH08002_4_getModeConfig(void);   
uint8_t BMH08002_4_setTimeInterval(uint16_t Time);
uint8_t BMH08002_4_setModeConfig(uint8_t mode_code);
	 
uint8_t BMH08002_4_setWorkStatus(uint8_t workStatus);
void BMH08002_4_getChecksum(uint8_t buff[],uint8_t lenght);
uint8_t BMH08002_4_writeEEPROM(uint8_t addr,uint8_t highByte,uint8_t lowByte);
uint16_t BMH08002_4_readEEPROM(uint8_t addr);
void BMH08002_4_writeBytes(uint8_t wbuf[], uint8_t wlen);
uint8_t BMH08002_4_readBytes(uint8_t rbuf[], uint8_t rlen, uint16_t timeout);  
void _BMH08002_4_delay(vu32 count);

#ifdef __cplusplus
}
#endif

#endif

