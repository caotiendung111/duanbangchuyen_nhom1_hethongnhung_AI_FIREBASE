#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ─── Wi-Fi / TCP Configuration ────────────────────────────────
#define WIFI_SSID       "caotiendung"
#define WIFI_PASSWORD   "caotiendung"
#define SERVER_IP       "10.187.4.16"
#define SERVER_PORT     8888

// ─── Firebase RTDB Configuration ──────────────────────────────
#define FIREBASE_HOST   "bangchuyen-a2516-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "IbXPvjLfRZCGljcwvJo1Cgtsqyq9rhded4JpaxvU"
#define FB_ROOT         "/bangchuyen"

// ─── GPIO Pin Mapping ──────────────────────────────────────────
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

// ─── LEDC Motor PWM Configuration ──────────────────────────────
#define MOTOR_LEDC_CH   0
#define MOTOR_LEDC_FREQ 5000
#define MOTOR_LEDC_RES  8

// ─── FreeRTOS Event Group Bits ─────────────────────────────────
#define EVT_IR_ENTRY    (1 << 0)
#define EVT_IR_GATE     (1 << 1)

// ─── Timing Configurations (Milliseconds) ─────────────────────
#define WDT_FEED_MS       3000
#define GATE_DEBOUNCE_MS  1500
#define IDLE_TIMEOUT_MS   30000

// ─── Servo Angular Positions (Degrees) ────────────────────────
#define SERVO1_REST     180
#define SERVO1_PREPARE  20
#define SERVO2_REST     0
#define SERVO2_PREPARE  120
#define SERVO_PREP_MS   1000
#define SERVO_HOLD_MS   300

// ─── Queue Depth Limits ───────────────────────────────────────
#define ITEM_QUEUE_DEPTH  8
#define SERVO_QUEUE_DEPTH 4

#endif // CONFIG_H
