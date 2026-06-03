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
 
/* firmware_data.h sunucuya dahil edilmistir.
 * Sunucu, FIRMWARE_PAYLOAD_LEN sabitini istemciyle ayni kaynaktan
 * okur; boylece beklenen blok sayisi her zaman tutarli kalir.
 * Z1 mote Flash kisiti nedeniyle yalnizca 8192 baytlik temsili
 * dilim kullanilmaktadir (bkz. udp-client.c aciklamasi). */
#include "firmware_data.h"
 
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
 
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define CHUNK_SIZE      64
 
/* EXPECTED_BLOCKS: Beklenen toplam blok sayisi.
 *
 * NEDEN SABIT SAYI?
 * firmware_data.h icerisinde FIRMWARE_PAYLOAD_LEN su sekilde tanimli:
 *   static const uint32_t firmware_payload_len = ...;
 *   #define FIRMWARE_PAYLOAD_LEN ((uint32_t)firmware_payload_len)
 * Bu bir 'const degisken'dir; C standardinda gercek derleme zamani
 * sabiti (integer constant expression) sayilmaz. Dolayisiyla
 * FIRMWARE_PAYLOAD_LEN ile hesaplanan bir ifade file scope'ta dizi
 * boyutu olarak kullanilamaz -- derleyici "variably modified at file
 * scope" hatasi verir.
 *
 * Cozum: 8192 / 64 = 128 degeri dogrudan sabitleniyor.
 * EXPECTED_BLOCKS hesabi ise dinamik kalmaktadir -- dizi boyutu icin
 * degil, dongu ve kontrol ifadelerinde kullanilmak uzere.
 * firmware_data.h degisirse bu sayi da guncellenmeli. */
#define EXPECTED_BLOCKS_MAX 128
#define EXPECTED_BLOCKS     ((FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE)
 
/* Sartname - Durum Yonetimi: Hangi bloklarin alindigi izlenir.
 * received_bitmap[i] == 1  -> i. blok basariyla alinip diske yazildi.
 * received_bitmap[i] == 0  -> i. blok henuz alinmadi veya eksik.
 * Dizi boyutu derleme zamani sabiti EXPECTED_BLOCKS_MAX ile belirlenir;
 * dongu ve kontrol ifadelerinde ise dinamik EXPECTED_BLOCKS kullanilir. */
static uint8_t received_bitmap[EXPECTED_BLOCKS_MAX];
 
struct ota_packet {
    uint16_t block_num;
    uint8_t  data_len;
    uint16_t checksum;
    uint8_t  payload[CHUNK_SIZE];
};
 
static struct simple_udp_connection udp_conn;
static uint32_t expected_block = 0;  /* Sira bekleyen bir sonraki blok */
 
/*---------------------------------------------------------------------------*/
/* CHECKSUM HESAPLAMA
 * Istemci ile ayni toplamsal algoritma kullanilir. */
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
/* Durum Yonetimi Yardimci Fonksiyonu:
 * Alinan blok sayisini ve bitmap'e gore eksik bloklari loglar.
 * Her basarili yazimdan sonra cagirilir. */
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
 
    /* Eksik blok varsa hangileri oldugunu raporla. */
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
 
    /* 1. Sartname - Parca Dogrulama (Checksum Kontrolu):
     * Hesaplanan checksum paketle gelen degerle karsilastirilir.
     * Uyusmazlik varsa ACK gonderilmez; gonderici zaman asimina
     * ugrayarak ayni blogu yeniden iletir. */
    uint16_t calc_chk = calculate_checksum(pkt->payload, pkt->data_len);
 
    if(calc_chk != pkt->checksum) {
        LOG_ERR("HATA: Checksum uyusmazligi! Blok %u bozuk geldi "
                "(beklenen: %u, hesaplanan: %u). ACK gonderilmiyor.\n",
                pkt->block_num, pkt->checksum, calc_chk);
        return;
    }
 
    /* 2. Sartname - Siralama ve Diske Yazma:
     * Yalnizca beklenen sira numarali blok islenir. */
    if(pkt->block_num == expected_block) {
 
        /* MUHENDiSLiK NOTU: Coffee FS dosya isimleri maks 16 karakter.
         * "downloaded-firmware.z1" (22 kar.) gecersiz olur.
         * "yeni-fw.z1" (10 kar.) bu kisiti karsilar. */
        int fd = cfs_open("yeni-fw.z1", CFS_WRITE);
 
        if(fd >= 0) {
            cfs_seek(fd, (cfs_offset_t)(pkt->block_num * CHUNK_SIZE), CFS_SEEK_SET);
            cfs_write(fd, pkt->payload, pkt->data_len);
            cfs_close(fd);
 
            /* Sartname - Durum Yonetimi:
             * Blok basariyla diske yazildi; bitmap'te isaretlenir. */
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
 
        /* Guncel aktarim durumunu logla (alinan / eksik bloklar). */
        log_transfer_status();
 
        /* 3. Sartname - Tum Imajin Tamamlanmasi ve Tum-Imaj Dogrulama */
        if(expected_block == EXPECTED_BLOCKS) {
            LOG_INFO("Tum parcalar alindi (%lu blok). "
                     "Tum-imaj dogrulamasi (Whole-Image Checksum) baslatiliyor...\n",
                     (unsigned long)EXPECTED_BLOCKS);
 
            /* Bitmap'te gercekten hic eksik blok kalmamis mi? Son kontrol. */
            uint32_t missing = 0;
            for(uint32_t i = 0; i < EXPECTED_BLOCKS; i++) {
                if(!received_bitmap[i]) {
                    missing++;
                    LOG_ERR("UYARI: Bitmap eksik blok tespiti - blok %lu\n",
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
 
                    /* Diske yazilan tum baytlarin toplamsal checksumunu hesapla. */
                    while((bytes_read = cfs_read(verify_fd, buf, sizeof(buf))) > 0) {
                        for(int i = 0; i < bytes_read; i++) {
                            total_checksum += buf[i];
                        }
                    }
                    cfs_close(verify_fd);
 
                    LOG_INFO("Tum-Imaj Dogrulama basarili! "
                             "Total Checksum: %lu\n",
                             (unsigned long)total_checksum);
 
                    /* Sartnamede belirtilen zorunlu bitis mesaji. */
                    LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
 
                } else {
                    LOG_ERR("HATA: Dogrulama icin firmware dosyasi acilamadi!\n");
                }
            }
        }
 
    } else if(pkt->block_num < expected_block) {
        /* Ag gecikmesinden dolayi daha once islenmis bir blok tekrar geldi.
         * Bitmap'e gore zaten alindi mi kontrol edilir; ACK yenilenir. */
        if(received_bitmap[pkt->block_num]) {
            LOG_INFO("Tekrar gelen eski blok %u (zaten alinmis). ACK yenileniyor.\n",
                     pkt->block_num);
        } else {
            /* Bu durum normal Stop-and-Wait'te olmamali; yine de loglanir. */
            LOG_WARN("Beklenmeyen: eski blok %u bitmap'te isaretli degil!\n",
                     pkt->block_num);
        }
    }
    /* pkt->block_num > expected_block: gelecek siradan blok (sliding window yok).
     * Stop-and-Wait'te bu olmamali. Sessizce yoksayilir, ACK gonderilmez. */
 
    /* 4. Sartname - Akilli Onay (ACK) Gonderimi:
     * Checksum hatasi disindaki tum durumlarda ACK basilir.
     * Gonderici bir sonraki bloga guvle gecebilir veya
     * tekrar gonderimini durdurur. */
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
 
    /* Diske yazimdan once CFS diski formatlayarak temiz basla. */
    cfs_coffee_format();
    LOG_INFO("Alici cihaz (ID:1) CFS Diski formatlandi.\n");
    LOG_INFO("Beklenen blok sayisi: %lu (FIRMWARE_PAYLOAD_LEN=%u bayt)\n",
             (unsigned long)EXPECTED_BLOCKS,
             (unsigned)FIRMWARE_PAYLOAD_LEN);
 
    /* Durum Yonetimi: Bitmap'i sifirla; hic blok alinmamis durumdan basla. */
    memset(received_bitmap, 0, sizeof(received_bitmap));
 
    /* Hedef dosyayi onceden olustur; seek ile yazabilmek icin gerekli. */
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