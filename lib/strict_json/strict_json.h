#pragma once
#include <stddef.h>
// RFC 8259 syntax check run before ArduinoJson, which is lenient (unquoted keys,
// single quotes, raw control characters). Returns false for anything that is not
// exactly one JSON value surrounded by optional whitespace, or nests deeper than maxDepth.
namespace strictjson {
struct Checker {
    const char* p; const char* end; int depth=0, maxDepth;
    Checker(const char* data,size_t len,int limit):p(data),end(data+len),maxDepth(limit) {}
    bool more() const { return p<end; }
    void ws() { while(more() && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }
    bool lit(const char* s) { for(;*s;++s,++p) if(!more()||*p!=*s) return false; return true; }
    static bool hex(char c) { return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
    bool string() {
        ++p;
        while(more()) {
            unsigned char c=*p++;
            if(c=='"') return true;
            if(c<0x20) return false;
            if(c=='\\') {
                if(!more()) return false; char e=*p++;
                if(e=='u') { for(int i=0;i<4;++i) { if(!more()||!hex(*p)) return false; ++p; } }
                else if(!(e=='"'||e=='\\'||e=='/'||e=='b'||e=='f'||e=='n'||e=='r'||e=='t')) return false;
            }
        }
        return false;
    }
    bool digits() { const char* s=p; while(more()&&*p>='0'&&*p<='9') ++p; return p>s; }
    bool number() {
        if(*p=='-') ++p;
        if(!more()) return false;
        if(*p=='0') ++p; else if(!digits()) return false;
        if(more()&&*p=='.') { ++p; if(!digits()) return false; }
        if(more()&&(*p=='e'||*p=='E')) { ++p; if(more()&&(*p=='+'||*p=='-')) ++p; if(!digits()) return false; }
        return true;
    }
    bool value() {
        ws(); if(!more()) return false;
        char c=*p;
        if(c=='{'||c=='[') {
            if(++depth>maxDepth) return false;
            char close=c=='{'?'}':']'; ++p; ws();
            if(more()&&*p==close) { ++p; --depth; return true; }
            for(;;) {
                if(c=='{') { ws(); if(!more()||*p!='"'||!string()) return false; ws(); if(!more()||*p++!=':') return false; }
                if(!value()) return false;
                ws(); if(!more()) return false;
                if(*p==',') { ++p; continue; }
                if(*p==close) { ++p; --depth; return true; }
                return false;
            }
        }
        if(c=='"') return string();
        if(c=='t') return lit("true"); if(c=='f') return lit("false"); if(c=='n') return lit("null");
        if(c=='-'||(c>='0'&&c<='9')) return number();
        return false;
    }
};
// 0 ok, 1 syntax error, 2 too deep
inline int check(const char* data,size_t len,int maxDepth) {
    Checker k(data,len,maxDepth);
    bool ok=k.value(); if(ok) { k.ws(); ok=!k.more(); }
    if(ok) return 0;
    return k.depth>maxDepth?2:1;
}
}
