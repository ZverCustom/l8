#include "net.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <net_session.h>
#include <DNSServer.h>
#include <atomic>
#include "config/config.h"
#include "system/system.h"

String wifiSsid, wifiPass;
static String sessionSsid,sessionPass;
static const KnownNet knownNets[] = DEFAULT_NETWORKS;
const char* netDefaultSsid() { return knownNets[0].ssid; }
const char* netDefaultPass() { return knownNets[0].pass; }

static bool apOn=false, wasConnected=false, startPending=false;
static uint32_t attempts=0;
static uint8_t lastReason=0;
static l8net::Session session;
static DNSServer dns;
static bool scanPending=false;
static int selectedChannel=0;
static uint8_t selectedBssid[6]={};
static bool selected=false;
static l8net::HttpActivity apHttp;
static std::atomic<uint32_t> droppedEvents{0};

struct NetEvent { WiFiEvent_t id; uint32_t at; uint8_t reason; };
static QueueHandle_t events=nullptr;

static void startAP() {
    if(apOn) return;
    WiFi.mode((WiFi.getMode()&WIFI_STA)?WIFI_AP_STA:WIFI_AP);
    if(WiFi.softAP(AP_SSID,AP_PASSWORD)) {
        apOn=true;
        dns.start(53,"*",WiFi.softAPIP());
        apHttp=l8net::HttpActivity{};
        sysLog("AP started "+WiFi.softAPIP().toString());
    } else Serial.println("Failed to start Access Point!");
}
static void startSTA() {
    uint32_t now=millis(); session.enter(l8net::Session::Associating,now); ++attempts; startPending=false;
    Serial.printf("Wi-Fi begin t=%lu attempt=%lu\n",now,attempts);
    wl_status_t result=WiFi.begin(sessionSsid.c_str(),sessionPass.c_str(),selected?selectedChannel:0,selected?selectedBssid:nullptr);
    Serial.printf("Wi-Fi begin result t=%lu status=%d\n",millis(),int(result));
    // Even an immediate failure retains the one-session deadline; the core
    // may already be starting its own first retry. No second begin is issued.
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
        // Ignore delayed events from a stopped/scan session. A voluntary
        // disconnect may also be generated inside the core's first retry;
        // association keeps its original deadline instead of calling begin again.
        bool active=session.phase==l8net::Session::Associating || session.phase==l8net::Session::Dhcp || session.phase==l8net::Session::Online;
        if(active && event.id==ARDUINO_EVENT_WIFI_STA_CONNECTED) session.enter(l8net::Session::Dhcp,event.at);
        if(active && event.id==ARDUINO_EVENT_WIFI_STA_LOST_IP) session.enter(l8net::Session::Dhcp,event.at);

    }
}
static void apOnly() {
    startPending=false; scanPending=false; selected=false;
    WiFi.scanDelete(); WiFi.disconnect(false,false);
    WiFi.mode(WIFI_AP); session.enter(l8net::Session::Idle,millis());
    startAP(); wasConnected=false;
}
void netBegin() {
    sessionSsid=wifiSsid; sessionPass=wifiPass;
    events=xQueueCreate(32,sizeof(NetEvent));
    WiFi.onEvent(onWifiEvent);
    WiFi.persistent(false); WiFi.setAutoReconnect(false);
    WiFi.setSleep(false); WiFi.setHostname("zver-l8");
    if(!wifiSsid.length() || !events) { apOnly(); return; }
    WiFi.mode(WIFI_STA);
    // Only boot performs a discovery scan, before serving AP clients.
    scanPending=true; session.enter(l8net::Session::Scan,millis());
}
void netReconnect() {
    sessionSsid=wifiSsid; sessionPass=wifiPass;
    WiFi.disconnect(false,false); wasConnected=false;
    if(!wifiSsid.length()) { apOnly(); return; }
    WiFi.enableSTA(true); selected=false; startPending=true;
    session.enter(l8net::Session::Stopping,millis());
}
void netTick() {
    if(apOn) dns.processNextRequest();
    consumeEvents(); uint32_t now=millis();
    if(session.phase==l8net::Session::Scan) {
        if(session.expired(now)) { apOnly(); return; }
        if(scanPending) {
            if(!(WiFi.getStatusBits()&STA_STARTED_BIT)) return;
            scanPending=false; WiFi.scanNetworks(true,false,false,120); return;
        }
        int count=WiFi.scanComplete();
        if(count==WIFI_SCAN_RUNNING) return;
        for(int i=0;i<count;++i) if(WiFi.SSID(i)==sessionSsid) {
            selected=true; selectedChannel=WiFi.channel(i); memcpy(selectedBssid,WiFi.BSSID(i),6); break;
        }
        WiFi.scanDelete();
        sysLog(selected?F("Saved network found at boot"):F("Saved network absent at boot; AP only"));
        if(!selected) { apOnly(); return; }
        startSTA(); startAP(); return;
    }
    if(startPending) {
        if(now-session.since>7000) { apOnly(); return; }
        if(session.phase==l8net::Session::Stopping && now-session.since<2000) return;
        if(WiFi.getStatusBits()&STA_STARTED_BIT) { startSTA(); startAP(); }
        return;
    }
    if(session.phase==l8net::Session::Idle) return;
    if(WiFi.status()==WL_CONNECTED) {
        if(!wasConnected) {
            wasConnected=true; session.enter(l8net::Session::Online,now);
            sysLog("Wi-Fi connected "+WiFi.SSID()+" "+WiFi.localIP().toString());
            if(MDNS.begin("zver-l8")) MDNS.addService("http","tcp",80);
        }
        return; // AP remains available; no background scans or retry loop.
    }
    bool associated=WiFi.getStatusBits()&STA_CONNECTED_BIT;
    if((wasConnected || session.phase==l8net::Session::Online) && associated) { wasConnected=false; session.enter(l8net::Session::Dhcp,now); }
    if(wasConnected || session.phase==l8net::Session::Online) {
        sysLog(F("Wi-Fi lost; AP only until manual reconnect or reboot"));
        MDNS.end(); apOnly(); return;
    }
    if(associated) session.enter(l8net::Session::Dhcp,now);
    if(session.expired(now)) { sysLog(F("Wi-Fi attempt timed out; AP only")); apOnly(); }
}
bool netApOn() { return apOn; }
void netApHttpActivity() { if(apOn) apHttp.touch(millis()); }
bool netApHttpActive() { return apOn && apHttp.active(millis()); }
uint32_t netDroppedEvents() { return droppedEvents.load(std::memory_order_relaxed); }
const char* netPhase() {
    switch(session.phase) {
        case l8net::Session::Idle:return "ap_only";
        case l8net::Session::Scan:return "boot_scan";
        case l8net::Session::Associating:return "associating";
        case l8net::Session::Dhcp:return "dhcp";
        case l8net::Session::Online:return "online";
        case l8net::Session::Stopping:return "disconnecting";
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
