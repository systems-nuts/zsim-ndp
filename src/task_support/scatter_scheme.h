#pragma once
#include <cstdint>

namespace task_support {

class CommModule;

class ScatterScheme {
public:
    enum Trigger {
        AfterGather, 
        Interval,
        OnDemand
    };
    Trigger trigger;
    uint32_t packetSize;
    ScatterScheme(Trigger _trigger, uint32_t _packetSize)
        : trigger(_trigger), packetSize(_packetSize){}
    virtual bool shouldTrigger(CommModule* commModule) = 0;
};

class AfterGatherScatter : public ScatterScheme {
public:
    AfterGatherScatter(uint32_t _packetSize)
        : ScatterScheme(Trigger::AfterGather, _packetSize) {}
    bool shouldTrigger(CommModule* commModule) override;
};

class IntervalScatter : public ScatterScheme {
public: 
    const uint32_t interval;
    IntervalScatter(uint32_t _packetSize, uint32_t _interval)
        : ScatterScheme(Trigger::Interval, _packetSize), interval(_interval) {}
    bool shouldTrigger(CommModule* CommModule) override;
};

class OnDemandScatter : public ScatterScheme {
public:
    const uint32_t threshold;
    OnDemandScatter(uint32_t _packetSize, uint32_t _threshold)
        : ScatterScheme(Trigger::AfterGather, _packetSize), threshold(_threshold) {}
    bool shouldTrigger(CommModule* commModule) override;
};

}