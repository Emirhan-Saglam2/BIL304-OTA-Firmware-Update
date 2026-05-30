# BIL304 OTA Firmware Transfer Projesi

## Proje Hakkinda
Contiki-NG uzerinde Cooja simulatoru kullanilarak gerceklestirilen OTA (Over-the-Air) firmware aktarim sistemi.

## Dugumler
- ID:1 - Alici dugum (udp-server.c)
- ID:2 - Gonderici dugum (udp-client.c)
- ID:3 - Iletici komsu dugum (udp-client.c)

## Ozellikler
- 64 byte blok boyutu ile parcali firmware aktarimi
- Stop-and-Wait protokolu ile guvenilir iletim
- Blok bazli checksum dogrulama
- Tum-imaj checksum dogrulamasi
- CFS (Coffee File System) ile kalici depolama

## Derleme
make TARGET=z1

## Simulasyon
Cooja ile BIL304-OS-Project-1.csc dosyasini ac
