#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>

// pin map
#define PIN_SCK  4
#define PIN_MISO 5
#define PIN_MOSI 6
#define PIN_SS   7
#define PIN_RST  10
#define PIN_LED  8

// runtime-configurable parameters (writable via PUT /config)
struct Config {
    uint32_t debounce_ms      = 3000;
    uint32_t response_timeout = 2000;
    uint32_t poll_interval_ms = 80;
    uint32_t post_read_delay  = 300;
} cfg;

// globals
MFRC522  rfid(PIN_SS, PIN_RST);
String   lastUID;
uint32_t lastReadTime = 0;
uint32_t bootTime     = 0;

// utilities
void blink(uint8_t count = 1, uint16_t ms = 100) {
    for (uint8_t i = 0; i < count; i++) {
        digitalWrite(PIN_LED, LOW);  delay(ms);
        digitalWrite(PIN_LED, HIGH); delay(ms);
    }
}

void sendDoc(const JsonDocument& doc) {
    serializeJson(doc, Serial);
    Serial.println();
}

String readSerialLine() {
    if (!Serial.available()) return "";
    String s = Serial.readStringUntil('\n');
    s.trim();
    return s;
}

String uidToHex(const MFRC522::Uid& uid) {
    String s;
    s.reserve(11);
    for (uint8_t i = 0; i < uid.size && i < 4; i++) {
        if (i) s += ':';
        if (uid.uidByte[i] < 0x10) s += '0';
        s += String(uid.uidByte[i], HEX);
    }
    s.toUpperCase();
    return s;
}

// response helpers
void respond(int status, const char* resource, int id = -1) {
    StaticJsonDocument<96> doc;
    doc["status"]   = status;
    doc["resource"] = resource;
    if (id >= 0) doc["id"] = id;
    sendDoc(doc);
}

void respondError(int status, const char* resource, const char* error, int id = -1) {
    StaticJsonDocument<128> doc;
    doc["status"]   = status;
    doc["resource"] = resource;
    doc["error"]    = error;
    if (id >= 0) doc["id"] = id;
    sendDoc(doc);
}

// GET /status
void getStatus(int id) {
    StaticJsonDocument<256> doc;
    doc["status"]            = 200;
    doc["resource"]          = "status";
    if (id >= 0) doc["id"]  = id;
    doc["data"]["uptime_ms"] = millis() - bootTime;
    doc["data"]["last_uid"]  = lastUID;
    doc["data"]["version"]   = "2.0.0";
    sendDoc(doc);
}

// GET /config
void getConfig(int id) {
    StaticJsonDocument<256> doc;
    doc["status"]                    = 200;
    doc["resource"]                  = "config";
    if (id >= 0) doc["id"]          = id;
    doc["data"]["debounce_ms"]      = cfg.debounce_ms;
    doc["data"]["response_timeout"] = cfg.response_timeout;
    doc["data"]["poll_interval_ms"] = cfg.poll_interval_ms;
    doc["data"]["post_read_delay"]  = cfg.post_read_delay;
    sendDoc(doc);
}

// PUT /config
void putConfig(JsonObjectConst d, int id) {
    if (d.containsKey("debounce_ms"))      cfg.debounce_ms      = d["debounce_ms"].as<uint32_t>();
    if (d.containsKey("response_timeout")) cfg.response_timeout = d["response_timeout"].as<uint32_t>();
    if (d.containsKey("poll_interval_ms")) cfg.poll_interval_ms = d["poll_interval_ms"].as<uint32_t>();
    if (d.containsKey("post_read_delay"))  cfg.post_read_delay  = d["post_read_delay"].as<uint32_t>();
    getConfig(id); // respond with the full updated config
}

// GET /uid
void getUid(int id) {
    StaticJsonDocument<128> doc;
    doc["status"]          = 200;
    doc["resource"]        = "uid";
    if (id >= 0) doc["id"] = id;
    doc["data"]["uid"]     = lastUID;
    sendDoc(doc);
}

// PUT /led
// state: "on" | "off" | "blink" (default)
// count:    number of blinks  (default 1,   only used when state == "blink")
// duration: ms per half-cycle (default 100, only used when state == "blink")
void putLed(JsonObjectConst d, int id) {
    const char* state = d["state"]    | "blink";
    uint8_t     count = d["count"]    | 1;
    uint16_t    dur   = d["duration"] | 100;

    if      (strcmp(state, "on")  == 0) digitalWrite(PIN_LED, LOW);
    else if (strcmp(state, "off") == 0) digitalWrite(PIN_LED, HIGH);
    else                                blink(count, dur);

    respond(200, "led", id);
}

// POST /access
// expected body: { "access": true | false }
// returns the parsed access value so the RFID wait-loop can act on it.
bool postAccess(JsonObjectConst d, int id) {
    bool granted = d["access"] | false;
    StaticJsonDocument<128> doc;
    doc["status"]          = 200;
    doc["resource"]        = "access";
    if (id >= 0) doc["id"] = id;
    doc["data"]["access"]  = granted;
    sendDoc(doc);
    return granted;
}

// POST /reset
void postReset(int id) {
    respond(200, "reset", id);
    delay(100);
    ESP.restart();
}

// command dispatcher
// returns true ONLY when POST /access is processed so the RFID wait-loop
// knows it has received its response and can break out.
bool dispatch(const String& line, bool& accessGranted) {
    StaticJsonDocument<256> req;
    if (deserializeJson(req, line)) return false; // parse error → ignore

    const char*     method   = req["method"]   | "";
    const char*     resource = req["resource"] | "";
    int             id       = req["id"]       | -1;
    bool            hasData  = req["data"].is<JsonObject>();
    JsonObjectConst data     = req["data"].as<JsonObjectConst>();

    // GET
    if (strcmp(method, "GET") == 0) {
        if      (strcmp(resource, "status") == 0) getStatus(id);
        else if (strcmp(resource, "config") == 0) getConfig(id);
        else if (strcmp(resource, "uid")    == 0) getUid(id);
        else respondError(404, resource, "resource not found", id);
        return false;
    }

    // PUT
    if (strcmp(method, "PUT") == 0) {
        if (!hasData) { respondError(400, resource, "data required", id); return false; }
        if      (strcmp(resource, "config") == 0) putConfig(data, id);
        else if (strcmp(resource, "led")    == 0) putLed(data, id);
        else respondError(404, resource, "resource not found", id);
        return false;
    }

    // POST
    if (strcmp(method, "POST") == 0) {
        if (strcmp(resource, "access") == 0) {
            if (!hasData) { respondError(400, resource, "data required", id); return false; }
            accessGranted = postAccess(data, id);
            return true; // signal: RFID wait-loop may exit
        }
        if (strcmp(resource, "reset") == 0) { postReset(id); return false; }
        respondError(404, resource, "resource not found", id);
        return false;
    }

    respondError(405, resource, "method not allowed", id);
    return false;
}

// setup
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 3000) delay(10);
    bootTime = millis();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH); // LED off (active-low)

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
    rfid.PCD_Init();
    delay(50);

    StaticJsonDocument<96> boot;
    boot["event"]   = "boot";
    boot["msg"]     = "ESP32-C3 RFID ready";
    boot["version"] = "2.0.0";
    sendDoc(boot);

    blink(2, 150);
}

// main loop
void loop() {
    // service any host command arriving while the reader is idle.
    String line = readSerialLine();
    if (line.length() > 0) {
        bool dummy = false;
        dispatch(line, dummy);
    }

    if (!rfid.PICC_IsNewCardPresent()) { delay(cfg.poll_interval_ms); return; }
    if (!rfid.PICC_ReadCardSerial())   { delay(cfg.poll_interval_ms); return; }

    String   uid = uidToHex(rfid.uid);
    uint32_t now = millis();

    // debounce: skip if the same card was read within debounce_ms
    if (uid == lastUID && (now - lastReadTime) < cfg.debounce_ms) {
        rfid.PICC_HaltA();
        delay(cfg.poll_interval_ms);
        return;
    }
    lastUID      = uid;
    lastReadTime = now;

    // emit card event to host
    StaticJsonDocument<96> cardEvent;
    cardEvent["event"] = "card";
    cardEvent["uid"]   = uid;
    sendDoc(cardEvent);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    // wait for POST /access — service other commands while waiting
    uint32_t waitStart    = millis();
    bool     gotResponse  = false;
    bool     accessGranted = false;

    while ((millis() - waitStart) < cfg.response_timeout) {
        String incoming = readSerialLine();
        if (incoming.length() > 0) {
            if (dispatch(incoming, accessGranted)) {
                gotResponse = true;
                break;
            }
            // non-access commands (e.g. GET /status) are handled inside
            // dispatch; keep waiting for the actual access response.
        }
        delay(30);
    }

    if      (!gotResponse)  blink(5, 50); // timeout
    else if (accessGranted) blink(1, 80); // granted
    else                    blink(3, 80); // denied

    delay(cfg.post_read_delay);
}
