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
*			Button implementation for STM3240G-EVAL.
* \author
*			Robert Budde <robert.budde@tu-dortmund.de>
*/
/*---------------------------------------------------------------------------*/

#include <stm32f4xx.h>                  /* STM32F4xx Definitions              */
#include "contiki-conf.h"
#include "dev/button-sensor.h"
#include "nvic.h"

/*---------------------------------------------------------------------------*/
static void init(void) {
  /* Configure GPIO for BUTTONSs */

  /* Enable clock for GPIOG */
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN;
  /* Enable clock for SYSCFG */
	RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
	
  /* Configure KEY/User-Button (PG15) pin as pull-up input */
  GPIOG->MODER &= ~GPIO_MODER_MODER15;
	GPIOG->PUPDR &= ~GPIO_PUPDR_PUPDR15;
	GPIOG->PUPDR |= GPIO_PUPDR_PUPDR15_0;
	
	SYSCFG->EXTICR[3] &= ~SYSCFG_EXTICR4_EXTI15;		/* clear EXTI15-mapping */
	SYSCFG->EXTICR[3] |= SYSCFG_EXTICR4_EXTI15_PG;  /* map to PG15 */

	EXTI->IMR &= ~EXTI_IMR_MR15;		/* disable interrupt */
	EXTI->EMR &= ~EXTI_EMR_MR15;	/* disable event */

	EXTI->RTSR &= ~EXTI_RTSR_TR15;	/* disable rising trigger */
	EXTI->FTSR |= EXTI_FTSR_TR15;	/* enable falling trigger */
	
//	EXTI->IMR |= EXTI_IMR_MR15;		/* enable interrupt */

	IRQ_init_enable(EXTI15_10_IRQn,0x0f,0x0f);
}

static void activate(void) {
	/* clear EXTI pending bit if set */
	EXTI->PR = EXTI_PR_PR15;
	/* enable interrupt */
	EXTI->IMR |= EXTI_IMR_MR15;
}

static void deactivate(void) {
	/* disable interrupt */
	EXTI->IMR &= ~EXTI_IMR_MR15;
}

static int active(void) {
  return (EXTI->IMR & EXTI_IMR_MR15) ? 1 : 0;
}

static int value(int type) {
  if (!active()) {
    return 0;
  }

  if ((GPIOG->IDR & GPIO_IDR_IDR_15) == 0) {
    sensors_changed(&button_sensor);
    return 1;
  }
  else {
    return 0;
  }
}

/*---------------------------------------------------------------------------*/
static int configure(int type, int value) {
  switch(type){
    case SENSORS_HW_INIT:
      init();
      return 1;
    case SENSORS_ACTIVE:
      if(value)        
        activate();
      else
        deactivate();
      return 1;
  }
       
  return 0;
}
/*---------------------------------------------------------------------------*/
static int status(int type) {
  switch(type) {
    case SENSORS_READY:
      return active();
  }
  
  return 0;
}

void EXTI15_10_IRQHandler(void)
{
  ENERGEST_ON(ENERGEST_TYPE_IRQ);

	/* check EXTI pending bit and interrupt mask */
	if (value(0)) {	
#if DEBOUNCE
    if(timer_expired(&debouncetimer)) {
      timer_set(&debouncetimer, CLOCK_SECOND / 5);
      sensors_changed(&button_sensor);
    }
#else
    sensors_changed(&button_sensor);
#endif
		/* clear EXTI pending bit */
		EXTI->PR = EXTI_PR_PR15;
	}
	
  ENERGEST_OFF(ENERGEST_TYPE_IRQ);  
}

/*---------------------------------------------------------------------------*/
SENSORS_SENSOR(button_sensor, BUTTON_SENSOR,
	       value, configure, status);

