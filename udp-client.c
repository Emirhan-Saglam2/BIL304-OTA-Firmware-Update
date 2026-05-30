#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"
#include "cfs/cfs.h"
#include "cfs/cfs-coffee.h"
#include "sys/node-id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Gonderilecek firmware verisini iceren header dosyasi
#include "firmware_data.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define SEND_INTERVAL		(2 * CLOCK_SECOND) // Zaman asimi (Timeout) suresi
#define CHUNK_SIZE 64

struct ota_packet {
    uint16_t block_num;
    uint8_t data_len;
    uint16_t checksum;
    uint8_t payload[CHUNK_SIZE];
};

static struct simple_udp_connection udp_conn;
static uint32_t current_block = 0; // Hangi bloktayiz?

// CHECKSUM HESAPLAMA
static uint16_t calculate_checksum(uint8_t *data, uint8_t len) {
    uint16_t sum = 0;
    for(int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);
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
  char *msg = (char *)data;
  
  if(strncmp(msg, "ACK:", 4) == 0) {
      uint32_t ack_num = (uint32_t)atoi(msg + 4);
      
      if(ack_num == current_block) {
          LOG_INFO("Onay alindi (ACK:%" PRIu32 "). Siradaki bloga geciliyor.\n", ack_num);
          current_block++;
      }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  uip_ipaddr_t dest_ipaddr;

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  if(node_id == 2) {
      LOG_INFO("Gonderici dugum (ID:2) baslatildi. Firmware bellekten okunacak.\n");
      // --- DEGISIKLIK 3: Toplam blok sayisi ve firmware boyutu loglaniyor ---
      // Kullaniciya kac paket gonderilecegini basta bildiriyoruz.
      uint32_t total_blk = (FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE;
      LOG_INFO("Firmware boyutu: %u bayt, Toplam blok: %lu\n",
               (unsigned)FIRMWARE_PAYLOAD_LEN, (unsigned long)total_blk);
  }

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      if(node_id == 2) {

        uint32_t expected_total_blocks = (FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE;
        
        if(current_block >= expected_total_blocks) {
            LOG_INFO("Tum firmware imaji (%lu blok) basariyla gonderildi! Aktarim durduruluyor.\n",
                     (unsigned long)expected_total_blocks);
            break;
        }

        struct ota_packet packet;
        packet.block_num = current_block; 

        uint32_t offset = current_block * CHUNK_SIZE;
        uint32_t remaining_bytes = FIRMWARE_PAYLOAD_LEN - offset;
        uint8_t bytes_to_copy = (remaining_bytes < CHUNK_SIZE) ? (uint8_t)remaining_bytes : CHUNK_SIZE;
        
        memcpy(packet.payload, &firmware_payload[offset], bytes_to_copy);
        packet.data_len = bytes_to_copy;
        packet.checksum = calculate_checksum(packet.payload, packet.data_len);

        LOG_INFO("Paket gonderiliyor... Blok: %u/%lu (Checksum: %u, Boyut: %u bayt)\n",
                 packet.block_num,
                 (unsigned long)(expected_total_blocks - 1),
                 packet.checksum,
                 packet.data_len);

        simple_udp_sendto(&udp_conn, &packet, sizeof(packet), &dest_ipaddr);
        
        // DİKKAT: current_block++ YAPMIYORUZ!
        // Artirma islemi sadece ACK geldiginde rx_callback icerisinde yapiliyor. (Stop-and-Wait)
      }
    } else {
      LOG_INFO("Not reachable yet\n");
    }

    etimer_set(&periodic_timer, SEND_INTERVAL);
  }

  PROCESS_END();
}
