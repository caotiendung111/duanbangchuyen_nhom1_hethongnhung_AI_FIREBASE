/*
 * =============================================================
 * PROJECT: HỆ THỐNG BĂNG CHUYỀN PHÂN LOẠI SẢN PHẨM (IoT)
 * Platform : ESP32 (FreeRTOS)
 * =============================================================
 */

#include <Arduino.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// ─── WiFi / TCP Config ────────────────────────────────────────
#define WIFI_SSID       "caotiendung"
#define WIFI_PASSWORD   "caotiendung"
#define SERVER_IP       "10.187.4.16"
#define SERVER_PORT     8888

// ─── Firebase Config ─────────────────────────────────────────
#define FIREBASE_HOST   "bangchuyen-a2516-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "IbXPvjLfRZCGljcwvJo1Cgtsqyq9rhded4JpaxvU"
#define FB_ROOT         "/bangchuyen"

// ─── GPIO ────────────────────────────────────────────────────
#define PIN_MOTOR_PWM   25
#define PIN_MOTOR_DIR   26
#define PIN_SERVO_1     27
#define PIN_SERVO_2     32
#define BTN_MODE        15
#define BTN_POWER       16
#define BTN_MOTOR_SW    17
#define BTN_SERVO1      18
#define BTN_SERVO2      19
#define PIN_IR_ENTRY    13
#define PIN_IR_GATE     23

// ─── PWM ─────────────────────────────────────────────────────
#define MOTOR_LEDC_CH   0
#define MOTOR_LEDC_FREQ 5000
#define MOTOR_LEDC_RES  8

// ─── EventGroup bits ─────────────────────────────────────────
#define EVT_IR_ENTRY    (1 << 0)
#define EVT_IR_GATE     (1 << 1)

// ─── Timing ──────────────────────────────────────────────────
#define WDT_FEED_MS       3000
#define GATE_DEBOUNCE_MS  1500
#define IDLE_TIMEOUT_MS   30000

// ─── Servo positions ─────────────────────────────────────────
#define SERVO1_REST     180
#define SERVO1_PREPARE  20
#define SERVO2_REST     0
#define SERVO2_PREPARE  120
#define SERVO_PREP_MS   1000
#define SERVO_HOLD_MS   300

// ─── Queue depths ────────────────────────────────────────────
#define ITEM_QUEUE_DEPTH  8
#define SERVO_QUEUE_DEPTH 4

// ─── Objects ─────────────────────────────────────────────────
Servo servo1, servo2;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences preferences;

// ─── WiFi / TCP ──────────────────────────────────────────────
WiFiClient tcpClient;
SemaphoreHandle_t xTCPMutex      = NULL;
volatile bool     tcpConnected   = false;

// ─── Firebase ────────────────────────────────────────────────
FirebaseData     fbdo;
FirebaseAuth     fbAuth;
FirebaseConfig   fbConfig;
volatile bool    fbReady      = false;
volatile bool    fbNeedWrite  = false; 

// ─── FreeRTOS handles ────────────────────────────────────────
TaskHandle_t xScanTaskHandle   = NULL;
TaskHandle_t xGateTaskHandle   = NULL;
TaskHandle_t xManualTaskHandle = NULL;
TaskHandle_t xMotorTaskHandle  = NULL;

SemaphoreHandle_t xCountMutex   = NULL;
SemaphoreHandle_t xLCDSem       = NULL;
SemaphoreHandle_t xConsoleMutex = NULL;
SemaphoreHandle_t xServoDoneSem = NULL;

QueueHandle_t      xAIQueue    = NULL;
QueueHandle_t      xItemQueue  = NULL;
QueueHandle_t      xServoQueue = NULL;
EventGroupHandle_t xIREvents   = NULL;

// ─── Shared state ────────────────────────────────────────────
volatile bool systemOn   = false;
volatile bool isAutoMode = false;
volatile bool motorOn    = false;

int count1 = 0, count2 = 0;
char itemName[20] = "--";

// Hàng đợi sản phẩm (Queue)
String productQueue[4] = {"", "", "", ""};
int queueCount = 0;

// Chặn Firebase ghi đè nút vật lý trong 2 giây
unsigned long lastLocalChange = 0; 

enum WDT_TASK { WDT_SCAN, WDT_GATE, WDT_MANUAL, WDT_MOTOR, WDT_SERVO, WDT_NUM };
volatile unsigned long wdtFeed[WDT_NUM] = {0};

// ─── Helpers ─────────────────────────────────────────────────
void dbg(const char* msg) {
    if (xConsoleMutex && xSemaphoreTake(xConsoleMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        Serial.println(msg);
        xSemaphoreGive(xConsoleMutex);
    }
}

void requestLCDUpdate() {
    if (xLCDSem) xSemaphoreGive(xLCDSem);
}

// ─── Motor helpers ───────────────────────────────────────────
void motorRun(uint8_t speed = 200) {
    digitalWrite(PIN_MOTOR_DIR, LOW);
    ledcWrite(MOTOR_LEDC_CH, speed);
}
void motorSlow(uint8_t speed = 100) {
    digitalWrite(PIN_MOTOR_DIR, LOW);
    ledcWrite(MOTOR_LEDC_CH, speed);
}
void motorStop() { ledcWrite(MOTOR_LEDC_CH, 0); }

// ─── ISR ─────────────────────────────────────────────────────
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
// TASK: LCD
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
// TASK: MOTOR
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
// TASK: SCAN (luồng IR Entry — AUTO mode)
// ═════════════════════════════════════════════════════════════
void vScanTask(void* pvParam) {
    unsigned long lastItemTime = millis();
    for (;;) {
        wdtFeed[WDT_SCAN] = millis();

        // ĐIỀU KIỆN MOTOR: Chỉ cần bật Hệ thống + Bật Động cơ là phải quay
        if (!systemOn || !motorOn) {
            motorStop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        motorRun(200);

        // Nếu ở chế độ MANUAL thì chỉ quay motor, không quét cảm biến
        if (!isAutoMode) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- DƯỚI ĐÂY LÀ LOGIC CHẾ ĐỘ AUTO ---
        xEventGroupClearBits(xIREvents, EVT_IR_ENTRY);
        EventBits_t bits = xEventGroupWaitBits(xIREvents, EVT_IR_ENTRY, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));

        if (!(bits & EVT_IR_ENTRY)) continue;

        lastItemTime = millis();
        { char dummy; while (xQueueReceive(xAIQueue, &dummy, 0) == pdTRUE) {} }

        motorSlow(100);
        vTaskDelay(pdMS_TO_TICKS(200));
        if (tcpConnected && xSemaphoreTake(xTCPMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            tcpClient.println("CAPTURE");
            xSemaphoreGive(xTCPMutex);
        }

        char aiResult = 'U';
        int pendingType = 0;
        char name[20] = "--";

        if (xQueueReceive(xAIQueue, &aiResult, pdMS_TO_TICKS(6000)) == pdTRUE
            && aiResult != 'U') {
            switch (aiResult) {
                case 'A': pendingType = 1; strcpy(name, "AP"); break;
                case 'B': pendingType = 1; strcpy(name, "BA"); break;
                case 'O': pendingType = 1; strcpy(name, "OR"); break;
                case 'M': pendingType = 2; strcpy(name, "MI"); break;
                default:  pendingType = 0; strcpy(name, "--"); break;
            }
        }

        strcpy(itemName, name);
        if (pendingType > 0 && queueCount < 4) {
            productQueue[queueCount++] = String(name);
            if (xQueueSend(xItemQueue, &pendingType, pdMS_TO_TICKS(100)) != pdTRUE) {}
            fbNeedWrite = true;
        }

        requestLCDUpdate();
        motorRun(200);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: GATE
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
        EventBits_t bits = xEventGroupWaitBits(
            xIREvents, EVT_IR_GATE,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(500)
        );

        if (!(bits & EVT_IR_GATE)) continue;

        if (millis() - lastGateTime < GATE_DEBOUNCE_MS) continue;
        lastGateTime = millis();

        int itemType = 0;
        if (xQueueReceive(xItemQueue, &itemType, pdMS_TO_TICKS(50)) == pdTRUE) {
            // Cập nhật queue hiển thị
            for(int i=0; i<queueCount-1; i++) productQueue[i] = productQueue[i+1];
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
        if (!systemOn || isAutoMode) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        
        bool irBlocked = (digitalRead(PIN_IR_ENTRY) == LOW);
        bool cBtn1 = (digitalRead(BTN_SERVO1) == LOW);
        bool cBtn2 = (digitalRead(BTN_SERVO2) == LOW);

        if (cBtn1 && !lBtn1 && irBlocked) {
            if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) { count1++; preferences.putInt("c1", count1); xSemaphoreGive(xCountMutex); }
            int cmd = 1; xQueueSend(xServoQueue, &cmd, 0); requestLCDUpdate(); fbNeedWrite = true;
        }
        if (cBtn2 && !lBtn2 && irBlocked) {
            if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) { count2++; preferences.putInt("c2", count2); xSemaphoreGive(xCountMutex); }
            int cmd = 2; xQueueSend(xServoQueue, &cmd, 0); requestLCDUpdate(); fbNeedWrite = true;
        }
        lBtn1 = cBtn1; lBtn2 = cBtn2;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: BUTTON
// ═════════════════════════════════════════════════════════════
void vButtonTask(void* pvParam) {
    unsigned long lastBtn1 = 0, lastBtn2 = 0;
    int clickCount1 = 0, clickCount2 = 0;

    for (;;) {
        // --- Nút POWER ---
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
            // --- Nút MODE ---
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
            // --- Nút MOTOR ---
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

            // --- XỬ LÝ HÀNG ĐỢI BẰNG NÚT SERVO (Chỉ ở chế độ AUTO) ---
            if (isAutoMode) {
                // Nút Servo 1 hoặc Servo 2 đều dùng chung logic này
                bool b1 = (digitalRead(BTN_SERVO1) == LOW);
                bool b2 = (digitalRead(BTN_SERVO2) == LOW);

                if (b1 || b2) {
                    vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
                    if (digitalRead(BTN_SERVO1) == LOW || digitalRead(BTN_SERVO2) == LOW) {
                        unsigned long now = millis();
                        
                        if (now - lastBtn1 < 500) { // DOUBLE CLICK -> CLEAR ALL
                            queueCount = 0;
                            int dummy; while(xQueueReceive(xItemQueue, &dummy, 0) == pdTRUE);
                            dbg("[BTN] Xoa sach hang doi!");
                            clickCount1 = 0; 
                        } else { // SINGLE CLICK
                            clickCount1 = 1;
                        }
                        
                        lastBtn1 = now;
                        // Đợi nhả nút
                        while (digitalRead(BTN_SERVO1) == LOW || digitalRead(BTN_SERVO2) == LOW) vTaskDelay(10);
                    }
                }

                // Kiểm tra nếu là click đơn (sau khi chờ 500ms không thấy click thứ 2)
                if (clickCount1 == 1 && (millis() - lastBtn1 > 500)) {
                    if (queueCount > 0) {
                        // Xóa phần tử đầu tiên
                        for(int i=0; i<queueCount-1; i++) productQueue[i] = productQueue[i+1];
                        productQueue[--queueCount] = "";
                        
                        // Lấy 1 vật ra khỏi queue RTOS để tránh gạt nhầm
                        int dummy; xQueueReceive(xItemQueue, &dummy, 0);
                        dbg("[BTN] Xoa 1 vat khoi hang doi");
                    }
                    clickCount1 = 0;
                    fbNeedWrite = true;
                    requestLCDUpdate();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: FIREBASE
// ═════════════════════════════════════════════════════════════
void vFirebaseTask(void* pvParam) {
    dbg("[FB] Bat dau khoi tao...");
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));

    fbConfig.database_url = FIREBASE_HOST;
    fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&fbConfig, &fbAuth);
    Firebase.reconnectNetwork(true);
    fbdo.setResponseSize(2048);
    
    // Đợi tối đa 10s cho Firebase ready, nếu không vẫn tiếp tục để không treo cả mạch
    int retry = 0;
    while (!Firebase.ready() && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    fbReady = true;
    dbg("[FB] Firebase san sang!");

    unsigned long lastReadMs = 0, lastWriteMs = 0;
    for (;;) {
        unsigned long now = millis();
        wdtFeed[WDT_NUM-1] = now;

        if (Firebase.ready() && (now - lastReadMs >= 500)) {
            lastReadMs = now;
            if (Firebase.RTDB.getJSON(&fbdo, FB_ROOT "/control")) {
                FirebaseJson json; json.setJsonData(fbdo.jsonString());
                FirebaseJsonData data;

                // Đồng bộ System, Mode, Motor (Ưu tiên App)
                if (now - lastLocalChange > 2000) {
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

                // Xử lý Reset vật phẩm
                if (json.get(data, "reset1") && data.boolValue == true) {
                    count1 = 0; preferences.putInt("c1", 0);
                    Firebase.RTDB.setBool(&fbdo, FB_ROOT "/control/reset1", false);
                    fbNeedWrite = true; requestLCDUpdate();
                    dbg("[FB] Reset Count 1");
                }
                if (json.get(data, "reset2") && data.boolValue == true) {
                    count2 = 0; preferences.putInt("c2", 0);
                    Firebase.RTDB.setBool(&fbdo, FB_ROOT "/control/reset2", false);
                    fbNeedWrite = true; requestLCDUpdate();
                    dbg("[FB] Reset Count 2");
                }
            }
        }

        if (Firebase.ready() && (now - lastWriteMs >= 3000 || fbNeedWrite)) {
            lastWriteMs = now; fbNeedWrite = false;
            
            String queueStr = "";
            for(int i=0; i<queueCount; i++) queueStr += productQueue[i] + (i == queueCount-1 ? "" : ", ");
            if (queueStr == "") queueStr = "(Trong)";

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
// TASK: WATCHDOG
// ═════════════════════════════════════════════════════════════
void vWatchdogTask(void* pvParam) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    const char* names[WDT_NUM] = {"SCAN", "GATE", "MANUAL", "MOTOR", "SERVO"};
    for (;;) {
        unsigned long now = millis();
        for (int i = 0; i < WDT_NUM; i++) {
            if (wdtFeed[i] > 0 && (now - wdtFeed[i]) > WDT_FEED_MS) {
                char msg[50];
                snprintf(msg, sizeof(msg), "[WDT] Task %s bi treo!", names[i]);
                dbg(msg);
                if (i != WDT_SERVO) motorStop();
                wdtFeed[i] = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════
// Helpers: WiFi + TCP
// ═════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    char log[60];
    snprintf(log, sizeof(log), "[WiFi] Dang ket noi SSID: %s...", WIFI_SSID);
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
        dbg("[WiFi] Da ket noi thanh cong!");
        dbg(WiFi.localIP().toString().c_str());
    } else {
        dbg("[WiFi] THAT BAI! Dang quet cac mang xung quanh de kiem tra...");
        int n = WiFi.scanNetworks();
        if (n == 0) {
            dbg("[WiFi] Khong tim thay bat ky mang nao!");
        } else {
            dbg("[WiFi] Cac mang tim thay:");
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
    if (ok) dbg("[TCP] Ket noi server thanh cong!");
    else    dbg("[TCP] Ket noi server that bai!");
    return ok;
}

// ═════════════════════════════════════════════════════════════
// TASK: NETWORK — tự động kết nối lại WiFi + TCP
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
            dbg("[WiFi] Mat ket noi — thu lai...");
            connectWiFi();
        }
        if (!tcpClient.connected()) {
            tcpConnected = false;
            dbg("[TCP] Mat ket noi server — thu lai...");
            while (!connectServer()) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            tcpConnected = true;
        }
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: TCP RX — nhận kết quả AI từ PC qua WiFi
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
                    snprintf(log, sizeof(log), "[TCP] AI nhan: '%c'", c);
                    dbg(log);
                }
            }
            xSemaphoreGive(xTCPMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ═════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);  // Vẫn dùng Serial để debug qua USB (tuỳ chọn)
    Wire.begin(21, 22);

    preferences.begin("hethognhung_data", false);
    systemOn   = preferences.getBool("sysOn", false);
    isAutoMode = preferences.getBool("auto",  false);
    count1     = preferences.getInt("c1", 0);
    count2     = preferences.getInt("c2", 0);
    motorOn    = false;

    pinMode(BTN_MODE,     INPUT_PULLUP);
    pinMode(BTN_POWER,    INPUT_PULLUP);
    pinMode(BTN_MOTOR_SW, INPUT_PULLUP);
    pinMode(BTN_SERVO1,   INPUT_PULLUP);
    pinMode(BTN_SERVO2,   INPUT_PULLUP);
    pinMode(PIN_IR_ENTRY, INPUT);
    pinMode(PIN_IR_GATE,  INPUT);
    pinMode(PIN_MOTOR_DIR, OUTPUT);

    ledcSetup(MOTOR_LEDC_CH, MOTOR_LEDC_FREQ, MOTOR_LEDC_RES);
    ledcAttachPin(PIN_MOTOR_PWM, MOTOR_LEDC_CH);
    ledcWrite(MOTOR_LEDC_CH, 0);

    servo1.attach(PIN_SERVO_1, 500, 2400);
    servo2.attach(PIN_SERVO_2, 500, 2400);
    servo1.write(SERVO1_REST);
    servo2.write(SERVO2_REST);

    lcd.init();
    lcd.backlight();

    xCountMutex   = xSemaphoreCreateMutex();
    xLCDSem       = xSemaphoreCreateBinary();
    xConsoleMutex = xSemaphoreCreateMutex();
    xServoDoneSem = xSemaphoreCreateBinary();
    xTCPMutex     = xSemaphoreCreateMutex();
    xAIQueue      = xQueueCreate(5,                sizeof(char));
    xItemQueue    = xQueueCreate(ITEM_QUEUE_DEPTH,  sizeof(int));
    xServoQueue   = xQueueCreate(SERVO_QUEUE_DEPTH, sizeof(int));
    xIREvents     = xEventGroupCreate();

    configASSERT(xCountMutex);
    configASSERT(xLCDSem);
    configASSERT(xConsoleMutex);
    configASSERT(xServoDoneSem);
    configASSERT(xTCPMutex);
    configASSERT(xAIQueue);
    configASSERT(xItemQueue);
    configASSERT(xServoQueue);
    configASSERT(xIREvents);

    attachInterrupt(digitalPinToInterrupt(PIN_IR_ENTRY), irEntryISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_IR_GATE),  irGateISR,  FALLING);

    xTaskCreatePinnedToCore(vServoTask,      "Servo",    2048, NULL, 1, NULL,               1);
    xTaskCreatePinnedToCore(vLCDTask,        "LCD",      2048, NULL, 1, NULL,               1);
    xTaskCreatePinnedToCore(vMotorTask,      "Motor",    2048, NULL, 2, &xMotorTaskHandle,  1);
    xTaskCreatePinnedToCore(vScanTask,       "Scan",     4096, NULL, 2, &xScanTaskHandle,   1);
    xTaskCreatePinnedToCore(vGateTask,       "Gate",     3072, NULL, 2, &xGateTaskHandle,   1);
    xTaskCreatePinnedToCore(vManualModeTask, "Manual",   3072, NULL, 2, &xManualTaskHandle, 1);
    xTaskCreatePinnedToCore(vButtonTask,     "Button",   2048, NULL, 2, NULL,               1);
    xTaskCreatePinnedToCore(vNetworkTask,    "Network",  4096, NULL, 2, NULL,               0); // Core 0: WiFi
    xTaskCreatePinnedToCore(vTCPRXTask,      "TCPRX",    2048, NULL, 2, NULL,               1);
    xTaskCreatePinnedToCore(vWatchdogTask,   "WDT",      2048, NULL, 3, NULL,               0);
    xTaskCreatePinnedToCore(vFirebaseTask,   "Firebase", 8192, NULL, 1, NULL,               0); // Core 0: Firebase RTDB

    requestLCDUpdate();
}

void loop() { vTaskDelay(portMAX_DELAY); }