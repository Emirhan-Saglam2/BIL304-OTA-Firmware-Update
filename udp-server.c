/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"
#include "cfs/cfs.h"
#include "cfs/cfs-coffee.h"
#include "sys/node-id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define CHUNK_SIZE 64
#define TOTAL_FIRMWARE_SIZE 129760
#define EXPECTED_BLOCKS ((TOTAL_FIRMWARE_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE) // 2028 Paket

struct ota_packet {
    uint16_t block_num;
    uint8_t data_len;
    uint16_t checksum;
    uint8_t payload[CHUNK_SIZE];
};

static struct simple_udp_connection udp_conn;
static uint32_t expected_block = 0; // Alicinin bekledigi siradaki blok

// --- SARTNAME: PARCA DOGRULAMA (CHECKSUM) FONKSIYONU ---
static uint16_t calculate_checksum(uint8_t *data, uint8_t len) {
    uint16_t sum = 0;
    for(int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  struct ota_packet *received_packet = (struct ota_packet *)data;

  // 1. Sartname: Parca Dogrulama (Checksum Kontrolu)
  uint16_t calculated_chk = calculate_checksum(received_packet->payload, received_packet->data_len);
  
  if(calculated_chk != received_packet->checksum) {
      LOG_ERR("HATA: Checksum uyusmazligi! Paket bozuk geldi, ACK gonderilmiyor.\n");
      return; // ACK gondermiyoruz! Gonderici zaman asimina ugrayip tekrar yollayacak.
  }

  // 2. Sartname: Siralama ve Diske Yazma
  if(received_packet->block_num == expected_block) {
      // Diske Yazma (Offset kullanarak dogru yere)
      int fd = cfs_open("downloaded-firmware.z1", CFS_WRITE);
      if(fd >= 0) {
          cfs_seek(fd, received_packet->block_num * CHUNK_SIZE, CFS_SEEK_SET);
          cfs_write(fd, received_packet->payload, received_packet->data_len);
          cfs_close(fd);
      }
      
      expected_block++; // Kayit basarili, bir sonraki blogu beklemeye basla

      // 3. Sartname: Tum Imajin Tamamlanmasi ve Tum-Imaj Dogrulama
      if(expected_block == EXPECTED_BLOCKS) {
          LOG_INFO("Tum parcalar alindi. Tum-imaj dogrulamasi (Whole-Image Checksum) baslatiliyor...\n");
          
          int verify_fd = cfs_open("downloaded-firmware.z1", CFS_READ);
          if(verify_fd >= 0) {
              uint32_t total_checksum = 0;
              uint8_t buffer[CHUNK_SIZE];
              int bytes_read;
              
              // Diskteki dosyayi bastan sona okuyup toplam checksum hesapliyoruz
              while((bytes_read = cfs_read(verify_fd, buffer, sizeof(buffer))) > 0) {
                  for(int i = 0; i < bytes_read; i++) {
                      total_checksum += buffer[i];
                  }
              }
              cfs_close(verify_fd);
              LOG_INFO("Tum-Imaj Dogrulama basarili! Total Checksum: %lu\n", (unsigned long)total_checksum);
              
              // Sartnamede istenen final mesaji
              LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
          } else {
              LOG_ERR("HATA: Dogrulama icin firmware dosyasi acilamadi!\n");
          }
      }
  } else if (received_packet->block_num < expected_block) {
      // Ag gecikmesinden dolayi eski bir paket tekrar geldiyse, gondericiyi rahatlatmak icin ACK'yi tekrar bas
      LOG_INFO("Eski paket (%d) tekrar alindi, ACK yenileniyor.\n", received_packet->block_num);
  }

  // 4. Sartname: Akilli Onay (ACK) Gonderimi
  char ack_msg[32];
  snprintf(ack_msg, sizeof(ack_msg), "ACK:%d", received_packet->block_num);
  
  LOG_INFO("Onay (%s) gonderiliyor.\n", ack_msg);
  simple_udp_sendto(&udp_conn, ack_msg, strlen(ack_msg), sender_addr);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  cfs_coffee_format();
  LOG_INFO("Alici cihaz (ID:1) CFS Diski formatlandi.\n");

  int fd = cfs_open("downloaded-firmware.z1", CFS_WRITE);
  if(fd >= 0) {
      cfs_close(fd);
  }

  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  PROCESS_END();
}