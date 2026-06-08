#ifndef TASKS_H
#define TASKS_H

#include <Arduino.h>

// FreeRTOS Task Prototypes
void vServoTask(void* pvParam);
void vLCDTask(void* pvParam);
void vMotorTask(void* pvParam);
void vScanTask(void* pvParam);
void vGateTask(void* pvParam);
void vManualModeTask(void* pvParam);
void vButtonTask(void* pvParam);
void vFirebaseTask(void* pvParam);
void vWatchdogTask(void* pvParam);
void vNetworkTask(void* pvParam);
void vTCPRXTask(void* pvParam);

// Interrupt Service Routine Prototypes
void IRAM_ATTR irEntryISR();
void IRAM_ATTR irGateISR();

#endif // TASKS_H
