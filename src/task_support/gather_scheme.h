#pragma once
#include <cstdint>

namespace task_support {

class CommModule;

class GatherScheme {
public:
    enum Trigger {
        Whenever,
        Interval,
        OnDemand
    };
    Trigger trigger;
    uint32_t packetSize; 

    GatherScheme(Trigger _trigger, uint32_t _packetSize) 
        : trigger(_trigger), packetSize(_packetSize) {}
    virtual bool shouldTrigger(CommModule* commModule) = 0;
};

class WheneverGather : public GatherScheme {
public:
    WheneverGather(uint32_t _packetSize) 
        : GatherScheme(Trigger::Whenever, _packetSize) {}
    bool shouldTrigger(CommModule* commModule) override {
        return true;
    }
};

class IntervalGather : public GatherScheme {
public:
    const uint32_t interval;

    IntervalGather(uint32_t _packetSize, uint32_t _interval) 
        : GatherScheme(Trigger::Interval, _packetSize), 
          interval(_interval) {}

    bool shouldTrigger(CommModule* commModule) override;
};

class OnDemandGather : public GatherScheme {
public:
    const uint32_t threshold;

    OnDemandGather(uint32_t _packetSize, uint32_t _threshold)
        : GatherScheme(Trigger::OnDemand, _packetSize), 
          threshold(_threshold) {}

    bool shouldTrigger(CommModule* commModule) override;
};

}