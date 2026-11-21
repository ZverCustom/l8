#include "net.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <net_retry.h>
#include <atomic>
#include "config/config.h"
#include "system/system.h"

String wifiSsid, wifiPass;
static const KnownNet knownNets[] = DEFAULT_NETWORKS;
const char* netDefaultSsid() { return knownNets[0].ssid; }
const char* netDefaultPass() { return knownNets[0].pass; }

static bool apOn=false, wasConnected=false, startPending=false;
static uint32_t downSince=0, attempts=0;
static uint8_t lastReason=0;
static l8net::Retry retry;
static l8net::HttpActivity apHttp;
static std::atomic<uint32_t> droppedEvents{0};
static constexpr uint32_t AP_DELAY=20000;
struct NetEvent { WiFiEvent_t id; uint32_t at; uint8_t reason; };
static QueueHandle_t events=nullptr;

static void startAP() {
    if(apOn) return;
    WiFi.mode(wifiSsid.length()?WIFI_AP_STA:WIFI_AP);
    if(WiFi.softAP(AP_SSID,AP_PASSWORD)) {
        apOn=true;
        apHttp=l8net::HttpActivity{};
        sysLog("AP started "+WiFi.softAPIP().toString());
    } else Serial.println("Failed to start Access Point!");
}
static void stopAP() {
    if(!apOn) return;
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apOn=false;
    Serial.println("AP stopped");
}
static void startSTA() {
    uint32_t now=millis(); retry.started(now); ++attempts; startPending=false;
    Serial.printf("Wi-Fi begin t=%lu attempt=%lu\n",now,attempts);
    wl_status_t result=WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());
    Serial.printf("Wi-Fi begin result t=%lu status=%d\n",millis(),int(result));
    if(result==WL_CONNECT_FAILED) retry.failed(millis());
}
static void onWifiEvent(WiFiEvent_t id,WiFiEventInfo_t info) {
    // Only queue from the event task. Logs/state changes run on the main loop;
    // no flash writes or suppressed early disconnect reasons in this callback.
    switch(id) {
        case ARDUINO_EVENT_WIFI_STA_START: case ARDUINO_EVENT_WIFI_STA_STOP:
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED: case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            break;
        default:return;
    }
    NetEvent event{id,millis(),uint8_t(id==ARDUINO_EVENT_WIFI_STA_DISCONNECTED?info.wifi_sta_disconnected.reason:0)};
    if(events && xQueueSend(events,&event,0)!=pdTRUE) droppedEvents.fetch_add(1,std::memory_order_relaxed);
}
static void consumeEvents() {
    static uint32_t reportedDrops=0;
    uint32_t drops=droppedEvents.load(std::memory_order_relaxed);
    if(drops!=reportedDrops) {
        Serial.printf("Wi-Fi event queue overflow: %lu events dropped\n",drops);
        reportedDrops=drops;
    }
    NetEvent event;
    while(events && xQueueReceive(events,&event,0)==pdTRUE) {
        Serial.printf("Wi-Fi event t=%lu %s",event.at,WiFi.eventName(event.id));
        if(event.id==ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            lastReason=event.reason;
            Serial.printf(" reason=%u (%s)",event.reason,WiFi.disconnectReasonName((wifi_err_reason_t)event.reason));
        }
        Serial.println();
        switch(event.id) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED: retry.associated(event.at); break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP: retry.online(event.at); break;
            case ARDUINO_EVENT_WIFI_STA_LOST_IP: retry.associated(event.at); break;
            case ARDUINO_EVENT_WIFI_STA_STOP: retry.failed(event.at); break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                retry.disconnected(event.at,event.reason==WIFI_REASON_ASSOC_LEAVE); break;
            default:break;
        }
    }
}
void netBegin() {
    events=xQueueCreate(32,sizeof(NetEvent));
    WiFi.onEvent(onWifiEvent);
    WiFi.persistent(false); WiFi.setAutoReconnect(false);
    WiFi.setSleep(false); WiFi.setHostname("zver-l8");
    downSince=millis();
    if(!wifiSsid.length() || !events) { startAP(); return; }
    WiFi.mode(WIFI_STA);
    // Wait for STA_START instead of beginning while the interface is starting.
    startPending=true;
}
void netReconnect() {
    WiFi.disconnect(false,false);
    retry.stopping(millis()); downSince=millis(); wasConnected=false;
    startPending=wifiSsid.length()>0;
    if(!startPending) { WiFi.mode(WIFI_AP); startAP(); }
    else WiFi.enableSTA(true);
}
void netTick() {
    consumeEvents();
    if(!wifiSsid.length() || !events) return;
    uint32_t now=millis();
    if(retry.phase==l8net::Phase::Stopping) {
        // disconnect() has been issued explicitly. Allow its event to arrive
        // before starting again; a missing event must not wedge reconnection.
        if(now-retry.since<2000) return;
        retry.failed(now);
    }
    if(startPending) {
        if(WiFi.getStatusBits()&STA_STARTED_BIT) startSTA();
        return;
    }
    if(WiFi.status()==WL_CONNECTED) {
        if(!wasConnected) {
            wasConnected=true; retry.online(now);
            sysLog("Wi-Fi connected "+WiFi.SSID()+" "+WiFi.localIP().toString());
            if(MDNS.begin("zver-l8")) MDNS.addService("http","tcp",80);
        }
        // Finish an AP editing session before removing its network.
        if(!netApHttpActive()) stopAP();
        return;
    }
    if(wasConnected) { wasConnected=false; downSince=now; sysLog(F("Wi-Fi lost")); }
    // Association and DHCP are distinct. WL_CONNECTED means IP is assigned;
    // its absence alone is not permission to restart an ongoing attempt.
    retry.reconcileOffline(now,WiFi.getStatusBits()&STA_CONNECTED_BIT);
    if(!apOn && now-downSince>=AP_DELAY) startAP();
    auto action=retry.next(now,apOn,netApHttpActive());
    if(action==l8net::Action::Cancel) {
        Serial.printf("Wi-Fi attempt timeout t=%lu phase=%d; disconnecting\n",now,int(retry.phase));
        retry.stopping(now); WiFi.disconnect(false,false);
    } else if(action==l8net::Action::Start) startSTA();
}
bool netApOn() { return apOn; }
void netApHttpActivity() { if(apOn) apHttp.touch(millis()); }
bool netApHttpActive() { return apOn && apHttp.active(millis()); }
uint32_t netDroppedEvents() { return droppedEvents.load(std::memory_order_relaxed); }
const char* netPhase() {
    if(!wifiSsid.length()) return "ap_only";
    switch(retry.phase) {
        case l8net::Phase::Idle:return "idle";
        case l8net::Phase::Connecting:return "associating";
        case l8net::Phase::Address:return "dhcp";
        case l8net::Phase::Online:return "online";
        case l8net::Phase::Failed:return "retry_wait";
        case l8net::Phase::Stopping:return "disconnecting";
    }
    return "unknown";
}
uint32_t netAttempts() { return attempts; }
uint8_t netLastDisconnectReason() { return lastReason; }
String netStatusString() {
    if(WiFi.status()==WL_CONNECTED) return "STA "+WiFi.SSID()+" "+WiFi.localIP().toString();
    if(apOn) return "AP "+WiFi.softAPIP().toString();
    return wifiSsid.length()?"connecting":"idle";
}
String netStaString() {
    if(WiFi.status()!=WL_CONNECTED) return "-";
    return WiFi.localIP().toString()+"   rssi "+String(WiFi.RSSI())+" dBm";
}
String netApString() { return apOn?String(AP_SSID)+"   "+WiFi.softAPIP().toString():String("off"); }
