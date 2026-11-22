#pragma once
#include <stddef.h>
#include <stdint.h>
// Caller owns storage and synchronization. Monotonic positions distinguish
// overwritten records from records added while a flash write is in progress.
class RamLog {
public:
    RamLog(char* memory, size_t capacity): data(memory), capacity(capacity) {}
    void append(const char* text, size_t size) {
        for(size_t i=0;i<size;++i) data[end++%capacity]=text[i];
        if(end-start>capacity) { start=end-capacity; if(flushed<start) { lost+=start-flushed; flushed=start; } }
    }
    size_t copy(uint64_t from, char* dest, size_t size) const {
        if(from<start) from=start;
        size_t n=from<end ? size_t(end-from) : 0;
        if(n>size) n=size;
        for(size_t i=0;i<n;++i) dest[i]=data[(from+i)%capacity];
        return n;
    }
    void clear() { start=end; flushed=end; lost=0; }
    void acknowledge(uint64_t through) { if(through>flushed) flushed=through>end?end:through; }
    char* data; size_t capacity;
    uint64_t start=0, end=0, flushed=0, lost=0;
};
