#include "system.h"
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <ram_log.h>
#include "led_control/led_control.h"

static const char* LOG_PATH = "/log.txt";
static const char* LOG_OLD = "/log.old";
const size_t LOG_MAX = 24 * 1024;

String sysResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return F("power on");
        case ESP_RST_SW:        return F("software reset");
        case ESP_RST_PANIC:     return F("PANIC");
        case ESP_RST_INT_WDT:   return F("interrupt watchdog");
        case ESP_RST_TASK_WDT:  return F("task watchdog");
        case ESP_RST_WDT:       return F("watchdog");
        case ESP_RST_DEEPSLEEP: return F("deep sleep");
        case ESP_RST_BROWNOUT:  return F("BROWNOUT");
        case ESP_RST_SDIO:      return F("sdio");
        case ESP_RST_EXT:       return F("external");
        default:                return F("unknown");
    }
}

String sysUptime() {
    unsigned long s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    return String(buf);
}

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif
String sysBuildInfo() {
    return String(F(GIT_REV " " __DATE__ " " __TIME__));
}

static void rotateIfNeeded() {
    if (!LittleFS.exists(LOG_PATH)) return;
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return;
    size_t size = f.size();
    f.close();
    if (size < LOG_MAX) return;
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG_PATH, LOG_OLD);
}

static SemaphoreHandle_t logMutex=nullptr;
static RamLog* journal=nullptr;
static uint32_t lastFlush=0;
static uint64_t reportedLost=0;

void sysLog(const String& msg) {
    String line=sysUptime()+" "+msg+"\n";
    if(journal) {
        xSemaphoreTake(logMutex,portMAX_DELAY);
        journal->append(line.c_str(),line.length());
        xSemaphoreGive(logMutex);
    }
    Serial.println(msg);
}

void sysBegin() {
    // Initialize before starting producer tasks. No String allocation under a
    // spinlock, and never hold the journal mutex while waiting for LED/flash.
    logMutex=xSemaphoreCreateMutex();
    char* storage=static_cast<char*>(heap_caps_malloc(16384,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(storage && logMutex) journal=new RamLog(storage,16384);
    else { free(storage); Serial.println("RAM journal unavailable"); }
    sysLog("--- boot, reset: "+sysResetReason()+", build "+sysBuildInfo());
}

// Called by the main loop only (including reboot/OTA handlers).
void sysFlush() {
    if(!journal) return;
    char* batch=static_cast<char*>(heap_caps_malloc(16384,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!batch) return;
    xSemaphoreTake(logMutex,portMAX_DELAY);
    uint64_t from=journal->flushed, lost=journal->lost;
    size_t size=journal->copy(from,batch,16384);
    xSemaphoreGive(logMutex);
    if(!size) { free(batch); return; }
    ledsPause();
    rotateIfNeeded();
    File f=LittleFS.open(LOG_PATH,"a");
    bool ok=bool(f);
    if(ok && lost!=reportedLost) f.printf("[RAM log overflow: %llu bytes lost]\n",lost);
    if(ok) { ok=f.write(reinterpret_cast<uint8_t*>(batch),size)==size; f.flush(); f.close(); }
    ledsResume();
    if(ok) {
        xSemaphoreTake(logMutex,portMAX_DELAY);
        journal->acknowledge(from+size);
        xSemaphoreGive(logMutex);
        reportedLost=lost;
    }
    free(batch);
}
void sysTick() {
    if(millis()-lastFlush<10000) return;
    lastFlush=millis(); sysFlush();
}

String sysLogTail(int maxBytes) {
    maxBytes=constrain(maxBytes,1,16384);
    String tail;
    // Pending RAM records are visible immediately without forcing a flash write.
    if(journal) {
        char* copy=static_cast<char*>(malloc(maxBytes+1));
        if(copy) {
            xSemaphoreTake(logMutex,portMAX_DELAY);
            uint64_t from=journal->flushed;
            if(journal->end-from>uint64_t(maxBytes)) from=journal->end-maxBytes;
            size_t n=journal->copy(from,copy,maxBytes); copy[n]=0;
            xSemaphoreGive(logMutex);
            tail=copy; free(copy);
        }
    }
    int remaining=maxBytes-tail.length();
    if(remaining>0) {
        File f=LittleFS.open(LOG_PATH,"r");
        if(f) {
            size_t size=f.size(); if(size>size_t(remaining)) f.seek(size-remaining);
            String disk; disk.reserve(remaining+1);
            while(f.available()) disk+=char(f.read());
            f.close(); tail=disk+tail;
        }
    }
    return tail.length()?tail:String("(no log)");
}

void sysClearLog() {
    if(journal) { xSemaphoreTake(logMutex,portMAX_DELAY); journal->clear(); xSemaphoreGive(logMutex); }
    ledsPause(); LittleFS.remove(LOG_PATH); LittleFS.remove(LOG_OLD); ledsResume();
    reportedLost=0; sysLog(F("log cleared"));
}
