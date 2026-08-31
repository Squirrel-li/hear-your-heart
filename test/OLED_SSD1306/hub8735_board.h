/*
┌───────────────────────────────────────────────────────┐
│				          		HUB 8735  board										│
└───────────────────────────────────────────────────────┘
More pin detail is in： 
Arduino15\packages\ideasHatch\hardware\AmebaPro2\4.0.11-Release\ameba_hub8735\variant.cpp
             
											    ┌────────┐
									┌───────┤			 	 ├───────┐
									│				│ 			 │			 │
									│				│			 	 │  	 	 │
								 1│SPK	 	└─┐ 	 ┌─┘  	 F2│28	Wire(I2C1_SDA)
	Wire1(I2C1_SDA)2│A1	   		╞════╡		   F1│27	Wire(I2C1_SCL)
	Wire1(I2C1_SCL)3│A0	   		│    │ 	 		 5V│26
								 4│A3	   		│    │			 DN│25
								 5│A2	   		│    │ 	  	 DP│24
								 6│F0	   		│    │ 	 		GND│23
									│	     		│    │ 	    	 │
								 7│F8	 		╔═╧════╧═╗		GND│22
		   					 8│F7	 		╚════════╝	 	U1R│21(F3)
								 9│F6			 HUB 8735			U1T│20(F4)
								10│F5			 		  				VCC│19
						(E2)11│U3R					   			GND│18
						(E1)12│U3T			 		  			F10│17
								13│GND		  			   		 A5│16
								14│5V									 3.3V│15
									└────────────────────────┘

*/