/*
 * main implementation: use this 'C' sample to create your own application
 *
 */

#include "ARMCM0plus.h"
#include "derivative.h" /* include peripheral declarations */
#include "bme.h"

#include "cpu/clock.h"
#include "cpu/systick.h"
#include "cpu/delay.h"
#include "comm/uart.h"
#include "comm/buffer.h"
#include "comm/io.h"

#include "i2c/i2c.h"
#include "inertial/mma8451q.h"

#include "nice_names.h"

void setup_gpios_for_led()
{
	// Set system clock gating to enable gate to port B
	SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK | SIM_SCGC5_PORTD_MASK;
	
	// Set Port B, pin 18 and 19 to GPIO mode
	PORTB->PCR[18] = PORT_PCR_MUX(1) | PORT_PCR_DSE_MASK; /* not using |= assignment here due to some of the flags being undefined at reset */
	PORTB->PCR[19] = PORT_PCR_MUX(1) | PORT_PCR_DSE_MASK;
	
	// Set Port d, pin 1 GPIO mode
	PORTD->PCR[1] = PORT_PCR_MUX(1) | PORT_PCR_DSE_MASK;
		
	// Data direction for port B, pin 18 and 19 and port D, pin 1 set to output 
	GPIOB->PDDR |= GPIO_PDDR_PDD(1<<18) | GPIO_PDDR_PDD(1<<19);
	GPIOD->PDDR |= GPIO_PDDR_PDD(1<<1);
	
	// LEDs are low active
	GPIOB->PCOR  = 1<<18; // clear output to light red LED
	GPIOB->PCOR  = 1<<19; // clear output to light green LED
	GPIOD->PSOR  = 1<<1;  // set output to clear blue LED
}

#define UART_RX_BUFFER_SIZE	(32)
#define UART_TX_BUFFER_SIZE	(32)
uint8_t uartInputData[UART_RX_BUFFER_SIZE], 
		uartOutputData[UART_TX_BUFFER_SIZE];
buffer_t uartInputFifo, 
		uartOutputFifo;

#define MMA8451Q_INT1_PIN	14
#define MMA8451Q_INT2_PIN	15

/**
 * @brief Indicates that polling the MMA8451Q is required
 */
static volatile uint8_t poll_mma8451q = 0;

/**
 * @brief Handler for interrupts on port A
 */
void PORTA_IRQHandler()
{
	register uint32_t fromMMA8451Q = (PORTA->ISFR & ((1 << MMA8451Q_INT1_PIN) | (1 << MMA8451Q_INT2_PIN)));
	if (fromMMA8451Q)
	{
		poll_mma8451q = 1;
		
		/* clear interrupts using BME decorated logical OR store 
		 * PORTA->ISFR |= (1 << MMA8451Q_INT1_PIN) | (1 << MMA8451Q_INT2_PIN); 
		 */
		BME_OR_W(&PORTA->ISFR, (1 << MMA8451Q_INT1_PIN) | (1 << MMA8451Q_INT2_PIN));
	}
}

int main(void)
{
	InitClock();
	InitSysTick();
	
	setup_gpios_for_led();
	
	I2C_Init();
	I2C_ResetBus();
	
	InitUart0();

	/* setting PTC8/9 to I2C0 for wire sniffing */
	SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK; /* clock to gate C */
	PORTC->PCR[8] = PORT_PCR_MUX(2) | ((1 << PORT_PCR_PE_SHIFT) | PORT_PCR_PE_MASK); /* SCL: alternative 2 with pull-up enabled */
	PORTC->PCR[9] = PORT_PCR_MUX(2) | ((1 << PORT_PCR_PE_SHIFT) | PORT_PCR_PE_MASK); /* SDA_ alternative 2 with pull-up enabled */
	
	/* initialize UART fifos */
	RingBuffer_Init(&uartInputFifo, &uartInputData, UART_RX_BUFFER_SIZE);
	RingBuffer_Init(&uartOutputFifo, &uartOutputData, UART_TX_BUFFER_SIZE);
	
	/* initialize UART0 interrupts */
	Uart0_InitializeIrq(&uartInputFifo, &uartOutputFifo);
	Uart0_EnableReceiveIrq();
	
	IO_SendZString("MMA8451Q");
		
	
	mma8451q_confreg_t configuration;
	MMA8451Q_FetchConfiguration(&configuration);
	
	
	/*
	uint8_t accelerometer = MMA8451Q_WhoAmI();
	IO_SendByte(accelerometer);
	*/
	
	/* configure interrupts for accelerometer */
	/* INT1_ACCEL is on PTA14, INT2_ACCEL is on PTA15 */
	SIM->SCGC5 |= (1 << SIM_SCGC5_PORTA_SHIFT) & SIM_SCGC5_PORTA_SHIFT; /* power to the masses */
	PORTA->PCR[MMA8451Q_INT1_PIN] = PORT_PCR_MUX(0x1) | PORT_PCR_IRQC(0b1010) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK; /* interrupt on falling edge, pull-up for open drain/active low line */
	PORTA->PCR[MMA8451Q_INT2_PIN] = PORT_PCR_MUX(0x1) | PORT_PCR_IRQC(0b1010) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK; /* interrupt on falling edge, pull-up for open drain/active low line */
	GPIOA->PDDR &= ~(GPIO_PDDR_PDD(1<<14) | GPIO_PDDR_PDD(1<<15));
	
	/* prepare interrupts for pin change / PORTA */
	NVIC_ICPR |= 1 << 30;	/* clear pending flag */
	NVIC_ISER |= 1 << 30;	/* enable interrupt */
	
	/* configure accelerometer */
	MMA8451Q_EnterPassiveMode();
	MMA8451Q_SetSensitivity(MMA8451Q_SENSITIVITY_2G, MMA8451Q_HPO_DISABLED);
	MMA8451Q_SetDataRate(MMA8451Q_DATARATE_1p5Hz, MMA8451Q_LOWNOISE_ENABLED);
	MMA8451Q_SetOversampling(MMA8451Q_OVERSAMPLING_HIGHRESOLUTION);
	MMA8451Q_ClearInterruptConfiguration();
	MMA8451Q_SetInterruptMode(MMA8451Q_INTMODE_OPENDRAIN, MMA8451Q_INTPOL_ACTIVELOW);
	MMA8451Q_ConfigureInterrupt(MMA8451Q_INT_DRDY, MMA8451Q_INTPIN_INT2);
	MMA8451Q_EnterActiveMode();
	
	MMA8451Q_FetchConfiguration(&configuration);
	
	/*
	accelerometer = MMA8451Q_SystemMode();
	IO_SendByte(accelerometer);
	
	accelerometer = MMA8451Q_Status();
	IO_SendByte(accelerometer);
	*/

	IO_SendZString("\r\n\0");

	mma8451q_acc_t acc;
	MMA8451Q_InitializeData(&acc);
	
	for(;;) 
	{
		/* lights partially  */
		GPIOB->PSOR = 1<<18;
		GPIOB->PCOR = 1<<19;
		GPIOD->PSOR = 1<<1;
		__WFI();
		
		/* as long as there is data in the buffer */
		while(!RingBuffer_Empty(&uartInputFifo))
		{
			/* light one led */
			GPIOB->PCOR = 1<<18;
			GPIOB->PSOR = 1<<19;
			GPIOD->PSOR = 1<<1;
			
			/* fetch byte */
			uint8_t data = IO_ReadByte();
			
			/* echo to output */
			IO_SendByte(data);
		}
		
		/* read accelerometer */
		if (poll_mma8451q)
		{
			poll_mma8451q = 0;
			MMA8451Q_ReadAcceleration14bitNoFifo(&acc);
			if (acc.status != 0) 
			{
				IO_SendSInt16AsString(acc.x);
				IO_SendByteUncommited(',');
				IO_SendSInt16AsString(acc.y);
				IO_SendByteUncommited(',');
				IO_SendSInt16AsString(acc.z);
				IO_SendZString("\r\n\0");
			}
		}
	}
	
	return 0;
}
