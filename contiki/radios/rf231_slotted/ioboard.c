/*
 * Copyright (c) 2012, TU Dortmund University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is part of the Contiki OS
 *
 */
/*---------------------------------------------------------------------------*/
/**
* \file
*			IO-Board specific implementation for STM32F4-Discovery
* \author
*			Robert Budde <robert.budde@tu-dortmund.de>
*/
/*---------------------------------------------------------------------------*/

#include <stm32f4xx.h>                  /* STM32F4xx Definitions              */
#include "ioboard.h"
#include "contiki-conf.h"

/*---------------------------------------------------------------------------*/
void ioboard_init(void) {   
  /* Enable clock for GPIOE                                  */
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN ;

  GPIOE->OSPEEDR = 0xffffffff; //set to value
  GPIOE->MODER = 0; //set to 0
	
  /* Configure LED (PE0-PE7) pins as push-pull outputs */
  GPIOE->MODER  |=  0x00005555; //set to value
  GPIOE->OTYPER &= ~0x000000ff; //set to 0 (PP)
	
	/* Configure BUTTONS (PE8-PE15) pins as inputs with pull-up */
	GPIOE->PUPDR  &= ~0xffff0000; //set to 0
	GPIOE->PUPDR  |=  0x55550000; //set to "01" (pull-up)
	
	ioboard_leds_off(0xff);
}

unsigned char ioboard_leds_get(void)
{
	return (GPIOE->ODR & 0xff);
}

void ioboard_leds_set(unsigned char leds)
{
	ioboard_leds_on(leds);
	ioboard_leds_off(~leds);
}

void ioboard_leds_on(unsigned char leds)
{
	GPIOE->BSRRH = leds;
}

void ioboard_leds_off(unsigned char leds)
{
	GPIOE->BSRRL = leds;
}

void ioboard_leds_toggle(unsigned char leds)
{
	GPIOE->ODR ^= (GPIOE->ODR & 0x00ff);
}

unsigned char ioboard_buttons_get(void)
{
	return (~(GPIOE->IDR >> 8));
}
