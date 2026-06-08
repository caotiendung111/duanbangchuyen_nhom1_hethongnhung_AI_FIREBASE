#include <Arduino.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>

#include "config.h"
#include "state.h"
#include "tasks.h"

// ═════════════════════════════════════════════════════════════
// SYSTEM INITIALIZATION AND TASK ALLOCATION
// ═════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);  // USB Serial port console for debugger connection
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

    // Create RTDB synchronization structures
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

    // Spawning concurrent tasks pinned to specific CPU cores
    xTaskCreatePinnedToCore(vServoTask,      "Servo",    2048, NULL, 2, NULL,               1);
    xTaskCreatePinnedToCore(vLCDTask,        "LCD",      2048, NULL, 1, NULL,               1);
    xTaskCreatePinnedToCore(vMotorTask,      "Motor",    2048, NULL, 2, &xMotorTaskHandle,  1);
    xTaskCreatePinnedToCore(vScanTask,       "Scan",     4096, NULL, 3, &xScanTaskHandle,   1);
    xTaskCreatePinnedToCore(vGateTask,       "Gate",     3072, NULL, 3, &xGateTaskHandle,   1);
    xTaskCreatePinnedToCore(vManualModeTask, "Manual",   3072, NULL, 2, &xManualTaskHandle, 1);
    xTaskCreatePinnedToCore(vButtonTask,     "Button",   2048, NULL, 2, NULL,               1);
    xTaskCreatePinnedToCore(vNetworkTask,    "Network",  4096, NULL, 2, NULL,               0); 
    xTaskCreatePinnedToCore(vTCPRXTask,      "TCPRX",    2048, NULL, 3, NULL,               1);
    xTaskCreatePinnedToCore(vWatchdogTask,   "WDT",      2048, NULL, 4, NULL,               0);
    xTaskCreatePinnedToCore(vFirebaseTask,   "Firebase", 8192, NULL, 1, NULL,               0); 

    requestLCDUpdate();
    dbg("=== Smart Conveyor Sorting System Initialized Successfully ===");
}

void loop() { 
    vTaskDelay(portMAX_DELAY); 
}