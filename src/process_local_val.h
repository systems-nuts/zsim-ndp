#pragma once
#include <cstdint>

extern uint32_t procIdx;
extern uint32_t lineBits; //process-local for performance, but logically global
extern uint32_t pageBits; //process-local for performance, but logically global
extern uint64_t procMask;

inline uint64_t getPhysicalLineAddr(uint64_t vAddr) {
    return procMask | (vAddr >> lineBits);
}