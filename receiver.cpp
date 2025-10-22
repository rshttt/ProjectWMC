#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Struktur data yang akan diterima. Harus sama dengan di pengirim.
typedef struct struct_message {
  char a[50];
  int b;
} struct_message;

struct_message myData;

// Callback function saat data diterima
void OnDataRecv(const esp_now_recv_info * info, const uint8_t *incomingData, int len) {
  memcpy(&myData, incomingData, sizeof(myData));
  Serial.print("Data diterima dari MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", info->src_addr[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  Serial.print("Data diterima: ");
  Serial.println(len);
  Serial.print("Pesan Char: ");
  Serial.println(myData.a);
  Serial.print("Nilai Int: ");
  Serial.println(myData.b);
  Serial.println();
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

  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("==========================");
  Serial.println("   RECEIVER SIAP   ");
  Serial.println("==========================");
}

void loop() {
  // Biarkan kosong, semua proses terjadi di callback
}