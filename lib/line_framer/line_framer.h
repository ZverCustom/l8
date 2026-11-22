#pragma once
#include <stddef.h>
#include <string>
// Newline framing for the USB command channel. Every byte of the line body is
// kept (a CR inside the body reaches the JSON check and is rejected there); only
// one CR directly before the LF is treated as part of a CRLF terminator. A body
// longer than `limit` bytes is dropped whole up to the next LF and reported once
// as Overflow, so its tail can never run as a new command.
struct LineFramer {
    enum Event { None, Line, Overflow };
    explicit LineFramer(size_t limit):limit(limit) {}
    Event feed(char c) {
        if(c=='\n') {
            if(dropping) { dropping=false; buffer.clear(); return Overflow; }
            if(!buffer.empty() && buffer.back()=='\r') buffer.pop_back();
            if(buffer.size()>limit) { buffer.clear(); return Overflow; }
            if(buffer.empty()) return None;
            line.swap(buffer); buffer.clear(); return Line;
        }
        if(dropping) return None;
        if(buffer.size()>limit) { dropping=true; buffer.clear(); buffer.shrink_to_fit(); return None; } // limit+1 kept for a final CR
        buffer.push_back(c); return None;
    }
    const std::string& text() const { return line; } // valid after Line
    void release() { line.clear(); line.shrink_to_fit(); }
    size_t limit; bool dropping=false; std::string buffer, line;
};
