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
 
#include "firmware_data.h"
 
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
 
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define CHUNK_SIZE      64
 
/* Senin istedigin dinamik formul eklendi */
#define EXPECTED_BLOCKS ((FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE)
 
/* Sartname — Durum Yonetimi: Hangi bloklarin alindigi izlenir.
 * RAM patlamasin ve derleyici hatasi vermesin diye fiziksel boyut 128'de birakildi. */
static uint8_t received_bitmap[128];
 
struct ota_packet {
    uint16_t block_num;
    uint8_t  data_len;
    uint16_t checksum;
    uint8_t  payload[CHUNK_SIZE];
};
 
static struct simple_udp_connection udp_conn;
static uint32_t expected_block = 0;  /* Sira bekleyen bir sonraki blok */
 
/*---------------------------------------------------------------------------*/
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
static void
log_transfer_status(void)
{
    uint32_t received_count = 0;
    uint32_t missing_count  = 0;
 
    for(uint32_t i = 0; i < EXPECTED_BLOCKS; i++) {
        if(received_bitmap[i]) {
            received_count++;
        } else {
            missing_count++;
        }
    }
 
    LOG_INFO("Durum: %lu/%lu blok alindi",
             (unsigned long)received_count,
             (unsigned long)EXPECTED_BLOCKS);
 
    if(missing_count > 0) {
        LOG_INFO(" | Eksik bloklar: ");
        for(uint32_t i = 0; i < EXPECTED_BLOCKS; i++) {
            if(!received_bitmap[i]) {
                LOG_INFO_("%lu ", (unsigned long)i);
            }
        }
    }
    LOG_INFO_("\n");
}
/*---------------------------------------------------------------------------*/
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
    struct ota_packet *pkt = (struct ota_packet *)data;
 
    uint16_t calc_chk = calculate_checksum(pkt->payload, pkt->data_len);
 
    if(calc_chk != pkt->checksum) {
        LOG_ERR("HATA: Checksum uyusmazligi! Blok %u bozuk geldi "
                "(beklenen: %u, hesaplanan: %u). ACK gonderilmiyor.\n",
                pkt->block_num, pkt->checksum, calc_chk);
        return;
    }
 
    if(pkt->block_num == expected_block) {
 
        int fd = cfs_open("yeni-fw.z1", CFS_WRITE);
 
        if(fd >= 0) {
            cfs_seek(fd, (cfs_offset_t)(pkt->block_num * CHUNK_SIZE), CFS_SEEK_SET);
            cfs_write(fd, pkt->payload, pkt->data_len);
            cfs_close(fd);
 
            received_bitmap[pkt->block_num] = 1;
            LOG_INFO("Blok %u/%lu diske yazildi (Boyut: %u bayt | Checksum: %u)\n",
                     pkt->block_num,
                     (unsigned long)(EXPECTED_BLOCKS - 1),
                     pkt->data_len,
                     pkt->checksum);
 
        } else {
            LOG_ERR("HATA: Blok %u icin CFS dosyasi acilamadi!\n", pkt->block_num);
        }
 
        expected_block++;
 
        log_transfer_status();
 
        if(expected_block == EXPECTED_BLOCKS) {
            LOG_INFO("Tum parcalar alindi (%lu blok). "
                     "Tum-imaj dogrulamasi (Whole-Image Checksum) baslatiliyor...\n",
                     (unsigned long)EXPECTED_BLOCKS);
 
            uint32_t missing = 0;
            for(uint32_t i = 0; i < EXPECTED_BLOCKS; i++) {
                if(!received_bitmap[i]) {
                    missing++;
                    LOG_ERR("UYARI: Bitmap eksik blok tespiti — blok %lu\n",
                            (unsigned long)i);
                }
            }
 
            if(missing > 0) {
                LOG_ERR("HATA: %lu blok eksik! Tum-imaj dogrulama atlaniyor.\n",
                        (unsigned long)missing);
            } else {
                int verify_fd = cfs_open("yeni-fw.z1", CFS_READ);
 
                if(verify_fd >= 0) {
                    uint32_t total_checksum = 0;
                    uint8_t  buf[CHUNK_SIZE];
                    int      bytes_read;
 
                    while((bytes_read = cfs_read(verify_fd, buf, sizeof(buf))) > 0) {
                        for(int i = 0; i < bytes_read; i++) {
                            total_checksum += buf[i];
                        }
                    }
                    cfs_close(verify_fd);
 
                    LOG_INFO("Tum-Imaj Dogrulama basarili! "
                             "Total Checksum: %lu\n",
                             (unsigned long)total_checksum);
 
                    LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
 
                } else {
                    LOG_ERR("HATA: Dogrulama icin firmware dosyasi acilamadi!\n");
                }
            }
        }
 
    } else if(pkt->block_num < expected_block) {
        if(received_bitmap[pkt->block_num]) {
            LOG_INFO("Tekrar gelen eski blok %u (zaten alinmis). ACK yenileniyor.\n",
                     pkt->block_num);
        } else {
            LOG_WARN("Beklenmeyen: eski blok %u bitmap'te isaretli degil!\n",
                     pkt->block_num);
        }
    }
 
    char ack_msg[32];
    snprintf(ack_msg, sizeof(ack_msg), "ACK:%u", pkt->block_num);
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
    LOG_INFO("Beklenen blok sayisi: %lu (FIRMWARE_PAYLOAD_LEN=%u bayt)\n",
             (unsigned long)EXPECTED_BLOCKS,
             (unsigned)FIRMWARE_PAYLOAD_LEN);
 
    memset(received_bitmap, 0, sizeof(received_bitmap));
 
    int fd = cfs_open("yeni-fw.z1", CFS_WRITE);
    if(fd >= 0) {
        cfs_close(fd);
        LOG_INFO("Hedef dosya 'yeni-fw.z1' olusturuldu.\n");
    } else {
        LOG_ERR("HATA: Hedef dosya olusturulamadi!\n");
    }
 
    simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                        UDP_CLIENT_PORT, udp_rx_callback);
 
    PROCESS_END();
}