/*********************************************************************************************************//**
 * @file    BMH08002_4_HT32/src/BMH08002_4.c
 * @version V1.0.1
 * @date    2024-09-01
 * @brief   The function of BMH08002_4 driver.
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

/* Includes ------------------------------------------------------------------------------------------------*/
#include "BMH08002_4.h"

/* Settings ------------------------------------------------------------------------------------------------*/
/* Private types -------------------------------------------------------------------------------------------*/
/* Private constants ---------------------------------------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------------------------------------*/
/* Global variables ----------------------------------------------------------------------------------------*/
uint32_t gBMH08002_4_SERIAL = BMH08002_4_SERIAL;
/* Private variables ---------------------------------------------------------------------------------------*/
uint16_t _BMH08002_4_checksum;
uint8_t  _BMH08002_4_recBuf[15] = {0}; // Array for storing received data
/* Global functions ----------------------------------------------------------------------------------------*/
/*********************************************************************************************************//**
 * @brief module serial number select.
 * @param serial_number: select serial number.
 * @retval BMH08002_4_SUCCESS/BMH08002_4_FAILURE
 ************************************************************************************************************/
BMH08002_4_selStatus BMH08002_4_selSerial(uint32_t serial_number)
{
  if(CHECK_SERIALn(serial_number) == -1)
  {
    return BMH08002_4_FAILURE;
  }

  gBMH08002_4_SERIAL = serial_number;
  return BMH08002_4_SUCCESS;
}

/*********************************************************************************************************//**
 * @brief  Module initialization using UART communication.
 * @param  void
 * @retval void
 ************************************************************************************************************/
void BMH08002_4_Init(void)
{
	USART_InitTypeDef USART_InitStructure = {0};
  USART_InitStructure.USART_BaudRate = 38400;
  USART_InitStructure.USART_WordLength = USART_WORDLENGTH_8B;
  USART_InitStructure.USART_StopBits = USART_STOPBITS_1;
  USART_InitStructure.USART_Parity = USART_PARITY_NO;
  USART_InitStructure.USART_Mode = USART_MODE_NORMAL;
  UARTM_Init(gBMH08002_4_SERIAL, &USART_InitStructure, BMH08002_4_UART_TXTIMEOUT);
	
	//OUTPUT:EN Pin
	{
	CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  CKCUClock.Bit.BMH08002_4_ENPIN_GPIO = 1;
  CKCUClock.Bit.AFIO       = 1;
  CKCU_PeripClockConfig(CKCUClock, ENABLE);
	}
	AFIO_GPxConfig(BMH08002_4_ENPIN_GPIO_ID, BMH08002_4_ENPIN_AFIO_PIN, AFIO_FUN_GPIO);
	GPIO_DirectionConfig(BMH08002_4_ENPIN_GPIO_PORT, BMH08002_4_ENPIN_GPIO_PIN, GPIO_DIR_OUT);
	
	GPIO_WriteOutBits(BMH08002_4_ENPIN_GPIO_PORT, BMH08002_4_ENPIN_GPIO_PIN, SET);
}

/*********************************************************************************************************//**
 * @brief  beginMeasure
 * @param  void
 * @retval result
            @arg 0 - Success
            @arg 1 - Failure   
 ************************************************************************************************************/
uint8_t BMH08002_4_beginMeasure()
{
  GPIO_WriteOutBits(BMH08002_4_ENPIN_GPIO_PORT, BMH08002_4_ENPIN_GPIO_PIN, SET);
  return BMH08002_4_setWorkStatus(0x00);
}

/*********************************************************************************************************//**
 * @brief  endMeasure
 * @param  void
 * @retval result
            @arg 0 - Success
            @arg 1 - Failure   
 ************************************************************************************************************/
uint8_t BMH08002_4_endMeasure()
{
  return BMH08002_4_setWorkStatus(0x01);
}

/*********************************************************************************************************//**
 * @brief  sleep
 * @param  void
 * @retval result
            @arg 0 - Success
            @arg 1 - Failure   
 ************************************************************************************************************/
uint8_t BMH08002_4_sleep()
{
	return BMH08002_4_setWorkStatus(0x02);
}

/*********************************************************************************************************//**
 * @brief  powerDown  EN=0
 * @param  void
 * @retval void 
 ************************************************************************************************************/
void BMH08002_4_powerDown()
{
	GPIO_WriteOutBits(BMH08002_4_ENPIN_GPIO_PORT, BMH08002_4_ENPIN_GPIO_PIN, RESET);
}

/*********************************************************************************************************//**
 * @brief  Judge the module measurement status and read the data.   
 * @param  buff[]:10 byte
 * @retval result
              @arg 0: No measurement data
              @arg 1: Generating data
              @arg 2: Data generation is completed and can be read
              @arg 3: data exception
 ************************************************************************************************************/
uint8_t BMH08002_4_requestInfoPackage(uint8_t buff[])
{
	uint8_t BMH08002_4_i=0;
  uint8_t BMH08002_4_temp,BMH08002_4_readFlag = 1;
	uint8_t BMH08002_4_rxBuf[15]={0};
  BMH08002_4_temp = BMH08002_4_readEEPROM(0x01);    //get work status,Timing send mode and waveform mode do not require sending commands
	if (((BMH08002_4_temp & 0x04) == 0x00) && ((BMH08002_4_temp & 0x02) == 0x02))
  {
      uint8_t BMH08002_4_txBuf[8]={0x55,0xb1,0x03,0x00,0x00,0x00,0x00,0xaa};
      BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
      BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
      _BMH08002_4_delay(60);
  } 
  
    BMH08002_4_readFlag = BMH08002_4_readBytes(BMH08002_4_rxBuf, 15, 30);
    if (BMH08002_4_readFlag==0x00 && BMH08002_4_rxBuf[0] == 0x55 && BMH08002_4_rxBuf[14] == 0xAA)
    {
       for(BMH08002_4_i=0;BMH08002_4_i<10;BMH08002_4_i++)
       {
         buff[BMH08002_4_i]=BMH08002_4_rxBuf[BMH08002_4_i+3];
       }
       return BMH08002_4_rxBuf[2];
    }
    return 3;
}

/*********************************************************************************************************//**
 * @brief  Query whether the 15-byte data sent by the module is received
 * @param  void
 * @retval result
              @arg true: 15-byte data received
              @arg false: 15-byte data not received
 * @others Only used in the mode of Tx Auto Output Info
 ************************************************************************************************************/
bool BMH08002_4_isInfoAvailable()
{
  uint8_t BMH08002_4_header[2] = {0x55, 0xB0}; // Fixed code for first 2 bytes of 15-byte data
  uint8_t BMH08002_4_recBuf[15] = {0}, BMH08002_4_recLen = 15;
  uint8_t BMH08002_4_i, BMH08002_4_num = 0, BMH08002_4_readCnt = 0, BMH08002_4_failCnt = 0, BMH08002_4_checkCode = 0;
  bool BMH08002_4_isHeader = false, BMH08002_4_result = false;

  /* Select hardSerial or softSerial according to the setting */
	BMH08002_4_num = UARTM_GetReadBufferLength(gBMH08002_4_SERIAL);

  /* Serial buffer contains at least one 15-byte data */
  if (BMH08002_4_num >= BMH08002_4_recLen)
  {
    while (BMH08002_4_failCnt < 2) // Didn't read the required data twice, exiting the loop
    {
      /* Find 2-byte data header */
      for (BMH08002_4_i = 0; BMH08002_4_i < 2;)
      {
				UARTM_ReadByte(gBMH08002_4_SERIAL,BMH08002_4_recBuf+BMH08002_4_i);

        if (BMH08002_4_recBuf[BMH08002_4_i] == BMH08002_4_header[BMH08002_4_i])
        {
          BMH08002_4_isHeader = true; // Fixed code is correct
          BMH08002_4_i++;             // Next byte
        }
        else if (BMH08002_4_recBuf[BMH08002_4_i] != BMH08002_4_header[BMH08002_4_i] && BMH08002_4_i > 0)
        {
          BMH08002_4_isHeader = false; // Next fixed code error
          BMH08002_4_failCnt++;
          break;
        }
        else if (BMH08002_4_recBuf[BMH08002_4_i] != BMH08002_4_header[BMH08002_4_i] && BMH08002_4_i == 0)
        {
          BMH08002_4_readCnt++; // 0x55 not found, continue
        }
        if (BMH08002_4_readCnt >= (BMH08002_4_num - 2))
        {
          BMH08002_4_readCnt = 0;
          BMH08002_4_isHeader = false; // Fixed code not found
          break;
        }
      }
      /* Find the correct fixed code */
      if (BMH08002_4_isHeader)
      {
        BMH08002_4_checkCode = BMH08002_4_recBuf[1]; // Sum checkCode
        for (BMH08002_4_i = 2; BMH08002_4_i < BMH08002_4_recLen; BMH08002_4_i++) // Read subsequent 13-byte data
        {
					UARTM_ReadByte(gBMH08002_4_SERIAL,BMH08002_4_recBuf+BMH08002_4_i);
          BMH08002_4_checkCode += BMH08002_4_recBuf[BMH08002_4_i]; // Sum checkCode
        }
        BMH08002_4_checkCode = BMH08002_4_checkCode - BMH08002_4_recBuf[BMH08002_4_recLen - 1] - BMH08002_4_recBuf[BMH08002_4_recLen - 2];

        /* Compare whether the check code is correct */
        if (BMH08002_4_checkCode == BMH08002_4_recBuf[BMH08002_4_recLen - 2])
        {
          for (BMH08002_4_i = 0; BMH08002_4_i < BMH08002_4_recLen; BMH08002_4_i++)
          {
            _BMH08002_4_recBuf[BMH08002_4_i] = BMH08002_4_recBuf[BMH08002_4_i]; // True, assign data to _BMH08002_4_recBuf[]
          }
          BMH08002_4_result = true;
          break; // Exit "while (failCnt < 2)" loop
        }
        else
        {
          BMH08002_4_failCnt++; // Error, failCnt plus 1, return "while (failCnt < 2)" loop
          BMH08002_4_checkCode = 0;
        }
      }
    }
  }
  return BMH08002_4_result;
}

/*********************************************************************************************************//**
 * @brief  Read the 15-byte data of sent by the module
 * @param  array: The array for storing the 15-byte module information
                  (refer to datasheet for meaning of each bit)
 * @retval void
 * @others Use after isInfoAvailable()
 ************************************************************************************************************/
void BMH08002_4_readInfoPackage(uint8_t buff[])
{
	uint8_t BMH08002_4_i = 0;
  for (BMH08002_4_i = 0; BMH08002_4_i < 15; BMH08002_4_i++)
  {
    buff[BMH08002_4_i] = _BMH08002_4_recBuf[BMH08002_4_i];
  }
}

/*********************************************************************************************************//**
 * @brief  Updating waveform
 * @param  void
 * @retval result
            @arg 0 - Success
            @arg 1 - Failure 
 ************************************************************************************************************/
uint8_t BMH08002_4_updateWave()
{
    uint8_t BMH08002_4_rxBuf[8]={0};
    uint8_t BMH08002_4_txBuf[8]={0x55,0xb1,0x02,0x00,0x00,0x00,0x00,0xaa};
    BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
    BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
    _BMH08002_4_delay(60);
    if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[2] == 0x02)
    {
       return 0;
    }
    else
    {
       return 1;
    }
}

/*********************************************************************************************************//**
 * @brief  Calibration detects finger sensitivity 
 * @param  void
 * @retval result
            @arg 0 - Success
            @arg 1 - Failure 
 * @others  Place the finger in the position that needs to be sensed and keep the finger relatively stable. 
            After sending this command, the calibration can be completed. 
            The detection distance should not exceed 2CM, otherwise it will affect the measurement.  
 ************************************************************************************************************/
uint8_t BMH08002_4_calSensitivity()
{
  uint8_t BMH08002_4_rxBuf[8]={0};
  uint8_t BMH08002_4_txBuf[8]={0x55,0xb1,0x06,0x00,0x00,0x00,0x00,0xaa};
  BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
  BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
  _BMH08002_4_delay(60);
  if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[2] == 0x06)
  {
    return 0;
  }
  else
  {
    return 1;
  }
}

/*********************************************************************************************************//**
 * @brief  Gets the timed send interval
 * @param  void
 * @retval intTime - the timed send interval 
 ************************************************************************************************************/
uint16_t BMH08002_4_getTimeInterval()
{
    uint8_t BMH08002_4_rxBuf[8]={0};
    uint8_t BMH08002_4_txBuf[8]={0x55,0xb1,0x05,0x0A,0x00,0x00,0x00,0xaa};
    BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
    BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
    _BMH08002_4_delay(60);
    if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[2] == 0x05)
    {
      return (BMH08002_4_rxBuf[4]<<8) + BMH08002_4_rxBuf[5];
    }
    return 0;
}

/*********************************************************************************************************//**
 * @brief  get the ModeConfig
 * @param  void
 * @retval result
         @arg rxBuf[5] -  ModeConfig
                        0x00:Timed transmission mode, red light off when finger is detected
                        0x01:Timed transmission mode, red light on when finger is detected
                        0x02:Ask-response mode, red light off when finger is detected
                        0x03:Ask-response mode, red light on when finger is detected
                        0x04:Continuous upload mode, red light off when finger is detected
                        0x05:Continuous upload mode, red light on when finger is detected
                        0x06:Timed transmission mode, red light off when finger is detected
                        0x07:Timed transmission mode, red light on when finger is detected    
         @arg 8 - Failure       
 ************************************************************************************************************/
uint8_t BMH08002_4_getModeConfig()
{
    uint8_t BMH08002_4_rxBuf[8]={0};
    uint8_t BMH08002_4_txBuf[8]={0x55,0xb1,0x05,0x01,0x00,0x00,0x00,0xaa};
    BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
    BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
    _BMH08002_4_delay(60);
		
    if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[2] == 0x05)
    {
      return BMH08002_4_rxBuf[5];
    }
    else
    {
      return 8;
    }
}

/*********************************************************************************************************//**
 * @brief  set Time Interval
 * @param  Time:Time Interval,1=4ms
 * @retval result
         @arg 0 - Success
         @arg 1 - Failure
 ************************************************************************************************************/
uint8_t BMH08002_4_setTimeInterval(uint16_t Time)
{
    uint8_t BMH08002_4_Time_H = Time>>8;
    uint8_t BMH08002_4_Time_L = Time &0xFF;
    uint8_t BMH08002_4_rxBuf[8]={0};
		uint8_t BMH08002_4_txBuf[8]={0};
		BMH08002_4_txBuf[0] = 0x55;
		BMH08002_4_txBuf[1] = 0xb1;
		BMH08002_4_txBuf[2] = 0x04;
		BMH08002_4_txBuf[3] = 0x0A;
		BMH08002_4_txBuf[4] = BMH08002_4_Time_H;
		BMH08002_4_txBuf[5] = BMH08002_4_Time_L;
		BMH08002_4_txBuf[6] = 0x00;
		BMH08002_4_txBuf[7] = 0xaa;
    BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
    BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
    _BMH08002_4_delay(60);
    if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[2] == 0x04)
    {
      return 0;
    }
    else
    {
      return 1;
    }
}

/*********************************************************************************************************//**
 * @brief  set Mode Config
 * @param  mode_code:   
             @arg 0x00:Timed transmission mode, red light off when finger is detected
             @arg 0x01:Timed transmission mode, red light on when finger is detected
             @arg 0x02:Ask-response mode, red light off when finger is detected
             @arg 0x03:Ask-response mode, red light on when finger is detected
             @arg 0x04:Continuous upload mode, red light off when finger is detected
             @arg 0x05:Continuous upload mode, red light on when finger is detected
             @arg 0x06:Timed transmission mode, red light off when finger is detected
             @arg 0x07:Timed transmission mode, red light on when finger is detected   
 * @retval result
         @arg 0 - Success
         @arg 1 - Failure
 ************************************************************************************************************/
uint8_t BMH08002_4_setModeConfig(uint8_t mode_code)
{
  if(0x00 < mode_code  && mode_code > 0x0F)//CTRL_LOGIC=0
  {
      return 1;
  }
   return BMH08002_4_writeEEPROM(0x01,0x00,mode_code);
}
	 

/* Private functions ---------------------------------------------------------------------------------------*/
/*********************************************************************************************************//**
 * @brief  setWorkStatus
 * @param  workStatus
         @arg 0: Start measuring
         @arg 1: End measurement
         @arg 2: Go to sleep
 * @retval result
         @arg 0 - Success
         @arg 1 - Failure
 ************************************************************************************************************/
uint8_t BMH08002_4_setWorkStatus(uint8_t workStatus)
{
  uint8_t BMH08002_4_rxBuf[8]={0};
	uint8_t BMH08002_4_txBuf[8]={0};
	BMH08002_4_txBuf[0] = 0x55;
	BMH08002_4_txBuf[1] = 0xb1;
	BMH08002_4_txBuf[2] = 0x01;
	BMH08002_4_txBuf[3] = 0x00;
	BMH08002_4_txBuf[4] = 0x00;
	BMH08002_4_txBuf[5] = workStatus;
	BMH08002_4_txBuf[6] = 0x00;
	BMH08002_4_txBuf[7] = 0xaa;
  BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
  BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
  _BMH08002_4_delay(60);
  if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[5] == workStatus)
  {
     return 0;
  }
  else
  {
     return 1;
  }
}

/*********************************************************************************************************//**
 * @brief  get the checksum
 * @param  The checksum are stored in txBuf[lenght-2]
 * @retval void
 ************************************************************************************************************/
void BMH08002_4_getChecksum(uint8_t buff[],uint8_t lenght)
{
	uint8_t BMH08002_4_i = 0;
  for (BMH08002_4_i = 1; BMH08002_4_i < (lenght - 2); BMH08002_4_i++)
  {
    buff[lenght - 2] +=buff[BMH08002_4_i];
  }
}

/*********************************************************************************************************//**
 * @brief  Write EEPROM
 * @param  addr:EEPROM address,write value
           highByte:high Byte
           lowByte:low Byte
 * @retval result
             @arg 1£ºwrite fail 
             @arg 0£ºWrite success
 * @others data_HighByte Writes the EEPROM address addr
           data_LowByte Writes the EEPROM address addr+1
 ************************************************************************************************************/
uint8_t BMH08002_4_writeEEPROM(uint8_t addr,uint8_t highByte,uint8_t lowByte)
{
  uint8_t BMH08002_4_rxBuf[8]={0};
	uint8_t BMH08002_4_txBuf[8]={0};
	BMH08002_4_txBuf[0] = 0x55;
	BMH08002_4_txBuf[1] = 0xb1;
	BMH08002_4_txBuf[2] = 0x04;
	BMH08002_4_txBuf[3] = addr;
	BMH08002_4_txBuf[4] = highByte;
	BMH08002_4_txBuf[5] = lowByte;
	BMH08002_4_txBuf[6] = 0x00;
	BMH08002_4_txBuf[7] = 0xaa;
  BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
  BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
  _BMH08002_4_delay(60);
  if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[1] == 0xB1 && BMH08002_4_rxBuf[2] == 0x04)
  {
    return 0;
  }
  else
  {
    return 1;
  }
}

/*********************************************************************************************************//**
 * @brief  Read EEPROM
 * @param  EEPROM address    
 * @retval EEPROM value
 * @others HighByte is the data for addr
           LowByte is the data for addr+1
 ************************************************************************************************************/
uint16_t BMH08002_4_readEEPROM(uint8_t addr)
{
  uint8_t BMH08002_4_rxBuf[8]={0};
	uint8_t BMH08002_4_txBuf[8]={0};
	uint16_t BMH08002_4_temp=0;
	BMH08002_4_txBuf[0] = 0x55;
	BMH08002_4_txBuf[1] = 0xb1;
	BMH08002_4_txBuf[2] = 0x05;
	BMH08002_4_txBuf[3] = addr;
	BMH08002_4_txBuf[4] = 0x00;
	BMH08002_4_txBuf[5] = 0x00;
	BMH08002_4_txBuf[6] = 0x00;
	BMH08002_4_txBuf[7] = 0xaa;

  BMH08002_4_getChecksum(BMH08002_4_txBuf,8);
  BMH08002_4_writeBytes(BMH08002_4_txBuf, 8);
  _BMH08002_4_delay(60);
  if (BMH08002_4_readBytes(BMH08002_4_rxBuf, 8, 30)==0x00 && BMH08002_4_rxBuf[0]==0x55 && BMH08002_4_rxBuf[7] == 0xAA && BMH08002_4_rxBuf[1] == 0xB1)
  {
      BMH08002_4_temp = BMH08002_4_rxBuf[4] << 8 | BMH08002_4_rxBuf[5];
  }
	
  return BMH08002_4_temp;
}

/*********************************************************************************************************//**
 * @brief  Write data through uart
 * @param  wbuf:The array for storing Data to be sent
           wlen:Length of data sent  
 * @retval void
 ************************************************************************************************************/
void BMH08002_4_writeBytes(uint8_t wbuf[], uint8_t wlen)
{
	  UARTM_DiscardReadBuffer(gBMH08002_4_SERIAL);
	  UARTM_Write(gBMH08002_4_SERIAL,wbuf,wlen);
}

/*********************************************************************************************************//**
 * @brief  Read data through UART
 * @param  rbuf: Used to store received data
           rlen: Length of data to be read
           timeout£ºtimeout time
 * @retval 0(BMH08002_4_CHECK_OK):OK
           1(BMH08002_4_CHECK_ERROR):CHECK_ERROR
           2(BMH08002_4_TIMEOUT_ERROR): timeout error
 ************************************************************************************************************/
uint8_t BMH08002_4_readBytes(uint8_t rbuf[], uint8_t rlen, uint16_t timeout)
{
	uint8_t _lenCount=0,_delayCount=0,BMH08002_4_checkSum=0;
  for(_lenCount = 0; _lenCount < rlen; _lenCount++)
	{
	 _delayCount = 0;
		while(UARTM_GetReadBufferLength(gBMH08002_4_SERIAL) == 0)
		{
			if(_delayCount > timeout)
			{
				return BMH08002_4_TIMEOUT_ERROR;
			}
			_BMH08002_4_delay(1);
			_delayCount++;
		}
		UARTM_ReadByte(gBMH08002_4_SERIAL,rbuf+_lenCount);
	}
	/* check Sum */
  for (_lenCount = 1; _lenCount < (rlen - 2); _lenCount++)
  {
    BMH08002_4_checkSum += rbuf[_lenCount];
  }
  if (BMH08002_4_checkSum == rbuf[rlen - 2])
  {
    return BMH08002_4_CHECK_OK; // Check correct
  }
  else
  {
    return BMH08002_4_CHECK_ERROR; // Check error
  }
}

/*********************************************************************************************************//**
 * @brief  delay ms.
 * @param  count
 * @retval void
 ************************************************************************************************************/
void _BMH08002_4_delay(vu32 count)
{
  count = SystemCoreClock / 8000 * count;
  while(count--);
}




