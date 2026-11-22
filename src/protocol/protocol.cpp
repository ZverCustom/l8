#include "protocol.h"
#include "settings/settings.h"
#include "vehicle/vehicle.h"
#include <strict_json.h>
namespace l8proto {
static Hooks hooks;
void setHooks(const Hooks& h) { hooks=h; }

const char* codeName(Code c) {
    switch(c) {
        case Code::Ok: return "OK"; case Code::BadJson: return "BAD_JSON"; case Code::InvalidArgument: return "INVALID_ARGUMENT";
        case Code::UnsupportedVersion: return "UNSUPPORTED_VERSION"; case Code::AuthRequired: return "AUTH_REQUIRED";
        case Code::UnknownCommand: return "UNKNOWN_COMMAND"; case Code::BootChanged: return "BOOT_CHANGED";
        case Code::RevisionConflict: return "REVISION_CONFLICT"; case Code::EditorBusy: return "EDITOR_BUSY";
        case Code::PreviewExpired: return "PREVIEW_EXPIRED"; case Code::Busy: return "BUSY"; case Code::OutputBlocked: return "OUTPUT_BLOCKED";
        case Code::MasterMustBeOff: return "MASTER_MUST_BE_OFF"; case Code::FrameTooLarge: return "FRAME_TOO_LARGE";
        case Code::UnsupportedCapability: return "UNSUPPORTED_CAPABILITY"; case Code::FlashWriteFailed: return "FLASH_WRITE_FAILED";
        case Code::InternalError: return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}
int httpStatus(Code c) {
    switch(c) {
        case Code::Ok: return 200;
        case Code::BadJson: case Code::InvalidArgument: case Code::UnsupportedVersion: return 400;
        case Code::AuthRequired: return 403; case Code::UnknownCommand: return 404;
        case Code::BootChanged: case Code::RevisionConflict: case Code::EditorBusy: case Code::PreviewExpired:
        case Code::Busy: case Code::OutputBlocked: case Code::MasterMustBeOff: return 409;
        case Code::FrameTooLarge: return 413; case Code::UnsupportedCapability: return 501;
        case Code::FlashWriteFailed: return 507; case Code::InternalError: return 500;
    }
    return 500;
}

struct Outcome {
    Code code=Code::Ok; String message;
    Outcome() {}
    Outcome(Code c,const char* text):code(c),message(text) {}
    Outcome(Code c,const String& text):code(c),message(text) {}
    bool ok() const { return code==Code::Ok; }
};
static Outcome fromSettings(const String& text) {
    switch(settingsLastError()) {
        case SettingsError::Conflict: return {Code::RevisionConflict,text};
        case SettingsError::EditorBusy: return {Code::EditorBusy,text};
        case SettingsError::PreviewExpired: return {Code::PreviewExpired,text};
        case SettingsError::Flash: return {Code::FlashWriteFailed,text};
        default: return {Code::InvalidArgument,text};
    }
}
static bool otaBusy() { return hooks.otaBusy && hooks.otaBusy(); }
static const char* bootId() { return hooks.bootId?hooks.bootId():""; }
// CAN settings changed by a configuration operation restart the bus once.
struct CanGuard {
    int mode=canMode, rate=canBitrateSel;
    void apply() const { if(mode!=canMode) vehicleSetMode(canMode); else if(rate!=canBitrateSel) vehicleSetBitrate(canBitrateSel); }
};

// ---- operations shared by v1 and legacy ----
static Outcome opMaster(bool on) {
    if(otaBusy()) return {Code::Busy,"OTA in progress"};
    if(on && !(hooks.powerReady && hooks.powerReady())) return {Code::OutputBlocked,"Power controller unavailable"};
    drive.masterOn=on; if(hooks.masterChanged) hooks.masterChanged(); return {};
}
static Outcome opReset(int channel) {
    if(otaBusy()) return {Code::Busy,"OTA in progress"};
    if(drive.masterOn) return {Code::MasterMustBeOff,"Turn Master off before reset"};
    if(hooks.resetLatch) hooks.resetLatch(channel); return {};
}
static Outcome noPreview() {
    if(otaBusy()) return {Code::Busy,"OTA in progress"};
    if(settingsPreviewActive()) return {Code::EditorBusy,"Save or discard preview first"};
    return {};
}
static Outcome opPresetSave(int slot) { Outcome o=noPreview(); if(!o.ok()) return o; settingsPresetSave(slot); return {}; }
static Outcome opPresetLoad(int slot) {
    Outcome o=noPreview(); if(!o.ok()) return o;
    CanGuard can; if(!settingsPresetLoad(slot)) return {Code::InvalidArgument,"Empty preset"}; can.apply(); return {};
}
static Outcome opCalibrate() {
    Outcome o=noPreview(); if(!o.ok()) return o;
    if(!(hooks.imuOk && hooks.imuOk())) return {Code::Busy,"IMU unavailable"};
    if(hooks.calibrate) hooks.calibrate(); settingsSave(); return {};
}
static Outcome opReconnect() { Outcome o=noPreview(); if(!o.ok()) return o; if(hooks.reconnect) hooks.reconnect(); return {}; }
// v1 requires Master OFF; the legacy action keeps its old behaviour of switching it off first.
static Outcome opReboot(bool requireMasterOff) {
    if(otaBusy()) return {Code::Busy,"OTA in progress"};
    if(requireMasterOff && drive.masterOn) return {Code::MasterMustBeOff,"Turn Master off before reboot"};
    drive.masterOn=false; if(hooks.masterChanged) hooks.masterChanged();
    if(hooks.reboot) hooks.reboot(); return {};
}
enum class Edit:uint8_t { Preview, Apply, Save, Discard, Touch };
static Outcome opEdit(Edit kind,JsonVariantConst req) {
    if(otaBusy()) return {Code::Busy,"OTA in progress"};
    CanGuard can; String e; bool ok;
    switch(kind) {
        case Edit::Preview: ok=settingsPreview(req,false,e); break;
        case Edit::Apply: ok=settingsPreview(req,true,e); break;
        case Edit::Save: ok=settingsCommitPreview(req,e); break;
        case Edit::Discard: ok=settingsDiscardPreview(req,e); break;
        default: ok=settingsTouchPreview(req,e); break;
    }
    if(!ok) return fromSettings(e);
    can.apply(); return {};
}
static Outcome opLegacySetConfig(JsonVariantConst config) {
    if(otaBusy()) return {Code::Busy,"OTA or preview in progress"};
    if(settingsPreviewActive()) return {Code::EditorBusy,"OTA or preview in progress"};
    CanGuard can; String e;
    if(!settingsApply(config,e)) return {Code::InvalidArgument,e};
    can.apply(); settingsSave(); return {};
}
static bool intIn(JsonVariantConst v,int lo,int hi) { return v.is<int>() && v.as<int>()>=lo && v.as<int>()<=hi; }
// Legacy action: same operations; field checks keep their previous messages.
static Outcome legacyAction(JsonVariantConst j) {
    const char* a=j["action"]|"";
    if(!strcmp(a,"master")) { if(!j["on"].is<bool>()) return {Code::InvalidArgument,"Expected on:boolean"}; return opMaster(j["on"]); }
    if(!strcmp(a,"reset")) {
        if(otaBusy()) return {Code::Busy,"OTA in progress"};
        if(drive.masterOn) return {Code::MasterMustBeOff,"Turn Master off before reset"};
        if(!intIn(j["channel"],-1,7)) return {Code::InvalidArgument,"Expected channel -1..7"};
        return opReset(j["channel"]);
    }
    if(!strcmp(a,"calibrate")) return opCalibrate();
    if(!strcmp(a,"reconnect")) return opReconnect();
    if(!strcmp(a,"reboot")) return opReboot(false);
    if(!strcmp(a,"preset_save")||!strcmp(a,"preset_load")) {
        Outcome o=noPreview(); if(!o.ok()) return o;
        if(!intIn(j["slot"],0,NUM_PRESETS-1)) return {Code::InvalidArgument,"Expected slot 0..2"};
        return !strcmp(a,"preset_save")?opPresetSave(j["slot"]):opPresetLoad(j["slot"]);
    }
    return {Code::InvalidArgument,"Unknown action"};
}

// ---- JSON helpers ----
struct CountingReader { // detects trailing bytes after the root value
    const char* p; const char* end; size_t used;
    CountingReader(const char* data,const char* stop):p(data),end(stop),used(0) {}
    int read() { if(p+used>=end) return -1; return uint8_t(p[used++]); }
    size_t readBytes(char* dst,size_t n) { size_t i=0; for(;i<n;++i) { int c=read(); if(c<0) break; dst[i]=char(c); } return i; }
};
static Code parse(DynamicJsonDocument& doc,const char* data,size_t len,String& message) {
    if(doc.capacity()==0) { message="Out of memory"; return Code::InternalError; }
    int syntax=strictjson::check(data,len,12);
    if(syntax==2) { message="Message too complex"; return Code::FrameTooLarge; }
    if(syntax) { message="Invalid JSON"; return Code::BadJson; }
    CountingReader reader(data,data+len);
    auto err=deserializeJson(doc,reader,DeserializationOption::NestingLimit(12));
    if(err==DeserializationError::NoMemory || err==DeserializationError::TooDeep) { message="Message too complex"; return Code::FrameTooLarge; }
    if(err) { message="Invalid JSON"; return Code::BadJson; }
    for(size_t i=reader.used;i<len;++i) if(data[i]!=' '&&data[i]!='\t'&&data[i]!='\r'&&data[i]!='\n') { message="Invalid JSON"; return Code::BadJson; }
    return Code::Ok;
}
static void putSaved(JsonObject o) { uint32_t saved; if(settingsSavedRevision(saved)) o["savedRevision"]=saved; else o["savedRevision"]=nullptr; }
// Envelope reflects the state after the command, success or not.
static void envelope(JsonDocument& out,JsonVariantConst id,bool ok) {
    out["v"]=1;
    if(settingsValidId(id)) out["id"]=id.as<const char*>(); else out["id"]=nullptr;
    out["bootId"]=bootId(); out["ok"]=ok; out["revision"]=settingsRevision(); putSaved(out.as<JsonObject>());
}
static String errorEnvelope(JsonVariantConst id,Code code,const String& message,int& http) {
    DynamicJsonDocument out(1024); envelope(out,id,false);
    auto e=out.createNestedObject("error"); e["code"]=codeName(code); e["message"]=message.c_str();
    http=httpStatus(code); String s; serializeJson(out,s); return s;
}
static String errorEnvelope(Code code,const char* message,int& http) { return errorEnvelope(JsonVariantConst(),code,String(message),http); }
static bool onlyKeys(JsonObjectConst o,std::initializer_list<const char*> extra) {
    for(JsonPairConst kv:o) {
        const char* k=kv.key().c_str(); bool known=!strcmp(k,"v")||!strcmp(k,"id")||!strcmp(k,"cmd")||!strcmp(k,"bootId");
        for(const char* x:extra) known|=!strcmp(k,x);
        if(!known) return false;
    }
    return true;
}
static bool editorFields(JsonObjectConst o) { return settingsValidId(o["client"]) && o["revision"].is<uint32_t>(); }
static void previewResult(JsonObject r) {
    r["master"]=drive.masterOn; r["previewActive"]=settingsPreviewActive();
    if(settingsPreviewActive()) r["previewOwner"]=settingsPreviewOwner(); else r["previewOwner"]=nullptr;
    r["previewRemainingMs"]=settingsPreviewRemainingMs();
}
static bool fillJson(JsonObject dest,const String& json) {
    DynamicJsonDocument tmp(json.length()*4+4096); if(deserializeJson(tmp,json.c_str())) return false;
    for(JsonPairConst kv:tmp.as<JsonObjectConst>()) dest[kv.key()]=kv.value();
    return true;
}

// ---- v1 ----
static const char* const READS[]={"info","status","get_config"};
static const char* const WRITES[]={"preview","apply","save","discard","touch","action"};
static bool member(const char* cmd,const char* const* list,size_t n) { for(size_t i=0;i<n;++i) if(!strcmp(cmd,list[i])) return true; return false; }
// Field schema and value checks for a mutation. Runs before any state change.
static Outcome validateMutation(const char* cmd,JsonObjectConst o) {
    const Outcome bad(Code::InvalidArgument,"Invalid or unknown field");
    if(!strcmp(cmd,"preview")) { if(!onlyKeys(o,{"client","revision","strip","patch"})||!editorFields(o)||!intIn(o["strip"],0,7)||!o["patch"].is<JsonObjectConst>()||o["patch"].size()==0) return bad; return {}; }
    if(!strcmp(cmd,"apply")) {
        if(!onlyKeys(o,{"client","revision","config"})||!editorFields(o)||!o["config"].is<JsonObjectConst>()) return bad;
        if(measureJson(o["config"])>MAX_CONFIG_BYTES) return {Code::FrameTooLarge,"Config is too large"};
        return {};
    }
    if(!strcmp(cmd,"save")||!strcmp(cmd,"discard")||!strcmp(cmd,"touch")) { if(!onlyKeys(o,{"client","revision"})||!editorFields(o)) return bad; return {}; }
    // action
    if(!o["action"].is<const char*>()) return bad;
    const char* a=o["action"];
    if(!strcmp(a,"master")) return onlyKeys(o,{"action","on"})&&o["on"].is<bool>()?Outcome():bad;
    if(!strcmp(a,"reset")) return onlyKeys(o,{"action","channel"})&&intIn(o["channel"],-1,7)?Outcome():bad;
    if(!strcmp(a,"preset_save")||!strcmp(a,"preset_load")) return onlyKeys(o,{"action","slot","client","revision"})&&intIn(o["slot"],0,NUM_PRESETS-1)&&editorFields(o)?Outcome():bad;
    if(!strcmp(a,"calibrate")) return onlyKeys(o,{"action","client","revision"})&&editorFields(o)?Outcome():bad;
    if(!strcmp(a,"reconnect")||!strcmp(a,"reboot")) return onlyKeys(o,{"action"})?Outcome():bad;
    return {Code::UnknownCommand,"Unknown action"};
}
// Config-changing actions behave like editor commands: no preview, current revision.
static Outcome configAction(JsonObjectConst o) {
    if(otaBusy()) return {Code::Busy,"OTA in progress"};
    if(settingsPreviewActive()) return {Code::EditorBusy,"Save or discard preview first"};
    if(o["revision"].as<uint32_t>()!=settingsRevision()) return {Code::RevisionConflict,"Configuration changed; reload"};
    return {};
}
static Outcome runAction(JsonObjectConst o) {
    const char* a=o["action"];
    if(!strcmp(a,"master")) return opMaster(o["on"]);
    if(!strcmp(a,"reset")) return opReset(o["channel"]);
    if(!strcmp(a,"reconnect")) return opReconnect();
    if(!strcmp(a,"reboot")) return opReboot(true);
    Outcome gate=configAction(o); if(!gate.ok()) return gate;
    if(!strcmp(a,"preset_save")) return opPresetSave(o["slot"]);
    if(!strcmp(a,"preset_load")) return opPresetLoad(o["slot"]);
    return opCalibrate();
}
static String dispatchV1(JsonObjectConst o,Transport transport,bool httpAuthorized,int& http) {
    JsonVariantConst id=o["id"];
    if(!o.containsKey("v")) return errorEnvelope(Code::UnsupportedVersion,"Missing v",http);
    if(!o["v"].is<int>() || o["v"].as<int>()!=1) return errorEnvelope(Code::UnsupportedVersion,"Unsupported protocol version",http);
    if(!settingsValidId(id)) return errorEnvelope(Code::InvalidArgument,"Expected id: printable ASCII, 1..48 bytes",http);
    if(!o["cmd"].is<const char*>()) return errorEnvelope(id,Code::InvalidArgument,"Expected cmd",http);
    const char* cmd=o["cmd"];
    bool read=member(cmd,READS,3), write=member(cmd,WRITES,6);
    if(!read && !write) return errorEnvelope(id,Code::UnknownCommand,"Unknown command",http);
    if(transport==Transport::Http && !httpAuthorized) return errorEnvelope(id,Code::AuthRequired,"Session expired; reload",http);
    if(o.containsKey("bootId") && !o["bootId"].is<const char*>()) return errorEnvelope(id,Code::InvalidArgument,"Expected bootId string",http);
    if(write) {
        Outcome v=validateMutation(cmd,o); if(!v.ok()) return errorEnvelope(id,v.code,v.message,http);
        if(!o["bootId"].is<const char*>()) return errorEnvelope(id,Code::InvalidArgument,"Mutations require bootId",http);
        if(strcmp(o["bootId"].as<const char*>(),bootId())) return errorEnvelope(id,Code::BootChanged,"Device rebooted; reload state",http);
        Outcome r;
        if(!strcmp(cmd,"action")) r=runAction(o);
        else r=opEdit(!strcmp(cmd,"preview")?Edit::Preview:!strcmp(cmd,"apply")?Edit::Apply:!strcmp(cmd,"save")?Edit::Save:!strcmp(cmd,"discard")?Edit::Discard:Edit::Touch,o);
        if(!r.ok()) return errorEnvelope(id,r.code,r.message,http);
        DynamicJsonDocument out(1024); envelope(out,id,true); auto res=out.createNestedObject("result");
        if(!strcmp(cmd,"action")) res["master"]=drive.masterOn; else previewResult(res);
        http=200; String s; serializeJson(out,s); return s;
    }
    // reads
    if(!strcmp(cmd,"get_config")) {
        const char* view=o["view"]|"baseline";
        if(o.containsKey("view") && !o["view"].is<const char*>()) return errorEnvelope(id,Code::InvalidArgument,"Expected view",http);
        bool effective=!strcmp(view,"effective");
        if(!effective && strcmp(view,"baseline")) return errorEnvelope(id,Code::InvalidArgument,"view is baseline or effective",http);
        if(effective && !settingsValidId(o["client"])) return errorEnvelope(id,Code::InvalidArgument,"effective view requires client",http);
        if(effective && settingsPreviewActive() && strcmp(settingsPreviewOwner(),o["client"].as<const char*>())) return errorEnvelope(id,Code::EditorBusy,"Another editor owns the preview",http);
        String config=settingsJson(false,!effective);
        DynamicJsonDocument out(config.length()*4+4096); envelope(out,id,true);
        auto res=out.createNestedObject("result"); res["view"]=effective?"effective":"baseline";
        if(!fillJson(res.createNestedObject("config"),config) || out.overflowed()) return errorEnvelope(id,Code::InternalError,"Config encoding failed",http);
        http=200; String s; serializeJson(out,s); return s;
    }
    DynamicJsonDocument out(12288); envelope(out,id,true); auto res=out.createNestedObject("result");
    if(!strcmp(cmd,"info")) { if(hooks.info) hooks.info(res); }
    else if(hooks.status) hooks.status(res);
    if(out.overflowed()) return errorEnvelope(id,Code::InternalError,"Response too large",http);
    http=200; String s; serializeJson(out,s); return s;
}
String handleV1(const char* data,size_t len,Transport transport,bool httpAuthorized,int& http) {
    if(len>MAX_COMMAND_BYTES) return errorEnvelope(Code::FrameTooLarge,"Message is too large",http);
    DynamicJsonDocument doc(40960); String message;
    Code c=parse(doc,data,len,message); if(c!=Code::Ok) return errorEnvelope(c,message.c_str(),http);
    if(!doc.is<JsonObject>()) return errorEnvelope(Code::InvalidArgument,"Expected a JSON object",http);
    return dispatchV1(doc.as<JsonObjectConst>(),transport,httpAuthorized,http);
}
String usbOverflow() { int http; return errorEnvelope(Code::FrameTooLarge,"Message is too large",http); }

// ---- legacy ----
static String legacyResult(const Outcome& r) {
    DynamicJsonDocument out(512); out["revision"]=settingsRevision(); putSaved(out.as<JsonObject>()); out["master"]=drive.masterOn; out["ok"]=r.ok();
    if(!r.ok()) out["error"]=r.message.length()?r.message.c_str():"Unknown command";
    String s; serializeJson(out,s); return s;
}
static Outcome legacyCommand(const char* cmd,JsonVariantConst d) {
    if(!strcmp(cmd,"set_config")) return opLegacySetConfig(d["config"]);
    if(!strcmp(cmd,"action")) return legacyAction(d);
    if(!strcmp(cmd,"preview")) return opEdit(Edit::Preview,d);
    if(!strcmp(cmd,"apply")) return opEdit(Edit::Apply,d);
    if(!strcmp(cmd,"save")) return opEdit(Edit::Save,d);
    if(!strcmp(cmd,"discard")) return opEdit(Edit::Discard,d);
    if(!strcmp(cmd,"touch")) return opEdit(Edit::Touch,d);
    return {Code::UnknownCommand,""};
}
String statusJson() { DynamicJsonDocument d(8192); if(hooks.status) hooks.status(d.to<JsonObject>()); String s; serializeJson(d,s); return s; }
String infoJson() { DynamicJsonDocument d(1536); if(hooks.info) hooks.info(d.to<JsonObject>()); String s; serializeJson(d,s); return s; }
String handleUsbLine(const char* data,size_t len) {
    int http;
    if(len>MAX_COMMAND_BYTES) return usbOverflow();
    DynamicJsonDocument doc(40960); String message;
    Code c=parse(doc,data,len,message); if(c!=Code::Ok) return errorEnvelope(c,message.c_str(),http);
    if(doc.is<JsonObject>() && doc.containsKey("v")) return dispatchV1(doc.as<JsonObjectConst>(),Transport::Usb,true,http);
    // Legacy (no v): keeps the 24000-byte limit and its payloads.
    if(len>MAX_LEGACY_BYTES) return "{\"error\":\"line too long\"}";
    const char* cmd=doc["cmd"]|"";
    if(!strcmp(cmd,"status")) return statusJson();
    if(!strcmp(cmd,"info")) return infoJson();
    if(!strcmp(cmd,"get_config")) return settingsJson(false,true);
    return legacyResult(legacyCommand(cmd,doc.as<JsonVariantConst>()));
}
static String legacyError(int code,const String& text,int& http) { DynamicJsonDocument d(512); d["error"]=text.c_str(); http=code; String s; serializeJson(d,s); return s; }
String legacyHttp(const char* route,const char* data,size_t len,int& http) {
    if(len>MAX_LEGACY_BYTES) return legacyError(413,"Config is too large",http);
    DynamicJsonDocument doc(24576); if(deserializeJson(doc,data,len)) return legacyError(400,"Invalid JSON",http);
    JsonVariantConst d=doc.as<JsonVariantConst>();
    if(!strcmp(route,"/api/config")) {
        Outcome r=opLegacySetConfig(d); if(!r.ok()) return legacyError(400,r.message,http);
        http=200; return drive.masterOn?"{\"ok\":true,\"master\":true}":"{\"ok\":true,\"master\":false}";
    }
    if(!strcmp(route,"/api/action")) {
        Outcome r=legacyAction(d); if(!r.ok()) return legacyError(400,r.message,http);
        http=200; return "{\"ok\":true}";
    }
    Edit kind=!strcmp(route,"/api/preview")?Edit::Preview:!strcmp(route,"/api/apply")?Edit::Apply:!strcmp(route,"/api/save")?Edit::Save:!strcmp(route,"/api/discard")?Edit::Discard:Edit::Touch;
    Outcome r=opEdit(kind,d);
    if(!r.ok()) {
        int code=r.code==Code::Busy||r.code==Code::RevisionConflict||r.code==Code::EditorBusy||r.code==Code::PreviewExpired?409:r.code==Code::FlashWriteFailed?507:400;
        return legacyError(code,r.message,http);
    }
    DynamicJsonDocument out(256); out["ok"]=true; out["revision"]=settingsRevision(); putSaved(out.as<JsonObject>()); out["master"]=drive.masterOn;
    http=200; String s; serializeJson(out,s); return s;
}
String sseV1(uint32_t seq) {
    DynamicJsonDocument d(12288); d["v"]=1; d["bootId"]=bootId(); d["seq"]=seq;
    d["revision"]=settingsRevision(); putSaved(d.as<JsonObject>());
    if(hooks.status) hooks.status(d.createNestedObject("status"));
    String s; serializeJson(d,s); return s;
}
}
