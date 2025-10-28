#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

/*
 * ======================================================================
 * GANTI DENGAN ALAMAT MAC DARI ESP32 PARTNER ANDA
 * ======================================================================
 */
uint8_t peerAddress[] = {0x94, 0x51, 0xDC, 0x2C, 0xE1, 0xEC};
/*
 * ======================================================================
 */


// Struktur data untuk mengirim pesan teks
typedef struct struct_message {
  char text[200]; // Buffer untuk menampung teks yang diketik
} struct_message;

struct_message myData;

esp_now_peer_info_t peerInfo;

// Buffer untuk menampung ketikan dari Serial Monitor
char sendBuffer[200];
int bufferIndex = 0;

// === CALLBACK FUNCTION ===

// Callback function saat data terkirim
void OnDataSent(const wifi_tx_info_t* tx_info, const esp_now_send_status_t status) {
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Pengiriman Berhasil" : "Pengiriman Gagal");
}

// Callback function saat data diterima
void OnDataRecv(const esp_now_recv_info * info, const uint8_t *incomingData, int len) {
  memcpy(&myData, incomingData, sizeof(myData));
  
  // Cetak pesan yang diterima
  Serial.print("\n[PESAN DARI SEBERANG]: ");
  Serial.println(myData.text);
  
  // Tampilkan prompt lagi
  Serial.print("Ketik pesan Anda: ");
}

// === SETUP ===

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Setel mode WiFi ke Station (STA)
  WiFi.mode(WIFI_STA);
  // Kunci di Channel 1 untuk stabilitas
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Inisialisasi ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Daftarkan KEDUA callback (kirim dan terima)
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Daftarkan peer (partner chat)
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 1; // Harus sama (Channel 1)
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Gagal menambahkan peer");
    return;
  }
  
  Serial.println("========================================");
  Serial.println("     ESP-NOW 2-Way Chat Terminal v1.0     ");
  Serial.print("MAC Address Anda: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Terhubung ke Peer: ");
  for(int i=0; i<6; i++) {
    Serial.printf("%02X", peerAddress[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println("\n========================================");
  Serial.print("Ketik pesan Anda (lalu tekan Enter): ");
}

// === LOOP ===

void loop() {
  // Cek apakah ada input dari Serial Monitor
  if (Serial.available()) {
    char c = Serial.read(); // Baca per karakter
    Serial.print(c); // Tampilkan karakter yang diketik (echo)

    if (c == '\n') { // Jika pengguna menekan tombol Enter
      
      // 1. Selesaikan string yang diketik
      sendBuffer[bufferIndex] = '\0'; // Tambahkan null terminator

      // 2. Salin string dari buffer ke struct
      strncpy(myData.text, sendBuffer, sizeof(myData.text));
      
      // 3. Kirim pesan
      esp_err_t result = esp_now_send(peerAddress, (uint8_t *) &myData, sizeof(myData));
      
      // 4. Reset buffer untuk pesan berikutnya
      bufferIndex = 0;
      memset(sendBuffer, 0, sizeof(sendBuffer)); // Kosongkan buffer
      
      Serial.print("\nKetik pesan Anda: ");

    } else {
      // Kumpulkan karakter ke buffer
      if (bufferIndex < sizeof(sendBuffer) - 1) {
        sendBuffer[bufferIndex] = c;
        bufferIndex++;
      }
    }
  }
  // Tidak perlu delay, biarkan loop berjalan cepat untuk responsif
}
