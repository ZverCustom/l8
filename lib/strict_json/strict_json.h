#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
// RFC 8259 check run before ArduinoJson, which is lenient (unquoted keys, single
// quotes, raw control characters, any byte >= 0x80) and compares strings as C
// strings. Rejects anything that is not exactly one JSON value with optional
// whitespace around it, and additionally:
//  - invalid UTF-8 (overlong, surrogates, > U+10FFFF, stray/truncated bytes) and
//    lone or misordered \u surrogates                        -> Syntax
//  - U+0000 anywhere in a string or key (raw or \u0000)      -> Forbidden
//  - two keys of one object that decode to the same string   -> Duplicate
//  - nesting deeper than maxDepth                            -> TooDeep
namespace strictjson {
enum Result { Ok=0, Syntax=1, TooDeep=2, Forbidden=3, Duplicate=4 };
// Length of one valid UTF-8 sequence starting at p (lead byte >= 0x80), 0 if invalid.
inline size_t utf8Sequence(const unsigned char* p,const unsigned char* end) {
    unsigned char c=p[0]; size_t n; uint32_t lo=0x80, hi=0xBF;
    if(c>=0xC2 && c<=0xDF) n=2;
    else if(c>=0xE0 && c<=0xEF) { n=3; if(c==0xE0) lo=0xA0; if(c==0xED) hi=0x9F; }
    else if(c>=0xF0 && c<=0xF4) { n=4; if(c==0xF0) lo=0x90; if(c==0xF4) hi=0x8F; }
    else return 0;
    if(size_t(end-p)<n) return 0;
    if(p[1]<lo || p[1]>hi) return 0;
    for(size_t i=2;i<n;++i) if(p[i]<0x80 || p[i]>0xBF) return 0;
    return n;
}
// Whole buffer is valid UTF-8 (used to check responses in tests).
inline bool validUtf8(const char* data,size_t len) {
    const unsigned char* p=(const unsigned char*)data; const unsigned char* end=p+len;
    while(p<end) { if(*p<0x80) { ++p; continue; } size_t n=utf8Sequence(p,end); if(!n) return false; p+=n; }
    return true;
}
struct Checker {
    const unsigned char* p; const unsigned char* end; int depth=0, maxDepth; Result fail=Syntax;
    Checker(const char* data,size_t len,int limit):p((const unsigned char*)data),end((const unsigned char*)data+len),maxDepth(limit) {}
    bool more() const { return p<end; }
    void ws() { while(more() && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }
    bool lit(const char* s) { for(;*s;++s,++p) if(!more()||*p!=(unsigned char)*s) return false; return true; }
    static int hex(unsigned char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
    bool hex4(uint32_t& v) { v=0; for(int i=0;i<4;++i) { if(!more()) return false; int h=hex(*p++); if(h<0) return false; v=v*16+h; } return true; }
    static void put(std::string* out,uint32_t cp) {
        if(!out) return;
        if(cp<0x80) out->push_back(char(cp));
        else if(cp<0x800) { out->push_back(char(0xC0|cp>>6)); out->push_back(char(0x80|(cp&0x3F))); }
        else if(cp<0x10000) { out->push_back(char(0xE0|cp>>12)); out->push_back(char(0x80|(cp>>6&0x3F))); out->push_back(char(0x80|(cp&0x3F))); }
        else { out->push_back(char(0xF0|cp>>18)); out->push_back(char(0x80|(cp>>12&0x3F))); out->push_back(char(0x80|(cp>>6&0x3F))); out->push_back(char(0x80|(cp&0x3F))); }
    }
    bool nul() { fail=Forbidden; return false; }
    // Validates one string; when `out` is set, stores its decoded UTF-8 value.
    bool string(std::string* out) {
        ++p;
        while(more()) {
            unsigned char c=*p;
            if(c=='"') { ++p; return true; }
            if(c<0x20) return c==0?nul():false;
            if(c>=0x80) { size_t n=utf8Sequence(p,end); if(!n) return false; if(out) out->append((const char*)p,n); p+=n; continue; }
            ++p;
            if(c!='\\') { if(out) out->push_back(char(c)); continue; }
            if(!more()) return false;
            unsigned char e=*p++; uint32_t cp;
            switch(e) {
                case '"': case '\\': case '/': cp=e; break;
                case 'b': cp='\b'; break; case 'f': cp='\f'; break; case 'n': cp='\n'; break; case 'r': cp='\r'; break; case 't': cp='\t'; break;
                case 'u': {
                    if(!hex4(cp)) return false;
                    if(cp==0) return nul();
                    if(cp>=0xDC00 && cp<=0xDFFF) return false;          // lone low surrogate
                    if(cp>=0xD800 && cp<=0xDBFF) {                       // needs a following low surrogate
                        uint32_t low;
                        if(!(more() && *p=='\\')) return false; ++p;
                        if(!(more() && *p=='u')) return false; ++p;
                        if(!hex4(low) || low<0xDC00 || low>0xDFFF) return false;
                        cp=0x10000+((cp-0xD800)<<10)+(low-0xDC00);
                    }
                    break;
                }
                default: return false;
            }
            put(out,cp);
        }
        return false;
    }
    bool digits() { const unsigned char* s=p; while(more()&&*p>='0'&&*p<='9') ++p; return p>s; }
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
        unsigned char c=*p;
        if(c=='{'||c=='[') {
            if(++depth>maxDepth) { fail=TooDeep; return false; }
            unsigned char close=c=='{'?'}':']'; ++p; ws();
            if(more()&&*p==close) { ++p; --depth; return true; }
            std::vector<std::string> keys;
            for(;;) {
                if(c=='{') {
                    ws(); if(!more()||*p!='"') return false;
                    std::string key; if(!string(&key)) return false;
                    for(const auto& k:keys) if(k==key) { fail=Duplicate; return false; }
                    keys.push_back(key);
                    ws(); if(!more()||*p++!=':') return false;
                }
                if(!value()) return false;
                ws(); if(!more()) return false;
                if(*p==',') { ++p; continue; }
                if(*p==close) { ++p; --depth; return true; }
                return false;
            }
        }
        if(c=='"') return string(nullptr);
        if(c=='t') return lit("true"); if(c=='f') return lit("false"); if(c=='n') return lit("null");
        if(c=='-'||(c>='0'&&c<='9')) return number();
        return false;
    }
};
inline Result check(const char* data,size_t len,int maxDepth) {
    Checker k(data,len,maxDepth);
    bool ok=k.value(); if(ok) { k.ws(); ok=!k.more(); if(!ok) k.fail=Syntax; }
    return ok?Ok:k.fail;
}
}
