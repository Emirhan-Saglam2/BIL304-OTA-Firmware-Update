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

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define SEND_INTERVAL		(2 * CLOCK_SECOND) // Zaman aşımı (Timeout) süresi
#define CHUNK_SIZE 64

struct ota_packet {
    uint16_t block_num;
    uint8_t data_len;
    uint16_t checksum;
    uint8_t payload[CHUNK_SIZE];
};

static struct simple_udp_connection udp_conn;
static uint32_t current_block = 0; // Hangi bloktayız?

// --- PROFESYONEL EKLENTİ: CHECKSUM HESAPLAMA ---
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
  // --- PROFESYONEL EKLENTİ: HAFİF SIKLET ACK KONTROLÜ ---
  char *msg = (char *)data;
  
  // 1. Gelen mesaj "ACK:" ile mi başlıyor kontrol et (strncmp çok hafiftir)
  if(strncmp(msg, "ACK:", 4) == 0) {
      // 2. "ACK:" yazısını atla (ilk 4 karakter) ve kalanı tam sayıya çevir (atoi)
      uint32_t ack_num = (uint32_t)atoi(msg + 4);
      
      if(ack_num == current_block) {
          LOG_INFO("Onay alindi (ACK:%" PRIu32 "). Siradaki bloga geciliyor.\n", ack_num);
          current_block++; // Onay geldi, bir sonraki bloğa geçme izni verildi!
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
      cfs_coffee_format(); 
      int init_fd = cfs_open("new-firmware.z1", CFS_WRITE);
      if(init_fd >= 0) {
          uint8_t buffer[CHUNK_SIZE];
          uint32_t total_size = 129760; 
          
          for(uint32_t i = 0; i < total_size; i += CHUNK_SIZE) {
              for(int j = 0; j < CHUNK_SIZE; j++) {
                  buffer[j] = (uint8_t)((i + j) & 0xFF); 
              }
              uint32_t to_write = (total_size - i > CHUNK_SIZE) ? CHUNK_SIZE : (total_size - i);
              cfs_write(init_fd, buffer, to_write);
          }
          cfs_close(init_fd);
          LOG_INFO("129KB Firmware CFS diskine basariyla gomuldu!\n");
      }
  }

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      if(node_id == 2) {

        // --- FREN MEKANİZMASI ---
            if(current_block >= 2028) {
                LOG_INFO("Tum firmware imaji (2028 blok) basariyla gonderildi! Aktarim durduruluyor.\n");
                break; // while(1) sonsuz dongusunden cik ve islemi bitir
            }

        struct ota_packet packet;
        packet.block_num = current_block; 
        packet.data_len = CHUNK_SIZE;

        int fd = cfs_open("new-firmware.z1", CFS_READ);
        if(fd >= 0) {
            cfs_seek(fd, packet.block_num * CHUNK_SIZE, CFS_SEEK_SET);
            int bytes_read = cfs_read(fd, packet.payload, CHUNK_SIZE);
            if(bytes_read < CHUNK_SIZE) {
                packet.data_len = bytes_read;
            }
            cfs_close(fd); 
        }

        // Paketi göndermeden önce matematiğini yapıp mühürlüyoruz
        packet.checksum = calculate_checksum(packet.payload, packet.data_len);

        LOG_INFO("Paket gonderiliyor... Blok: %d (Checksum: %u)\n", packet.block_num, packet.checksum);
        simple_udp_sendto(&udp_conn, &packet, sizeof(packet), &dest_ipaddr);
        
        // DİKKAT: Burada current_block++ YAPMIYORUZ! 
        // Çünkü artırma işlemini sadece ACK geldiğinde rx_callback içinde yapıyoruz.
      }
    } else {
      LOG_INFO("Not reachable yet\n");
    }

    etimer_set(&periodic_timer, SEND_INTERVAL); // Sabit bekleme süresi
  }

  PROCESS_END();
}