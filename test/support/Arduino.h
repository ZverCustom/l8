#pragma once
#include <string>
#include <cstring>
#include <stdint.h>
#include <cmath>
using std::isfinite;
class String {
    std::string value;
public:
    String()=default;
    String(const char* v):value(v?v:""){}
    String(const std::string& v):value(v){}
    const char* c_str()const{return value.c_str();}
    size_t length()const{return value.length();}
    bool operator!=(const char* other)const{return value!=other;}
    bool concat(const char* text,size_t count){value.append(text,count);return true;}
    bool concat(const char* text){value.append(text);return true;}
    bool concat(char c){value.push_back(c);return true;}
};
struct SerialStub { template<class T> void println(const T&){} };
extern SerialStub Serial;
extern uint32_t testMillis;
inline uint32_t millis(){return testMillis;}
#ifndef __APPLE__
inline size_t strlcpy(char* dst,const char* src,size_t n){size_t size=strlen(src);if(n){size_t copy=size<n-1?size:n-1;memcpy(dst,src,copy);dst[copy]=0;}return size;}
#endif
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
