#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>

#include <avr/sfr_defs.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdbool.h>

#include "serialcom.h"
#include "game.h"
#include "board.h"
#include "protocol.h"
#include "src\buffer\buffer.h"

#define COM_SEND_QUEUE_LOCK_MAX_WAIT 50
#define COM_SEND_QUEUE_SIZE MAX_FRAME_SIZE
#define COM_BUFFER_FULL_DELAY 1
//#define configUSE_COUNTING_SEMAPHORES  1
static const uint8_t _COM_RX_QUEUE_LENGTH = 30;
static QueueHandle_t _x_com_received_chars_queue = NULL;
static QueueHandle_t com_send_frame_buffer = NULL;
static SemaphoreHandle_t com_send_queue_add_sem = NULL;

//COM PROTOCOL VARS
static bool RTS = true;
static uint8_t last_sent_frame[MAX_FRAME_SIZE];
static uint8_t last_sent_frame_size = 0;
static TickType_t last_frame_time = 0;
static uint8_t ack_timeouts = 0;


void sеriаl_соm_tаsk(void *pvParameters)
{
	#if (configUSE_APPLICATION_TASK_TAG == 1)
	// Set task no to be used for tracing with R2R-Network
	//vTaskSetApplicationTaskTag( NULL, ( void * ) 3 );
	#endif

	com_send_frame_buffer = xQueueCreate(COM_SEND_QUEUE_SIZE, sizeof(uint8_t));
	com_send_queue_add_sem = xSemaphoreCreateBinary(); 
	_x_com_received_chars_queue = xQueueCreate( _COM_RX_QUEUE_LENGTH, ( unsigned portBASE_TYPE ) sizeof( uint8_t ) );
	init_com(_x_com_received_chars_queue);

	
	uint8_t frame[MAX_FRAME_SIZE];
	uint8_t frame_size = 0;

	TickType_t serial_com_task_lastwake = xTaskGetTickCount();
	
	while(1)
	{
		
		vTaskDelayUntil(&serial_com_task_lastwake, SERIAL_COM_TASK_PERIOD);
		
		
		if(get_frame_from_queue(_x_com_received_chars_queue, frame, MAX_FRAME_SIZE, &frame_size))
		{
			uint8_t data_length = 0;
			Frame_type_t type = unpack_data_frame(frame, MAX_FRAME_SIZE, &data_length);
			if (type == ACK)
			{
				
				RTS = true;
				last_sent_frame[0] = 0;
				last_sent_frame_size = 0;
				ack_timeouts = 0;
			}
			else if(type == DATA)
			{
				uint8_t ack_frame[LINK_FLAG_SIZE + HEADER_SIZE + CRC_SIZE];
				uint8_t ack_frame_size = 0;
				if ((ack_frame_size = get_acknowledge_frame(ack_frame, sizeof(ack_frame))))
				{
					//25
					if(xSemaphoreTake(com_send_queue_add_sem, 50))
					for (uint8_t i = 0; i < ack_frame_size; i++)
					{
						if (!xQueueSend(com_send_frame_buffer, ack_frame+i, 0))
						break;
					}
					xSemaphoreGive(com_send_queue_add_sem);
				}
				
				handle_serial_input(frame, data_length);
			}
			else
			{
				RTS = true; 

			}
			
			frame[0] = 0;
			frame_size = 0;
		}


		if(RTS)
		{
			
			if(get_frame_from_queue(com_send_frame_buffer, last_sent_frame, MAX_FRAME_SIZE, &last_sent_frame_size))
			{
				send_current_frame();
			}
		}
		else
		{
			if ((xTaskGetTickCount() - last_frame_time) >= ACK_TIMEOUT)
			{
				ack_timeouts++; 
				if(ack_timeouts <= MAX_CONSECUTIVE_ACK_TIMEOUTS)
				{
					send_current_frame(); 
				}
				else
				{
					
					RTS = true;
					last_sent_frame[0] = 0;
					last_sent_frame_size = 0;
					ack_timeouts = 0;
				}
			}
		}
	}
}
void send_current_frame()
{
	if(last_sent_frame_size)
	{
		
		for(uint8_t i = 0; i < last_sent_frame_size;)
		{
			if(com_send_byte(last_sent_frame[i]) == BUFFER_OK)
			i++; 
			else
			vTaskDelay(COM_BUFFER_FULL_DELAY); 
		}
		last_frame_time = xTaskGetTickCount();
		RTS = is_ack_frame(last_sent_frame, MIN_FRAME_SIZE); 
	}
}
uint8_t count_illegal_chars(uint8_t *byte_buffer, uint8_t buffer_size)
{
	uint8_t c = 0;
	for(uint8_t i = 0; i < buffer_size; i++)
	{
		if((*byte_buffer == LINK_FLAG) || (*byte_buffer == LINK_ESC))
		c++;

		byte_buffer++;
	}
	return c;
}
bool send_bytes(uint8_t *byte_buffer, uint8_t buffer_size)
{
	if (buffer_size > MAX_PAYLOAD_SIZE)
	return false;
	uint8_t *frame;
	uint8_t frame_size = buffer_size + FRAME_OVERHEAD + (count_illegal_chars(byte_buffer, buffer_size) * 2);
	if(frame_size > MAX_FRAME_SIZE)
	frame_size = MAX_FRAME_SIZE;
	
	frame = pvPortMalloc(frame_size);
	if(!frame)
	return false;
	frame_size = get_data_frame(frame, frame_size, byte_buffer, buffer_size);
	if (frame_size)
	{
		if (uxQueueSpacesAvailable(com_send_frame_buffer) >= frame_size)
		{
			if(xSemaphoreTake(com_send_queue_add_sem, 50))
			for (uint8_t i = 0; i < frame_size; i++)
			{
				xQueueSend(com_send_frame_buffer, frame+i, 0);
			}
			xSemaphoreGive(com_send_queue_add_sem);
			vPortFree(frame);
			return true;
		}
	}
	vPortFree(frame);
	return false;
}

void handle_serial_input(uint8_t *buffer, uint8_t length)
{
	if(length == 1)
	{ //keyboard input
		if(*buffer == 0x26)
		bat_move(PL_TWO, D_UP);
		
		else if(*buffer == 0x28)
		bat_move(PL_TWO, D_DOWN);
	}
}