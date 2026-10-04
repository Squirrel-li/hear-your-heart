/*********************************************************************************************************//**
 * @file    BMH08002_4_HT32/example_readDataInfo/main.c
 * @version V1.0.1
 * @date    2024-09-01
 * @brief   Main program.
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
uint8_t Mode=0;
uint8_t rBuf[15]={0};
uint8_t Status=0;
uint8_t flag=0;

/* Private variables ---------------------------------------------------------------------------------------*/
/* Global functions ----------------------------------------------------------------------------------------*/
void Mode_ask(void);
void Mode_continuous_timing(void);
void _delay(vu32 count);

/*********************************************************************************************************//**
  * @brief  Main program.
  * @retval None
  ***********************************************************************************************************/
int main(void)
{
	RETARGET_Configuration();           //Initialise the serial monitor with a baud rate of 115200
	BMH08002_4_Init();                  // Module initialization
	BMH08002_4_setModeConfig(0x02);     //Query response mode, red light on when finger is detected
	BMH08002_4_setTimeInterval(300);
	printf("Please place your finger.\r\n");
	_delay(2000);                       //Wait for finger placement
	BMH08002_4_beginMeasure();
	Mode = BMH08002_4_getModeConfig();
	if(Mode == 0x02 || Mode == 0x03)
	{
		Mode = 1;
	}
	else Mode = 0;
	while(1)
	{
		switch(Mode)
		{
			case 1:
				Mode_ask();
				break;
			default:
				Mode_continuous_timing();
		}
	}
	
}

/*********************************************************************************************************//**
 * @brief  Mode_ask
 * @param  void
 * @retval void
 ************************************************************************************************************/
void Mode_ask (void)
{
    Status= BMH08002_4_requestInfoPackage(rBuf);
    if (Status==0x02)
    {
      printf("Measurement completed,Can remove fingers \r\n"); 
      printf("SpO2:"); 
			printf("%d ",rBuf[0]);
			printf("'/. \r\n"); 
			
      printf("Heart rate:"); 
			printf("%d ",rBuf[1]);
      printf("BMP \r\n"); 
			
      printf("PI:"); 
			printf("%.1f ",(float)rBuf[2]/10);
      printf("'/.  \r\n"); 
			
      BMH08002_4_endMeasure(); //stop Measure
      BMH08002_4_sleep();      //enter Halt
    }
    if (Status==0x01 && flag!=1)
    {
        printf("Don't move your fingers \r\n");
        flag=1;
    }
    if (Status==0x00 && flag!=0)
    {
        printf("Please reposition your fingers \r\n");
        flag=0;
    }
}

/*********************************************************************************************************//**
 * @brief  Mode_continuous_timing
 * @param  void
 * @retval void
 ************************************************************************************************************/
void Mode_continuous_timing(void)
{    
	uint8_t BMH08002_4_i=0;
  if (BMH08002_4_isInfoAvailable() == true) // Scaning the serial port received buffer to receive the information sent by the module
  {
    BMH08002_4_readInfoPackage(rBuf);
    for(BMH08002_4_i=0;BMH08002_4_i<15;BMH08002_4_i++)
    {
      printf("%d ",BMH08002_4_i);
      printf(":");
			printf("%d \r\n",rBuf[BMH08002_4_i]); 
    }
		
  }
}

/*********************************************************************************************************//**
 * @brief  delay ms.
 * @param  count
 * @retval void
 ************************************************************************************************************/
void _delay(vu32 count)
{
  count = SystemCoreClock / 8000 * count;
  while(count--);
}

