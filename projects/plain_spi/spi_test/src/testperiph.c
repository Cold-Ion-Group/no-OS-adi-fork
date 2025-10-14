/*
 *
 * Xilinx, Inc.
 * XILINX IS PROVIDING THIS DESIGN, CODE, OR INFORMATION "AS IS" AS A 
 * COURTESY TO YOU.  BY PROVIDING THIS DESIGN, CODE, OR INFORMATION AS
 * ONE POSSIBLE   IMPLEMENTATION OF THIS FEATURE, APPLICATION OR 
 * STANDARD, XILINX IS MAKING NO REPRESENTATION THAT THIS IMPLEMENTATION 
 * IS FREE FROM ANY CLAIMS OF INFRINGEMENT, AND YOU ARE RESPONSIBLE 
 * FOR OBTAINING ANY RIGHTS YOU MAY REQUIRE FOR YOUR IMPLEMENTATION
 * XILINX EXPRESSLY DISCLAIMS ANY WARRANTY WHATSOEVER WITH RESPECT TO 
 * THE ADEQUACY OF THE IMPLEMENTATION, INCLUDING BUT NOT LIMITED TO 
 * ANY WARRANTIES OR REPRESENTATIONS THAT THIS IMPLEMENTATION IS FREE 
 * FROM CLAIMS OF INFRINGEMENT, IMPLIED WARRANTIES OF MERCHANTABILITY 
 * AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/*
 * 
 *
 * This file is a generated sample test application.
 *
 * This application is intended to test and/or illustrate some 
 * functionality of your system.  The contents of this file may
 * vary depending on the IP in your system and may use existing
 * IP driver functions.  These drivers will be generated in your
 * SDK application project when you run the "Generate Libraries" menu item.
 *
 */

#include <stdio.h>
#include "xparameters.h"
#include "xil_cache.h"
#include "xintc.h"
#include "intc_header.h"
#include "xaxiethernet.h"
#include "xaxiethernet_example.h"
#include "xaxidma.h"
#include "axiethernet_intr_header.h"
#include "axidma_header.h"
#include "xgpio.h"
#include "gpio_header.h"
#include "iic_header.h"
#include "xspi.h"
#include "spi_header.h"
#include "spi_intr_header.h"
#include "xtmrctr.h"
#include "tmrctr_header.h"
#include "tmrctr_intr_header.h"
#include "uartlite_header.h"
int main () 
{
   static XIntc intc;
   static XAxiEthernet axi_ethernet_AxiEthernet;
   static XAxiDma  axi_ethernet_AxiDma;


   static XSpi axi_spi_Spi;
   static XTmrCtr axi_timer_Timer;
   Xil_ICacheEnable();
   Xil_DCacheEnable();
   print("---Entering main---\n\r");

while(1)
   {
      int status;

      print("\r\n Running IntcSelfTestExample() for axi_intc...\r\n");

      status = IntcSelfTestExample(XPAR_AXI_INTC_DEVICE_ID);

      if (status == 0) {
         print("IntcSelfTestExample PASSED\r\n");
      }
      else {
         print("IntcSelfTestExample FAILED\r\n");
      }
   }

   {
       int Status;

       Status = IntcInterruptSetup(&intc, XPAR_AXI_INTC_DEVICE_ID);
       if (Status == 0) {
          print("Intc Interrupt Setup PASSED\r\n");
       }
       else {
         print("Intc Interrupt Setup FAILED\r\n");
      }
   }


   /* AxiEthernetPolledExample does not support AXI DMA   */

   {
      int Status;

      print("\r\nRunning AxiEthernetSgDmaIntrExample() for axi_ethernet...\r\n");

      Status = AxiEthernetSgDmaIntrExample(&intc, &axi_ethernet_AxiEthernet,
                     &axi_ethernet_AxiDma,
                     XPAR_AXI_ETHERNET_DEVICE_ID,
                     XPAR_AXI_ETHERNET_DMA_DEVICE_ID,
                     XPAR_AXI_INTC_AXI_ETHERNET_INTERRUPT_INTR,
                     XPAR_AXIETHERNET_0_CONNECTED_DMARX_INTR,
                     XPAR_AXIETHERNET_0_CONNECTED_DMATX_INTR);

      if (Status == 0) {
         print("AxiEthernet Interrupt Test PASSED.\r\n");
      }
      else {
         print("AxiEthernet Interrupt Test FAILED.\r\n");
      }

   }



   {
      int status;


      print("\r\n Running AxiDMASelfTestExample() for axi_ethernet_dma...\r\n");

      status = AxiDMASelfTestExample(XPAR_AXI_ETHERNET_DMA_DEVICE_ID);

      if (status == 0) {
         print("AxiDMASelfTestExample PASSED\r\n");
      }
      else {
         print("AxiDMASelfTestExample FAILED\r\n");
      }
   }



   {
      int status;


      print("\r\n Running IicSelfTestExample() for axi_iic_main...\r\n");

      status = IicSelfTestExample(XPAR_AXI_IIC_MAIN_DEVICE_ID);

      if (status == 0) {
         print("IicSelfTestExample PASSED\r\n");
      }
      else {
         print("IicSelfTestExample FAILED\r\n");
      }
   }



   {
      XStatus status;
      
      print("\r\n Running SpiSelfTestExample() for axi_spi...\r\n");
      
      status = SpiSelfTestExample(XPAR_AXI_SPI_DEVICE_ID);
      
      if (status == 0) {
         print("SpiSelfTestExample PASSED\r\n");
      }
      else {
         print("SpiSelfTestExample FAILED\r\n");
      }
   }
    {
       XStatus Status;

       print("\r\n Running Interrupt Test for axi_spi...\r\n");

       Status = SpiIntrExample(&intc, &axi_spi_Spi, \
                                XPAR_AXI_SPI_DEVICE_ID, \
                                XPAR_AXI_INTC_AXI_SPI_IP2INTC_IRPT_INTR);
      if (Status == 0) {
         print("Spi Interrupt Test PASSED\r\n");
      } 
      else {
         print("Spi Interrupt Test FAILED\r\n");
      }

    }



   {
      int status;
      
      print("\r\n Running TmrCtrSelfTestExample() for axi_timer...\r\n");
      
      status = TmrCtrSelfTestExample(XPAR_AXI_TIMER_DEVICE_ID, 0x0);
      
      if (status == 0) {
         print("TmrCtrSelfTestExample PASSED\r\n");
      }
      else {
         print("TmrCtrSelfTestExample FAILED\r\n");
      }
   }
   {
      int Status;

      print("\r\n Running Interrupt Test  for axi_timer...\r\n");
      
      Status = TmrCtrIntrExample(&intc, &axi_timer_Timer, \
                                 XPAR_AXI_TIMER_DEVICE_ID, \
                                 XPAR_AXI_INTC_AXI_TIMER_INTERRUPT_INTR, 0);
	
      if (Status == 0) {
         print("Timer Interrupt Test PASSED\r\n");
      } 
      else {
         print("Timer Interrupt Test FAILED\r\n");
      }

   }


   /*
    * Peripheral SelfTest will not be run for axi_uart
    * because it has been selected as the STDOUT device
    */




   {
      int status;
      
      print("\r\nRunning UartLiteSelfTestExample() for sys_mb_debug...\r\n");
      status = UartLiteSelfTestExample(XPAR_SYS_MB_DEBUG_DEVICE_ID);
      if (status == 0) {
         print("UartLiteSelfTestExample PASSED\r\n");
      }
      else {
         print("UartLiteSelfTestExample FAILED\r\n");
      }
      
   }


   print("---Exiting main---\n\r");
   Xil_DCacheDisable();
   Xil_ICacheDisable();
   return 0;
}
