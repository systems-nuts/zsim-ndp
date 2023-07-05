#include <fstream>
#include "comm_support/profiling/gather_scatter_profiler.h"
#include "zsim.h"
using namespace pimbridge;

GatherScatterProfiler::GatherScatterProfiler(Config& config, const std::string& prefix) {
    futex_init(&lock);
    enableTrace = config.get<bool>(prefix + "enable", false);
    if (enableTrace) {
        pathPrefix = config.get<const char*>(prefix + "pathPrefix");
        this->bwUtil.resize(zinfo->commModules.size());
        this->intervalLength.resize(zinfo->commModules.size());
        uint32_t i = 0;
        for (auto l : zinfo->commModules) {
            this->intervalLength[i].resize(l.size());
            this->bwUtil[i++].resize(l.size());
        }
    }
}

void GatherScatterProfiler::initTransfer(uint32_t level, uint32_t commId) {
    if (!enableTrace) { return; }
    futex_lock(&lock);
    this->bwUtil[level][commId].push_back(GatherScatterProfiler::BwUtilEntry());
    // TBY TODO: only support gather now;
    assert(level > 0);
    uint64_t lastComm = ((CommModule*)(zinfo->commModules[level][commId]))->getLastGatherPhase();
    this->intervalLength[level][commId].push_back(zinfo->numPhases - lastComm);
    futex_unlock(&lock);
}

void GatherScatterProfiler::record(uint32_t level, uint32_t commId, 
                             uint32_t childIdx, uint32_t size) {
    if (!enableTrace) { return; }                            
    futex_lock(&lock);
    this->bwUtil[level][commId].back().recordTransfer(childIdx, size);
    futex_unlock(&lock);
}

void GatherScatterProfiler::toFile() {
    if (!enableTrace) { return; }
    info("Trace begin in %s", pathPrefix.c_str());
    bwUtilToFile();
    info("Trace end");
}

void GatherScatterProfiler::bwUtilToFile() {
    std::ofstream outfile;
    outfile.open((pathPrefix + "-bw.out").c_str(), std::ios::out);
    for (uint32_t level = 1; level < this->bwUtil.size(); ++level) {
        for (uint32_t module = 0; module < bwUtil[level].size(); ++module) {
            outfile << "commModule " << level << "-" << module << std::endl;
            assert(bwUtil[level][module].size() == intervalLength[level][module].size());
            for (uint32_t item = 0; item < bwUtil[level][module].size(); item++){
                outfile << "interval: " << intervalLength[level][module][item];
                auto& e = bwUtil[level][module][item];
                outfile << " bwUtil: " << e.sum << "   ";
                for (uint32_t i = 0; i < e.maxNumChild; ++i) {
                    outfile << e.transferSizes[i] << " ";
                }
                outfile << std::endl;
            }
            outfile << std::endl; 
        }
    }
    outfile.close();
}