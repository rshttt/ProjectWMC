#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_now.h>
#include <esp_wifi.h> // Untuk esp_wifi_set_channel

// --- 1. KONFIGURASI GLOBAL ---
// --- GANTI MAC INI DENGAN MAC ADDRESS DARI ESP32 PARTNER ANDA ---
uint8_t peerAddress[] = {0x10, 0x06, 0x1C, 0xF6, 0x86, 0x0C};
const uint8_t WIFI_CHANNEL = 1; // Channel Wi-Fi untuk ESP-NOW & AP

const char* ap_ssid = "ESP32_File_Transfer_Micro";
const char* ap_password = "password123";

#define MAX_FILENAME_LEN 64
#define CHUNK_DATA_SIZE  200 // Ukuran data payload per ESP-NOW packet (max 250)

// --- 2. STRUKTUR PROTOKOL TRANSFER FILE (SAMA UNTUK KEDUA ESP32) ---
enum PacketType {
  PKT_INFO_FILE,    // Mengirim nama file, ukuran, jumlah chunk
  PKT_DATA_CHUNK,   // Mengirim kepingan data file
  PKT_ACK,          // Pengakuan (Acknowledgement) bahwa chunk diterima
  PKT_END_FILE      // Menandakan akhir transfer file
};

// Struktur untuk paket info file awal
typedef struct {
  PacketType type;
  char fileName[MAX_FILENAME_LEN];
  long fileSize;
  unsigned long totalChunks;
} FileInfoPacket;

// Struktur untuk paket data chunk
typedef struct {
  PacketType type;
  unsigned long chunkNum;  // Nomor urut chunk
  size_t dataSize;         // Ukuran data aktual dalam chunk (bisa kurang dari CHUNK_DATA_SIZE)
  uint8_t data[CHUNK_DATA_SIZE];
} DataChunkPacket;

// Struktur untuk paket END
typedef struct {
  PacketType type;
} struct_end;

// Struktur untuk paket ACK
typedef struct {
  PacketType type;
  unsigned long ackChunkNum; // Nomor chunk yang di-ACK
  bool success;              // Status ACK (opsional, untuk deteksi error lebih lanjut)
} AckPacket;

// --- 3. VARIABEL GLOBAL UNTUK MANAJEMEN STATE ---
WebServer server(80);
File currentUploadFile;
String currentUploadFileName;
File currentReceivingFile; // File yang sedang di-download via ESP-NOW
char currentReceivingFileName[MAX_FILENAME_LEN];
unsigned long nextExpectedChunk = 0; // Nomor chunk yang diharapkan berikutnya
bool isReceivingFile = false; // Status apakah sedang dalam proses menerima file
unsigned long lastAckReceived = 0; // Untuk sisi pengirim, melacak ACK terakhir
bool ackReceivedFlag = false; // Flag untuk sinyal ACK diterima
unsigned long currentSendingChunk = 0; // Nomor chunk yang sedang dikirim
String g_fileToSend = "";

// --- 4. DEKLARASI FUNGSI (Untuk kerapian kode) ---
void handleRoot();
void handleFileUpload();
void handleFileList();
void handleDownload();
void handleNotFound();
void sendFile();

// --- 5. CALLBACK ESP-NOW ---
// Dijalankan setelah ESP32 mencoba mengirim paket ESP-NOW
void OnDataSent(const wifi_tx_info_t* tx_info, const esp_now_send_status_t status) {
  // Callback ini tidak bisa langsung memproses ACK yang datang dari penerima
  // karena ACK adalah paket terpisah yang diterima oleh OnDataRecv.
  // Ini hanya memberitahu status pengiriman paket yang BARU SAJA DIKIRIM.
  Serial.print("Status Pengiriman Paket: ");
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Berhasil");
    // Jika Anda punya logika retransmisi berbasis ACK, ini bisa digunakan
    // untuk membersihkan antrian paket yang dikirim
  } else {
    Serial.println("Gagal");
    // Jika pengiriman gagal di level ESP-NOW, mungkin perlu pengiriman ulang
    // atau menandai error.
  }
}

// Dijalankan saat ESP32 menerima paket ESP-NOW
void OnDataRecv(const esp_now_recv_info * info, const uint8_t *incomingData, int len) {
  PacketType type;
  memcpy(&type, incomingData, sizeof(type)); // Baca hanya tipe paket

  // --- LOGIKA PENERIMA FILE ---
  if (type == PKT_INFO_FILE) {
    FileInfoPacket packet;
    memcpy(&packet, incomingData, sizeof(packet));

    Serial.printf("\n[RCV] Menerima info file: %s (%ld bytes, %lu chunks)\n", packet.fileName, packet.fileSize, packet.totalChunks);
    
    // Siapkan untuk mulai menerima file baru
    isReceivingFile = true;
    nextExpectedChunk = 0;
    strncpy(currentReceivingFileName, packet.fileName, sizeof(currentReceivingFileName));

    // Buka file di LittleFS untuk menulis
    if (LittleFS.exists(currentReceivingFileName)) {
        LittleFS.remove(currentReceivingFileName); // Hapus jika sudah ada
    }
    currentReceivingFile = LittleFS.open(currentReceivingFileName, "w");
    if (!currentReceivingFile) {
      Serial.println("[RCV] ERROR: Gagal membuat file di LittleFS!");
      isReceivingFile = false;
      return;
    }

    // Kirim ACK bahwa info file diterima
    AckPacket ack;
    ack.type = PKT_ACK;
    ack.ackChunkNum = 0; // ACK untuk paket info (chunk 0)
    ack.success = true;
    esp_now_send(info->src_addr, (uint8_t*)&ack, sizeof(ack));
    Serial.println("[RCV] Info File diterima. Mengirim ACK (Chunk 0).");

  } else if (type == PKT_DATA_CHUNK && isReceivingFile) {
    DataChunkPacket packet;
    memcpy(&packet, incomingData, sizeof(packet));

    if (packet.chunkNum == nextExpectedChunk) { // Jika ini chunk yang diharapkan
      if (currentReceivingFile) {
        currentReceivingFile.write(packet.data, packet.dataSize);
        nextExpectedChunk++;
        Serial.print("."); // Progres
        
        // Kirim ACK untuk chunk yang berhasil diterima
        AckPacket ack;
        ack.type = PKT_ACK;
        ack.ackChunkNum = packet.chunkNum;
        ack.success = true;
        esp_now_send(info->src_addr, (uint8_t*)&ack, sizeof(ack));
      }
    } else { // Jika chunk tidak sesuai urutan (mungkin hilang atau duplikat)
      Serial.printf("[RCV] Peringatan: Menerima chunk %lu tapi menunggu %lu. Mengirim ulang ACK untuk yang diharapkan.\n", packet.chunkNum, nextExpectedChunk);
      // Kirim ACK untuk chunk terakhir yang berhasil diterima (agar pengirim tahu harus kirim ulang)
      AckPacket ack;
      ack.type = PKT_ACK;
      ack.ackChunkNum = nextExpectedChunk == 0 ? 0 : nextExpectedChunk - 1; // Kirim ACK untuk chunk sebelumnya
      ack.success = false; // Menandakan ada ketidaksesuaian
      esp_now_send(info->src_addr, (uint8_t*)&ack, sizeof(ack));
    }
    
  } else if (type == PKT_END_FILE && isReceivingFile) {
    if (currentReceivingFile) {
      currentReceivingFile.close();
      Serial.println("\n[RCV] File berhasil diterima dan disimpan!");
      isReceivingFile = false;
      
      // Kirim ACK akhir
      AckPacket ack;
      ack.type = PKT_ACK;
      ack.ackChunkNum = nextExpectedChunk; // Menandakan semua chunk diterima
      ack.success = true;
      esp_now_send(info->src_addr, (uint8_t*)&ack, sizeof(ack));
    }

  // --- LOGIKA PENGIRIM UNTUK MENERIMA ACK ---
  } else if (type == PKT_ACK) {
    AckPacket ack;
    memcpy(&ack, incomingData, sizeof(ack));
    // Ini adalah ACK yang diterima oleh sisi PENGIRIM
    // untuk paket yang baru saja dia kirim.
    lastAckReceived = ack.ackChunkNum;
    ackReceivedFlag = true; // Set flag agar loop pengiriman bisa melanjutkan
    Serial.printf("[SND] ACK diterima untuk chunk %lu. Status: %s\n", ack.ackChunkNum, ack.success ? "Sukses" : "Gagal/Rewind");
  }
}

// --- 6. SETUP ---
void setup() {
  Serial.begin(115200);
  delay(10000);

  // Inisialisasi LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS mount failed!");
    return;
  }
  Serial.println("LittleFS berhasil di-mount.");

  // Konfigurasi WiFi AP_STA untuk Web Server dan ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Access Point IP: "); Serial.println(IP);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE); // Kunci channel

  // --- ESP-NOW Setup ---
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed!");
    return;
  }
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("ERROR: Gagal menambahkan peer!");
    return;
  }
  Serial.printf("ESP-NOW Peer %02X:%02X:%02X:%02X:%02X:%02X ditambahkan di Channel %d\n",
                peerAddress[0], peerAddress[1], peerAddress[2], peerAddress[3], peerAddress[4], peerAddress[5], WIFI_CHANNEL);

  // --- Web Server Setup ---
  server.on("/", HTTP_GET, handleRoot);
  
  server.on("/upload", HTTP_POST, []() {
    server.sendHeader("Location", "/files", true);
    server.send(302, "text/plain", "Upload Selesai, Mengalihkan...");
  }, handleFileUpload);

  server.on("/files", HTTP_GET, handleFileList);

  server.on("/download", HTTP_GET, handleDownload);

  server.on("/sendfile", HTTP_GET, [](){
    String filename = server.arg("name");
    if (filename.length() > 0) {
      g_fileToSend = "/" + filename;
      server.send(200, "text/plain", "Mulai mengirim " + filename);
      sendFile(); // Panggil fungsi kirim file
    } else {
      server.send(400, "text/plain", "Nama file tidak diberikan.");
    }
  });

  server.on("/delete", HTTP_GET, [](){
    String filename = server.arg("name");
    if (filename.length() > 0) {
      // Buat path file yang lengkap
      String fullpath = "/" + filename;
      
      if (LittleFS.exists(fullpath)) {
        // Hapus file
        LittleFS.remove(fullpath);
        Serial.println("[WEB] File dihapus: " + fullpath);
        // Arahkan pengguna kembali ke daftar file
        server.sendHeader("Location", "/files", true);
        server.send(302, "text/plain", "File dihapus, mengalihkan...");
      } else {
        server.send(404, "text/plain", "File tidak ditemukan!");
      }
    } else {
      server.send(400, "text/plain", "Nama file tidak diberikan.");
    }
  });

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server dimulai.");
}

// --- 7. LOOP ---
void loop() {
  server.handleClient(); // Proses permintaan dari klien
  // Logika pengiriman file berbasis ACK akan berjalan secara sinkron di sendFile()
  // atau bisa juga dijalankan di loop jika mau asinkron penuh (lebih kompleks)
}

// --- 8. IMPLEMENTASI FUNGSI WEB SERVER ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>ESP32 File Transfer</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;} button{padding:10px;margin:5px;}</style>";
  html += "</head><body><h1>ESP32 File Transfer</h1>";
  html += "<h2>1. Upload File ke ESP32 Ini:</h2>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='upload'><br>";
  html += "<button type='submit'>Upload ke Memori ESP32</button>";
  html += "</form>";
  html += "<h2>2. Kelola File:</h2>";
  html += "<p><a href='/files'><button>Lihat/Kirim/Download File</button></a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    
    currentUploadFileName = filename; // Simpan nama file ke variabel global
    
    Serial.print("[WEB] Mulai upload dari browser: "); Serial.println(currentUploadFileName);
    currentUploadFile = LittleFS.open(currentUploadFileName, "w"); // Gunakan variabel global
  
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (currentUploadFile) currentUploadFile.write(upload.buf, upload.currentSize); // Gunakan variabel global
  
  } else if (upload.status == UPLOAD_FILE_END) {
    if (currentUploadFile) currentUploadFile.close(); // Gunakan variabel global
    
    Serial.print("[WEB] Upload dari browser selesai: "); Serial.println(currentUploadFileName); // Gunakan variabel global
  }
}

void handleFileList() {
  String html = "<!DOCTYPE html><html><head><title>File List</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;} button{padding:8px;margin:3px;}</style>";
  html += "</head><body><h1>Daftar File di ESP32</h1><ul>";
  
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) {
    html += "<li>Gagal membuka direktori root!</li>";
  } else {
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String filename = String(file.name());
        if (filename.startsWith("/")) {
          filename = filename.substring(1);
        } // Hapus '/' di awal
        html += "<li>" + filename + " (" + file.size() + " bytes) ";
        html += "<a href='/download?name=" + filename + "'><button>Download</button></a>";
        html += "<a href='/sendfile?name=" + filename + "'><button>Kirim via ESP-NOW</button></a>";
        html += "<a href='/delete?name=" + filename + "' onclick='return confirm(\"Yakin ingin menghapus " + filename + "?\");'><button class='delete'>Delete</button></a>";
        html += "</li>";
      }
      file = root.openNextFile();
    }
  }
  html += "</ul><p><a href='/'><button>Kembali ke Halaman Utama</button></a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleDownload() {
  String filename = server.arg("name");
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  File downloadFile = LittleFS.open(filename, "r");
  if (downloadFile) {
    server.sendHeader("Content-Disposition", "attachment; filename=" + filename.substring(1));
    server.streamFile(downloadFile, "application/octet-stream"); // Kirim sebagai binary stream
    downloadFile.close();
  } else {
    server.send(404, "text/plain", "File tidak ditemukan!");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Halaman tidak ditemukan.");
}

// --- 9. FUNGSI PENGIRIMAN FILE VIA ESP-NOW (SEGMENTASI & ACK) ---
void sendFile() {
  Serial.printf("\n[SND] Memulai pengiriman file '%s' via ESP-NOW...\n", g_fileToSend.c_str());
  File file = LittleFS.open(g_fileToSend.c_str(), "r");
  if (!file) {
    Serial.println("[SND] ERROR: Gagal membuka file untuk dikirim!");
    return;
  }

  // --- 1. Kirim Paket INFO_FILE ---
  FileInfoPacket infoPacket;
  infoPacket.type = PKT_INFO_FILE;
  strncpy(infoPacket.fileName, g_fileToSend.c_str(), sizeof(infoPacket.fileName));
  infoPacket.fileSize = file.size();
  infoPacket.totalChunks = (file.size() + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE;
  
  currentSendingChunk = 0; // Reset counter chunk yang dikirim
  ackReceivedFlag = false; // Reset flag ACK

  // Kirim info file, tunggu ACK (chunk 0)
  for (int retry = 0; retry < 5; retry++) { // Coba 5 kali
    esp_now_send(peerAddress, (uint8_t*)&infoPacket, sizeof(infoPacket));
    Serial.println("[SND] Mengirim PKT_INFO_FILE. Menunggu ACK (Chunk 0)...");
    unsigned long startTime = millis();
    while (!ackReceivedFlag && (millis() - startTime < 1000)) { // Tunggu ACK max 1 detik
      delay(1);
      server.handleClient(); // Tetap layani web server saat menunggu
    }
    if (ackReceivedFlag && lastAckReceived == 0) { // ACK untuk info paket berhasil
      break;
    }
    Serial.println("[SND] Timeout atau ACK salah, kirim ulang PKT_INFO_FILE...");
  }
  if (!ackReceivedFlag) {
    Serial.println("[SND] GAGAL: Tidak ada ACK untuk PKT_INFO_FILE setelah beberapa kali coba.");
    file.close();
    return;
  }

  // --- 2. Kirim Paket DATA_CHUNK ---
  DataChunkPacket dataPacket;
  dataPacket.type = PKT_DATA_CHUNK;
  
  unsigned long sentBytes = 0;

  while (file.available()) {
    dataPacket.chunkNum = currentSendingChunk;
    dataPacket.dataSize = file.read(dataPacket.data, CHUNK_DATA_SIZE);
    
    ackReceivedFlag = false; // Reset flag untuk chunk ini

    for (int retry = 0; retry < 5; retry++) { // Coba kirim & tunggu ACK 5 kali
      esp_now_send(peerAddress, (uint8_t*)&dataPacket, sizeof(dataPacket));
      Serial.print(">"); // Progres pengiriman chunk

      unsigned long startTime = millis();
      while (!ackReceivedFlag && (millis() - startTime < 500)) { // Tunggu ACK max 0.5 detik
        delay(1);
        server.handleClient();
      }

      if (ackReceivedFlag && lastAckReceived == currentSendingChunk) { // ACK yang benar diterima
        currentSendingChunk++;   // Lanjut ke chunk berikutnya
        sentBytes += dataPacket.dataSize;
        break; // Keluar dari loop retry, lanjut ke chunk berikutnya
      } else if (ackReceivedFlag && lastAckReceived < currentSendingChunk) { // Penerima minta mundur
        Serial.printf("\n[SND] Penerima minta mundur ke chunk %lu. Kirim ulang dari sana.\n", lastAckReceived + 1);
        file.seek(lastAckReceived * CHUNK_DATA_SIZE); // Pindah posisi file
        currentSendingChunk = lastAckReceived + 1; // Mulai kirim dari chunk ini
        ackReceivedFlag = false;
        // Kita tidak 'break', kita terus di loop retry ini untuk kirim ulang chunk yang diminta
      } else { // Timeout atau ACK yang salah diterima (misal untuk chunk yang sudah lama)
        Serial.printf("\n[SND] Timeout atau ACK salah untuk chunk %lu. Mengirim ulang.\n", currentSendingChunk);
      }
    }

    if (!ackReceivedFlag && lastAckReceived != currentSendingChunk) { // Jika setelah retry tetap gagal
      Serial.println("[SND] GAGAL: Tidak ada ACK setelah beberapa kali coba. Pengiriman dibatalkan.");
      file.close();
      return;
    }
  }

  // --- 3. Kirim Paket END_FILE ---
  struct_end endPacket;
  endPacket.type = PKT_END_FILE;
  esp_now_send(peerAddress, (uint8_t*)&endPacket, sizeof(endPacket));
  Serial.println("\n[SND] Pengiriman semua chunk file selesai. Mengirim paket END.");
  
  file.close();
  Serial.println("[SND] File berhasil dikirim.");
}
