#include "config.h"
#include "state.h"
#include "tasks.h"

// ─── Shared Hardware Drivers & Storage Definitions ───────────
Servo servo1;
Servo servo2;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences preferences;

// ─── Shared Network & TCP States Definitions ─────────────────
WiFiClient tcpClient;
SemaphoreHandle_t xTCPMutex = NULL;
volatile bool tcpConnected = false;

// ─── Shared Firebase RTDB Handles Definitions ────────────────
FirebaseData fbdo;
FirebaseData fbdo_write;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;
volatile bool fbReady = false;
volatile bool fbNeedWrite = false;

// ─── Shared FreeRTOS Task & Sync Handles Definitions ─────────
TaskHandle_t xScanTaskHandle = NULL;
TaskHandle_t xGateTaskHandle = NULL;
TaskHandle_t xManualTaskHandle = NULL;
TaskHandle_t xMotorTaskHandle = NULL;

SemaphoreHandle_t xCountMutex = NULL;
SemaphoreHandle_t xLCDSem = NULL;
SemaphoreHandle_t xConsoleMutex = NULL;
SemaphoreHandle_t xServoDoneSem = NULL;

QueueHandle_t xAIQueue = NULL;
QueueHandle_t xItemQueue = NULL;
QueueHandle_t xServoQueue = NULL;
EventGroupHandle_t xIREvents = NULL;

// ─── Shared System Mode Flags Definitions ────────────────────
volatile bool systemOn = false;
volatile bool isAutoMode = false;
volatile bool motorOn = false;
volatile bool isScanning = false;

int count1 = 0;
int count2 = 0;
char itemName[20] = "--";

String productQueue[4] = {"", "", "", ""};
int queueCount = 0;

unsigned long lastLocalChange = 0;
volatile int manualArmedServo = 0;

volatile unsigned long wdtFeed[WDT_NUM] = {0};

// ─── Utility Helper Function Implementations ─────────────────
void dbg(const char* msg) {
    if (xConsoleMutex && xSemaphoreTake(xConsoleMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        Serial.println(msg);
        xSemaphoreGive(xConsoleMutex);
    }
}

void requestLCDUpdate() {
    if (xLCDSem) xSemaphoreGive(xLCDSem);
}

// ─── Motor Control Drivers ───────────────────────────────────
void motorRun(uint8_t speed) {
    digitalWrite(PIN_MOTOR_DIR, LOW);
    ledcWrite(MOTOR_LEDC_CH, speed);
}

void motorSlow(uint8_t speed) {
    digitalWrite(PIN_MOTOR_DIR, LOW);
    ledcWrite(MOTOR_LEDC_CH, speed);
}

void motorStop() { 
    ledcWrite(MOTOR_LEDC_CH, 0); 
}

// ─── Interrupt Service Routines (ISRs) ────────────────────────
void IRAM_ATTR irEntryISR() {
    BaseType_t xWoken = pdFALSE;
    xEventGroupSetBitsFromISR(xIREvents, EVT_IR_ENTRY, &xWoken);
    portYIELD_FROM_ISR(xWoken);
}

void IRAM_ATTR irGateISR() {
    BaseType_t xWoken = pdFALSE;
    xEventGroupSetBitsFromISR(xIREvents, EVT_IR_GATE, &xWoken);
    portYIELD_FROM_ISR(xWoken);
}

// ═════════════════════════════════════════════════════════════
// TASK: SERVO
// ═════════════════════════════════════════════════════════════
void vServoTask(void* pvParam) {
    int cmd;
    for (;;) {
        wdtFeed[WDT_SERVO] = millis();
        if (xQueueReceive(xServoQueue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (cmd) {
                case 1:
                case 3:
                    servo1.write(SERVO1_PREPARE);
                    vTaskDelay(pdMS_TO_TICKS(SERVO_PREP_MS));
                    servo1.write(SERVO1_REST);
                    vTaskDelay(pdMS_TO_TICKS(SERVO_HOLD_MS));
                    break;
                case 2:
                case 4:
                    servo2.write(SERVO2_PREPARE);
                    vTaskDelay(pdMS_TO_TICKS(SERVO_PREP_MS));
                    servo2.write(SERVO2_REST);
                    vTaskDelay(pdMS_TO_TICKS(SERVO_HOLD_MS));
                    break;
            }
            xSemaphoreGive(xServoDoneSem);
        }
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: LCD DISPLAY
// ═════════════════════════════════════════════════════════════
void vLCDTask(void* pvParam) {
    char buf[17];
    for (;;) {
        xSemaphoreTake(xLCDSem, portMAX_DELAY);
        int c1, c2;
        if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            c1 = count1; c2 = count2;
            xSemaphoreGive(xCountMutex);
        } else { c1 = 0; c2 = 0; }

        lcd.setCursor(0, 0);
        lcd.print(isAutoMode ? "AUTO  " : "MANUAL");
        lcd.setCursor(7, 0); lcd.print(systemOn ? "ON " : "OFF");
        lcd.setCursor(11, 0); lcd.print("M:");
        lcd.setCursor(13, 0); lcd.print((systemOn && motorOn) ? "RUN" : "OFF");

        snprintf(buf, sizeof(buf), "1:%-2d 2:%-2d   [%s]", c1, c2, itemName);
        lcd.setCursor(0, 1); lcd.print(buf);
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: MOTOR ACTUATOR
// ═════════════════════════════════════════════════════════════
void vMotorTask(void* pvParam) {
    uint32_t speedCmd = 0;
    for (;;) {
        wdtFeed[WDT_MOTOR] = millis();
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &speedCmd, pdMS_TO_TICKS(50)) == pdTRUE) {
            ledcWrite(MOTOR_LEDC_CH, (uint8_t)speedCmd);
        }
        if (!systemOn || !motorOn) motorStop();
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: SCAN (IR Entry Interrupt - AUTO Mode)
// ═════════════════════════════════════════════════════════════
void vScanTask(void* pvParam) {
    unsigned long lastItemTime = millis();
    for (;;) {
        wdtFeed[WDT_SCAN] = millis();

        // Check if motor should run based on system state
        if (!systemOn || !motorOn) {
            motorStop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        motorRun(200);

        // In MANUAL mode, only run motor, bypass sensors
        if (!isAutoMode) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- AUTO MODE LOGIC ---
        xEventGroupClearBits(xIREvents, EVT_IR_ENTRY);
        EventBits_t bits = xEventGroupWaitBits(xIREvents, EVT_IR_ENTRY, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));

        if (!(bits & EVT_IR_ENTRY)) continue;

        lastItemTime = millis();
        { char dummy; while (xQueueReceive(xAIQueue, &dummy, 0) == pdTRUE) {} }

        // Slow down conveyor during camera scanning
        motorSlow(100);
        vTaskDelay(pdMS_TO_TICKS(200));
        
        isScanning = true;
        if (tcpConnected && xSemaphoreTake(xTCPMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            tcpClient.println("CAPTURE");
            xSemaphoreGive(xTCPMutex);
        }

        char aiResult = 'U';
        int pendingType = 0;
        char name[20] = "--";

        // Wait up to 6 seconds for AI response from TCP server
        if (xQueueReceive(xAIQueue, &aiResult, pdMS_TO_TICKS(6000)) == pdTRUE && aiResult != 'U') {
            switch (aiResult) {
                case 'A': pendingType = 1; strcpy(name, "AP"); break; // Apple
                case 'B': pendingType = 1; strcpy(name, "BA"); break; // Banana
                case 'O': pendingType = 1; strcpy(name, "OR"); break; // Orange
                case 'M': pendingType = 2; strcpy(name, "MI"); break; // Milk
                default:  pendingType = 0; strcpy(name, "--"); break;
            }
        }

        strcpy(itemName, name);
        if (pendingType > 0 && queueCount < 4) {
            productQueue[queueCount++] = String(name);
            xQueueSend(xItemQueue, &pendingType, pdMS_TO_TICKS(100));
            fbNeedWrite = true;
        }

        requestLCDUpdate();
        isScanning = false;
        motorRun(200);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: GATE (IR Gate sorting actuator triggers)
// ═════════════════════════════════════════════════════════════
void vGateTask(void* pvParam) {
    unsigned long lastGateTime = 0;

    for (;;) {
        wdtFeed[WDT_GATE] = millis();

        if (!systemOn || !isAutoMode || !motorOn) {
            lastGateTime = millis();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        xEventGroupClearBits(xIREvents, EVT_IR_GATE);
        EventBits_t bits = xEventGroupWaitBits(xIREvents, EVT_IR_GATE, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));

        if (!(bits & EVT_IR_GATE)) continue;

        // Block gate triggering while scanning is active
        while (isScanning) {
            wdtFeed[WDT_GATE] = millis();
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (millis() - lastGateTime < GATE_DEBOUNCE_MS) continue;
        lastGateTime = millis();

        int itemType = 0;
        if (xQueueReceive(xItemQueue, &itemType, pdMS_TO_TICKS(50)) == pdTRUE) {
            // Update UI product queue structure
            for(int i = 0; i < queueCount - 1; i++) productQueue[i] = productQueue[i + 1];
            productQueue[--queueCount] = "";
            fbNeedWrite = true;

            int servoCmd = (itemType == 1) ? 3 : 4;
            if (xQueueSend(xServoQueue, &servoCmd, pdMS_TO_TICKS(3000)) == pdTRUE) {
                if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (itemType == 1) { count1++; preferences.putInt("c1", count1); }
                    else               { count2++; preferences.putInt("c2", count2); }
                    xSemaphoreGive(xCountMutex);
                }
                requestLCDUpdate();
                xSemaphoreTake(xServoDoneSem, pdMS_TO_TICKS(3000));
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: MANUAL MODE
// ═════════════════════════════════════════════════════════════
void vManualModeTask(void* pvParam) {
    bool lBtn1 = true, lBtn2 = true;
    for (;;) {
        wdtFeed[WDT_MANUAL] = millis();
        if (!systemOn || isAutoMode) { 
            manualArmedServo = 0;
            vTaskDelay(pdMS_TO_TICKS(100)); 
            continue; 
        }
        
        bool irEntry = (digitalRead(PIN_IR_ENTRY) == LOW);
        bool irGate  = (digitalRead(PIN_IR_GATE) == LOW);
        bool cBtn1   = (digitalRead(BTN_SERVO1) == LOW);
        bool cBtn2   = (digitalRead(BTN_SERVO2) == LOW);

        // Step 1: Arm Servo when item passes IR Entry sensor
        if (irEntry) {
            if (cBtn1 && lBtn1) {
                manualArmedServo = 1;
                dbg("[MANUAL] Armed Servo 1");
            } else if (cBtn2 && lBtn2) {
                manualArmedServo = 2;
                dbg("[MANUAL] Armed Servo 2");
            }
        }

        // Step 2: Trigger armed Servo when item reaches IR Gate sensor
        if (manualArmedServo > 0 && irGate) {
            int cmd = manualArmedServo;
            if (xQueueSend(xServoQueue, &cmd, 0) == pdTRUE) {
                if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (cmd == 1) { count1++; preferences.putInt("c1", count1); }
                    else          { count2++; preferences.putInt("c2", count2); }
                    xSemaphoreGive(xCountMutex);
                }
                dbg("[MANUAL] Servo Activated!");
                requestLCDUpdate();
                fbNeedWrite = true;
                manualArmedServo = 0;
                vTaskDelay(pdMS_TO_TICKS(1000)); // Prevent bouncing while item leaves gate
            }
        }

        lBtn1 = (cBtn1 == HIGH);
        lBtn2 = (cBtn2 == HIGH);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: PHYSICAL BUTTON CONTROL
// ═════════════════════════════════════════════════════════════
void vButtonTask(void* pvParam) {
    unsigned long lastBtn1 = 0;
    int clickCount1 = 0;

    for (;;) {
        // --- POWER Button ---
        if (digitalRead(BTN_POWER) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (digitalRead(BTN_POWER) == LOW) {
                systemOn = !systemOn;
                if (!systemOn) { motorOn = false; motorStop(); }
                preferences.putBool("sysOn", systemOn);
                fbNeedWrite = true;
                lastLocalChange = millis();
                requestLCDUpdate();
                while (digitalRead(BTN_POWER) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        if (systemOn) {
            // --- MODE Button ---
            if (digitalRead(BTN_MODE) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (digitalRead(BTN_MODE) == LOW) {
                    isAutoMode = !isAutoMode;
                    preferences.putBool("auto", isAutoMode);
                    fbNeedWrite = true;
                    lastLocalChange = millis();
                    requestLCDUpdate();
                    while (digitalRead(BTN_MODE) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            // --- MOTOR Switch Button ---
            if (digitalRead(BTN_MOTOR_SW) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (digitalRead(BTN_MOTOR_SW) == LOW) {
                    motorOn = !motorOn;
                    if (!motorOn) motorStop();
                    fbNeedWrite = true;
                    lastLocalChange = millis();
                    requestLCDUpdate();
                    while (digitalRead(BTN_MOTOR_SW) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            // --- Queue management (AUTO Mode) ---
            if (isAutoMode) {
                // Button 1: Queue item deletion / clear
                if (digitalRead(BTN_SERVO1) == LOW) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if (digitalRead(BTN_SERVO1) == LOW) {
                        unsigned long now = millis();
                        if (now - lastBtn1 < 500) { // Double Click -> Clear All
                            queueCount = 0;
                            int dummy; while(xQueueReceive(xItemQueue, &dummy, 0) == pdTRUE);
                            dbg("[BTN] Product queue cleared!");
                            clickCount1 = 0; 
                        } else {
                            clickCount1 = 1;
                        }
                        lastBtn1 = now;
                        while (digitalRead(BTN_SERVO1) == LOW) vTaskDelay(10);
                    }
                }

                if (clickCount1 == 1 && (millis() - lastBtn1 > 500)) {
                    if (queueCount > 0) {
                        for(int i = 0; i < queueCount - 1; i++) productQueue[i] = productQueue[i + 1];
                        productQueue[--queueCount] = "";
                        int dummy; xQueueReceive(xItemQueue, &dummy, 0);
                        dbg("[BTN] Removed 1 item from product queue");
                    }
                    clickCount1 = 0; fbNeedWrite = true; requestLCDUpdate();
                }

                // Button 2: Reset total counts
                if (digitalRead(BTN_SERVO2) == LOW) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if (digitalRead(BTN_SERVO2) == LOW) {
                        if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            count1 = 0; count2 = 0;
                            preferences.putInt("c1", 0); preferences.putInt("c2", 0);
                            xSemaphoreGive(xCountMutex);
                        }
                        dbg("[BTN] All counters reset!");
                        fbNeedWrite = true; requestLCDUpdate();
                        while (digitalRead(BTN_SERVO2) == LOW) vTaskDelay(10);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: FIREBASE CLIENT
// ═════════════════════════════════════════════════════════════
void vFirebaseTask(void* pvParam) {
    dbg("[FB] Initializing Firebase client...");
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));

    fbConfig.database_url = FIREBASE_HOST;
    fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&fbConfig, &fbAuth);
    Firebase.reconnectNetwork(true);
    fbdo.setResponseSize(2048);
    
    // Non-blocking timeout wait for connection establishment (max 10 seconds)
    int retry = 0;
    while (!Firebase.ready() && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    fbReady = true;
    dbg("[FB] Firebase connection established!");

    unsigned long lastReadMs = 0, lastWriteMs = 0;
    for (;;) {
        unsigned long now = millis();
        wdtFeed[WDT_NUM-1] = now;

        // Fetch command structures periodically (every 3 seconds to preserve database bandwidth)
        if (Firebase.ready() && (now - lastReadMs >= 3000)) {
            lastReadMs = now;
            if (Firebase.RTDB.getJSON(&fbdo, FB_ROOT "/control")) {
                FirebaseJson json; json.setJsonData(fbdo.jsonString());
                FirebaseJsonData data;

                // Sync control registers if local changes are not override-locked (lock for 5 seconds)
                if (now - lastLocalChange > 5000) { 
                    if (json.get(data, "systemOn")) {
                        bool val = data.boolValue;
                        if (val != systemOn) { 
                            systemOn = val; 
                            if(!systemOn) { motorOn = false; motorStop(); }
                            requestLCDUpdate(); 
                        }
                    }
                    if (json.get(data, "isAutoMode")) {
                        bool val = data.boolValue;
                        if (val != isAutoMode) { isAutoMode = val; requestLCDUpdate(); }
                    }
                    if (json.get(data, "motorOn")) {
                        bool val = data.boolValue;
                        if (val != motorOn) { 
                            motorOn = val; 
                            if(!motorOn) motorStop();
                            requestLCDUpdate(); 
                        }
                    }
                }

                // Handle Reset commands from mobile dashboard app
                if (json.get(data, "reset1") && data.boolValue == true) {
                    if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        count1 = 0; preferences.putInt("c1", 0);
                        xSemaphoreGive(xCountMutex);
                    }
                    Firebase.RTDB.setBool(&fbdo_write, FB_ROOT "/control/reset1", false);
                    fbNeedWrite = true; requestLCDUpdate();
                    dbg("[FB] Counter 1 (Fruit) reset via database command");
                }
                if (json.get(data, "reset2") && data.boolValue == true) {
                    if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        count2 = 0; preferences.putInt("c2", 0);
                        xSemaphoreGive(xCountMutex);
                    }
                    Firebase.RTDB.setBool(&fbdo_write, FB_ROOT "/control/reset2", false);
                    fbNeedWrite = true; requestLCDUpdate();
                    dbg("[FB] Counter 2 (Milk) reset via database command");
                }
            }
        }

        // Push state updates to RTDB (Batch updates to avoid overhead)
        if (Firebase.ready() && (now - lastWriteMs >= 2000) && (fbNeedWrite || (now - lastWriteMs >= 5000))) {
            lastWriteMs = now; fbNeedWrite = false;
            
            String queueStr = "";
            for(int i = 0; i < queueCount; i++) queueStr += productQueue[i] + (i == queueCount - 1 ? "" : ", ");
            if (queueStr == "") queueStr = "(Empty)";

            FirebaseJson update;
            update.set("status/count1", count1);
            update.set("status/count2", count2);
            update.set("status/lastItem", itemName);
            update.set("status/queue", queueStr);
            update.set("status/connected", true);
            update.set("control/systemOn", systemOn);
            update.set("control/motorOn", motorOn);
            update.set("control/isAutoMode", isAutoMode);
            
            Firebase.RTDB.updateNode(&fbdo, FB_ROOT, &update);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: SOFT-WATCHDOG TIMER (WDT)
// ═════════════════════════════════════════════════════════════
void vWatchdogTask(void* pvParam) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    const char* names[WDT_NUM] = {"SCAN", "GATE", "MANUAL", "MOTOR", "SERVO"};
    for (;;) {
        unsigned long now = millis();
        for (int i = 0; i < WDT_NUM; i++) {
            if (wdtFeed[i] > 0 && (now - wdtFeed[i]) > WDT_FEED_MS) {
                char msg[50];
                snprintf(msg, sizeof(msg), "[WDT] Task %s hung!", names[i]);
                dbg(msg);
                if (i != WDT_SERVO) motorStop();
                wdtFeed[i] = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════
// NETWORK SETUP UTILITIES
// ═════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    char log[60];
    snprintf(log, sizeof(log), "[WiFi] Connecting to Wi-Fi SSID: %s...", WIFI_SSID);
    dbg(log);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        retry++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        dbg("[WiFi] Wi-Fi connection successful!");
        dbg(WiFi.localIP().toString().c_str());
    } else {
        dbg("[WiFi] Connection failed! Scanning local networks...");
        int n = WiFi.scanNetworks();
        if (n == 0) {
            dbg("[WiFi] No networks discovered!");
        } else {
            dbg("[WiFi] Discovered networks:");
            for (int i = 0; i < n; ++i) {
                char ssid[50];
                snprintf(ssid, sizeof(ssid), "  - %s (%d dBm)", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
                dbg(ssid);
            }
        }
    }
}

bool connectServer() {
    if (xSemaphoreTake(xTCPMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    tcpClient.stop();
    bool ok = tcpClient.connect(SERVER_IP, SERVER_PORT);
    xSemaphoreGive(xTCPMutex);
    if (ok) dbg("[TCP] Connected to AI server successfully!");
    else    dbg("[TCP] Connection to AI server failed!");
    return ok;
}

// ═════════════════════════════════════════════════════════════
// TASK: NETWORK (Wi-Fi and TCP Server connection manager)
// ═════════════════════════════════════════════════════════════
void vNetworkTask(void* pvParam) {
    connectWiFi();
    while (!connectServer()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    tcpConnected = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (WiFi.status() != WL_CONNECTED) {
            tcpConnected = false;
            dbg("[WiFi] Wi-Fi link lost - reconnecting...");
            connectWiFi();
        }
        if (!tcpClient.connected()) {
            tcpConnected = false;
            dbg("[TCP] TCP connection lost - reconnecting...");
            while (!connectServer()) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            tcpConnected = true;
        }
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: TCP RX (Wi-Fi interface to parse AI results)
// ═════════════════════════════════════════════════════════════
void vTCPRXTask(void* pvParam) {
    for (;;) {
        if (tcpConnected && xSemaphoreTake(xTCPMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            while (tcpClient.available() > 0) {
                char c = (char)tcpClient.read();
                if (c == 'A' || c == 'B' || c == 'O' || c == 'M' || c == 'U') {
                    if (uxQueueSpacesAvailable(xAIQueue) == 0) {
                        char dummy;
                        xQueueReceive(xAIQueue, &dummy, 0);
                    }
                    xQueueSend(xAIQueue, &c, 0);
                    char log[30];
                    snprintf(log, sizeof(log), "[TCP] AI received: '%c'", c);
                    dbg(log);
                }
            }
            xSemaphoreGive(xTCPMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
