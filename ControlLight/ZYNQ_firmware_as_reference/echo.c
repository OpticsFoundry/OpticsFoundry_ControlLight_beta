/*
 * Copyright (C) 2009 - 2019 Xilinx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */

/*
 * TCP command server of the sequencer.
 *
 * 2026-11-26 hardening:
 * - only one client at a time: a new connection replaces the previous one
 * - pbuf chains are processed completely (previously only the first pbuf of a chain was used)
 * - command lines are split byte by byte with bounds checks; overlong lines and lines arriving
 *   while the command ring buffer is full are dropped (previously off-by-one write, dropped
 *   whole packets, and returned ERR_MEM after freeing the pbuf)
 * - binary transfers never write beyond the destination buffer; if more data is announced than
 *   fits, the excess is discarded and the transfer is marked as failed
 * - the connection pointer is cleared when the connection closes or is reset, and all send
 *   functions check it; server_send_binary can no longer loop forever on a dead connection
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


#include "lwip/tcp.h"
#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

//to enable timer functionality
#include "platform.h"
#include "xparameters.h"
#include "xtime_l.h"
//typedef unsigned char bool;
#include <stdbool.h>
#include "sequencer_identity.h"

extern bool DebugModeOn;
extern void RunEventLoop(); //defined in main.c

static u32 ElapsedMs(XTime start) {
	XTime now;
	XTime_GetTime(&now);
	return (u32)((now - start) / (COUNTS_PER_SECOND / 1000));
}

u8 client_connected = 0;

u8 is_client_connected() {
	return client_connected;
}

int transfer_data() {
	return 0;
}

void print_app_header()
{
	xil_printf("\n\r\n\r----- FireflyControl TCP server IP4 ------\n\r");
	xil_printf("listening on TCP port %d, discovery on UDP port %d\n\r", SEQUENCER_TCP_PORT, SEQUENCER_DISCOVERY_UDP_PORT);
}

//commands are lines of text, terminated by '\n'
#define MaxCommandBuffer 64   //number of commands that can wait to be treated
#define MaxCommandLength 128  //including terminating zero
unsigned char CommandBuffer [MaxCommandBuffer*MaxCommandLength];

u8 ReveiveMode = 0; //0: ASCII commands, 1: binary data into buffer
static u32 NextEmptyCommandBufferNumber = 0;
static u32 WritePositionInNextEmptyCommandBuffer = 0;
static bool DiscardCurrentCommand = false;
static u32 NextCommandToDo = 0;
static u32 NumberCommandsInBuffer = 0;
static u8* BinaryBufferStart = NULL;
static u32 BinaryBufferLengthInBytes = 0;
static u32 BinaryBufferReceivedInBytes = 0;
static u32 BinaryBufferExpectedInBytes = 0;
struct tcp_pcb *last_tpcb = NULL; //the connected client; NULL if there is none
static u8 Digital_data_successfully_received = 0;

static void ResetReceiveState(void) {
	ReveiveMode = 0;
	NextEmptyCommandBufferNumber = 0;
	WritePositionInNextEmptyCommandBuffer = 0;
	DiscardCurrentCommand = false;
	NextCommandToDo = 0;
	NumberCommandsInBuffer = 0;
	BinaryBufferStart = NULL;
	BinaryBufferLengthInBytes = 0;
	BinaryBufferReceivedInBytes = 0;
	BinaryBufferExpectedInBytes = 0;
	Digital_data_successfully_received = 0;
}

void server_clear(u32* a_buffer, u32 a_length){
	if (DebugModeOn) xil_printf("server_clear called ");
	ResetReceiveState();
}

void server_set_ascii_mode() {
	if (DebugModeOn) xil_printf("server_set_ascii_mode called ");
	ReveiveMode = 0;
}


unsigned char *server_get_command(long double timeout_in_seconds) {
	if (DebugModeOn) xil_printf("sgc ");
	if (NumberCommandsInBuffer == 0) {
		XTime tStart, tEnd;
    	XTime_GetTime(&tStart);
    	long double wait_time_in_seconds = 0;
		while ((NumberCommandsInBuffer == 0) && (wait_time_in_seconds < timeout_in_seconds)) {
			RunEventLoop();
			XTime_GetTime(&tEnd);
			wait_time_in_seconds = (long double)((tEnd - tStart) *2)/(long double)XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ;
		}
		if (!(wait_time_in_seconds < timeout_in_seconds)) {
			if (ReveiveMode == 1) { //digital data expected
				server_set_ascii_mode();
				{if (DebugModeOn) xil_printf("Timeout during digital data transfer. Switching back to ASCII mode.\r\n");}
			}
		}
		if (NumberCommandsInBuffer == 0) return NULL;
	}
	//The returned string stays valid until MaxCommandBuffer further commands have been received.
	unsigned char * return_command = CommandBuffer + MaxCommandLength*NextCommandToDo;
	NextCommandToDo++;
	if (NextCommandToDo>=MaxCommandBuffer) NextCommandToDo = 0;
	NumberCommandsInBuffer--;
	return return_command;
}

u32_t server_read_u32() {
	unsigned char *u32_as_string = server_get_command(/*timeout*/10.0);
	if (u32_as_string == NULL) {
		if (DebugModeOn) xil_printf("server_read_u32 : no data received\r\n");
		return 0;
	}
	return strtoul((char*)u32_as_string, NULL, 10);
}

u8_t server_read_u8() {
	unsigned char *u8_as_string = server_get_command(/*timeout*/10.0);
	if (u8_as_string == NULL) {
		if (DebugModeOn) xil_printf("server_read_u8 : no data received\r\n");
		return 0;
	}
	return atoi((char*)u8_as_string);
}

double server_read_double() {
	unsigned char *double_as_string = server_get_command(/*timeout*/10.0);
	if (double_as_string == NULL) {
		if (DebugModeOn) xil_printf("server_read_double : no data received\r\n");
		return 0;
	}
	return atof((char*)double_as_string);
}

//ToDo: use one of the following two commands to check if digital data transfer was successful before doing anything with that data
u8 server_last_digital_data_transfer_successful() {
	return Digital_data_successfully_received;
}

bool server_wait_till_digital_tranfer_done() {
	XTime tStart, tEnd;
	XTime_GetTime(&tStart);
	long double wait_time_in_seconds = 0;
	long double timeout_in_seconds = 5.0;
	while ((!server_last_digital_data_transfer_successful()) && (wait_time_in_seconds < timeout_in_seconds)) {
		RunEventLoop();
		XTime_GetTime(&tEnd);
		wait_time_in_seconds = (long double)((tEnd - tStart) *2)/(long double)XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ;
	}
	if (!(wait_time_in_seconds < timeout_in_seconds)) {
		if (DebugModeOn) xil_printf("server_wait_till_digital_tranfer_done : didn't receive modification data\r\n");
		return false;
	}
	return true;
}

/* binary mode: copy data into the binary buffer; returns the number of bytes consumed */
static u32 ProcessBinaryData(const u8* data, u32 length) {
	u32 to_read = BinaryBufferExpectedInBytes - BinaryBufferReceivedInBytes;
	if (to_read > length) to_read = length;
	if (BinaryBufferReceivedInBytes < BinaryBufferLengthInBytes) {  /* never write beyond the buffer */
		u32 fits = BinaryBufferLengthInBytes - BinaryBufferReceivedInBytes;
		memcpy(BinaryBufferStart + BinaryBufferReceivedInBytes, data, (to_read < fits) ? to_read : fits);
	}
	BinaryBufferReceivedInBytes += to_read;
	if (BinaryBufferReceivedInBytes >= BinaryBufferExpectedInBytes) {
		ReveiveMode = 0; //switch back to ascii mode
		if (BinaryBufferExpectedInBytes <= BinaryBufferLengthInBytes) {
			Digital_data_successfully_received = 1;
		} else {
			Digital_data_successfully_received = 0;
			xil_printf("server : binary data (%lu bytes) didn't fit into buffer (%lu bytes); excess discarded, transfer failed\r\n",
					(unsigned long)BinaryBufferExpectedInBytes, (unsigned long)BinaryBufferLengthInBytes);
		}
	}
	return to_read;
}

extern void CS_StopExecution(void);      /* OpticsFoundryCPUCommandSequencer */
extern void CS_InterruptExecution(void);

/* Commands that must act while a CPU command sequence is executing. FireflyControlLoop doesn't dispatch
 * commands then, so previously a running CPU command sequence couldn't be stopped. These commands are
 * also queued normally; executing them a second time is harmless. */
static void HandleUrgentCommand(const char* command) {
	if ((strcmp(command, "cs_stop_sequence") == 0) || (strcmp(command, "cs_stop_sequence0") == 0)) CS_StopExecution();
	else if ((strcmp(command, "cs_interrupt_sequence") == 0) || (strcmp(command, "cs_interrupt_sequence0") == 0)) CS_InterruptExecution();
}

/* ASCII mode: split data into '\n' terminated commands */
static void ProcessAsciiData(const u8* data, u32 length) {
	for (u32 n = 0; n < length; n++) {
		u8 c = data[n];
		if (c == '\n') {
			if (DiscardCurrentCommand) {
				xil_printf("server : command dropped (longer than %d characters, or more than %d untreated commands)\r\n", MaxCommandLength - 1, MaxCommandBuffer);
			} else {
				CommandBuffer[NextEmptyCommandBufferNumber*MaxCommandLength + WritePositionInNextEmptyCommandBuffer] = 0; //terminate string with 0
				if (DebugModeOn) xil_printf("server_recv_callback: received %s\r\n",(char *)(&(CommandBuffer[NextEmptyCommandBufferNumber*MaxCommandLength])));
				HandleUrgentCommand((const char *)(&(CommandBuffer[NextEmptyCommandBufferNumber*MaxCommandLength])));
				NumberCommandsInBuffer++;
				NextEmptyCommandBufferNumber++;
				if (NextEmptyCommandBufferNumber >= MaxCommandBuffer) NextEmptyCommandBufferNumber = 0;
			}
			WritePositionInNextEmptyCommandBuffer = 0;
			DiscardCurrentCommand = false;
		} else if (c != '\r') {
			if (NumberCommandsInBuffer >= MaxCommandBuffer) {
				DiscardCurrentCommand = true;  /* the slot to write to still holds an untreated command */
			} else if (WritePositionInNextEmptyCommandBuffer >= (MaxCommandLength - 1)) {
				DiscardCurrentCommand = true;  /* no space for the terminating zero */
			} else if (!DiscardCurrentCommand) {
				CommandBuffer[NextEmptyCommandBufferNumber*MaxCommandLength + WritePositionInNextEmptyCommandBuffer] = c;
				WritePositionInNextEmptyCommandBuffer++;
			}
		}
	}
}

static void ForgetConnection(struct tcp_pcb *tpcb) {
	if (tpcb == last_tpcb) {
		last_tpcb = NULL;
		client_connected = 0;
	}
}

static void error_callback(void *arg, err_t err) {
	/* the pcb has already been freed by lwIP */
	if (DebugModeOn) xil_printf("\r\nTCP connection reset (%d)\r\n", err);
	ForgetConnection((struct tcp_pcb *)arg);
}

static err_t CloseConnection(struct tcp_pcb *tpcb) {
	ForgetConnection(tpcb);
	tcp_arg(tpcb, NULL);
	tcp_recv(tpcb, NULL);
	tcp_err(tpcb, NULL);
	if (tcp_close(tpcb) != ERR_OK) {
		tcp_abort(tpcb);
		return ERR_ABRT;
	}
	return ERR_OK;
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb,
                               struct pbuf *p, err_t err)
{
	if (DebugModeOn) xil_printf("r ");
	if (!p) { /* remote side closed the connection */
		if (DebugModeOn) xil_printf("\r\nTCP disconnected\r\n");
		return CloseConnection(tpcb);
	}

	/* indicate that the data has been received */
	tcp_recved(tpcb, p->tot_len);

	if ((err != ERR_OK) || (tpcb != last_tpcb)) { /* data of a connection that has been replaced */
		pbuf_free(p);
		return ERR_OK;
	}

	for (struct pbuf *q = p; q != NULL; q = q->next) {
		const u8* data = (const u8*)q->payload;
		u32 length = q->len;
		while (length > 0) {
			if (ReveiveMode == 1) {
				if (DebugModeOn) xil_printf("server_recv_callback : binary receive %d \r\n", length);
				u32 used = ProcessBinaryData(data, length);
				if (used == 0) ReveiveMode = 0;  /* can't happen; avoids an endless loop */
				data += used;
				length -= used;
			} else {
				if (DebugModeOn) xil_printf("server_recv_callback : ASCII receive %d \r\n", length);
				ProcessAsciiData(data, length);
				length = 0;
			}
		}
	}

	/* free the received pbuf */
	pbuf_free(p);

	return ERR_OK;
}

err_t server_send_ascii(char* buf) {
	size_t plen = strlen(buf);
	if (last_tpcb == NULL) {
		if (DebugModeOn) xil_printf("server_send_ascii: no client connected\n\r");
		return ERR_CONN;
	}
	XTime start;
	XTime_GetTime(&start);
	while (tcp_sndbuf(last_tpcb) < plen) { /* wait (at most 1 s) for space in the send buffer */
		tcp_output(last_tpcb);
		RunEventLoop();
		if (last_tpcb == NULL) return ERR_CONN;
		if (ElapsedMs(start) > 1000) {
			xil_printf("server_send_ascii: no space in tcp_sndbuf\n\r");
			return ERR_MEM;
		}
	}
	err_t err = tcp_write(last_tpcb, buf, plen, TCP_WRITE_FLAG_COPY);
	if (DebugModeOn) xil_printf("server_send_ascii: sending %s\r\n",buf);
	return err;
}

void server_write_u32(u32_t a_u32) {
	char buf[20];
	sprintf(buf, "%u\n",a_u32);
	server_send_ascii(buf);
	if (DebugModeOn) xil_printf("server_write_u32 : wrote %lu\r\n", a_u32);
}

err_t server_send_binary(char* buf, u32 length, bool DebugModeOn) {
	//data longer than the send buffer is sent in several parts
	if (DebugModeOn) xil_printf("server_send_binary: need to send %u bytes\r\n",length);
	u32 n = 0;
	XTime last_progress;
	XTime_GetTime(&last_progress);
	while (n < length) {
		if (last_tpcb == NULL) {
			if (DebugModeOn) xil_printf("server_send_binary: connection closed\n\r");
			return ERR_CONN;
		}
		u32 free_buf = tcp_sndbuf(last_tpcb); // available buffer space for sending (in bytes).
		if (free_buf > 0) {
			u32 chunk = ((length - n) < free_buf) ? (length - n) : free_buf;
			if (chunk > 0xFFFF) chunk = 0xFFFF;
			err_t err = tcp_write(last_tpcb, buf + n, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
			if (err == ERR_OK) {
				if (DebugModeOn) xil_printf("server_send_binary: sending %u bytes\r\n", chunk);
				n += chunk;
				XTime_GetTime(&last_progress);
			} else if (err != ERR_MEM) {
				if (DebugModeOn) xil_printf("server_send_binary: tcp_write error %d\n\r", err);
				return err;
			}
		}
		//invoke communication to send the data
		err_t err = tcp_output(last_tpcb);
		if (err != ERR_OK) {
			if (DebugModeOn) xil_printf("server_send_binary: could not send\n\r");
			return err;
		}
		if (n < length) {
			RunEventLoop();
			if (ElapsedMs(last_progress) > 5000) {
				xil_printf("server_send_binary: timeout, %lu of %lu bytes sent\n\r", (unsigned long)n, (unsigned long)length);
				return ERR_TIMEOUT;
			}
		}
	}
	return ERR_OK;
}

void server_set_binary_mode(u8* a_buffer, u32 a_length, u32 a_expected_length) {
	if (DebugModeOn) xil_printf("server_set_binary_mode : expecting %d bytes\r\n", a_expected_length);
	BinaryBufferStart = a_buffer;
	BinaryBufferLengthInBytes = (a_buffer != NULL) ? a_length : 0;
	BinaryBufferReceivedInBytes = 0;
	BinaryBufferExpectedInBytes = a_expected_length; //switch back to ascii mode once this number of bytes has been received
	Digital_data_successfully_received = 0;
	if (BinaryBufferExpectedInBytes > BinaryBufferLengthInBytes) {
		/* stay in sync with the sender, but discard the excess data and report failure */
		xil_printf("server_set_binary_mode : buffer size (%lu) not big enough for expected binary data size (%lu); data will be discarded\n\r",
				(unsigned long)BinaryBufferLengthInBytes, (unsigned long)BinaryBufferExpectedInBytes);
	}
	if (BinaryBufferExpectedInBytes == 0) {
		ReveiveMode = 0;
		Digital_data_successfully_received = 1;
	} else {
		ReveiveMode = 1;
	}
	server_send_ascii("Ready\n"); //This Ready should be needed to avoid receiving binary data in ASCII mode. However, it works without and it doesn't work if I put it. FS 2025 04 02
}

static int echo_connection = 1;

int get_connection_number() {
	if (DebugModeOn) xil_printf("get_connection_number called\r\n");
	return echo_connection-1;
}

// accept_callback is called when a new client connects to this TCP server
err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	if ((err != ERR_OK) || (newpcb == NULL)) return ERR_VAL;
	if (DebugModeOn) xil_printf("\r\nNew TCP client connected\r\n");
	if (last_tpcb != NULL) {
		/* only one client at a time: the new connection replaces the previous one */
		xil_printf("\r\nTCP: new client connected, closing the previous connection\r\n");
		CloseConnection(last_tpcb);
	}
	ResetReceiveState();
	last_tpcb = newpcb;

	/* the pcb is the callback argument, so that the error callback knows which connection failed */
	tcp_arg(newpcb, newpcb);
	tcp_recv(newpcb, recv_callback);
	tcp_err(newpcb, error_callback);

	echo_connection++;
	client_connected = 1;

	return ERR_OK;
}


int start_application()
{
	struct tcp_pcb *pcb;
	err_t err;
	unsigned port = SEQUENCER_TCP_PORT;

	/* create new TCP PCB structure */
	pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
	if (!pcb) {
		xil_printf("Error creating PCB. Out of Memory\n\r");
		return -1;
	}

	/* bind to specified @port */
	err = tcp_bind(pcb, IP_ANY_TYPE, port);
	if (err != ERR_OK) {
		xil_printf("Unable to bind to port %d: err = %d\n\r", port, err);
		return -2;
	}

	/* we do not need any arguments to callback functions */
	tcp_arg(pcb, NULL);

	/* listen for connections */
	pcb = tcp_listen(pcb);
	if (!pcb) {
		xil_printf("Out of memory while tcp_listen\n\r");
		return -3;
	}

	/* specify callback to use for incoming connections */
	tcp_accept(pcb, accept_callback);

	if (DebugModeOn) xil_printf("TCP echo server started @ port %d\n\r", port);

	/* answer discovery requests (UDP) */
	SequencerIdentity_start_discovery();

	return 0;
}
