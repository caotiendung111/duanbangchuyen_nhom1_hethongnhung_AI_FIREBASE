#ifndef STATE_H
#define STATE_H

#include <Arduino.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// ─── Shared Hardware Drivers & Storage ────────────────────────
extern Servo servo1;
extern Servo servo2;
extern LiquidCrystal_I2C lcd;
extern Preferences preferences;

// ─── Shared Network & TCP States ──────────────────────────────
extern WiFiClient tcpClient;
extern SemaphoreHandle_t xTCPMutex;
extern volatile bool tcpConnected;

// ─── Shared Firebase RTDB Handles ─────────────────────────────
extern FirebaseData fbdo;
extern FirebaseData fbdo_write;
extern FirebaseAuth fbAuth;
extern FirebaseConfig fbConfig;
extern volatile bool fbReady;
extern volatile bool fbNeedWrite;

// ─── Shared FreeRTOS Task & Sync Handles ──────────────────────
extern TaskHandle_t xScanTaskHandle;
extern TaskHandle_t xGateTaskHandle;
extern TaskHandle_t xManualTaskHandle;
extern TaskHandle_t xMotorTaskHandle;

extern SemaphoreHandle_t xCountMutex;
extern SemaphoreHandle_t xLCDSem;
extern SemaphoreHandle_t xConsoleMutex;
extern SemaphoreHandle_t xServoDoneSem;

extern QueueHandle_t xAIQueue;
extern QueueHandle_t xItemQueue;
extern QueueHandle_t xServoQueue;
extern EventGroupHandle_t xIREvents;

// ─── Shared System Mode Flags ─────────────────────────────────
extern volatile bool systemOn;
extern volatile bool isAutoMode;
extern volatile bool motorOn;
extern volatile bool isScanning;

extern int count1;
extern int count2;
extern char itemName[20];

// Shared Product Queue Buffer
extern String productQueue[4];
extern int queueCount;

// Shared Local vs Cloud Sync Controls
extern unsigned long lastLocalChange;
extern volatile int manualArmedServo;

// Watchdog Shared Feeds
enum WDT_TASK { WDT_SCAN, WDT_GATE, WDT_MANUAL, WDT_MOTOR, WDT_SERVO, WDT_NUM };
extern volatile unsigned long wdtFeed[WDT_NUM];

// ─── Shared Utility Helper Function Declarations ──────────────
void dbg(const char* msg);
void requestLCDUpdate();
void motorRun(uint8_t speed = 200);
void motorSlow(uint8_t speed = 100);
void motorStop();

#endif // STATE_H
