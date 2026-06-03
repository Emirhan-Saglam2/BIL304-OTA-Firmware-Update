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
 
/* Gonderilecek firmware verisini iceren header dosyasi.
 * Z1 mote Flash kapasitesi 92 KB oldugu icin 129 KB buyuklugundeki
 * new-firmware.z1 dogrudan gomulememektedir. Bu nedenle hocanin
 * saglayip firmware_data.h uzerinden paylastigi ELF imajinin ilk
 * 4096 bayti (64 blok) temsili olarak kullanilmaktadir.
 * static const tanimlamasi veriyi RAM yerine Flash (.rodata) bolgesine
 * yerlestirerek 8 KB RAM kisitini korumaktadir. */
#include "firmware_data.h"
 
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
 
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
 
/* SEND_INTERVAL: ACK bekleme / yeniden gonderim zaman asimi suresi.
 * Bu sure icinde ACK gelmezse ayni blok tekrar gonderilir. */
#define SEND_INTERVAL   (2 * CLOCK_SECOND)
 
#define CHUNK_SIZE      64
 
/* Sartname: Yeniden gonderim icin maksimum deneme siniri.
 * Bu sinir asildiktan sonra aktarim hata ile sonlandirilir.
 * Sonsuz dongu korumasini saglar. */
#define MAX_RETRIES     5
 
struct ota_packet {
    uint16_t block_num;
    uint8_t  data_len;
    uint16_t checksum;
    uint8_t  payload[CHUNK_SIZE];
};
 
static struct simple_udp_connection udp_conn;
static uint32_t current_block = 0;  /* Suanda gonderilmeye calisilan blok */
 
/* Sartname — Yeniden Gonderim:
 * Her zamanlayici dolumunda ayni blok icin kac kez deneme yapildigini
 * izler. ACK alindigi anda sifirlanir. MAX_RETRIES asiminda aktarim
 * durdurulur. */
static uint8_t retry_count = 0;
 
/* Aktarimin tamamlandigini veya hata ile sonlandigini bildiren bayrak.
 * Ana dongu bu bayrak set edilince temiz bicimde cikar. */
static bool transfer_done = false;
 
/*---------------------------------------------------------------------------*/
/* CHECKSUM HESAPLAMA
 * Toplamsal (additive) 16-bit checksum. Her iki tarafta ayni algoritma
 * kullanilarak parca butunlugu dogrulanir. */
static uint16_t
calculate_checksum(uint8_t *data, uint8_t len)
{
    uint16_t sum = 0;
    for(int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}
/*---------------------------------------------------------------------------*/
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
            LOG_INFO("Onay alindi (ACK:%" PRIu32 "). "
                     "Yeniden deneme sayaci sifirlaniyor, siradaki bloga geciliyor.\n",
                     ack_num);
            /* Sartname — Yeniden Gonderim:
             * ACK basariyla alindi; sayaci sifirla ve bir sonraki bloga gec. */
            retry_count = 0;
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
        uint32_t total_blk = (FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE;
        LOG_INFO("Gonderici dugum (ID:2) baslatildi. Firmware bellekten okunacak.\n");
        LOG_INFO("Firmware boyutu: %u bayt | Toplam blok: %lu | "
                 "Maks yeniden deneme: %d\n",
                 (unsigned)FIRMWARE_PAYLOAD_LEN,
                 (unsigned long)total_blk,
                 MAX_RETRIES);
    }
 
    etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
 
    while(1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
 
        /* Aktarim tamamlandi ya da hata ile sonlandi; donguden cik. */
        if(transfer_done) {
            break;
        }
 
        if(NETSTACK_ROUTING.node_is_reachable() &&
           NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
 
            if(node_id == 2) {
                uint32_t expected_total_blocks =
                    (FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE;
 
                /* Tum bloklar basariyla ACK alindi; aktarim tamam. */
                if(current_block >= expected_total_blocks) {
                    LOG_INFO("Tum firmware imaji (%lu blok) basariyla "
                             "gonderildi! Aktarim durduruluyor.\n",
                             (unsigned long)expected_total_blocks);
                    transfer_done = true;
                    break;
                }
 
                /* Sartname — Yeniden Gonderim: Maksimum deneme siniri kontrolu.
                 * Bu blok icin MAX_RETRIES kez gonderildi, hic ACK gelmedi.
                 * Alici erisimi olmayabilir; aktarim hata ile sonlandiriliyor. */
                if(retry_count >= MAX_RETRIES) {
                    LOG_ERR("HATA: Blok %lu icin %d kez denendi, ACK alinamadi. "
                            "Aktarim sonlandiriliyor.\n",
                            (unsigned long)current_block,
                            MAX_RETRIES);
                    transfer_done = true;
                    break;
                }
 
                /* Paketi hazirla ve gonder. */
                struct ota_packet packet;
                packet.block_num = (uint16_t)current_block;
 
                uint32_t offset         = current_block * CHUNK_SIZE;
                uint32_t remaining      = FIRMWARE_PAYLOAD_LEN - offset;
                uint8_t  bytes_to_copy  = (remaining < CHUNK_SIZE)
                                          ? (uint8_t)remaining
                                          : (uint8_t)CHUNK_SIZE;
 
                memcpy(packet.payload, &firmware_payload[offset], bytes_to_copy);
                packet.data_len = bytes_to_copy;
                packet.checksum = calculate_checksum(packet.payload,
                                                     packet.data_len);
 
                retry_count++;  /* Bu deneme sayildi; ACK gelince sifirlanacak. */
 
                LOG_INFO("Paket gonderiliyor... Blok: %u/%lu | "
                         "Checksum: %u | Boyut: %u bayt | "
                         "Deneme: %u/%u\n",
                         packet.block_num,
                         (unsigned long)(expected_total_blocks - 1),
                         packet.checksum,
                         packet.data_len,
                         retry_count,
                         MAX_RETRIES);
 
                simple_udp_sendto(&udp_conn, &packet, sizeof(packet),
                                  &dest_ipaddr);
 
                /* DIKKAT: current_block++ YAPILMIYOR.
                 * Stop-and-Wait protokolu geregi artirma yalnizca
                 * udp_rx_callback icinde ACK alindiginda yapilir. */
            }
 
        } else {
            LOG_INFO("Not reachable yet\n");
        }
 
        etimer_set(&periodic_timer, SEND_INTERVAL);
    }
 
    PROCESS_END();
}