# BIL304 OTA Firmware Transfer Projesi

## 🎥 Demo Video

> **[https://youtu.be/s1y-EFT1ah8]**
>
> Video içeriği: Cooja simülasyonu canlı gösterimi, geliştirilen bölümün anlatması, kullanılan checksum algoritmasının teorik açıklaması.

---

## Proje Hakkında

Contiki-NG üzerinde Cooja simülatörü kullanılarak gerçekleştirilen OTA (Over-the-Air) firmware aktarım sistemi. Gönderici düğüm, firmware imajını 64 baytlık bloklara bölerek alıcı düğüme iletir; alıcı her bloğu doğrular ve CFS (Coffee File System) üzerine kalıcı olarak yazar.

---

## Düğümler

| ID | Rol | Kaynak |
|----|-----|--------|
| 1 | Alıcı düğüm (root) | `udp-server.c` |
| 2 | Gönderici düğüm | `udp-client.c` |
| 3 | İletici komşu düğüm | `udp-client.c` (pasif) |

---

## Paket Yapısı

Her blok şu yapıda bir UDP paketiyle iletilir:

```c
struct ota_packet {
    uint16_t block_num;        // 2 bayt — blok sıra numarası
    uint8_t  data_len;         // 1 bayt — geçerli veri uzunluğu
    uint16_t checksum;         // 2 bayt — toplamsal bütünlük değeri
    uint8_t  payload[64];      // 64 bayt — firmware verisi
};
// Toplam paket boyutu: 69 bayt
```

| Alan | Boyut | Açıklama |
|------|-------|----------|
| `block_num` | 2 B | Hangi bloğun gönderildiği |
| `data_len` | 1 B | Son blokta 64'ten küçük olabilir |
| `checksum` | 2 B | Payload üzerinden hesaplanır |
| `payload` | 64 B | Firmware'in ilgili dilimi |

### Neden 64 bayt?

Z1 mote, 8 KB RAM ile çalışır. 64 baytlık blok boyutu; RAM kullanımını minimize ederken aktarım verimliliğini dengeler. 8192 baytlık temsili firmware dilimi toplamda **128 blok** olarak iletilir.

---

## Checksum Algoritması

### Teorik Açıklama

Her paketin bütünlüğü **toplamsal (additive) 16-bit checksum** ile doğrulanır. Gönderici, payload'daki tüm baytları toplayarak checksum hesaplar ve pakete ekler. Alıcı aynı işlemi tekrarlar; değerler uyuşmazsa paket bozuk kabul edilir ve ACK gönderilmez.

Toplamsal checksum, düşük işlemci gücüne sahip gömülü sistemler için tercih edilir: CRC32 gibi polinom tabanlı algoritmalara kıyasla hesaplama maliyeti çok düşüktür.

### Kod

```c
/* udp-client.c & udp-server.c — aynı fonksiyon her iki tarafta da tanımlı */
static uint16_t
calculate_checksum(uint8_t *data, uint8_t len)
{
    uint16_t sum = 0;
    for(int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}
```

**Kullanım — Gönderici:**
```c
packet.checksum = calculate_checksum(packet.payload, packet.data_len);
```

**Kullanım — Alıcı:**
```c
uint16_t calc_chk = calculate_checksum(pkt->payload, pkt->data_len);
if(calc_chk != pkt->checksum) {
    LOG_ERR("Checksum uyuşmazlığı! Blok %u bozuk. ACK gönderilmiyor.\n",
            pkt->block_num);
    return;  // ACK gönderilmez → gönderici timeout'tan sonra tekrar gönderir
}
```

---

## Stop-and-Wait Protokolü

### Teorik Açıklama

Projede **Stop-and-Wait ARQ** (Automatic Repeat Request) protokolü uygulanmıştır. Gönderici her seferinde yalnızca bir blok gönderir ve alıcıdan ACK (onay) gelene kadar bekler. ACK 2 saniye içinde gelmezse aynı blok yeniden gönderilir. Maksimum 5 deneme hakkı aşılırsa aktarım hata ile sonlandırılır.

```
Gönderici                    Alıcı
    |                           |
    |--- Blok 0 (checksum) ---> |
    |                           | [checksum doğrula, diske yaz]
    |<-------- ACK:0 ---------- |
    |                           |
    |--- Blok 1 (checksum) ---> |
    |         (kayıp)           |
    |    [2s timeout]           |
    |--- Blok 1 (tekrar) -----> |
    |<-------- ACK:1 ---------- |
    |          ...              |
```

### Kod — Gönderici Tarafı

```c
/* udp-client.c */
#define SEND_INTERVAL  (2 * CLOCK_SECOND)  // ACK bekleme süresi
#define MAX_RETRIES    5                    // Maksimum yeniden deneme

// Her timer dolumunda çalışır:
if(retry_count >= MAX_RETRIES) {
    LOG_ERR("Blok %lu için %d kez denendi, ACK alınamadı.\n",
            current_block, MAX_RETRIES);
    transfer_done = true;
    break;
}

simple_udp_sendto(&udp_conn, &packet, sizeof(packet), &dest_ipaddr);
retry_count++;

// ACK gelince (udp_rx_callback içinde):
retry_count = 0;
current_block++;   // Yalnızca ACK onayından sonra sonraki bloğa geç
```

---

## Durum Yönetimi

Alıcı düğüm, hangi blokların başarıyla alındığını bir bitmap dizisiyle izler:

```c
/* udp-server.c */
static uint8_t received_bitmap[128];  // Her indeks bir bloğu temsil eder

// Başarılı yazım sonrası:
received_bitmap[pkt->block_num] = 1;

// Aktarım sonunda eksik blok kontrolü:
for(uint32_t i = 0; i < EXPECTED_BLOCKS; i++) {
    if(!received_bitmap[i]) {
        LOG_ERR("Eksik blok: %lu\n", i);
    }
}
```

---

## Kalıcı Depolama — CFS (Coffee File System)

Contiki-NG'nin yerleşik dosya sistemi Coffee FS kullanılarak bloklar diske yazılır. Dosya adı `yeni-fw.z1` olarak belirlendi (Coffee FS 16 karakter sınırı nedeniyle kısaltıldı).

```c
/* udp-server.c */
int fd = cfs_open("yeni-fw.z1", CFS_WRITE);
if(fd >= 0) {
    // Her bloğu doğru ofsete yaz
    cfs_seek(fd, (cfs_offset_t)(pkt->block_num * CHUNK_SIZE), CFS_SEEK_SET);
    cfs_write(fd, pkt->payload, pkt->data_len);
    cfs_close(fd);
}
```

---

## Tüm-İmaj Bütünlük Doğrulaması

Tüm bloklar alındıktan sonra diske yazılan imajın tamamı okutularak global checksum hesaplanır:

```c
/* udp-server.c */
int verify_fd = cfs_open("yeni-fw.z1", CFS_READ);
uint32_t total_checksum = 0;
uint8_t  buf[64];
int      bytes_read;

while((bytes_read = cfs_read(verify_fd, buf, sizeof(buf))) > 0) {
    for(int i = 0; i < bytes_read; i++) {
        total_checksum += buf[i];
    }
}
cfs_close(verify_fd);
LOG_INFO("Tüm-İmaj Doğrulama başarılı! Total Checksum: %lu\n", total_checksum);
LOG_INFO("Yüklenmeye hazır yeni firmware alımı tamamlandı.\n");
```

---

## OTA Metadata Yapısı

`ota-metadata.h` dosyası, CC1352R gibi gerçek donanımlarda çift-slot OTA mimarisini yönetmek için tasarlanmış metadata yapısını tanımlar:

```c
typedef struct {
    uint32_t magic;           // 0x4F544131 — yapının geçerlilik damgası
    uint32_t active_slot;     // Şu an çalışan imajın slotu (A=0 / B=1)
    uint32_t candidate_slot;  // İndirilecek yeni imajın slotu
    uint32_t state_a;         // Slot A durumu
    uint32_t state_b;         // Slot B durumu
    uint32_t crc_a;           // Slot A imajının CRC32 değeri
    uint32_t crc_b;           // Slot B imajının CRC32 değeri
    uint32_t metadata_crc32;  // Metadata bütünlük kontrolü
} ota_boot_metadata_t;
```

İmaj durum makinesi: `EMPTY → DOWNLOADING → VERIFIED → PENDING → CONFIRMED`

---

## Derleme

```bash
make TARGET=z1
```

---

## Simülasyon

Cooja simülatörünü başlatın ve `BIL304-OS-Project-1.csc` dosyasını açın.

Beklenen log çıktısı (alıcı düğüm):
```
[INFO] Blok 0/127 diske yazıldı (Boyut: 64 bayt | Checksum: XXXX)
[INFO] Durum: 1/128 blok alındı
...
[INFO] Tüm-İmaj Doğrulama başarılı! Total Checksum: XXXXXX
[INFO] Yüklenmeye hazır yeni firmware alımı tamamlandı.
```
