/*
┌───────────────────────────────────────────────────────┐
│				          HUB 8735 ultra board									│
└───────────────────────────────────────────────────────┘
More pin detail is in： 
Arduino15\packages\ideasHatch\hardware\AmebaPro2\4.0.11-Release\ameba_hub8735_ultra\variant.cpp

											 ┌────────┐
									┌────┤			 	├─────┐
									│5V	 │ 				│	3.3V│
									│GND │			 	│  	14│ (PA5)
Wire1I2C1_SDA(PF2)│0	 └─┐		┌─┘  	15│ (PD16)
Wire1I2C1_SCL(PF1)│1	   ╞════╡		 GND│
						 (PF0)│2	   │    │ 	 VCC│
Wire(I2C_SDA)(PE4)│3	   │    │			16│ (PF3)
Wire(I2C_SCL)(PE3)│4	   │    │ 	  17│ (PF4)
						 (PE2)│5	   │    │ 	 GND│	
									│	     │    │ 	    │
						 (PE1)│6	 ╔═╧════╧═╗		18│ (PF5)
						(PD17)│7	 ╚════════╝	 	19│ (PF6)	
						(PD15)│8 HUB 8735 ultra 20│ (PF7)
						 (PA2)│9			   				21│ (PF8)
						 (PA3)│10			   				22│ (PF11)
						(PF14)│11			   	┌┐┌┐	23│ (PF12)
						(PF15)│12		  	  └┘└┘	24│ (PF13)
						(PF10)│13							 SPK│
									│			     					│
									│	┌─────┐ ┌┐┌─────┐	│
									│	│ OTG │ └┘│ BURN│	│
									└─┤     ├───┤ DBG ├─┘
									  └─────┘   └─────┘
									
								Func button:  12  (PF15)
								Camera  LED:	13	(PF10)
									Green LED:  25  (PE6)
                  Blue  LED:  26  (PF9)
                
*/