/*
 * KameraIOT v4.0 - STABIL
 *
 * Tek mod: Flash + Sabit yanma
 * Gece modunda ilk aciliste %100 parlaklik (radar efekti, tek sefer)
 * Hareket devam ederse sekans bastan baslar (ama radar efekti tekrar tetiklenmez)
 * twitter.com/buywinrar
 * Kutuphaneler: PubSubClient, ArduinoJson v7+
 * Reset: Pin 2 -> GND 3sn
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "mbedtls/md5.h"

#define AP_SSID "KameraIOT"
#define AP_PASS "123456"
#define FW_VERSION "4.0"
#define RESET_PIN 2
#define RESET_HOLD_MS 3000
#define MAX_CAMERAS 16
#define MAX_GROUPS 8
#define MAX_PER_GROUP 10

#define MQTT_HOST "writemqtthosthere" //dontforget
#define MQTT_PORT 8883
#define MQTT_USER "admin"
#define MQTT_PASS "mqttpasshere"
#define TOPIC_PREFIX "kameraiot/"

struct Camera {
  char name[32];
  char ip[32];
  char user[32];
  char pass[48];
  int port;
  bool enabled;
  // Runtime
  unsigned long lockedUntil;
  int consecutiveFails;
  volatile bool shouldStop;  // graceful stop
};

struct Group {
  char name[32];
  bool enabled;
  int flashDuration;
  int steadyDuration;
  int flashInterval;
  int dayBrightness;
  int nightBrightness;
  int dayStartH, dayStartM;
  int nightStartH, nightStartM;
  bool radarEnabled;        // ilk %100 flash acik mi
  int radarBrightness;      // radar parlaklik (varsayilan 100)
  int triggerCams[MAX_PER_GROUP];
  int nTriggers;
  int lightCams[MAX_PER_GROUP];
  int nLights;
};

struct Settings {
  char wifiSSID[64];
  char wifiPass[64];
  char deviceId[32];
  int numCameras;
  int numGroups;
  Camera cameras[MAX_CAMERAS];
  Group groups[MAX_GROUPS];
};

struct GroupState {
  bool active;
  unsigned long sequenceStart;
  int state;               // 1=flash, 2=steady
  bool flashOn;
  unsigned long lastToggle;
  int currentBrightness;
  bool radarFired;
};

// ===== GLOBAL =====
Settings cfg;
Preferences prefs;
WebServer setupServer(80);
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

bool wifiConnected = false;
bool apMode = false;
bool mqttConnected = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastStatusPublish = 0;
unsigned long resetPinPressedAt = 0;
String topicBase;

GroupState groupStates[MAX_GROUPS];
volatile bool motionFlags[MAX_CAMERAS];
TaskHandle_t alertTasks[MAX_CAMERAS];
static int taskIndices[MAX_CAMERAS];
bool streamConnected[MAX_CAMERAS];

// Config fingerprint - stream restart karari icin
String lastStreamFingerprint = "";

SemaphoreHandle_t mtx;

// ===== PREFERENCES =====

void loadSettings() {
  prefs.begin("kamiot", true);

  strlcpy(cfg.wifiSSID, prefs.getString("wS", "").c_str(), 64);
  strlcpy(cfg.wifiPass, prefs.getString("wP", "").c_str(), 64);

  String did = prefs.getString("did", "");
  if (did.length() == 0) {
    uint64_t mac = ESP.getEfuseMac();
    did = "esp" + String((uint32_t)(mac & 0xFFFFFFFF), HEX);
  }
  strlcpy(cfg.deviceId, did.c_str(), 32);

  String camJson = prefs.getString("cams", "[]");
  String grpJson = prefs.getString("grps", "[]");
  prefs.end();

  JsonDocument doc;
  deserializeJson(doc, camJson);
  cfg.numCameras = 0;
  for (JsonObject c : doc.as<JsonArray>()) {
    if (cfg.numCameras >= MAX_CAMERAS) break;
    Camera& cam = cfg.cameras[cfg.numCameras];
    strlcpy(cam.name, c["name"] | "Kamera", 32);
    strlcpy(cam.ip, c["ip"] | "", 32);
    strlcpy(cam.user, c["user"] | "admin", 32);
    strlcpy(cam.pass, c["pass"] | "", 48);
    cam.port = c["port"] | 0;
    cam.enabled = c["en"] | true;
    cam.lockedUntil = 0;
    cam.consecutiveFails = 0;
    cam.shouldStop = false;
    cfg.numCameras++;
  }

  JsonDocument doc2;
  deserializeJson(doc2, grpJson);
  cfg.numGroups = 0;
  for (JsonObject g : doc2.as<JsonArray>()) {
    if (cfg.numGroups >= MAX_GROUPS) break;
    Group& grp = cfg.groups[cfg.numGroups];
    strlcpy(grp.name, g["name"] | "Grup", 32);
    grp.enabled = g["en"] | true;
    grp.flashDuration = g["fd"] | 5000;
    grp.steadyDuration = g["sd"] | 15000;
    grp.flashInterval = g["fi"] | 500;
    grp.dayBrightness = g["dbr"] | 50;
    grp.nightBrightness = g["nbr"] | 4;
    grp.dayStartH = g["dh"] | 7;
    grp.dayStartM = g["dm"] | 30;
    grp.nightStartH = g["nh"] | 19;
    grp.nightStartM = g["nm"] | 0;
    grp.radarEnabled = g["re"] | true;
    grp.radarBrightness = g["rb"] | 100;

    grp.nTriggers = 0;
    if (g["tc"].is<JsonArray>()) {
      for (int v : g["tc"].as<JsonArray>()) {
        if (grp.nTriggers < MAX_PER_GROUP) grp.triggerCams[grp.nTriggers++] = v;
      }
    }

    grp.nLights = 0;
    if (g["lc"].is<JsonArray>()) {
      for (int v : g["lc"].as<JsonArray>()) {
        if (grp.nLights < MAX_PER_GROUP) grp.lightCams[grp.nLights++] = v;
      }
    }

    cfg.numGroups++;
  }

  Serial.printf("Ayarlar yuklendi: %d kamera, %d grup\n", cfg.numCameras, cfg.numGroups);
}

void saveCamsGroups() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < cfg.numCameras; i++) {
    JsonObject c = arr.add<JsonObject>();
    c["name"] = cfg.cameras[i].name;
    c["ip"] = cfg.cameras[i].ip;
    c["user"] = cfg.cameras[i].user;
    c["pass"] = cfg.cameras[i].pass;
    c["port"] = cfg.cameras[i].port;
    c["en"] = cfg.cameras[i].enabled;
  }
  String camJson;
  serializeJson(doc, camJson);

  JsonDocument doc2;
  JsonArray arr2 = doc2.to<JsonArray>();
  for (int i = 0; i < cfg.numGroups; i++) {
    JsonObject g = arr2.add<JsonObject>();
    Group& gr = cfg.groups[i];
    g["name"] = gr.name;
    g["en"] = gr.enabled;
    g["fd"] = gr.flashDuration;
    g["sd"] = gr.steadyDuration;
    g["fi"] = gr.flashInterval;
    g["dbr"] = gr.dayBrightness;
    g["nbr"] = gr.nightBrightness;
    g["dh"] = gr.dayStartH;
    g["dm"] = gr.dayStartM;
    g["nh"] = gr.nightStartH;
    g["nm"] = gr.nightStartM;
    g["re"] = gr.radarEnabled;
    g["rb"] = gr.radarBrightness;
    JsonArray tc = g["tc"].to<JsonArray>();
    for (int j = 0; j < gr.nTriggers; j++) tc.add(gr.triggerCams[j]);
    JsonArray lc = g["lc"].to<JsonArray>();
    for (int j = 0; j < gr.nLights; j++) lc.add(gr.lightCams[j]);
  }
  String grpJson;
  serializeJson(doc2, grpJson);

  prefs.begin("kamiot", false);
  prefs.putString("cams", camJson);
  prefs.putString("grps", grpJson);
  prefs.end();
}

void saveWifi() {
  prefs.begin("kamiot", false);
  prefs.putString("wS", cfg.wifiSSID);
  prefs.putString("wP", cfg.wifiPass);
  prefs.putString("did", cfg.deviceId);
  prefs.end();
}

void factoryReset() {
  Serial.println("FABRIKA SIFIRLAMA!");
  prefs.begin("kamiot", false);
  prefs.clear();
  prefs.end();
  delay(500);
  ESP.restart();
}

// Stream konfigurasyon fingerprint'i - sadece kamera bilgileri ve trigger listeleri
String computeStreamFingerprint() {
  String f = "";
  for (int i = 0; i < cfg.numCameras; i++) {
    Camera& c = cfg.cameras[i];
    f += String(c.ip) + ":" + String(c.port) + ":" + String(c.user) + ":" + String(c.pass) + ":" + String(c.enabled ? 1 : 0) + "|";
  }
  f += "#";
  for (int i = 0; i < cfg.numGroups; i++) {
    Group& g = cfg.groups[i];
    if (!g.enabled) continue;
    for (int j = 0; j < g.nTriggers; j++) f += String(g.triggerCams[j]) + ",";
    f += ";";
  }
  return f;
}

// ===== SAAT =====

int getCurrentBrightness(int groupId) {
  if (groupId < 0 || groupId >= cfg.numGroups) return 50;
  Group& g = cfg.groups[groupId];
  struct tm t;
  if (!getLocalTime(&t, 0)) return g.dayBrightness;
  int mins = t.tm_hour * 60 + t.tm_min;
  int dayStart = g.dayStartH * 60 + g.dayStartM;
  int nightStart = g.nightStartH * 60 + g.nightStartM;
  return (mins >= dayStart && mins < nightStart) ? g.dayBrightness : g.nightBrightness;
}

bool isDayModeForGroup(int groupId) {
  if (groupId < 0 || groupId >= cfg.numGroups) return true;
  Group& g = cfg.groups[groupId];
  struct tm t;
  if (!getLocalTime(&t, 0)) return true;
  int mins = t.tm_hour * 60 + t.tm_min;
  int dayStart = g.dayStartH * 60 + g.dayStartM;
  int nightStart = g.nightStartH * 60 + g.nightStartM;
  return (mins >= dayStart && mins < nightStart);
}

// ===== DIGEST =====

String md5Hash(String input) {
  unsigned char output[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_md5_finish(&ctx, output);
  mbedtls_md5_free(&ctx);
  String hex = "";
  for (int i = 0; i < 16; i++) {
    if (output[i] < 0x10) hex += "0";
    hex += String(output[i], HEX);
  }
  return hex;
}

String extractField(String src, String field) {
  int start = src.indexOf(field + "=\"");
  if (start < 0) return "";
  start += field.length() + 2;
  int end = src.indexOf("\"", start);
  return src.substring(start, end);
}

String buildDigestAuth(Camera& cam, String method, String uri, String authHeader) {
  String realm = extractField(authHeader, "realm");
  String nonce = extractField(authHeader, "nonce");
  String qop = extractField(authHeader, "qop");
  String nc = "00000001";
  String cnonce = String(esp_random(), HEX);
  String ha1 = md5Hash(String(cam.user) + ":" + realm + ":" + String(cam.pass));
  String ha2 = md5Hash(method + ":" + uri);
  String resp = md5Hash(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
  return "Digest username=\"" + String(cam.user) + "\", realm=\"" + realm +
         "\", nonce=\"" + nonce + "\", uri=\"" + uri +
         "\", qop=" + qop + ", nc=" + nc +
         ", cnonce=\"" + cnonce + "\", response=\"" + resp + "\"";
}

// ===== ISAPI BRIGHTNESS =====

bool setBrightnessOnCamera(int camIdx, int brightness) {
  if (camIdx < 0 || camIdx >= cfg.numCameras) return false;
  Camera& cam = cfg.cameras[camIdx];
  if (!cam.enabled || cam.port == 0) return false;

  String url = "http://" + String(cam.ip) + ":" + String(cam.port) + "/ISAPI/Image/channels/1/supplementLight";
  String uri = "/ISAPI/Image/channels/1/supplementLight";
  String xml = "<SupplementLight><whiteLightBrightness>" + String(brightness) + "</whiteLightBrightness></SupplementLight>";

  HTTPClient http1;
  http1.begin(url);
  const char* hkeys[] = {"WWW-Authenticate"};
  http1.collectHeaders(hkeys, 1);
  http1.setTimeout(3000);
  int c1 = http1.GET();
  if (c1 != 401) { http1.end(); return false; }
  String ah = http1.header("WWW-Authenticate");
  http1.end();
  if (ah.length() == 0) return false;

  String digest = buildDigestAuth(cam, "PUT", uri, ah);
  HTTPClient http2;
  http2.begin(url);
  http2.addHeader("Content-Type", "application/xml");
  http2.addHeader("Authorization", digest);
  http2.setTimeout(3000);
  int c2 = http2.PUT(xml);
  http2.end();
  return c2 == 200;
}

// Paralel brightness: her kamera icin kisa omurlu task
struct BrightnessCmd {
  int camIdx;
  int brightness;
};

void brightnessTask(void* param) {
  BrightnessCmd* cmd = (BrightnessCmd*)param;
  setBrightnessOnCamera(cmd->camIdx, cmd->brightness);
  delete cmd;
  vTaskDelete(NULL);
}

void setGroupBrightness(int groupId, int brightness) {
  Group& g = cfg.groups[groupId];

  if (g.nLights == 1) {
    setBrightnessOnCamera(g.lightCams[0], brightness);
  } else {
    for (int i = 0; i < g.nLights; i++) {
      BrightnessCmd* cmd = new BrightnessCmd{g.lightCams[i], brightness};
      xTaskCreate(brightnessTask, "br", 4096, cmd, 2, NULL);
    }
  }
  groupStates[groupId].currentBrightness = brightness;
}

// ===== GRUP MOTORU =====

void startGroupSequence(int groupId) {
  if (groupId < 0 || groupId >= cfg.numGroups) return;
  if (!cfg.groups[groupId].enabled) return;
  if (cfg.groups[groupId].nLights == 0) return;

  xSemaphoreTake(mtx, portMAX_DELAY);
  GroupState& gs = groupStates[groupId];
  bool wasActive = gs.active;
  gs.active = true;
  gs.sequenceStart = millis();
  gs.flashOn = false;
  gs.lastToggle = 0;
  gs.state = 1;
  // Radar efekt sadece yeni sekans basladiginda ates eder
  // Aktif sekans tekrar tetiklenirse radar tekrar yanmaz (komsu rahatsiz etmeyelim)
  gs.radarFired = wasActive;
  xSemaphoreGive(mtx);
  Serial.printf("Grup %d basladi%s\n", groupId, wasActive ? " (yenileme)" : "");
}

void stopGroupSequence(int groupId) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  groupStates[groupId].active = false;
  groupStates[groupId].state = 0;
  xSemaphoreGive(mtx);
  setGroupBrightness(groupId, 0);
  Serial.printf("Grup %d durdu\n", groupId);
}

void processGroup(int groupId) {
  GroupState& gs = groupStates[groupId];
  if (!gs.active) return;

  Group& g = cfg.groups[groupId];
  unsigned long now = millis();
  unsigned long elapsed = now - gs.sequenceStart;
  int br = getCurrentBrightness(groupId);

  switch (gs.state) {
    case 1: // FLASH
    {
      // Radar efekt: ilk giriste ayarli parlaklik yak (tek sefer)
      if (!gs.radarFired) {
        if (cfg.groups[groupId].radarEnabled) {
          setGroupBrightness(groupId, cfg.groups[groupId].radarBrightness);
        }
        gs.radarFired = true;
        gs.lastToggle = now;
        gs.flashOn = true;
        if (cfg.groups[groupId].radarEnabled) return;
      }

      if (elapsed >= (unsigned long)g.flashDuration) {
        gs.state = 2;
        gs.sequenceStart = now;
        setGroupBrightness(groupId, br);
        return;
      }

      if (now - gs.lastToggle >= (unsigned long)g.flashInterval) {
        gs.flashOn = !gs.flashOn;
        setGroupBrightness(groupId, gs.flashOn ? br : 0);
        gs.lastToggle = now;
      }
      break;
    }
    case 2: // STEADY
    {
      if (gs.currentBrightness != br) setGroupBrightness(groupId, br);
      if (elapsed >= (unsigned long)g.steadyDuration) {
        stopGroupSequence(groupId);
        return;
      }
      break;
    }
  }
}

// ===== ALERTSTREAM TASK =====

void alertStreamTask(void* param) {
  int camIdx = *((int*)param);

  for (;;) {
    // Graceful stop kontrolu
    if (camIdx < cfg.numCameras && cfg.cameras[camIdx].shouldStop) {
      streamConnected[camIdx] = false;
      Serial.printf("[cam %d] Task graceful stop\n", camIdx);
      vTaskDelete(NULL);
      return;
    }

    if (!wifiConnected || camIdx >= cfg.numCameras || !cfg.cameras[camIdx].enabled) {
      streamConnected[camIdx] = false;
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    Camera& cam = cfg.cameras[camIdx];
    if (cam.port == 0 || strlen(cam.ip) == 0) {
      streamConnected[camIdx] = false;
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      continue;
    }

    if (cam.lockedUntil > millis()) {
      streamConnected[camIdx] = false;
      vTaskDelay(30000 / portTICK_PERIOD_MS);
      continue;
    }

    int backoff = min(5 + cam.consecutiveFails * 5, 60);

    String url = "http://" + String(cam.ip) + ":" + String(cam.port) + "/ISAPI/Event/notification/alertStream";
    String uri = "/ISAPI/Event/notification/alertStream";

    HTTPClient http1;
    http1.begin(url);
    const char* hkeys[] = {"WWW-Authenticate"};
    http1.collectHeaders(hkeys, 1);
    http1.setTimeout(8000);
    int c1 = http1.GET();

    if (c1 != 401) {
      Serial.printf("[%s] Auth hata: %d\n", cam.name, c1);
      if (c1 == 403 || c1 == 423) {
        cam.consecutiveFails++;
        if (cam.consecutiveFails >= 3) {
          cam.lockedUntil = millis() + 300000;
          Serial.printf("[%s] 5 dakika kilitlendi\n", cam.name);
        }
      }
      http1.end();
      vTaskDelay(backoff * 1000 / portTICK_PERIOD_MS);
      continue;
    }

    String ah = http1.header("WWW-Authenticate");
    http1.end();

    if (ah.length() == 0) {
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    String digest = buildDigestAuth(cam, "GET", uri, ah);
    HTTPClient http2;
    http2.begin(url);
    http2.addHeader("Authorization", digest);
    http2.setTimeout(60000);
    int c2 = http2.sendRequest("GET");

    if (c2 != 200) {
      Serial.printf("[%s] Stream hata: %d\n", cam.name, c2);
      if (c2 == 401 || c2 == 403 || c2 == 423) {
        cam.consecutiveFails++;
        if (cam.consecutiveFails >= 3) {
          cam.lockedUntil = millis() + 300000;
        }
      }
      http2.end();
      vTaskDelay(backoff * 1000 / portTICK_PERIOD_MS);
      continue;
    }

    Serial.printf("[%s:%d] AlertStream baglandi\n", cam.name, cam.port);
    cam.consecutiveFails = 0;
    cam.lockedUntil = 0;
    streamConnected[camIdx] = true;

    WiFiClient* stream = http2.getStreamPtr();
    String buffer = "";
    unsigned long lastData = millis();

    while (http2.connected() && wifiConnected && !cam.shouldStop) {
      if (stream->available() > 0) {
        int readCount = 0;
        while (stream->available() && readCount < 512) {
          buffer += (char)stream->read();
          readCount++;
        }
        lastData = millis();

        int endIdx;
        while ((endIdx = buffer.indexOf("</EventNotificationAlert>")) >= 0) {
          String oneEvent = buffer.substring(0, endIdx + 25);
          if (oneEvent.indexOf("<eventType>VMD</eventType>") >= 0 &&
              oneEvent.indexOf("<eventState>active</eventState>") >= 0) {
            Serial.printf("[%s] HAREKET!\n", cam.name);
            xSemaphoreTake(mtx, portMAX_DELAY);
            motionFlags[camIdx] = true;
            xSemaphoreGive(mtx);
          }
          buffer = buffer.substring(endIdx + 25);
        }
        if (buffer.length() > 8192) buffer = "";
      } else {
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      if (millis() - lastData > 120000) break;
    }

    http2.end();
    streamConnected[camIdx] = false;

    // Graceful stop geldi mi?
    if (cam.shouldStop) {
      Serial.printf("[%s] Graceful stop\n", cam.name);
      vTaskDelete(NULL);
      return;
    }

    Serial.printf("[%s] Koptu, yeniden denenecek\n", cam.name);
    vTaskDelay(3000 / portTICK_PERIOD_MS);
  }
}

bool isCameraInAnyTrigger(int camIdx) {
  for (int i = 0; i < cfg.numGroups; i++) {
    if (!cfg.groups[i].enabled) continue;
    for (int j = 0; j < cfg.groups[i].nTriggers; j++) {
      if (cfg.groups[i].triggerCams[j] == camIdx) return true;
    }
  }
  return false;
}

// Graceful shutdown: task'a dur sinyali gonder, bekleyip sil
void stopAlertTask(int camIdx) {
  if (!alertTasks[camIdx]) return;

  cfg.cameras[camIdx].shouldStop = true;
  Serial.printf("[cam %d] Graceful stop isteniyor\n", camIdx);

  // Task'in kendini silmesi icin max 10sn bekle
  for (int i = 0; i < 100; i++) {
    if (!streamConnected[camIdx]) break;
    delay(100);
  }

  // Hala yasiyorsa force kill
  if (alertTasks[camIdx]) {
    // eTaskGetState ile kontrol edebiliriz ama basit tutup delay'e guvenelim
    vTaskDelay(500 / portTICK_PERIOD_MS);
    // Task kendini silmis olmali simdiye kadar, handle'i bos yap
  }

  alertTasks[camIdx] = NULL;
  cfg.cameras[camIdx].shouldStop = false;
}

void stopAllAlertTasks() {
  for (int i = 0; i < MAX_CAMERAS; i++) {
    if (alertTasks[i]) stopAlertTask(i);
  }
}

void startAlertTasks() {
  for (int i = 0; i < cfg.numCameras; i++) {
    if (!cfg.cameras[i].enabled) continue;
    if (cfg.cameras[i].port == 0) continue;
    if (!isCameraInAnyTrigger(i)) continue;
    if (alertTasks[i]) continue; // zaten calisiyor

    taskIndices[i] = i;
    cfg.cameras[i].shouldStop = false;
    String taskName = "alert" + String(i);
    xTaskCreatePinnedToCore(alertStreamTask, taskName.c_str(), 12000, &taskIndices[i], 1, &alertTasks[i], 0);
  }
}

void handleCameraMotion(int camIdx) {
  for (int i = 0; i < cfg.numGroups; i++) {
    if (!cfg.groups[i].enabled) continue;
    for (int j = 0; j < cfg.groups[i].nTriggers; j++) {
      if (cfg.groups[i].triggerCams[j] == camIdx) {
        startGroupSequence(i);
        if (mqttConnected) {
          JsonDocument doc;
          doc["camera"] = cfg.cameras[camIdx].name;
          doc["camIdx"] = camIdx;
          doc["group"] = i;
          doc["groupName"] = cfg.groups[i].name;
          doc["time"] = millis();
          String out;
          serializeJson(doc, out);
          mqtt.publish((topicBase + "event/motion").c_str(), out.c_str());
        }
        break;
      }
    }
  }
}

// ===== MQTT =====

String getTopic(String sub) { return topicBase + sub; }

void mqttPublishStatus() {
  if (!mqttConnected) return;

  JsonDocument doc;
  doc["device"] = cfg.deviceId;
  doc["fw"] = FW_VERSION;
  doc["wifi"] = wifiConnected;
  doc["rssi"] = WiFi.RSSI();
  doc["ip"] = WiFi.localIP().toString();
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();

  JsonArray cams = doc["cameras"].to<JsonArray>();
  for (int i = 0; i < cfg.numCameras; i++) {
    JsonObject c = cams.add<JsonObject>();
    c["idx"] = i;
    c["name"] = cfg.cameras[i].name;
    c["streamOk"] = streamConnected[i];
    c["locked"] = (cfg.cameras[i].lockedUntil > millis());
    c["fails"] = cfg.cameras[i].consecutiveFails;
  }

  JsonArray grps = doc["groups"].to<JsonArray>();
  for (int i = 0; i < cfg.numGroups; i++) {
    JsonObject g = grps.add<JsonObject>();
    g["idx"] = i;
    g["name"] = cfg.groups[i].name;
    g["active"] = groupStates[i].active;
    g["brightness"] = groupStates[i].currentBrightness;
    g["isDay"] = isDayModeForGroup(i);
  }

  String out;
  serializeJson(doc, out);
  mqtt.publish(getTopic("status").c_str(), out.c_str(), true);
}

void mqttPublishConfig() {
  if (!mqttConnected) return;

  JsonDocument doc;
  doc["device"] = cfg.deviceId;

  JsonArray cams = doc["cameras"].to<JsonArray>();
  for (int i = 0; i < cfg.numCameras; i++) {
    JsonObject c = cams.add<JsonObject>();
    c["name"] = cfg.cameras[i].name;
    c["ip"] = cfg.cameras[i].ip;
    c["user"] = cfg.cameras[i].user;
    c["port"] = cfg.cameras[i].port;
    c["en"] = cfg.cameras[i].enabled;
  }

  JsonArray grps = doc["groups"].to<JsonArray>();
  for (int i = 0; i < cfg.numGroups; i++) {
    JsonObject g = grps.add<JsonObject>();
    Group& gr = cfg.groups[i];
    g["name"] = gr.name;
    g["en"] = gr.enabled;
    g["fd"] = gr.flashDuration;
    g["sd"] = gr.steadyDuration;
    g["fi"] = gr.flashInterval;
    g["dbr"] = gr.dayBrightness;
    g["nbr"] = gr.nightBrightness;
    g["dh"] = gr.dayStartH;
    g["dm"] = gr.dayStartM;
    g["nh"] = gr.nightStartH;
    g["nm"] = gr.nightStartM;
    g["re"] = gr.radarEnabled;
    g["rb"] = gr.radarBrightness;
    JsonArray tc = g["tc"].to<JsonArray>();
    for (int j = 0; j < gr.nTriggers; j++) tc.add(gr.triggerCams[j]);
    JsonArray lc = g["lc"].to<JsonArray>();
    for (int j = 0; j < gr.nLights; j++) lc.add(gr.lightCams[j]);
  }

  String out;
  serializeJson(doc, out);
  mqtt.publish(getTopic("config").c_str(), out.c_str(), true);
}

void mqttHandleCommand(String cmd, String payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (cmd == "trigger") {
    int gid = doc["group"] | 0;
    if (gid >= 0 && gid < cfg.numGroups) startGroupSequence(gid);
  }
  else if (cmd == "stop") {
    int gid = doc["group"] | 0;
    if (gid >= 0 && gid < cfg.numGroups) stopGroupSequence(gid);
  }
  else if (cmd == "config") {
    if (err) { Serial.println("Config parse hata"); return; }

    // Once aktif gruplarin sekanslarini durdur
    for (int i = 0; i < cfg.numGroups; i++) {
      if (groupStates[i].active) stopGroupSequence(i);
    }

    // Kameralar
    if (doc["cameras"].is<JsonArray>()) {
      cfg.numCameras = 0;
      for (JsonObject c : doc["cameras"].as<JsonArray>()) {
        if (cfg.numCameras >= MAX_CAMERAS) break;
        Camera& cam = cfg.cameras[cfg.numCameras];
        strlcpy(cam.name, c["name"] | "Kamera", 32);
        strlcpy(cam.ip, c["ip"] | "", 32);
        strlcpy(cam.user, c["user"] | "admin", 32);
        const char* newPass = c["pass"] | "";
        if (strlen(newPass) > 0) strlcpy(cam.pass, newPass, 48);
        cam.port = c["port"] | 0;
        cam.enabled = c["en"] | true;
        cam.lockedUntil = 0;
        cam.consecutiveFails = 0;
        cam.shouldStop = false;
        cfg.numCameras++;
      }
    }

    // Gruplar
    if (doc["groups"].is<JsonArray>()) {
      cfg.numGroups = 0;
      for (JsonObject g : doc["groups"].as<JsonArray>()) {
        if (cfg.numGroups >= MAX_GROUPS) break;
        Group& grp = cfg.groups[cfg.numGroups];
        strlcpy(grp.name, g["name"] | "Grup", 32);
        grp.enabled = g["en"] | true;
        grp.flashDuration = g["fd"] | 5000;
        grp.steadyDuration = g["sd"] | 15000;
        grp.flashInterval = g["fi"] | 500;
        grp.dayBrightness = g["dbr"] | 50;
        grp.nightBrightness = g["nbr"] | 4;
        grp.dayStartH = g["dh"] | 7;
        grp.dayStartM = g["dm"] | 30;
        grp.nightStartH = g["nh"] | 19;
        grp.nightStartM = g["nm"] | 0;
        grp.radarEnabled = g["re"] | true;
        grp.radarBrightness = g["rb"] | 100;

        grp.nTriggers = 0;
        if (g["tc"].is<JsonArray>()) {
          for (int v : g["tc"].as<JsonArray>()) {
            if (grp.nTriggers < MAX_PER_GROUP) grp.triggerCams[grp.nTriggers++] = v;
          }
        }
        grp.nLights = 0;
        if (g["lc"].is<JsonArray>()) {
          for (int v : g["lc"].as<JsonArray>()) {
            if (grp.nLights < MAX_PER_GROUP) grp.lightCams[grp.nLights++] = v;
          }
        }
        cfg.numGroups++;
      }
    }

    saveCamsGroups();

    // Fingerprint degistiyse stream'leri restart et
    String newFp = computeStreamFingerprint();
    if (newFp != lastStreamFingerprint) {
      Serial.println("Stream konfigurasyon degisti, task'lar yeniden baslatiliyor");
      stopAllAlertTasks();
      delay(500); // kameranin socket'i kapatmasi icin
      startAlertTasks();
      lastStreamFingerprint = newFp;
    } else {
      Serial.println("Stream konfigurasyon ayni, task'lara dokunulmadi");
    }

    mqttPublishConfig();
    Serial.println("Config guncellendi");
  }
  else if (cmd == "getConfig") mqttPublishConfig();
  else if (cmd == "getStatus") mqttPublishStatus();
  else if (cmd == "restart") { delay(500); ESP.restart(); }
  else if (cmd == "reset") factoryReset();
  else if (cmd == "unlock") {
    int camIdx = doc["camera"] | -1;
    if (camIdx >= 0 && camIdx < cfg.numCameras) {
      cfg.cameras[camIdx].lockedUntil = 0;
      cfg.cameras[camIdx].consecutiveFails = 0;
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String t = String(topic);
  String base = topicBase + "cmd/";
  if (t.startsWith(base)) {
    String cmd = t.substring(base.length());
    Serial.printf("CMD: %s\n", cmd.c_str());
    mqttHandleCommand(cmd, msg);
  }
}

bool mqttConnect() {
  if (mqtt.connected()) return true;
  String clientId = "kamiot-" + String(cfg.deviceId);
  Serial.println("MQTT: " + clientId);
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                   getTopic("online").c_str(), 1, true, "false")) {
    Serial.println("MQTT BAGLANDI");
    mqttConnected = true;
    mqtt.subscribe((topicBase + "cmd/+").c_str());
    mqtt.publish(getTopic("online").c_str(), "true", true);
    mqttPublishConfig();
    mqttPublishStatus();
    return true;
  } else {
    Serial.printf("MQTT hata: %d\n", mqtt.state());
    mqttConnected = false;
    return false;
  }
}

// ===== WIFI KURULUM =====

void handleSetupPage() {
  String html = R"rawhtml(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>KameraIOT Kurulum</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0a0a0a;color:#e5e5e5;padding:20px}
.wrap{max-width:420px;margin:0 auto}
h1{color:#f59e0b;font-size:24px;text-align:center;margin-bottom:6px}
.sub{text-align:center;color:#666;font-size:13px;margin-bottom:24px}
.card{background:#141414;border:1px solid #2a2a2a;border-radius:14px;padding:20px;margin-bottom:14px}
label{display:block;font-size:13px;color:#a1a1aa;margin:14px 0 6px}
label:first-child{margin-top:0}
input,select{width:100%;padding:11px;background:#1f1f1f;border:1px solid #3a3a3a;color:#fff;border-radius:8px;font-size:14px}
.btn{display:block;width:100%;padding:14px;background:#f59e0b;color:#000;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;margin-top:16px}
.scan{font-size:12px;color:#f59e0b;cursor:pointer;text-align:right;margin-bottom:8px}
.msg{padding:12px;border-radius:8px;margin-top:14px;text-align:center;font-size:14px}
.ok{background:#064e3b;color:#6ee7b7}.err{background:#7f1d1d;color:#fca5a5}
.did{background:#141414;padding:10px;border-radius:6px;font-family:monospace;font-size:13px;text-align:center;color:#f59e0b;margin-top:10px;border:1px solid #2a2a2a}
.info{font-size:11px;color:#71717a;text-align:center;margin-top:16px}
</style></head><body><div class='wrap'>
<h1>KameraIOT</h1><div class='sub'>WiFi Kurulum</div>
<div class='card'>
<div class='scan' onclick='scanWifi()'>WiFi Tara</div>
<select id='ssidSel' onchange="document.getElementById('ssid').value=this.value">
<option value=''>-- Tarayin veya manuel girin --</option></select>
<label>SSID</label><input id='ssid'>
<label>Sifre</label><input id='wpass' type='password'>
</div>
<button class='btn' onclick='save()'>Kaydet ve Baglan</button>
<div id='msg'></div>
<div class='info'>Cihaz ID:</div>
<div class='did'>)rawhtml" + String(cfg.deviceId) + R"rawhtml(</div>
<script>
function scanWifi(){
  document.querySelector('.scan').textContent='Taraniyor...';
  fetch('/api/scan').then(r=>r.json()).then(d=>{
    let s=document.getElementById('ssidSel');
    s.innerHTML="<option value=''>-- Secin --</option>";
    d.forEach(n=>s.innerHTML+="<option value='"+n.ssid+"'>"+n.ssid+" ("+n.rssi+"dB)</option>");
    document.querySelector('.scan').textContent='WiFi Tara';
  });
}
function save(){
  let ssid=document.getElementById('ssid').value;
  if(!ssid){alert('SSID gerekli');return}
  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid,wpass:document.getElementById('wpass').value})})
  .then(r=>r.json()).then(r=>{document.getElementById('msg').innerHTML="<div class='msg "+(r.ok?"ok":"err")+"'>"+r.msg+"</div>"});
}
</script></body></html>
)rawhtml";
  setupServer.send(200, "text/html", html);
}

void handleApiScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    j += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  j += "]";
  WiFi.scanDelete();
  setupServer.send(200, "application/json", j);
}

void handleApiWifi() {
  String body = setupServer.arg("plain");
  JsonDocument doc;
  deserializeJson(doc, body);
  const char* ssid = doc["ssid"] | "";
  const char* wp = doc["wpass"] | "";
  if (strlen(ssid) == 0) {
    setupServer.send(400, "application/json", "{\"ok\":false,\"msg\":\"SSID gerekli\"}");
    return;
  }
  strlcpy(cfg.wifiSSID, ssid, 64);
  strlcpy(cfg.wifiPass, wp, 64);
  saveWifi();
  setupServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"Kaydedildi! Yeniden baslatiliyor...\"}");
  delay(1000);
  ESP.restart();
}

// ===== WIFI =====

bool connectWifi() {
  if (strlen(cfg.wifiSSID) == 0) return false;
  Serial.println("WiFi: " + String(cfg.wifiSSID));
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.deviceId);
  WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nIP: " + WiFi.localIP().toString());
    wifiConnected = true;
    return true;
  }
  wifiConnected = false;
  return false;
}

void startAP() {
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  Serial.println("AP: " + String(AP_SSID));
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  setupServer.on("/", handleSetupPage);
  setupServer.on("/api/scan", handleApiScan);
  setupServer.on("/api/wifi", HTTP_POST, handleApiWifi);
  setupServer.begin();
}

void checkResetPin() {
  static bool wasPressed = false;
  bool pressed = (digitalRead(RESET_PIN) == LOW);
  if (pressed && !wasPressed) {
    resetPinPressedAt = millis();
    wasPressed = true;
  } else if (!pressed) {
    wasPressed = false;
  } else if (pressed && wasPressed) {
    if (millis() - resetPinPressedAt >= RESET_HOLD_MS) {
      factoryReset();
    }
  }
}

// ===== SETUP & LOOP =====

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== KameraIOT v" FW_VERSION " ===");

  pinMode(RESET_PIN, INPUT_PULLUP);
  mtx = xSemaphoreCreateMutex();

  for (int i = 0; i < MAX_CAMERAS; i++) {
    alertTasks[i] = NULL;
    motionFlags[i] = false;
    streamConnected[i] = false;
  }
  for (int i = 0; i < MAX_GROUPS; i++) {
    groupStates[i] = {};
  }

  loadSettings();
  topicBase = String(TOPIC_PREFIX) + cfg.deviceId + "/";
  Serial.println("Device ID: " + String(cfg.deviceId));

  if (strlen(cfg.wifiSSID) > 0 && connectWifi()) {
    configTime(3 * 3600, 0, "pool.ntp.org");
    tlsClient.setInsecure();
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(8192);
    mqtt.setKeepAlive(60);
    mqtt.setSocketTimeout(30);

    startAlertTasks();
    lastStreamFingerprint = computeStreamFingerprint();
  } else {
    startAP();
  }
}

void loop() {
  checkResetPin();

  if (apMode) {
    setupServer.handleClient();
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      wifiConnected = false;
      mqttConnected = false;
    }
    connectWifi();
    delay(1000);
    return;
  }

  if (!mqtt.connected()) {
    mqttConnected = false;
    if (millis() - lastMqttAttempt > 5000) {
      mqttConnect();
      lastMqttAttempt = millis();
    }
  } else {
    mqtt.loop();
  }

  if (mqttConnected && millis() - lastStatusPublish > 10000) {
    mqttPublishStatus();
    lastStatusPublish = millis();
  }

  for (int i = 0; i < cfg.numCameras; i++) {
    if (motionFlags[i]) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      motionFlags[i] = false;
      xSemaphoreGive(mtx);
      handleCameraMotion(i);
    }
  }

  for (int i = 0; i < cfg.numGroups; i++) {
    processGroup(i);
  }

  delay(10);
}
