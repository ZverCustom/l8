#pragma once
#include <stddef.h>
#include <string>
// Newline framing for the USB command channel. A line longer than `limit` bytes
// (CR/LF excluded) is dropped whole: everything up to the next '\n' is discarded
// and reported once as Overflow, so its tail can never run as a new command.
struct LineFramer {
    enum Event { None, Line, Overflow };
    explicit LineFramer(size_t limit):limit(limit) {}
    Event feed(char c) {
        if(c=='\r') return None;
        if(c=='\n') {
            if(dropping) { dropping=false; buffer.clear(); return Overflow; }
            if(buffer.empty()) return None;
            line.swap(buffer); buffer.clear(); return Line;
        }
        if(dropping) return None;
        if(buffer.size()>=limit) { dropping=true; buffer.clear(); buffer.shrink_to_fit(); return None; }
        buffer.push_back(c); return None;
    }
    const std::string& text() const { return line; } // valid after Line
    void release() { line.clear(); line.shrink_to_fit(); }
    size_t limit; bool dropping=false; std::string buffer, line;
};
