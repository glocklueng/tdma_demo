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
 * \file This file implements the higher layer of the rf231 slotted
 *       physical driver. 
 *
 * \author Philipp Spliethoff <philipp.spliethoff@tu-dortmund.de>
 */
/*---------------------------------------------------------------------------*/


#include <stdio.h>
#include <string.h>
#include "contiki.h"
#include <stm32f4xx.h> /* STM32F4xx Definitions */
#include <clock.h>
#define delay_us( us )   ( clock_delay_usec( us ) )
#include "dev/leds.h"
#include "dev/spi.h"
#include "rf231_slotted.h"
#include "net/packetbuf.h"
#include "net/rime/rimestats.h"
#include "net/netstack.h"
#include "sys/timetable.h"
#include "lib/random.h"
#include "dev/button-sensor.h"
#include "rf231_slotted_registermap.h"
#include "slotted_frame.h"

#ifdef SLOTTED_KOORDINATOR
#ifdef JITTER_SIMULATION
#warning Building for Slotted Koordinator in simulation mode
#else
#warning Building for Slotted Koordinator

#endif /* JITTER_SIMULATION */
#else
#warning Building for Slotted Client
#endif /* SLOTTED_KOORDINATOR */


/*---------------------------------------------------------------------------*/
/*****************************************************************************
 * Variable Definitions
 *****************************************************************************/
PROCESS(rf231_slotted_process, "Slotted TDMA Driver");

#ifdef SLOTTED_KOORDINATOR
static uint8_t txBuffer[BEACON_LENGTH]; /**< Koordinator dummy Paket */
#else
static uint8_t txBuffer[RESPONSE_LENGTH]; /**< Client dummy Paket */
#endif

/* Received frames are buffered to rxframe in the interrupt routine in
   hal.c */
uint8_t rxframe_head,rxframe_tail;
hal_rx_frame_t rxframe[MAX_CLIENTS];

static ring_buffer_t PeriodBuffer;          /**< A ring buffer to store the last
					         measured periods */
uint32_t lastBeaconTime;                    /**< time of the last beacon */
extern uint32_t slotTime;                   /**< Offsett of the timeslot of this
					         client */
static proto_conf_t rf231_slotted_config;   /**< contorl struct for the mac
			   		         driver */
static volatile counter; 
uint8_t sqn;                                /**< Sequence Number - only needed if
				                 the framer isn't used */

uint8_t  volatile state = RF231_STATE_UNINIT;

/* RF231 hardware delay times, from datasheet */
typedef enum{
    TIME_TO_ENTER_P_ON               = 510, /**<  Transition time from VCC is applied to P_ON - most favorable case! */
    TIME_P_ON_TO_TRX_OFF             = 510, /**<  Transition time from P_ON to TRX_OFF. */
    TIME_SLEEP_TO_TRX_OFF            = 880, /**<  Transition time from SLEEP to TRX_OFF. */
    TIME_RESET                       = 6,   /**<  Time to hold the RST pin low during reset */
    TIME_ED_MEASUREMENT              = 140, /**<  Time it takes to do a ED measurement. */
    TIME_CCA                         = 140, /**<  Time it takes to do a CCA. */
    TIME_PLL_LOCK                    = 150, /**<  Maximum time it should take for the PLL to lock. */
    TIME_FTN_TUNING                  = 25,  /**<  Maximum time it should take to do the filter tuning. */
    TIME_NOCLK_TO_WAKE               = 6,   /**<  Transition time from *_NOCLK to being awake. */
    TIME_CMD_FORCE_TRX_OFF           = 1,   /**<  Time it takes to execute the FORCE_TRX_OFF command. */
    TIME_TRX_OFF_TO_PLL_ACTIVE       = 180, /**<  Transition time from TRX_OFF to: RX_ON, PLL_ON, TX_ARET_ON and RX_AACK_ON. */
    TIME_STATE_TRANSITION_PLL_ACTIVE = 1,   /**<  Transition time from PLL active state to another. */
}radio_trx_timing_t;


/*---------------------------------------------------------------------------*/
/******************************************************************************
 * Prototypes
 ******************************************************************************/
 int rf231_init(void);
 int rf231_on(void);
 int rf231_off(void);
 void rf231_warm_reset(void);
 void rf231_reset_state_machine(void);
 void rf231_wait_idle(void);
 bool rf231_is_ready_to_sendy(void);
 uint8_t rf231_get_trx_state(void);
 radio_status_t rf231_set_trx_state(uint8_t new_state);
 void rf231_upload_packet(unsigned short payload_len);
int create_packet(void);

static int rf231_read(void *buf, unsigned short bufsize);
static int rf231_prepare(const void *data, unsigned short len);
static int rf231_transmit(unsigned short len);
static int rf231_send(const void *data, unsigned short len);
static int rf231_receiving_packet(void);
static int rf231_pending_packet(void);
static int rf231_cca(void);

 const struct radio_driver rf231_slotted_driver =
  {
    rf231_init,
    rf231_prepare,
    rf231_transmit,
    rf231_send,
    rf231_read,
    rf231_cca,
    rf231_receiving_packet,
    rf231_pending_packet,
    rf231_on,
    rf231_off
  };
 
/*****************************************************************************
 * Radio Driver API Function definition
 *****************************************************************************/
/*----------------------------------------------------------------------------*/
/**
 * \brief on - activate the rf213 hardware This Function activates and resets
 * the at86rf231 hardware and initialises the phy driver
 *
 * \param 
 * \return void
 * 
 */
 static void
 on(void)
{
  ENERGEST_ON(ENERGEST_TYPE_LISTEN);
#ifdef RF230BB_HOOK_RADIO_ON
  RF230BB_HOOK_RADIO_ON();
#endif

  /* If radio is off (slptr high), turn it on */
  if (hal_get_slptr()) {
    ENERGEST_ON(ENERGEST_TYPE_LED_RED);

    /* SPI based radios. The wake time depends on board capacitance.  Make
     * sure the delay is long enough, as using SPI too soon will reset the
     * MCU!  Use 2x the nominal value for safety. 1.5x is not long enough
     * for Raven!
     */
    hal_set_slptr_low();
    delay_us(2*TIME_SLEEP_TO_TRX_OFF);
  }

  /* if tdma koord: go directly to send mode after activation 
   * otherwise start listening */
#ifdef SLOTTED_KOORDINATOR
  rf231_set_trx_state(PLL_ON);
  state = RF231_STATE_SEND;
#else /* SLOTTED_KOORDINATOR */
  rf231_set_trx_state(RX_AACK_ON);
  state = RF231_STATE_IDLE;
#endif /* SLOTTED_KOORDINATOR */

  rf231_wait_idle();
}


/*----------------------------------------------------------------------------*/
/**
 * \brief  turn off the radio.
 * \param  void
 * \return void 
 * 
 * This function turns the Hardware radio off. If there are any turn
 * off hooks defined they will bi executed before the radio is shut
 * down. Any ongoning transmission will be completed before the
 * hardware os turned off.
 */
 static void
off(void)
{
#ifdef RF230BB_HOOK_RADIO_OFF
  RF230BB_HOOK_RADIO_OFF();
#endif

  /* Wait for any transmission to end */
  rf231_wait_idle(); 

#if RADIOALWAYSON
/* Do not transmit autoacks when stack thinks radio is off */
  rf231_set_trx_state(RX_ON);
#else 
  /* Force the device into TRX_OFF. */   
  rf231_reset_state_machine();
#if RADIOSLEEPSWHENOFF

  /* Sleep Radio */
  hal_set_slptr_high();
  ENERGEST_OFF(ENERGEST_TYPE_LED_RED);
#endif
#endif /* RADIOALWAYSON */

   ENERGEST_OFF(ENERGEST_TYPE_LISTEN);
}


/*----------------------------------------------------------------------------*/
/**
 * \brief  Initialise the rf231 slotted driver
 * \param  void
 * \return int - 1 if the initialisation was successfull
 *               0 otherwise
 * 
 * This function initialises the rf231_slotted driver. Address
 * configuration is done and the radio is turned on and starts sending
 * beacons or listens for them (depending on KOORDINATOR or CLIENT
 * Mode)
 * 
 */
 int
rf231_init(void)
{
  uint8_t i;
  uint8_t tvers, tmanu;
  uint8_t address_0;
  /* Initialise the Config Structure */
  PeriodBuffer.PutPos = 0;
  PeriodBuffer.Count = 0;
  rf231_slotted_config.clientProcessing = 0;
  rf231_slotted_config.guardInterval = 0;
  rf231_slotted_config.Period = 0;
  rf231_slotted_config.numClients = 0;
  rf231_slotted_config.clientSlotLength = 0;
  rf231_slotted_config.beaconCount = 0;

  /* set the correct slot for a client. Use the last byte of the client number
   * and set a hardcoded offset. */
#ifndef SLOTTED_KOORDINATOR
  address_0 = *((uint8_t*)0x1FFF7A10+0);          /* read the last byte of node
						   * Number */
  if(address_0 == 0x3c){
    rf231_slotted_config.slotOffsett = TDMA_BEACON_TICKS + CLIENT_PROCESSING_TIME_TICKS;
  } else if(address_0 == 0x2a){
    rf231_slotted_config.slotOffsett = TDMA_BEACON_TICKS + CLIENT_PROCESSING_TIME_TICKS + TDMA_SLOT_TICKS;
  } else if(address_0 == 0x2b){
    rf231_slotted_config.slotOffsett = TDMA_BEACON_TICKS + CLIENT_PROCESSING_TIME_TICKS + 2 * TDMA_SLOT_TICKS;
  } else {
    rf231_slotted_config.slotOffsett = TDMA_BEACON_TICKS + CLIENT_PROCESSING_TIME_TICKS + 2 * TDMA_SLOT_TICKS;
  }
#endif /* SLOTTED_KOORDINATOR */

  state = RF231_STATE_INACTIVE;

/* set the dummy Payload with 0 bits */
#ifdef SLOTTED_KOORDINATOR
  memset(&txBuffer, 0, sizeof(uint8_t) * BEACON_LENGTH);
#else
  memset(&txBuffer, 0, sizeof(uint8_t) * RESPONSE_LENGTH);
#endif


  /* create a dummy packet */
  /* hardopded dummy packet. Its better to use the framer! see packet_create
     function below */
#ifdef SLOTTED_KOORDINATOR
  txBuffer[0]=0xa0;            /* fcf*/
  txBuffer[1]=0x06;
  txBuffer[2]=0x00;            /* sqn */
  txBuffer[3]=TDMA_PAN_ID_1;   /* src PAN ID */
  txBuffer[4]=TDMA_PAN_ID_0;
  txBuffer[5]=(*((uint8_t*)0x1FFF7A10+2));       /* src address */
  txBuffer[6]=(*((uint8_t*)0x1FFF7A10+0));
  txBuffer[7]=0x03;                              /* cycle time */
  txBuffer[8]=0xe8;
  txBuffer[9]=0x03;                              /* clients */
  txBuffer[10]=0x11;          /* Payload start */
  txBuffer[11]=0x11;
  txBuffer[12]=0x11;
  txBuffer[13]=0x11;
  txBuffer[14]=0x22;         /* PL 2*/
  txBuffer[15]=0x22;
  txBuffer[16]=0x22;
  txBuffer[17]=0x22;
  txBuffer[18]=0x33;        /* pl 3 */
  txBuffer[19]=0x33;
  txBuffer[20]=0x33;
  txBuffer[21]=0x33;
  txBuffer[22]=0x00;        /* FCS */
  txBuffer[23]=0x00;
#else
  txBuffer[0]=0xa2;            /* fcf*/
  txBuffer[1]=0x26;
  txBuffer[2]=0x00;            /* sqn */
  txBuffer[3]=TDMA_PAN_ID_1;   /* dst PAN ID */
  txBuffer[4]=TDMA_PAN_ID_0;
  txBuffer[5]=0x2e;            /* dst address of koord */
  txBuffer[6]=0x1b;
  txBuffer[7]=(*((uint8_t*)0x1FFF7A10+2));       /* dst address */
  txBuffer[8]=(*((uint8_t*)0x1FFF7A10+0));
  txBuffer[9]=0x04;                              /* payload length */
  txBuffer[10]=0x00;          /* Payload start */
  txBuffer[11]=0x22;
  txBuffer[12]=0x33;
  txBuffer[13]=0x44;
  txBuffer[14]=0x00;         /* PL 2*/
  txBuffer[15]=0x00;
#endif

  /* Wait in case VCC just applied */
  delay_us(TIME_TO_ENTER_P_ON);
  /* Initialize Hardware Abstraction Layer */
  hal_init();

  /* Set receive txBuffers empty and point to the first */
  for (i=0;i<RF230_CONF_RX_BUFFERS;i++) rxframe[i].length=0;
  rxframe_head=0;rxframe_tail=0;
  
  /* Do full rf230 Reset */
  hal_set_rst_low();
  hal_set_slptr_low();

  /* On powerup a TIME_RESET delay is needed here, however on some other MCU reset
   * (JTAG, WDT, Brownout) the radio may be sleeping. It can enter an uncertain
   * state (sending wrong hardware FCS for example) unless the full wakeup delay
   * is done.
   * Wake time depends on board capacitance; use 2x the nominal delay for safety.
   * See www.avrfreaks.net/index.php?name=PNphpBB2&file=viewtopic&t=78725
   */
  delay_us(2*TIME_SLEEP_TO_TRX_OFF);
  hal_set_rst_high();

  /* Force transition to TRX_OFF */
  hal_subregister_write(SR_TRX_CMD, CMD_FORCE_TRX_OFF);
  delay_us(TIME_P_ON_TO_TRX_OFF);
  
  /* Verify that it is a supported version */
  /* Note gcc optimizes this away if DEBUG is not set! */
  tvers = hal_register_read(RG_VERSION_NUM);
  tmanu = hal_register_read(RG_MAN_ID_0);
  
  if((tvers != RF230_REVA) && (tvers != RF230_REVB)) {
    PRINTF("rf230: Unsupported version %u\n",tvers);
  }
  if(tmanu != SUPPORTED_MANUFACTURER_ID) {
    PRINTF("rf230: Unsupported manufacturer ID %u\n",tmanu); 
  } 
  PRINTF("rf230: Version %u, ID %u\n",tvers,tmanu);
  
  rf231_warm_reset();

  process_start(&rf231_slotted_process, NULL);

  on();
  
  /* bring the radio to send or receive state and upload a packet and reset the
     timer module */
#ifdef SLOTTED_KOORDINATOR
  rf231_set_trx_state(PLL_ON);
  rf231_upload_packet(24);
#else
  rf231_set_trx_state(RX_AACK_ON);
#endif
  /* Enabel interrupts */
  hal_register_write(RG_IRQ_MASK, RF230_SUPPORTED_INTERRUPT_MASK);

#ifdef SLOTTED_KOORDINATOR
  hal_set_TX_Timer(TDMA_PERIOD_TICKS);
#endif
  hal_reset_counter();

  return 1;
}


/*----------------------------------------------------------------------------*/
/**
 * \brief  rf231_warm_reset - do a soft reset of the rf231 hardware
 * \param    
 * \return void 
 * 
 * Used to reinitialize radio parameters without losing pan and mac address,
 * channel, power, etc.
 */
void
rf231_warm_reset(void) {

  /* Configure the interrupts */
  hal_register_write(RG_IRQ_MASK, RF230_SUPPORTED_INTERRUPT_MASK);
  uint8_t status = hal_register_read(RG_IRQ_MASK);

  /* Set up number of automatic retries 0-15 (0 implies PLL_ON sends
   * instead of the extended TX_ARET mode */
  hal_subregister_write(SR_MAX_FRAME_RETRIES, 0 );
 
  /* Set up carrier sense/clear channel assesment parameters for
   * extended operating mode */
  hal_subregister_write(SR_MAX_CSMA_RETRIES, 0 );

  /* set the short address and panID */
  hal_subregister_write(SR_SHORT_ADDR_0, (*((uint8_t*)0x1FFF7A10+0)));  
  hal_subregister_write(SR_SHORT_ADDR_1, (*((uint8_t*)0x1FFF7A10+2)));  
  hal_subregister_write(SR_PAN_ID_0, TDMA_PAN_ID_0);
  hal_subregister_write(SR_PAN_ID_1, TDMA_PAN_ID_1); 

  /* set IEEE address */
  hal_subregister_write(SR_IEEE_ADDR_0, (*((uint8_t*)0x1FFF7A10+0)));
  hal_subregister_write(SR_IEEE_ADDR_1, (*((uint8_t*)0x1FFF7A10+2)));
  hal_subregister_write(SR_IEEE_ADDR_2, (*((uint8_t*)0x1FFF7A10+4)));
  hal_subregister_write(SR_IEEE_ADDR_3, 0xfe);
  hal_subregister_write(SR_IEEE_ADDR_4, 0xff);
  hal_subregister_write(SR_IEEE_ADDR_5, 0x00);
  hal_subregister_write(SR_IEEE_ADDR_6, 0x00);
  hal_subregister_write(SR_IEEE_ADDR_7, 0x02);

  /* frame version settings */
  hal_subregister_write(SR_AACK_FVN_MODE, 3);
  /* set as PAN koordiantor */
  hal_subregister_write(SR_I_AM_COORD, 1 );
  /* disable auto acknowledge mode */ 
  hal_subregister_write(SR_AACK_DIS_ACK, 0 );  
  /* enable reserved frame filtering */
  hal_subregister_write(SR_AACK_FLTR_RES_FT, 1);
  hal_subregister_write(SR_AACK_UPLD_RES_FT, 0);

  /* enable promiscious mode for testing */
  hal_subregister_write(SR_AACK_PROM_MODE, 1);

  /* Receiver sensitivity. If nonzero rf231/128rfa1 saves 0.5ma in rx mode */
  /* Not implemented on rf230 but does not hurt to write to it */
#ifdef RF230_MIN_RX_POWER
#if RF230_MIN_RX_POWER > 84
#warning rf231 power threshold clipped to -48dBm by hardware register
 hal_register_write(RG_RX_SYN, 0xf);
#elif RF230_MIN_RX_POWER < 0
#error RF230_MIN_RX_POWER can not be negative!
#endif
  hal_register_write(RG_RX_SYN, RF230_MIN_RX_POWER/6 + 1); //1-15 -> -90 to -48dBm
#endif

  /* CCA energy threshold = -91dB + 2*SR_CCA_ED_THRESH. Reset defaults to -77dB */
  /* Use RF230 base of -91;  RF231 base is -90 according to datasheet */
#ifdef RF230_CONF_CCA_THRES
#if RF230_CONF_CCA_THRES < -91
#warning
#warning RF230_CONF_CCA_THRES below hardware limit, setting to -91dBm
#warning
  hal_subregister_write(SR_CCA_ED_THRES,0);  
#elif RF230_CONF_CCA_THRES > -61
#warning
#warning RF230_CONF_CCA_THRES above hardware limit, setting to -61dBm
#warning
  hal_subregister_write(SR_CCA_ED_THRES,15);  
#else
  hal_subregister_write(SR_CCA_ED_THRES,(RF230_CONF_CCA_THRES+91)/2);  
#endif
#endif

  /* Use automatic CRC unless manual is specified */
#if RF230_CONF_CHECKSUM
  hal_subregister_write(SR_TX_AUTO_CRC_ON, 0);
#else
  hal_subregister_write(SR_TX_AUTO_CRC_ON, 1);
#endif

#if RF231_HAS_PA
  hal_subregister_write(SR_PA_EXT_EN, 1);
#endif

/* Limit tx power for testing miniature Raven mesh */
#ifdef RF230_MAX_TX_POWER
  set_txpower(RF230_MAX_TX_POWER);  //0=3dbm 15=-17.2dbm
#endif

  
}


/******************************************************************************
 * Local static Functions
 ******************************************************************************/
/*----------------------------------------------------------------------------*/
/** \brief  This function return the Radio Transceivers current state.
 *
 *  \retval     P_ON               When the external supply voltage (VDD) is
 *                                 first supplied to the transceiver IC, the
 *                                 system is in the P_ON (Poweron) mode.
 *  \retval     BUSY_RX            The radio transceiver is busy receiving a
 *                                 frame.
 *  \retval     BUSY_TX            The radio transceiver is busy transmitting a
 *                                 frame.
 *  \retval     RX_ON              The RX_ON mode enables the analog and digital
 *                                 receiver blocks and the PLL frequency
 *                                 synthesizer.
 *  \retval     TRX_OFF            In this mode, the SPI module and crystal
 *                                 oscillator are active.
 *  \retval     PLL_ON             Entering the PLL_ON mode from TRX_OFF will
 *                                 first enable the analog voltage regulator. The
 *                                 transceiver is ready to transmit a frame.
 *  \retval     BUSY_RX_AACK       The radio was in RX_AACK_ON mode and received
 *                                 the Start of Frame Delimiter (SFD). State
 *                                 transition to BUSY_RX_AACK is done if the SFD
 *                                 is valid.
 *  \retval     BUSY_TX_ARET       The radio transceiver is busy handling the
 *                                 auto retry mechanism.
 *  \retval     RX_AACK_ON         The auto acknowledge mode of the radio is
 *                                 enabled and it is waiting for an incomming
 *                                 frame.
 *  \retval     TX_ARET_ON         The auto retry mechanism is enabled and the
 *                                 radio transceiver is waiting for the user to
 *                                 send the TX_START command.
 *  \retval     RX_ON_NOCLK        The radio transceiver is listening for
 *                                 incomming frames, but the CLKM is disabled so
 *                                 that the controller could be sleeping.
 *                                 However, this is only true if the controller
 *                                 is run from the clock output of the radio.
 *  \retval     RX_AACK_ON_NOCLK   Same as the RX_ON_NOCLK state, but with the
 *                                 auto acknowledge module turned on.
 *  \retval     BUSY_RX_AACK_NOCLK Same as BUSY_RX_AACK, but the controller
 *                                 could be sleeping since the CLKM pin is
 *                                 disabled.
 *  \retval     STATE_TRANSITION   The radio transceiver's state machine is in
 *                                 transition between two states.
 */
uint8_t
rf231_get_trx_state(void)
{
    return hal_subregister_read(SR_TRX_STATUS);
}

/*----------------------------------------------------------------------------*/
/** \brief  This function will reset the state machine (to TRX_OFF) from any of
 *          its states, except for the SLEEP state.
 */
static void
rf231_reset_state_machine(void)
{
    if (hal_get_slptr()) DEBUGFLOW('"');
    hal_set_slptr_low();
    delay_us(TIME_NOCLK_TO_WAKE);
    hal_subregister_write(SR_TRX_CMD, CMD_FORCE_TRX_OFF);
    delay_us(TIME_CMD_FORCE_TRX_OFF);
}

/*---------------------------------------------------------------------------*/
static char
rf230_isidle(void)
{
  uint8_t radio_state;
  if (hal_get_slptr()) {
    DEBUGFLOW(']');
	return 1;
  } else {
  radio_state = hal_subregister_read(SR_TRX_STATUS);
  if (radio_state != BUSY_TX_ARET &&
      radio_state != BUSY_RX_AACK &&
      radio_state != STATE_TRANSITION &&
      radio_state != BUSY_RX && 
      radio_state != BUSY_TX) {
    return(1);
  } else {
//    printf(".%u",radio_state);
    return(0);
  }
  }
}

static void
rf231_wait_idle(void)
{
int i;
  for (i=0;i<10000;i++) {  //to avoid potential hangs
 // while (1) {
    if (rf230_isidle()) break;
  }
  if (i>=10000) {DEBUGFLOW('H');DEBUGFLOW('R');}
}

/*----------------------------------------------------------------------------*/
/** \brief  This function will change the current state of the radio
 *          transceiver's internal state machine.
 *
 *  \param     new_state        Here is a list of possible states:
 *             - RX_ON        Requested transition to RX_ON state.
 *             - TRX_OFF      Requested transition to TRX_OFF state.
 *             - PLL_ON       Requested transition to PLL_ON state.
 *             - RX_AACK_ON   Requested transition to RX_AACK_ON state.
 *             - TX_ARET_ON   Requested transition to TX_ARET_ON state.
 *
 *  \retval    RADIO_SUCCESS          Requested state transition completed
 *                                  successfully.
 *  \retval    RADIO_INVALID_ARGUMENT Supplied function parameter out of bounds.
 *  \retval    RADIO_WRONG_STATE      Illegal state to do transition from.
 *  \retval    RADIO_BUSY_STATE       The radio transceiver is busy.
 *  \retval    RADIO_TIMED_OUT        The state transition could not be completed
 *                                  within resonable time.
 */
radio_status_t
rf231_set_trx_state(uint8_t new_state)
{
    uint8_t original_state;
  radio_status_t set_state_status;
	
    /* Check function paramter and current state of the radio
     *  transceiver.*/
    if (!((new_state == TRX_OFF)    ||
          (new_state == RX_ON)      ||
          (new_state == PLL_ON)     ||
          (new_state == RX_AACK_ON) ||
          (new_state == TX_ARET_ON))){
        return RADIO_INVALID_ARGUMENT;
    }
	if (hal_get_slptr()) {
        return RADIO_WRONG_STATE;
    }

    /* Wait for radio to finish previous operation */
    rf231_wait_idle();
 
    original_state = rf231_get_trx_state();
    if (new_state == original_state){
        return RADIO_SUCCESS;
    }

    /* At this point it is clear that the requested new_state is:
    * TRX_OFF, RX_ON, PLL_ON, RX_AACK_ON or TX_ARET_ON. 
    *
    * The radio transceiver can be in one of the following states:
    * TRX_OFF, RX_ON, PLL_ON, RX_AACK_ON, TX_ARET_ON. */
    if(new_state == TRX_OFF){
        rf231_reset_state_machine(); /* Go to TRX_OFF from any state. */
    } else {
        /* It is not allowed to go from RX_AACK_ON or TX_AACK_ON and
	 * directly to TX_AACK_ON or RX_AACK_ON respectively. Need to
	 * go via RX_ON or PLL_ON. */
        if ((new_state == TX_ARET_ON) &&
            (original_state == RX_AACK_ON)){
            /* First do intermediate state transition to PLL_ON, then
             * to TX_ARET_ON.  The final state transition to
             * TX_ARET_ON is handled after the if-else if. */
            hal_subregister_write(SR_TRX_CMD, PLL_ON);
            delay_us(TIME_STATE_TRANSITION_PLL_ACTIVE);
        } else if ((new_state == RX_AACK_ON) &&
                 (original_state == TX_ARET_ON)){
            /* First do intermediate state transition to RX_ON, then
             * to RX_AACK_ON.  The final state transition to
             * RX_AACK_ON is handled after the if-else if. */
            hal_subregister_write(SR_TRX_CMD, RX_ON);
            delay_us(TIME_STATE_TRANSITION_PLL_ACTIVE);
        }

        /* Any other state transition can be done directly. */
        hal_subregister_write(SR_TRX_CMD, new_state);

        /* When the PLL is active most states can be reached in
         * 1us. However, from TRX_OFF the PLL needs time to
         * activate. */
        if (original_state == TRX_OFF){
            delay_us(TIME_TRX_OFF_TO_PLL_ACTIVE);
        } else {
            delay_us(TIME_STATE_TRANSITION_PLL_ACTIVE);
        }
    } /*  end: if(new_state == TRX_OFF) ... */

    /* Verify state transition. */
    set_state_status = RADIO_TIMED_OUT;

    if (rf231_get_trx_state() == new_state) {
        set_state_status = RADIO_SUCCESS;
    }

    return set_state_status;
}

/*---------------------------------------------------------------------------*/
bool
rf231_is_ready_to_sendy() {
	switch(rf231_get_trx_state()) {
		case BUSY_TX:
		case BUSY_TX_ARET:
			return false;
	}
	
	return true;
}

/*---------------------------------------------------------------------------*/
static void
flushrx(void)
{
  rxframe[rxframe_head].length=0;
}


static int create_packet(void)
{
  /* create structure to store result. */
  frame_create_params_t params;
  frame_result_t result;

  /* Build the FCF. */
#ifdef SLOTTED_KOORDINATOR
  params.fcf.frameType = BEACON_FRAME_TYPE;
  params.fcf.panIdCompression = false;
  params.fcf.destAddrMode = 0;
  params.dest_pid = 0;
  params.payload_len = BEACON_PAYLOAD_LENGTH;
#else
  params.fcf.frameType = RESPONSE_FRAME_TYPE;
  params.fcf.panIdCompression = true;
  params.fcf.destAddrMode = SHORTADDRMODE;
  params.dest_pid = TDMA_PAN_ID;
  params.dest_addr.addr16 = 0x2e1b;      /* adress of the
					 coordinator. better to read
					 from the received beacons */
  params.payload_len = MAX_RESPONSE_PAYLOAD;
#endif

/* parameter that are the same for client and coord */
  params.fcf.securityEnabled = false;
  params.fcf.framePending = false;
  params.fcf.ackRequired = false;
  params.fcf.frameVersion = TDMA_FRAME_VERSION;
  /* Increment and set the data sequence number. */
  params.seq = sqn++;
  /* Complete the addressing fields. */
  params.fcf.srcAddrMode = SHORTADDRMODE;
  params.src_pid = TDMA_PAN_ID;
  //  params.src_addr.addr16 = ;

  /* Copy the payload data. */
params.payload =  txBuffer;

#ifdef SLOTTED_KOORDINATOR
params.bhdr.cycleTime = 1000;
params.bhdr.maxClients = MAX_CLIENTS;
#endif

/* HACK:
 * FCF field ist not set correctly. set it hardcoded.
 *
 * TODO: correct FCF settings
 */
#ifdef SLOTTED_KOORDINATOR
 params.fcf.word_val = 0xa006;
#else
 params.fcf.word_val = 0xa226;
#endif

 result.frame = buffer;

  /* Create transmission frame. */
  frame_tx_create(&params, &result);

  hal_frame_write(result.frame, result.length);

}

/*---------------------------------------------------------------------------*/
static void 
ringbuffer_add(ring_buffer_t *buffer, uint32_t value)
{
  buffer->Buff[buffer->PutPos] = value;
  buffer->PutPos = ((buffer->PutPos + 1) & PERIOD_BUFFER_MASK);
  ++(buffer->PutPos);
  ++(buffer->Count);
  if(buffer->Count > PERIOD_BUFFER_LENGTH) {
    buffer->Count=PERIOD_BUFFER_LENGTH;
  }
}

void
rf231_upload_packet(unsigned short payload_len)
{
    hal_frame_write(txBuffer, payload_len);
}

/*---------------------------------------------------------------------------*/
static uint32_t
ringbuffer_get_last(ring_buffer_t *buffer)
{
  return buffer->Buff[buffer->PutPos];
}

/*---------------------------------------------------------------------------*/
static uint32_t
ringbuffer_clear(ring_buffer_t *buffer)
{
  buffer->Count = 0;
  buffer->PutPos = 0;
}



/*---------------------------------------------------------------------------*/
static int
rf231_prepare(const void *payload, unsigned short payload_len)
{
  /* leave as empty function.  
   *
   * preparations are made by the slotted process.  
   *
   * TODO Implement a meachanism to store the packets taht shall be
   * transmitted in the packet queue. At the Moment we are sending
   * empty packages only for testing */

  return 1;
}

/*---------------------------------------------------------------------------*/
static int 
rf231_transmit(unsigned short payload_len)
{

  /* leave as empty function. transmission is timed by the slotted process */


  return 1; 
}

/*---------------------------------------------------------------------------*/
static int
rf231_send(const void *payload, unsigned short payload_len)
{
  /* leave as empty function.  
   *
   * sending is made by the slotted process.
   *
   * TODO Implement a meachanism to store the packets taht shall be
   * transmitted in the packet queue. At the Moment we are sending
   * empty packages only for testing */

  return 1;
}

/*---------------------------------------------------------------------------*/
static int
rf231_read(void *buf, unsigned short bufsize)
{
  return 1;
}

/*---------------------------------------------------------------------------*/
static int
rf231_cca(void)
{
  return 1;
}

/*---------------------------------------------------------------------------*/
int
rf231_receiving_packet(void)
{
  return 0;
}

/*---------------------------------------------------------------------------*/
static int
rf231_pending_packet(void)
{
  return 0;
}

/*---------------------------------------------------------------------------*/
int
rf231_off(void)
{
  off();
  return 1;
}

/*---------------------------------------------------------------------------*/
int
rf231_on(void)
{
#ifdef SLOTTED_KOORDINATOR
  state = RF231_STATE_SEND;
#else
  state = RF231_STATE_IDLE;
#endif
  /** start the timer */
  /** bring transceiver into RX_ON */
  return 1;
}

/*----------------------------------------------------------------------------*/
/**
 * \brief  rf231_slotted_IC_irqh - Input Capture Interrupt routine
 * \param  capture  timer value of the last capture
 * \return void 
 * 
 * This Function is called when a Input capture interrupt occured and sets the
 * time of the last receiver packet.
 */
void 
rf231_slotted_IC_irqh(uint32_t capture)
{
  /* write IC value into the IC buffer and generate a IC event to process the new value*/
  /* store the new value into our ringbuffer */
  ringbuffer_add(&PeriodBuffer, capture);
  lastBeaconTime = capture;
}

/*----------------------------------------------------------------------------*/
/**
 * \brief  calculate_period - estimate the length of the last pariod
 * \return void 
 * 
 * This function estimates the period length of the beacon frame arrivals
 */
 static void 
calculate_period()
{
  int i;
  uint32_t AvgPeriod;

  if (state == RF231_STATE_ACTIVE_PLL) {
    /* substract last stored value with the first sotred and divide by number of periods */
    rf231_slotted_config.Period = (PeriodBuffer.Buff[(PeriodBuffer.PutPos) - 1] 
				   - PeriodBuffer.Buff[PeriodBuffer.PutPos])  >> NUM_PERIODS_BASE;
  } else {
    /* calculate using FIR filtering */
    rf231_slotted_config.Period = (0.95 * rf231_slotted_config.Period) 
      + (0.05 * (PeriodBuffer.Buff[(PeriodBuffer.PutPos) - 1]  - PeriodBuffer.Buff[PeriodBuffer.PutPos]));
}

/*---------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/**
 * \brief  PROCESS_THREAD - the tdma mac thread
 * \param  
 * \param  Events  
 * \param  Data  
 * \return int
 * 
 * Process to handle input packets
 * Receive interrupts cause this process to be polled
 * It calls the core MAC layer which calls rf231_read to get the packet
 * rf231processflag can be printed in the main idle loop for debugging
 * we need to handle the following EVENTS:
 *
 * INPUT_CAPTURE_EVENT
 * HANDLE_PACKET_EVENT
 * BEACON_MISSED_EVENT
 * TX_MODE_TIMER_EVENT
 * BEACON_RECEIVED_EVENT
 * FRAME_SEND_EVENT
 */
  PROCESS_THREAD(rf231_slotted_process, ev, data)
{
  PROCESS_BEGIN();

  while(1) {
    PROCESS_WAIT_EVENT();
    if (ev == INPUT_CAPTURE_EVENT){
      // hal_set_oc(lastBeaconTime + rf231_slotted_config.slotOffsett + 1000);
    }
    if(ev == HANDLE_PACKET_EVENT){
      hal_frame_read(rxframe);
      if(rxframe[0].data[0] == 0xa0) {
	/* set the next send time */
        hal_set_oc(lastBeaconTime + rf231_slotted_config.slotOffsett - HARDWARE_DELAY_TICKS);
	/* set the ioboard leds ti the received frame value */
	ioboard_leds_set(rxframe[0].data[10]);
	/* change the radio to send mode and upload the package */
	state = RF231_STATE_SEND;
	rf231_set_trx_state(PLL_ON);
	rf231_upload_packet(16);
	/* set the TX_MODE Timer to sqitch back to receive mode after the timer is expired */
	hal_set_TX_Mode_Timer(lastBeaconTime + TDMA_PERIOD_TICKS - (2 * KOORD_PROCESSING_TIME_TICKS));
	/* toggle the green LED to indicate correct state of the Protocoll */
	if (counter == 500) {
	  leds_on(LEDS_GREEN);
	  ++counter;
	} else if (counter >= 1000) {
	  leds_off(LEDS_GREEN);
	  counter = 0;
	} else {
	  ++counter;
	}
      }
    }
    if(ev==TX_MODE_TIMER_EVENT){
#ifdef SLOTTED_KOORDINATOR
      /* TX Mode Timer expired and device is a koordinator. Bring radio to send mode and upload the next
	 beacon including the button states */
      state = RF231_STATE_SEND;
      txBuffer[10] = ioboard_buttons_get();
      rf231_set_trx_state(PLL_ON);
      rf231_upload_packet(24);
      /* toggle the green LED to indicate correct state of the Protocoll */
      if (counter == 500) {
	leds_on(LEDS_GREEN);
	ioboard_leds_set(0xff);
	++counter;
      } else if (counter >= 1000) {
	leds_off(LEDS_GREEN);
	ioboard_leds_set(0x0);
	counter = 0;
      } else {
	++counter;
      }
#else /* SLOTTED_KOORDINATOR */
      /* as client set bring the radio into receive mode */
      state = RF231_STATE_IDLE;
      rf231_set_trx_state(RX_AACK_ON);
#endif /* SLOTTED_KOORDINATOR */
    }
    if(ev==FRAME_SEND_EVENT){
#ifdef SLOTTED_KOORDINATOR
      /* set radio to receive mode and set the time for the next send mode switch */
      state = RF231_STATE_IDLE;
      rf231_set_trx_state(RX_AACK_ON);
      hal_set_TX_Mode_Timer(hal_get_oc() - KOORD_PROCESSING_TIME_TICKS);
#endif
    }
    if ((ev==sensors_event) && (data == &button_sensor)){
      /* print the curretn information to usart */
      printf("The Period is : %i\n\r", rf231_slotted_config.Period);
    }
  }

  PROCESS_END();
}
