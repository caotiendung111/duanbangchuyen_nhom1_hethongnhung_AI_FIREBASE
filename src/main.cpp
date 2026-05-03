/*
 * =============================================================
 * PBL4 - HỆ THỐNG BĂNG CHUYỀN PHÂN LOẠI SẢN PHẨM
 * Platform : ESP32 (FreeRTOS) — v4
 * =============================================================
 * THAY ĐỔI v4:
 *   - Manual mode: nút SERVO1/SERVO2 chỉ hoạt động khi
 *     IR Entry đang bị che (có vật trước cảm biến)
 *   - Tất cả chức năng khác giữ nguyên
 * =============================================================
 */

#include <Arduino.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
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
char itemName[3] = "--";

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
        if (xQueueReceive(xServoQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            wdtFeed[WDT_SERVO] = millis();
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

        if (!systemOn || !isAutoMode || !motorOn) {
            lastItemTime = millis();
            motorStop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        motorRun(200);

        if (millis() - lastItemTime > IDLE_TIMEOUT_MS) {
            dbg("[SCAN] 30s idle -> tat motor.");
            motorOn = false;
            motorStop();
            strcpy(itemName, "--");
            preferences.putBool("motorOn", false);
            requestLCDUpdate();
            lastItemTime = millis();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        xEventGroupClearBits(xIREvents, EVT_IR_ENTRY);
        EventBits_t bits = xEventGroupWaitBits(
            xIREvents, EVT_IR_ENTRY,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(200)
        );

        if (!(bits & EVT_IR_ENTRY)) continue;

        lastItemTime = millis();
        dbg("[SCAN] IR Entry! Flush AI queue, bat dau scan...");

        { char dummy; while (xQueueReceive(xAIQueue, &dummy, 0) == pdTRUE) {} }

        motorSlow(100);
        vTaskDelay(pdMS_TO_TICKS(200));
        // Gửi CAPTURE qua TCP thay vì Serial
        if (tcpConnected && xSemaphoreTake(xTCPMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            tcpClient.println("CAPTURE");
            xSemaphoreGive(xTCPMutex);
            dbg("[TCP] Gui: CAPTURE");
        } else {
            dbg("[TCP] Chua ket noi — bo qua CAPTURE.");
        }

        char aiResult = 'U';
        int pendingType = 0;
        char name[3] = "--";

        if (xQueueReceive(xAIQueue, &aiResult, pdMS_TO_TICKS(6000)) == pdTRUE
            && aiResult != 'U') {
            char log[50];
            snprintf(log, sizeof(log), "[SCAN] AI: '%c'", aiResult);
            dbg(log);
            switch (aiResult) {
                case 'A': pendingType = 1; strcpy(name, "AP"); break;
                case 'B': pendingType = 1; strcpy(name, "BA"); break;
                case 'O': pendingType = 1; strcpy(name, "OR"); break;
                case 'M': pendingType = 2; strcpy(name, "MI"); break;
                default:  pendingType = 0; strcpy(name, "--"); break;
            }
        } else {
            dbg("[SCAN] AI timeout/Unknown -> khong gat.");
            pendingType = 0;
        }

        strcpy(itemName, name);
        requestLCDUpdate();

        if (pendingType > 0) {
            if (xQueueSend(xItemQueue, &pendingType, pdMS_TO_TICKS(100)) != pdTRUE) {
                dbg("[SCAN] itemQueue day! Bo qua vat nay.");
            } else {
                char log[60];
                snprintf(log, sizeof(log), "[SCAN] Push type=%d, so vat cho=%d",
                         pendingType, (int)uxQueueMessagesWaiting(xItemQueue));
                dbg(log);
            }
        }

        motorRun(200);
        // Chờ vật ra khỏi IR Entry hoàn toàn trước khi về IDLE
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: GATE (luồng IR Gate — AUTO mode)
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

        unsigned long now = millis();
        if (now - lastGateTime < GATE_DEBOUNCE_MS) {
            dbg("[GATE] Nhieu IR Gate, bo qua.");
            continue;
        }
        lastGateTime = now;

        int itemType = 0;
        if (xQueueReceive(xItemQueue, &itemType, pdMS_TO_TICKS(50)) != pdTRUE
            || itemType == 0) {
            dbg("[GATE] Queue rong (unknown) -> bo qua.");
            continue;
        }

        char log[60];
        snprintf(log, sizeof(log), "[GATE] Pop type=%d, con lai=%d",
                 itemType, (int)uxQueueMessagesWaiting(xItemQueue));
        dbg(log);

        int servoCmd = (itemType == 1) ? 3 : 4;
        if (xQueueSend(xServoQueue, &servoCmd, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (itemType == 1) { count1++; preferences.putInt("c1", count1); }
                else               { count2++; preferences.putInt("c2", count2); }
                xSemaphoreGive(xCountMutex);
            }
            strcpy(itemName, "--");
            requestLCDUpdate();
            dbg(itemType == 1 ? "[GATE] Gat servo1 (trai cay)." : "[GATE] Gat servo2 (sua).");

            xSemaphoreTake(xServoDoneSem, pdMS_TO_TICKS(3000));
            xEventGroupClearBits(xIREvents, EVT_IR_GATE);
            lastGateTime = millis();
        } else {
            dbg("[GATE] servoQueue day!");
        }
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: MANUAL MODE
//
// THAY ĐỔI v4:
//   Nút SERVO1/SERVO2 chỉ hoạt động khi IR Entry đang bị che
//   (digitalRead(PIN_IR_ENTRY) == LOW → có vật chắn cảm biến)
//   IR Gate không dùng trong manual mode
// ═════════════════════════════════════════════════════════════
void vManualModeTask(void* pvParam) {
    bool lBtn1 = true, lBtn2 = true;

    for (;;) {
        wdtFeed[WDT_MANUAL] = millis();

        if (!systemOn || isAutoMode) {
            lBtn1 = lBtn2 = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!motorOn) {
            motorStop();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        motorRun(200);

        // Đọc trạng thái IR Entry: LOW = có vật che cảm biến
        bool irBlocked = (digitalRead(PIN_IR_ENTRY) == LOW);

        bool cBtn1 = (digitalRead(BTN_SERVO1) == LOW);
        bool cBtn2 = (digitalRead(BTN_SERVO2) == LOW);

        // Nút SERVO1: chỉ xử lý khi IR Entry đang bị che
        if (cBtn1 && !lBtn1 && irBlocked) {
            if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                count1++;
                preferences.putInt("c1", count1);
                xSemaphoreGive(xCountMutex);
            }
            int cmd = 1;
            xQueueSend(xServoQueue, &cmd, 0);
            requestLCDUpdate();
            dbg("[MANUAL] Co vat + Nut SERVO1 -> Servo1 gat");
        } else if (cBtn1 && !lBtn1 && !irBlocked) {
            dbg("[MANUAL] Nut SERVO1 nhung khong co vat tai IR Entry, bo qua.");
        }

        // Nút SERVO2: chỉ xử lý khi IR Entry đang bị che
        if (cBtn2 && !lBtn2 && irBlocked) {
            if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                count2++;
                preferences.putInt("c2", count2);
                xSemaphoreGive(xCountMutex);
            }
            int cmd = 2;
            xQueueSend(xServoQueue, &cmd, 0);
            requestLCDUpdate();
            dbg("[MANUAL] Co vat + Nut SERVO2 -> Servo2 gat");
        } else if (cBtn2 && !lBtn2 && !irBlocked) {
            dbg("[MANUAL] Nut SERVO2 nhung khong co vat tai IR Entry, bo qua.");
        }

        lBtn1 = cBtn1;
        lBtn2 = cBtn2;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ═════════════════════════════════════════════════════════════
// TASK: BUTTON
// ═════════════════════════════════════════════════════════════
void vButtonTask(void* pvParam) {
    bool lMode=1, lPwr=1, lMot=1, lBtn1=1, lBtn2=1;
    unsigned long lastBtn1 = 0, lastBtn2 = 0;

    for (;;) {
        bool cMode = digitalRead(BTN_MODE);
        if (cMode == LOW && lMode == HIGH) {
            isAutoMode = !isAutoMode;
            preferences.putBool("auto", isAutoMode);
            int dummy; while (xQueueReceive(xItemQueue, &dummy, 0) == pdTRUE) {}
            dbg(isAutoMode ? "[BTN] AUTO" : "[BTN] MANUAL");
            requestLCDUpdate();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        lMode = cMode;

        bool cPwr = digitalRead(BTN_POWER);
        if (cPwr == LOW && lPwr == HIGH) {
            systemOn = !systemOn;
            preferences.putBool("sysOn", systemOn);
            if (!systemOn) {
                motorOn = false;
                motorStop();
                servo1.write(SERVO1_REST);
                servo2.write(SERVO2_REST);
                strcpy(itemName, "--");
                int di; while (xQueueReceive(xItemQueue,  &di, 0) == pdTRUE) {}
                int ds; while (xQueueReceive(xServoQueue, &ds, 0) == pdTRUE) {}
                dbg("[BTN] TAT he thong.");
            } else {
                dbg("[BTN] BAT he thong.");
            }
            requestLCDUpdate();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        lPwr = cPwr;

        if (systemOn) {
            bool cMot = digitalRead(BTN_MOTOR_SW);
            if (cMot == LOW && lMot == HIGH) {
                motorOn = !motorOn;
                if (!motorOn) motorStop();
                dbg(motorOn ? "[BTN] Motor ON" : "[BTN] Motor OFF");
                requestLCDUpdate();
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            lMot = cMot;

            if (isAutoMode) {
                bool c1 = digitalRead(BTN_SERVO1);
                if (c1 == LOW && lBtn1 == HIGH) {
                    unsigned long now = millis();
                    if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        if (now - lastBtn1 < 400) count1 = 0;
                        else if (count1 > 0) count1--;
                        preferences.putInt("c1", count1);
                        xSemaphoreGive(xCountMutex);
                    }
                    lastBtn1 = now;
                    requestLCDUpdate();
                }
                lBtn1 = c1;

                bool c2 = digitalRead(BTN_SERVO2);
                if (c2 == LOW && lBtn2 == HIGH) {
                    unsigned long now = millis();
                    if (xSemaphoreTake(xCountMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        if (now - lastBtn2 < 400) count2 = 0;
                        else if (count2 > 0) count2--;
                        preferences.putInt("c2", count2);
                        xSemaphoreGive(xCountMutex);
                    }
                    lastBtn2 = now;
                    requestLCDUpdate();
                }
                lBtn2 = c2;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
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
    dbg("[WiFi] Dang ket noi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    dbg("[WiFi] Da ket noi!");
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

    preferences.begin("pbl4_data", false);
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

    requestLCDUpdate();
    dbg("=== PBL4 v5 WiFi TCP KHOI DONG ===");
}

void loop() { vTaskDelay(portMAX_DELAY); }