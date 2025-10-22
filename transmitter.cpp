#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// GANTI DENGAN MAC ADDRESS PENERIMA
uint8_t broadcastAddress[] = {0x94, 0x51, 0xDC, 0x2C, 0xE1, 0xEC};

// Struktur data yang akan dikirim. Harus sama di pengirim dan penerima.
typedef struct struct_message {
  char a[50];
  int b;
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

// Callback function saat data terkirim
void OnDataSent(const wifi_tx_info_t* tx_info, const esp_now_send_status_t status) {
  Serial.print("\r\nStatus Pengiriman Terakhir: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Berhasil" : "Gagal");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Inisialisasi ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Gagal menambahkan peer");
    return;
  }
  
  Serial.println("==========================");
  Serial.println("  TRANSMITTER SIAP  ");
  Serial.println("==========================");
}

void loop() {
  snprintf(myData.a, sizeof(myData.a), "%s", "Halo, ini pesan dari Transmitter");
  myData.b = random(1, 100);

  Serial.printf("Mengirim Nilai Int: %d\n", myData.b);

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
  
  if (result == !(ESP_OK)) {
    Serial.println("Gagal mengirim data");
  }

  delay(5000);
}