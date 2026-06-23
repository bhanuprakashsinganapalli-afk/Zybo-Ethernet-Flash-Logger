/*
 * Copyright (C) 2018 - 2022 Xilinx, Inc. All rights reserved.
 * Copyright (C) 2022 - 2024 Advanced Micro Devices, Inc.  All rights reserved
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
 */

/** Connection handle for a UDP Server session */

#include "udp_perf_server.h"
#include "flash.h"
#include <string.h>

/* Constants */
#define FLASH_PORT        5002
#define FLASH_LOG_START 0x400000U
#define FLASH_LOG_END   (FLASH_LOG_START + 0x100000U)  
#define FLASH_RECORD_SIZE 0x100U                        /* 256 B per slot  */

/*
 * DDR_BUF_SIZE: 1 byte reserved for NUL terminator so a max UDP payload
 * of 255 chars never overflows.  The usable payload for a 'W' command is
 * therefore DDR_BUF_SIZE - 1 - 2 (cmd byte + space) = 253 chars.
 */
#define DDR_BUF_SIZE      256U

/*
 * Maximum text length that can safely be written to flash:
 *   - FlashBuffer is DDR_BUF_SIZE bytes
 *   - Flash_Write enforces a single 256-byte page and rejects crossing
 *     a page boundary, so we conservatively limit to 252 bytes + NUL
 *     to stay within one page regardless of the record's address offset.
 */
#define FLASH_MAX_TEXT    252U

/* Globals */
u32 FlashAddress = FLASH_LOG_START;   /* single authoritative definition   */

extern struct netif server_netif;

static struct udp_pcb *pcb;
static struct udp_pcb *flash_pcb;
static struct perf_stats server;

static u8 DDRBuffer[DDR_BUF_SIZE];
static u8 FlashBuffer[DDR_BUF_SIZE];

/* Report interval in ms */
#define REPORT_INTERVAL_TIME (INTERIM_REPORT_INTERVAL * 20)

/* Forward declarations */
u32  FindNextFreeAddress(void);
void CountRecords(void);
void DumpFlash(void);
void SearchRecord(char *Text);
void ReadRecords(void);
/* print_app_header  */
void print_app_header(void)
{
    xil_printf("UDP server listening on port %d\r\n", UDP_CONN_PORT);
    xil_printf("On Host: Run $iperf -c %s -i %d -t 300 -u -b <bandwidth>\r\n",
               inet_ntoa(server_netif.ip_addr),
               INTERIM_REPORT_INTERVAL);
}

/* print_udp_conn_stats */
static void print_udp_conn_stats(void)
{
    xil_printf("[%3d] local %s port %d connected with ",
               server.client_id, inet_ntoa(server_netif.ip_addr),
               UDP_CONN_PORT);
    xil_printf("%s port %d\r\n", inet_ntoa(pcb->remote_ip),
               pcb->remote_port);
    xil_printf("[ ID] Interval\t     Transfer     Bandwidth\t");
    xil_printf("    Lost/Total Datagrams\n\r");
}

/* stats_buffer */
static void stats_buffer(char *outString, double data, enum measure_t type)
{
    int conv = KCONV_UNIT;
    const char *format;
    double unit = 1024.0;

    if (type == SPEED)
        unit = 1000.0;

    while (data >= unit && conv <= KCONV_GIGA) {
        data /= unit;
        conv++;
    }

    if (data < 9.995)
        format = "%4.2f %c";
    else if (data < 99.95)
        format = "%4.1f %c";
    else
        format = "%4.0f %c";

    sprintf(outString, format, data, kLabel[conv]);
}

/* udp_conn_report */
static void udp_conn_report(u64_t diff, enum report_type report_type)
{
    u64_t total_len, cnt_datagrams, cnt_dropped_datagrams, total_packets;
    u32_t cnt_out_of_order_datagrams = 0;
    double duration, bandwidth = 0;
    char data[16], perf[16], time[64], drop[64];

    if (report_type == INTER_REPORT) {
        total_len             = server.i_report.total_bytes;
        cnt_datagrams         = server.i_report.cnt_datagrams;
        cnt_dropped_datagrams = server.i_report.cnt_dropped_datagrams;
    } else {
        server.i_report.last_report_time = 0;
        total_len                    = server.total_bytes;
        cnt_datagrams                = server.cnt_datagrams;
        cnt_dropped_datagrams        = server.cnt_dropped_datagrams;
        cnt_out_of_order_datagrams   = server.cnt_out_of_order_datagrams;
    }

    total_packets = cnt_datagrams + cnt_dropped_datagrams;
    duration = diff / 1000.0;
    if (duration)
        bandwidth = (total_len / duration) * 8.0;

    stats_buffer(data, total_len, BYTES);
    stats_buffer(perf, bandwidth, SPEED);

    sprintf(time, "%4.1f-%4.1f sec",
            (double)server.i_report.last_report_time,
            (double)(server.i_report.last_report_time + duration));
    sprintf(drop, "%4llu/%5llu (%.2g%%)",
            cnt_dropped_datagrams,
            total_packets,
            (total_packets > 0) ? (100.0 * cnt_dropped_datagrams) / total_packets : 0.0);

    xil_printf("[%3d] %s  %sBytes  %sbits/sec  %s\n\r",
               server.client_id, time, data, perf, drop);

    if (report_type == INTER_REPORT) {
        server.i_report.last_report_time += duration;
    } else if (cnt_out_of_order_datagrams) {
        xil_printf("[%3d] %s  %u datagrams received out-of-order\n\r",
                   server.client_id, time, cnt_out_of_order_datagrams);
    }
}

/* reset_stats */
static void reset_stats(void)
{
    server.client_id++;
    server.start_time  = get_time_ms();
    server.end_time    = 0;
    server.total_bytes = 0;
    server.cnt_datagrams              = 0;
    server.cnt_dropped_datagrams      = 0;
    server.cnt_out_of_order_datagrams = 0;
    server.expected_datagram_id       = 0;

    server.i_report.start_time            = 0;
    server.i_report.total_bytes           = 0;
    server.i_report.cnt_datagrams         = 0;
    server.i_report.cnt_dropped_datagrams = 0;
    server.i_report.last_report_time      = 0;
}

/* udp_recv_perf_traffic */
static void udp_recv_perf_traffic(void *arg, struct udp_pcb *tpcb,
                                  struct pbuf *p,
                                  const ip_addr_t *addr, u16_t port)
{
    static u8_t first = 1;
    static u64_t now  = 0;
    u32_t drop_datagrams = 0;
    s32_t recv_id;

#ifdef __MICROBLAZE__
    s16_t *payload = (s16_t *)(p->payload);
    recv_id = (ntohs(payload[0]) << 16) | ntohs(payload[1]);
#else
    recv_id = ntohl(*((int *)(p->payload)));
#endif

    if (first && (recv_id == 0 || recv_id == 1)) {
        pcb->remote_ip   = *addr;
        pcb->remote_port = port;
        reset_stats();
        print_udp_conn_stats();
        first = 0;
    } else if (first) {
        pbuf_free(p);
        return;
    }

    if (recv_id < 0) {
        u64_t diff_ms = (now > server.start_time) ?
                        (now - server.start_time) : 0;
        udp_sendto(tpcb, p, addr, port);
        udp_conn_report(diff_ms, UDP_DONE_SERVER);
        xil_printf("UDP test passed Successfully\n\r");
        first = 1;
        pbuf_free(p);
        return;
    }

    if (server.expected_datagram_id != recv_id) {
        if (server.expected_datagram_id < recv_id) {
            drop_datagrams = recv_id - server.expected_datagram_id;
            server.cnt_dropped_datagrams  += drop_datagrams;
            server.expected_datagram_id    = recv_id + 1;
        } else {
            server.cnt_out_of_order_datagrams++;
        }
    } else {
        server.expected_datagram_id++;
    }

    server.cnt_datagrams++;
    server.total_bytes += p->tot_len;

    if (REPORT_INTERVAL_TIME) {
        now = get_time_ms();

        server.i_report.cnt_datagrams++;
        server.i_report.cnt_dropped_datagrams += drop_datagrams;
        server.i_report.total_bytes           += p->tot_len;

        if (server.i_report.start_time) {
            u64_t diff_ms = now - server.i_report.start_time;
            if (diff_ms >= REPORT_INTERVAL_TIME) {
                udp_conn_report(diff_ms, INTER_REPORT);
                server.i_report.start_time            = 0;
                server.i_report.total_bytes           = 0;
                server.i_report.cnt_datagrams         = 0;
                server.i_report.cnt_dropped_datagrams = 0;
            }
        } else {
            server.i_report.start_time = now;
        }
    }

    pbuf_free(p);
}

/* flash_recv_callback */
static void flash_recv_callback(void *arg, struct udp_pcb *tpcb,
                                struct pbuf *p,
                                const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)tpcb;
    (void)addr;
    (void)port;

    if (p == NULL)
        return;

    memset(DDRBuffer, 0, sizeof(DDRBuffer));

    /* Clamp copy length so DDRBuffer always has a NUL terminator */
    u16_t copy_len = (p->len < (DDR_BUF_SIZE - 1)) ? p->len : (DDR_BUF_SIZE - 1);
    memcpy(DDRBuffer, p->payload, copy_len);
    DDRBuffer[copy_len] = '\0';
    u32 i;

xil_printf("UDP Payload (%d bytes)\r\n", p->len);

for(i = 0; i < copy_len; i++)
{
    xil_printf(
        "Byte[%03d] = 0x%02X (%c)\r\n",
        (unsigned long)i,
        DDRBuffer[i],
        (DDRBuffer[i] >= 32 && DDRBuffer[i] <= 126) ?
        DDRBuffer[i] : '.');
}
    xil_printf("CMD : %s\r\n", DDRBuffer);

    if (DDRBuffer[0] == 'W')
    {
        if (strlen((char *)DDRBuffer) < 3)
        {
            xil_printf("No Text Entered\r\n");
        }
        else
        {
            /* Use the defined constant instead of an inline literal */
            if (FlashAddress >= FLASH_LOG_END)
            {
                xil_printf("Flash log area full\r\n");
            }
            else
            {
                char *src = (char *)&DDRBuffer[2];
                size_t text_len = strlen(src);
                if (text_len > FLASH_MAX_TEXT)
                    text_len = FLASH_MAX_TEXT;

                memcpy((char *)FlashBuffer, src, text_len);
                FlashBuffer[text_len] = '\0';

                Flash_Write(FlashAddress, FlashBuffer,
                            (u32)(text_len + 1));

                xil_printf("Stored In Flash @ 0x%06X\r\n",
                           (unsigned int)FlashAddress);

                FlashAddress += FLASH_RECORD_SIZE;
            }
        }
    }
    else if (DDRBuffer[0] == 'R')
{
     ReadRecords();
}
    else if (DDRBuffer[0] == 'C')
    {
        CountRecords();
    }
    else if (DDRBuffer[0] == 'D')
    {
        DumpFlash();
    }
    else if (DDRBuffer[0] == 'S')
    {
        if (strlen((char *)DDRBuffer) > 2)
            SearchRecord((char *)&DDRBuffer[2]);
    }
    else if (DDRBuffer[0] == 'E')
{
    u32 addr;

    xil_printf("Erasing Flash...\r\n");

    for(addr = FLASH_LOG_START;
        addr < FLASH_LOG_END;
        addr += 0x1000)
    {
        Flash_EraseSector(addr);
    }

    FlashAddress = FLASH_LOG_START;

    xil_printf("Flash Erased\r\n");
}
    else
    {
        xil_printf("Unknown Command\r\n");
    }

    pbuf_free(p);
}

/* FindNextFreeAddress  */
u32 FindNextFreeAddress(void)
{
    u32 addr;
    u8  Buffer[16];

    for (addr = FLASH_LOG_START; addr < FLASH_LOG_END; addr += FLASH_RECORD_SIZE)
    {
        memset(Buffer, 0, sizeof(Buffer));
        Flash_Read(addr, Buffer, sizeof(Buffer));

        if (Buffer[0] == 0xFF)
            return addr;
    }

    return FLASH_LOG_END;   /* log area exhausted */
}

/* CountRecords */
void CountRecords(void)
{
    u32 used = (FlashAddress > FLASH_LOG_START) ?
               (FlashAddress - FLASH_LOG_START) : 0U;

    xil_printf("\r\nTotal Records : %d\r\n",
               (int)(used / FLASH_RECORD_SIZE));
}

/* DumpFlash */
void DumpFlash(void)
{
    u32 addr;
    int i;

    xil_printf("\r\nFlash Dump\r\n");
    xil_printf("-----------------------------------------------\r\n");

    for (addr = FLASH_LOG_START;
         addr < FlashAddress;
         addr += FLASH_RECORD_SIZE)
    {
        Flash_Read(addr, FlashBuffer, 16);

xil_printf("ADDR = 0x%06X : ", (unsigned int)addr);

/* Print text */
for(i = 0; i < 16; i++)
{
    if(FlashBuffer[i] == 0x00 ||
       FlashBuffer[i] == 0xFF)
    {
        break;
    }

    xil_printf("%c", FlashBuffer[i]);
}

xil_printf(" | HEX : ");

/* Print hex */
for(i = 0; i < 16; i++)
{
    xil_printf("%02X ", FlashBuffer[i]);
}

xil_printf("\r\n");
    }
}
void ReadRecords(void)
{
    u32 addr;
    int i;

    xil_printf("\r\nRecords In Flash\r\n");
    xil_printf("-----------------------------------------------\r\n");

    for (addr = FLASH_LOG_START;
         addr < FlashAddress;
         addr += FLASH_RECORD_SIZE)
    {
        Flash_Read(addr, FlashBuffer, 16);

        xil_printf("ADDR = 0x%06X : ", (unsigned int)addr);

        /* Print text */
        for(i = 0; i < 16; i++)
        {
            if(FlashBuffer[i] == 0x00 ||
               FlashBuffer[i] == 0xFF)
            {
                break;
            }

            xil_printf("%c", FlashBuffer[i]);
        }

        xil_printf(" | HEX : ");

        /* Print hex */
        for(i = 0; i < 16; i++)
        {
            xil_printf("%02X ", FlashBuffer[i]);
        }

        xil_printf("\r\n");
    }
}
/*SearchRecord */
void SearchRecord(char *Text)
{
    u32 addr;
    int Found = 0;

    for (addr = FLASH_LOG_START; addr < FlashAddress; addr += FLASH_RECORD_SIZE)
    {
        memset(FlashBuffer, 0, sizeof(FlashBuffer));
        Flash_Read(addr, FlashBuffer, 64);

        if (strstr((char *)FlashBuffer, Text))
        {
            Found = 1;
            xil_printf("Found In Record %d : %s\r\n",
                       (int)((addr - FLASH_LOG_START) / FLASH_RECORD_SIZE),
                       FlashBuffer);
        }
    }

    if (!Found)
        xil_printf("Record Not Found\r\n");
}

/* start_application */
void start_application(void)
{
    err_t err;
    err_t flash_err;

       
    /* Performance PCB */
    pcb = udp_new();
    if (!pcb) {
        xil_printf("UDP server: Error creating PCB. Out of Memory\r\n");
        return;
    }

    err = udp_bind(pcb, IP_ADDR_ANY, UDP_CONN_PORT);
    if (err != ERR_OK) {
        xil_printf("UDP server: Unable to bind to port %d: err = %d\r\n",
                   UDP_CONN_PORT, err);
        udp_remove(pcb);
        return;
    }

    udp_recv(pcb, udp_recv_perf_traffic, NULL);

    /* Flash PCB  */
    flash_pcb = udp_new();
    if (!flash_pcb) {
        xil_printf("Flash PCB Create Failed\r\n");
        return;
    }

    flash_err = udp_bind(flash_pcb, IP_ADDR_ANY, FLASH_PORT);
    if (flash_err != ERR_OK) {
        xil_printf("Flash bind failed\r\n");
        udp_remove(flash_pcb);
        return;
    }

    udp_recv(flash_pcb, flash_recv_callback, NULL);
    FlashAddress = FindNextFreeAddress();
    xil_printf("Next Free Address : 0x%06X\r\n", (unsigned int)FlashAddress);
    xil_printf("Flash server listening on port %d\r\n", FLASH_PORT);
}
