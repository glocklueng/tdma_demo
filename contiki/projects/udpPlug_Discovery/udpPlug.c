#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"

#include <string.h>
#include <stdio.h>
#include <dev/watchdog.h>
#include "dev/button-sensor.h"
#include "dev/leds.h"


#define DEBUG DEBUG_PRINT
#include "net/uip-debug.h"
#include "net/rpl/rpl.h"

#include "udpPlug.h"

#define UIP_IP_BUF   ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])

#define MY_PAYLOAD_LEN 120

#define SEND_INTERVAL		2 * CLOCK_SECOND

static struct uip_udp_conn *udp_conn;

volatile uint16_t my_temp;
volatile uint16_t my_light;

PROCESS(udp_plug_process, "UDP server process");
AUTOSTART_PROCESSES(&udp_plug_process);

/*---------------------------------------------------------------------------*/

static void tcpip_handler(void)
{
  if(uip_newdata()) {
	
    ((uint8_t *)uip_appdata)[uip_datalen()] = 0;
	
	if (uip_datalen() >= 2)
	{
		switch (*(uint8_t*)uip_appdata)
		{
			case PROFILE_ROUTING_TABLE:
				//nothing
				break;
			case PROFILE_SERVICE_DISCOVER:
				handle_service_discover_frame((uint8_t*)uip_appdata);
				break;
			case PROFILE_SENSOR_DATA:
				handle_sensor_data_frame((uint8_t*)uip_appdata);
				break;
			case PROFILE_SWITCH:
				handle_switch_frame((uint8_t*)uip_appdata);
				break;
			default:
				break;
		}
	}
	/* Restore server connection to allow data from any node */
  }


}

void plug_send_message(uip_ipaddr_t *dest, void *data, uint8_t length)
{
	uip_ipaddr_copy(&udp_conn->ripaddr, dest);
	udp_conn->rport = UIP_HTONS(PLUG_PORT);
	
	uip_udp_packet_send(udp_conn, data, length);
	
	udp_conn->rport = 0;
	memset(&udp_conn->ripaddr, 0, sizeof(udp_conn->ripaddr));
}

void handle_service_discover_frame(uint8_t *frame)
{
	uint8_t buf[4];
		if (frame[1] == REQUEST_MSG)
		{
			buf[0] = PROFILE_SERVICE_DISCOVER;
			buf[1] = REPLY_MSG;
			
			*(uint16_t *)&buf[2] = (HAS_SWITCH  << SWITCH_BIT) 	
									| (HAS_TEMP_SENSOR  << TEMP_SENSOR_BIT) 
									| (HAS_LIGHT_SENSOR  << LIGHT_SENSOR_BIT);
									
			plug_send_message(&UIP_IP_BUF->srcipaddr ,buf, 4);
		}
}

void handle_sensor_data_frame(uint8_t *frame)
{
	uint8_t buf[9];
	uint8_t length = 4;
	if (frame[1] == REQUEST_MSG)
	{
		buf[0] = PROFILE_SENSOR_DATA;
		buf[1] = REPLY_MSG;
		
		*(uint16_t *)&buf[2] = (HAS_SWITCH  << SWITCH_BIT) 	
										| (HAS_TEMP_SENSOR  << TEMP_SENSOR_BIT) 
										| (HAS_LIGHT_SENSOR  << LIGHT_SENSOR_BIT);
										
		if (HAS_SWITCH)
		{
			if (LEDS_YELLOW & leds_get())
				buf[length] = 0x01;
			else
				buf[length] = 0x00;
				
			length += 1;
		}
		
		if (HAS_TEMP_SENSOR)
		{
			*(uint16_t *)&buf[length] = my_temp;
			length += 2;
		}
		
		if (HAS_LIGHT_SENSOR)
		{
			*(uint16_t *)&buf[length] = my_light;
			length += 2;
		}
		
		plug_send_message(&UIP_IP_BUF->srcipaddr ,buf, length);
	}

}

void handle_switch_frame(uint8_t *frame)
{
	if (frame[1] == REQUEST_MSG)
	{
		switch((switch_cmd_t) frame[2])
		{
			case SWITCH_ON:
				leds_on(LEDS_YELLOW);
				break;
			case SWITCH_OFF:
				leds_off(LEDS_YELLOW);
				break;
			case SWITCH_TOGGLE:
				leds_toggle(LEDS_YELLOW);
				break;
		}
	}
}

static void timeout_handler(void)
{

#if HAS_TEMP_SENSOR
	my_temp = get_temperature();
	read_temperature();
#endif

#if HAS_LIGHT_SENSOR
	my_light = get_brightness();
	light_sensor_start_conversion();
#endif

}

PROCESS_THREAD(udp_plug_process, ev, data)
{
	static struct etimer et;
	PROCESS_BEGIN();
  
	PRINTF("UDP server started\r\n");
	
	SENSORS_ACTIVATE(button_sensor);
	
	udp_conn = udp_new(NULL, UIP_HTONS(0), NULL);
	udp_bind(udp_conn, UIP_HTONS(PLUG_PORT));
	PRINTF("listening on udp port %u\r\n",UIP_HTONS(udp_conn->lport));
	etimer_set(&et, SEND_INTERVAL);
	while(1) {
		
		PROCESS_YIELD();
		
		if(etimer_expired(&et)) {
			leds_toggle(LEDS_RED);
			timeout_handler();
			etimer_restart(&et);
		}

		if(ev == tcpip_event) {
			PRINTF("Calling tcpip_Handler\r\n");
			tcpip_handler();
		}
		
		if (ev == sensors_event && data == &button_sensor) {
			PRINTF("Button Pressed\r\n");
		}
		
	}

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
