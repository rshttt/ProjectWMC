#include "WiFi.h"

void setup(){
  Serial.begin(115200);
  delay(500);
  WiFi.mode(WIFI_MODE_STA);
}

void loop(){
  // Kode ini akan berjalan berulang kali
  Serial.print("Alamat MAC ESP32 ini adalah: ");
  Serial.println(WiFi.macAddress());
  
  // Beri jeda 5 detik agar tidak membanjiri monitor
  delay(5000); 
}