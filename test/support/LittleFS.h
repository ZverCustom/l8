#pragma once
#include <Arduino.h>
#include <map>
#include <memory>
struct File {
    std::shared_ptr<std::string> value; size_t offset=0;
    explicit operator bool()const{return bool(value);}
    size_t size()const{return value?value->size():0;}
    int read(){return value&&offset<value->size()?uint8_t((*value)[offset++]):-1;}
    size_t readBytes(char* dst,size_t n){size_t i=0;for(;i<n;++i){int c=read();if(c<0)break;dst[i]=c;}return i;}
    bool shortWrite=false;
    size_t print(const String& text){if(!value)return 0;if(shortWrite){*value=std::string(text.c_str()).substr(0,text.length()/2);return text.length()/2;}*value=text.c_str();return text.length();}
    void flush(){} void close(){}
};
struct FakeFS {
    std::map<std::string,std::shared_ptr<std::string>> files;
    bool failWrite=false,failInstall=false,failBackup=false,failBegin=false,shortWrite=false; int writes=0;
    bool begin(bool){return !failBegin;}
    bool exists(const char* path){return files.count(path);}
    File open(const char* path,const char* mode){if(*mode=='w'){if(failWrite)return {};files[path]=std::make_shared<std::string>();++writes;}auto it=files.find(path);if(it==files.end())return File{};File f{it->second};f.shortWrite=*mode=='w'&&shortWrite;return f;}
    bool remove(const char* path){return files.erase(path);}
    bool rename(const char* from,const char* to){if(!files.count(from)||(failInstall&&!strcmp(to,"/config-v3.json"))||(failBackup&&!strcmp(to,"/config-v3.bak")))return false;files[to]=files[from];files.erase(from);return true;}
};
extern FakeFS LittleFS;
