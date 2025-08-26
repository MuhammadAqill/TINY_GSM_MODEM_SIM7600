#define TINY_GSM_MODEM_SIM7600

#include <WiFi.h>
#include <HTTPClient.h>
#include <TinyGsmClient.h>
#include <ArduinoJson.h>

// ===================== WiFi Setup =====================
const char* ssid     = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD_NAME";

// ===================== Telegram Bot Setup =====================
const char* botToken = "TOKEN_BOT_TELEGRAM";    // ganti token
const char* chatID   = "YOUR_ID_CHAT";      // ganti chat id

// ===================== SIM7600 Setup =====================
#define MODEM_RX 16
#define MODEM_TX 17
#define MODEM_BAUD 115200

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

const char apn[] = "diginet"; // APN SIM card

// ===================== Flags & Data =====================
bool connectedViaWiFi = false;
bool connectedViaSIM = false;
bool gpsEnabled = false;

String messageLat = "";
String messageLon = "";

float lastLat = 0.0;
float lastLon = 0.0;

// ===================== Forward Declarations =====================
void connectInternet();
void initializeGPS();
void gpsTask(void *pvParameters);
void telegramTask(void *pvParameters);
void sendTelegramMessage(String message);

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=== ESP32 + SIM7600 GPS Tracker ===");

  // Start SIM7600 serial
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  modem.restart();

  // Always initialize GPS first
  initializeGPS();

  // Then connect internet (WiFi or SIM7600 fallback)
  connectInternet();

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(gpsTask, "gpsTask", 8192, NULL, 1, NULL, 0);  // Core 0
  xTaskCreatePinnedToCore(telegramTask, "telegramTask", 8192, NULL, 1, NULL, 1); // Core 1
}

void loop() {
  // nothing, handled by tasks
}

// ===================== Connect Internet =====================
void connectInternet() {
  WiFi.begin(ssid, password);
  unsigned long wifiTimeout = millis() + 10000;

  while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected.");
    connectedViaWiFi = true;
  } else {
    Serial.println("\nWiFi failed. Trying SIM7600...");

    if (modem.waitForNetwork() && modem.gprsConnect(apn, "", "")) {
      Serial.println("SIM7600: GPRS connected.");
      connectedViaSIM = true;
    } else {
      Serial.println("SIM7600: GPRS failed.");
    }
  }
}

// ===================== GPS Init =====================
void initializeGPS() {
  Serial.println("\n--- Initializing GPS ---");

  SerialAT.println("AT+CGPS=0");
  delay(1000);
  SerialAT.println("AT+CGPS=1,1");
  delay(2000);

  SerialAT.println("AT+CGPS?");
  delay(1000);

  while (SerialAT.available()) {
    String response = SerialAT.readString();
    Serial.print("GPS Status: ");
    Serial.println(response);
    if (response.indexOf("+CGPS: 1") >= 0) {
      gpsEnabled = true;
    }
  }

  if (gpsEnabled) {
    Serial.println("GPS enabled successfully.");
  } else {
    Serial.println("Failed to enable GPS, retrying via TinyGSM...");
    if (modem.enableGPS()) {
      gpsEnabled = true;
      Serial.println("GPS enabled via TinyGSM.");
    }
  }
}

// ===================== GPS Task =====================
void gpsTask(void *pvParameters) {
  while (1) {
    if (gpsEnabled) {
      float lat, lon;
      bool success = modem.getGPS(&lat, &lon);

      if (success && lat != 0 && lon != 0) {
        lastLat = lat;
        lastLon = lon;

        messageLat = "Latitude: " + String(lat, 6);
        messageLon = "Longitude: " + String(lon, 6);

        Serial.println("=== GPS DATA ===");
        Serial.println(messageLat);
        Serial.println(messageLon);
        Serial.printf("Google Maps: https://www.google.com/maps?q=%.6f,%.6f\n", lat, lon);
        Serial.println("================");

      } else {
        Serial.println("Waiting for GPS fix...");
      }
    } else {
      initializeGPS();
    }

    vTaskDelay(10000 / portTICK_PERIOD_MS);  // every 10s
  }
}

// ===================== Telegram Task =====================
void telegramTask(void *pvParameters) {
  while (1) {
    if (lastLat != 0 && lastLon != 0) {
      // Send Lat and Lon separately
      sendTelegramMessage(messageLat);
      sendTelegramMessage(messageLon);

      // Send Google Maps link
      String mapLink = "Google Maps: https://www.google.com/maps?q=" + String(lastLat, 6) + "," + String(lastLon, 6);
      sendTelegramMessage(mapLink);
    } else {
      sendTelegramMessage("❌ GPS not fixed yet...");
      sendTelegramMessage(messageLat);
      sendTelegramMessage(messageLon);
      sendTelegramMessage("kontol");
    }

    vTaskDelay(60000 / portTICK_PERIOD_MS);  // every 60s
  }
}

// ===================== Send Telegram =====================
void sendTelegramMessage(String message) {
  if (connectedViaWiFi) {
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(botToken) + "/sendMessage";

    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"chat_id\":\"" + String(chatID) + "\",\"text\":\"" + message + "\"}";
    int httpCode = http.POST(payload);

    if (httpCode > 0) {
      Serial.printf("Telegram message sent (WiFi), code: %d\n", httpCode);
    } else {
      Serial.printf("Telegram WiFi error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();

  } else if (connectedViaSIM) {
    if (client.connect("api.telegram.org", 443)) {
      String postData = "{\"chat_id\":\"" + String(chatID) + "\",\"text\":\"" + message + "\"}";

      client.println("POST /bot" + String(botToken) + "/sendMessage HTTP/1.1");
      client.println("Host: api.telegram.org");
      client.println("Content-Type: application/json");
      client.println("Content-Length: " + String(postData.length()));
      client.println("Connection: close");
      client.println();
      client.println(postData);

      client.stop();
      Serial.println("Telegram message sent via SIM7600");
    } else {
      Serial.println("SIM7600: Failed to connect Telegram API");
    }
  }
}
