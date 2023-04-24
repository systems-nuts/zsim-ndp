#include "pim_memory/pim_bank_memory.h"

using namespace pimbridge;


PimBankMemory::PimBankMemory(Config& config, uint32_t _lineSize, 
                             uint32_t frequency, uint32_t domain, 
                             g_string _name, std::string cfgPrefix) : name(_name) {
    mainMem = new InnerMemoryInterface();
    hiddenMem = new InnerMemoryInterface();

    std::string nameStr(_name.c_str());
    g_string mainName((nameStr + "-main").c_str());
    g_string hiddenName((nameStr + "-hidden").c_str());
    mainMem->init(config, _lineSize, frequency, domain, mainName, 
        cfgPrefix + "inner-mem.");
    hiddenMem->init(config, _lineSize, frequency, domain, hiddenName, 
        cfgPrefix + "inner-mem.");

    lineSize = _lineSize;
    transferLineIdStart = 0x100;
    nTranferLine = 4096;
    memoryLineIdStart = transferLineIdStart + nTranferLine;

    futex_init(&this->lock);                            
}

PimBankMemory::~PimBankMemory() {
    delete mainMem;
    delete hiddenMem;
}

uint64_t PimBankMemory::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }
    if (req.type == PUTS) { 
        return req.cycle; 
    }
    if (req.is(MemReq::Flag::TRANSFER_REGION)) {
        return transferRegionAccess(req);
    }
    if (this->inTransferRegion(req.lineAddr)) {
        return this->bypassRequest(req);
    } else {
        return mainMem->memObj->access(req);
    }
}

uint64_t PimBankMemory::transferRegionAccess(MemReq& req) {
    Address newLineAddr = this->convertTransferRegionAddr(req.lineAddr);
    MemReq req2 = req;
    req2.lineAddr = newLineAddr;
    MESIState dummyState = MESIState::I;
    req2.state = &dummyState;
    return this->mainMem->memObj->access(req2);
}

uint64_t PimBankMemory::bypassRequest(MemReq& req) {
    return hiddenMem->memObj->access(req);
}

// TBY TODO: check the correctness transfer region implementation

bool PimBankMemory::inTransferRegion(Address lineAddr) {
    return (lineAddr >= this->transferLineIdStart && lineAddr < memoryLineIdStart);
}

uint64_t PimBankMemory::convertTransferRegionAddr(uint64_t addrKey) {
    uint64_t lineAddr = (addrKey % nTranferLine) + transferLineIdStart;
    return lineAddr;
}
