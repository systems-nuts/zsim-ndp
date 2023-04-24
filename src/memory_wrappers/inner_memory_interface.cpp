#include "assert.h"
#include "inner_memory_interface.h"
#include "config.h"
#include "ddr_mem.h"
#include "mem_channel.h"
#include "mem_channel_backend.h"
#include "mem_channel_backend_ddr.h"
#include "mem_ctrls.h"
#include "ddr_mem.h"
#include "weave_md1_mem.h"
#include "dramsim_mem_ctrl.h"
#include "detailed_mem.h"

void InnerMemoryInterface::init(Config& config, uint32_t lineSize, 
        uint32_t frequency, uint32_t domain, g_string& name, 
        std::string prefix) {
    this->memObj = BuildMemoryController(config, lineSize, frequency, 
        domain, name, prefix);
}  

MemObject* InnerMemoryInterface::BuildMemoryController( 
        Config& config, 
        uint32_t lineSize, 
        uint32_t frequency, 
        uint32_t domain, 
        g_string& name, 
        const std::string& prefix) {
    std::string type = config.get<const char*>(prefix + "type", "Simple");
    uint32_t latency = (type == "DDR")? -1 : config.get<uint32_t>(prefix + "latency", 100);
    MemObject* mem = nullptr;
    if (type == "Simple") {
        mem = new SimpleMemory(latency, name);
    } else if (type == "MD1") {
        uint32_t bandwidth = config.get<uint32_t>(prefix + "bandwidth", 6400);
        mem = new MD1Memory(lineSize, frequency, bandwidth, latency, name);
    } else if (type == "WeaveMD1") {
        uint32_t bandwidth = config.get<uint32_t>(prefix + "bandwidth", 6400);
        uint32_t boundLatency = config.get<uint32_t>(prefix + "boundLatency", latency);
        mem = new WeaveMD1Memory(lineSize, frequency, bandwidth, latency, 
            boundLatency, domain, name);
    } else if (type == "WeaveSimple") {
        uint32_t boundLatency = config.get<uint32_t>(prefix + "boundLatency", 100);
        mem = new WeaveSimpleMemory(latency, boundLatency, domain, name);
    } else if (type == "DDR") {
        mem = BuildDDRMemory(config, lineSize, frequency, domain, name, prefix);
    } else if (type == "DRAMSim") {
        uint64_t cpuFreqHz = 1000000 * frequency;
        uint32_t capacity = config.get<uint32_t>(prefix + "capacityMB", 16384);
        std::string dramTechIni = config.get<const char*>(prefix + "techIni");
        std::string dramSystemIni = config.get<const char*>(prefix + "systemIni");
        std::string outputDir = config.get<const char*>(prefix + "outputDir");
        std::string traceName = config.get<const char*>(prefix + "traceName");
        mem = new DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName,
            capacity, cpuFreqHz, latency, domain, name);
    } else if (type == "Channel") {
        mem = BuildMemChannel(config, lineSize, frequency, domain, name, prefix);
    } else if (type == "Detailed") {
        // FIXME(dsm): Don't use a separate config file... see DDRMemory
        g_string mcfg = config.get<const char*>(prefix + "paramFile", "");
        mem = new MemControllerBase(mcfg, lineSize, frequency, domain, name);
    } else if (type == "Channel") {
        mem = BuildMemChannel(config, lineSize, frequency, domain, name, prefix);
    } else {
        panic("Invalid memory controller type %s", type.c_str());
    }
    return mem;
}

MemObject* InnerMemoryInterface::BuildMemChannel(
        Config& config, 
        uint32_t lineSize, 
        uint32_t sysFreqMHz, 
        uint32_t domain, 
        g_string& name, 
        const std::string& prefix) {
    std::string channelType = config.get<const char*>(prefix + "channelType");
    uint32_t memFreqMHz = config.get<uint32_t>(prefix + "channelFreq");  // MHz
    uint32_t channelWidth = config.get<uint32_t>(prefix + "channelWidth");  // bits
    // chFreq = memFreqMHz;
    // chWidth = channelWidth;

    uint32_t queueDepth = config.get<uint32_t>(prefix + "queueDepth", 16);
    uint32_t controllerLatency = config.get<uint32_t>(prefix + "controllerLatency", 0);

    bool waitForWriteAck = config.get<bool>(prefix + "waitForWriteAck", false);

    MemChannelBackend* be = nullptr;
    MemObject* mem = nullptr;

    if (channelType == "Simple" ) {
        this->simModel = BoundWeaveModel;
        uint32_t latency = config.get<uint32_t>(prefix + "latency", 300);  // memory cycles
        be = new MemChannelBackendSimple(memFreqMHz, latency, channelWidth, queueDepth);
        assert(be);
        mem = new MemChannel(be, sysFreqMHz, controllerLatency, waitForWriteAck, domain, name);
    } else if (channelType == "DDR") {
        this->simModel = BoundWeaveModel;
        uint32_t ranksPerChannel = config.get<uint32_t>(prefix + "ranksPerChannel", 1);
        uint32_t banksPerRank = config.get<uint32_t>(prefix + "banksPerRank", 8);            // DDR3
        uint32_t bankGroupsPerRank = config.get<uint32_t>(prefix + "bankGroupsPerRank", 1);  // single bank group
        const char* pagePolicy = config.get<const char*>(prefix + "pagePolicy", "close");
        bool deferWrites = config.get<bool>(prefix + "deferWrites", true);
        uint32_t pageSize = config.get<uint32_t>(prefix + "pageSize", 1024);         // 1 kB
        uint32_t burstCount = config.get<uint32_t>(prefix + "burstCount", 8);        // DDR3
        uint32_t deviceIOWidth = config.get<uint32_t>(prefix + "deviceIOWidth", 8);  // bits
        const char* addrMapping = config.get<const char*>(prefix + "addrMapping", "rank:col:bank");
        uint32_t maxRowHits = config.get<uint32_t>(prefix + "maxRowHits", -1u);            // bits
        uint32_t powerDownCycles = config.get<uint32_t>(prefix + "powerDownCycles", -1u);  // system cycles
        if (powerDownCycles == 0) powerDownCycles = -1u;                                   // 0 also means never power down.
        if (powerDownCycles != -1u) {
            // To mem cycles.
            powerDownCycles = powerDownCycles * sysFreqMHz / memFreqMHz + 1;
        }

        MemChannelBackendDDR::Timing timing;
        // All in mem cycles.
#define SETT(name) \
    timing.name = config.get<uint32_t>(prefix + "timing.t" #name)
#define SETTDEFVAL(name, defval) \
    timing.name = config.get<uint32_t>(prefix + "timing.t" #name, defval)
        SETT(CAS);
        SETT(RAS);
        SETT(RCD);
        SETT(RP);
        SETT(RRD);
        SETT(RTP);
        SETT(RFC);
        SETTDEFVAL(WR, 1);
        SETTDEFVAL(WTR, 0);
        SETTDEFVAL(CCD, 0);
        SETTDEFVAL(CWL, timing.CAS - 1);             // default value from DRAMSim2, WL = RL - 1
        SETTDEFVAL(REFI, 7800 * memFreqMHz / 1000);  // 7.8 us
        SETTDEFVAL(RPab, timing.RP);
        SETTDEFVAL(FAW, 0);
        SETTDEFVAL(RTRS, 1);
        SETTDEFVAL(CMD, 1);
        SETTDEFVAL(XP, 0);
        SETTDEFVAL(CKE, 0);
        SETTDEFVAL(RRD_S, timing.RRD);
        SETTDEFVAL(CCD_S, timing.CCD);
        SETTDEFVAL(WTR_S, timing.WTR);
#undef SETT
#undef SETTDEFVAL
        timing.BL = burstCount / 2;  // double-data-rate
        timing.rdBurstChannelOccupyOverhead = config.get<uint32_t>(prefix + "timing.rdBurstChannelOccupyOverhead", 0);
        timing.wrBurstChannelOccupyOverhead = config.get<uint32_t>(prefix + "timing.wrBurstChannelOccupyOverhead", 0);

        MemChannelBackendDDR::Power power;
        // VDD in V, to mV.
        double vdd = config.get<double>(prefix + "power.VDD", 0);
        power.VDD = (uint32_t)(vdd * 1000);
        // IDD in mA, to uA.
#define SETIDD(name) \
    power.IDD##name = (uint32_t)(config.get<double>(prefix + "power.IDD" #name, 0) * 1000)
        SETIDD(0);
        SETIDD(2N);
        SETIDD(2P);
        SETIDD(3N);
        SETIDD(3P);
        SETIDD(4R);
        SETIDD(4W);
        SETIDD(5);
#undef SETIDD
        double channelWirePicoJoulePerBit = config.get<double>(prefix + "power.channelWirePicoJoulePerBit", 0);
        power.channelWireFemtoJoulePerBit = (uint32_t)(channelWirePicoJoulePerBit * 1000);

        be = new MemChannelBackendDDR(name, ranksPerChannel, banksPerRank, bankGroupsPerRank, pagePolicy, pageSize, burstCount,
                                      deviceIOWidth, channelWidth, memFreqMHz, timing, power, addrMapping, queueDepth, deferWrites,
                                      maxRowHits, powerDownCycles);
        assert(be);
        mem = new MemChannel(be, sysFreqMHz, controllerLatency, waitForWriteAck, domain, name);
    } else {
        panic("Invalid memory channel type %s", channelType.c_str());
    }

    assert(mem);
    return mem;
}

DDRMemory* InnerMemoryInterface::BuildDDRMemory(
        Config& config, 
        uint32_t lineSize, 
        uint32_t frequency, 
        uint32_t domain, 
        g_string& name, 
        const std::string& prefix) {
    uint32_t ranksPerChannel = config.get<uint32_t>(prefix + "ranksPerChannel", 4);
    uint32_t banksPerRank = config.get<uint32_t>(prefix + "banksPerRank", 8);  // DDR3 std is 8
    uint32_t pageSize = config.get<uint32_t>(prefix + "pageSize", 8*1024);  // 1Kb cols, x4 devices
    const char* tech = config.get<const char*>(prefix + "tech", "DDR3-1333-CL10");  // see cpp file for other techs
    const char* addrMapping = config.get<const char*>(prefix + "addrMapping", "rank:col:bank");  // address splitter interleaves channels; row always on top

    // If set, writes are deferred and bursted out to reduce WTR overheads
    bool deferWrites = config.get<bool>(prefix + "deferWrites", true);
    bool closedPage = config.get<bool>(prefix + "closedPage", true);

    // Max row hits before we stop prioritizing further row hits to this bank.
    // Balances throughput and fairness; 0 -> FCFS / high (e.g., -1) -> pure FR-FCFS
    uint32_t maxRowHits = config.get<uint32_t>(prefix + "maxRowHits", 4);

    // Request queues
    uint32_t queueDepth = config.get<uint32_t>(prefix + "queueDepth", 16);
    uint32_t controllerLatency = config.get<uint32_t>(prefix + "controllerLatency", 10);  // in system cycles

    auto mem = new DDRMemory(zinfo->lineSize, pageSize, ranksPerChannel, banksPerRank, frequency, tech,
            addrMapping, controllerLatency, queueDepth, maxRowHits, deferWrites, closedPage, domain, name);
    return mem;
}
