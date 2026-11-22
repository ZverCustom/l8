#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
// One command layer for HTTP and USB. Transports only frame, authorize and pick
// the HTTP status; every command (v1 or legacy) runs through the same operations.
namespace l8proto {
enum class Code:uint8_t { Ok, BadJson, InvalidArgument, UnsupportedVersion, AuthRequired, UnknownCommand,
    BootChanged, RevisionConflict, EditorBusy, PreviewExpired, Busy, OutputBlocked, MasterMustBeOff,
    FrameTooLarge, UnsupportedCapability, FlashWriteFailed, InternalError };
const char* codeName(Code code);
int httpStatus(Code code);
// Device side effects owned by the hub/power/IMU code; faked in native tests.
struct Hooks {
    const char* (*bootId)()=nullptr;
    bool (*otaBusy)()=nullptr;        // OTA upload running, image staged or reboot scheduled
    bool (*powerReady)()=nullptr;     // power controller confirmed (I/O ok, no I/O latch)
    void (*masterChanged)()=nullptr;  // push drive.masterOn to the power task
    void (*resetLatch)(int channel)=nullptr;
    bool (*imuOk)()=nullptr;
    void (*calibrate)()=nullptr;      // writes IMU offsets into drive
    void (*reconnect)()=nullptr;
    void (*reboot)()=nullptr;         // inhibit outputs, flush settings, schedule restart
    void (*info)(JsonObject)=nullptr;
    void (*status)(JsonObject)=nullptr;
};
void setHooks(const Hooks& hooks);
constexpr size_t MAX_COMMAND_BYTES=24576, MAX_LEGACY_BYTES=24000, MAX_CONFIG_BYTES=24000;
enum class Transport:uint8_t { Http, Usb };
// A v1 message: HTTP body of POST /api/command, or a USB line whose object has "v".
String handleV1(const char* data,size_t len,Transport transport,bool httpAuthorized,int& http);
// One USB line: v1 when the root object has "v", otherwise the legacy USB command.
String handleUsbLine(const char* data,size_t len);
// USB line longer than MAX_COMMAND_BYTES (already dropped up to its newline).
String usbOverflow();
// Legacy HTTP routes (/api/config POST, /api/preview|apply|save|discard|touch, /api/action).
// The caller has already checked X-L8-Token.
String legacyHttp(const char* route,const char* data,size_t len,int& http);
// SSE `/api/events?v=1` data: {v,bootId,seq,revision,savedRevision,status}.
String sseV1(uint32_t seq);
// Legacy status/info JSON objects, shared by GET routes, USB and SSE.
String statusJson();
String infoJson();
}
