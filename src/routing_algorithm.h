#ifndef ROUTING_ALGORITHM_H_
#define ROUTING_ALGORITHM_H_

#include "bithacks.h"
#include "g_std/g_vector.h"
#include "log.h"

class RoutingAlgorithm : public GlobAlloc {
    /** The first N routers are terminal routers if there are N terminals. */
    public:
        virtual uint32_t getNumTerminals() const = 0;
        virtual uint32_t getNumRouters() const = 0;
        virtual uint32_t getNumPorts() const = 0;
        virtual uint32_t getCenterRouterId() const = 0;
        virtual void nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) = 0;
};

/**
 * Direct connections between all N routers.
 *
 * Each router has N ports. Number the port same to the connected router ID.
 */
class DirectRoutingAlgorithm : public RoutingAlgorithm {
    private:
        const uint32_t numRouters;

    public:
        DirectRoutingAlgorithm(uint32_t _numRouters) : numRouters(_numRouters) {}

        uint32_t getNumTerminals() const { return numRouters; }

        uint32_t getNumRouters() const { return numRouters; }

        uint32_t getNumPorts() const { return numRouters; }

        uint32_t getCenterRouterId() const { return 0;  /* All routers are equal. */ }

        void nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) {
            *nextId = destinationId;
            *portId = destinationId;
        }
};

/**
 * Local-only routing.
 *
 * Same as direct routing, with an additional sanity check that only allows local connections.
 */
class LocalRoutingAlgorithm : public RoutingAlgorithm {
    private:
        const uint32_t numRouters;

    public:
        LocalRoutingAlgorithm(uint32_t _numRouters) : numRouters(_numRouters) {}

        uint32_t getNumTerminals() const { return numRouters; }

        uint32_t getNumRouters() const { return numRouters; }

        uint32_t getNumPorts() const { return 1; /* Local port only. */ }

        uint32_t getCenterRouterId() const { return 0;  /* All routers are equal. */ }

        void nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) {
            if (currentId != destinationId)
                panic("LocalRoutingAlgorithm: attempted to go remote (%u to %u). Maybe inconsistent address mapping?",
                        currentId, destinationId);
            *nextId = destinationId;
            *portId = 0;
        }
};

/**
 * Dimension-order (X -> Y) routing for 2D mesh network.
 *
 * Each router has 5 ports: E, W, N, S, local. X increases in E, Y increases in N.
 */
class Mesh2DDimensionOrderRoutingAlgorithm : public RoutingAlgorithm {
    private:
        const uint32_t dimX;
        const uint32_t dimY;

    public:
        const uint32_t PortE = 0;
        const uint32_t PortW = 1;
        const uint32_t PortN = 2;
        const uint32_t PortS = 3;
        const uint32_t PortL = 4;
        const uint32_t PortNum = 5;

    public:
        Mesh2DDimensionOrderRoutingAlgorithm(uint32_t _dimX, uint32_t _dimY)
            : dimX(_dimX), dimY(_dimY) {}

        uint32_t getNumTerminals() const { return dimX * dimY; }

        uint32_t getNumRouters() const { return dimX * dimY; }

        uint32_t getNumPorts() const { return PortNum; }

        uint32_t getCenterRouterId() const { return (dimX / 2) * dimY + (dimY / 2); }

        void nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) {
            auto curX = currentId / dimY;
            auto curY = currentId % dimY;
            auto dstX = destinationId / dimY;
            auto dstY = destinationId % dimY;
            assert(curX < dimX);
            assert(dstX < dimX);
            uint32_t nextX = curX;
            uint32_t nextY = curY;
            if (curX != dstX) {
                // Route on X.
                nextX = (curX > dstX) ? curX - 1 : curX + 1;
                *portId = (curX > dstX) ? PortW : PortE;
            } else {
                // Route on Y.
                nextY = (curY > dstY) ? curY - 1 : curY + 1;
                *portId = (curY > dstY) ? PortS : PortN;
            }
            assert(nextX < dimX);
            assert(nextY < dimY);
            *nextId = nextX * dimY + nextY;
        }
};

/**
 * Star-topology routing, with \c chains chains, each of which has length \c length.
 *
 * Number from inner to outer along each chain, then turn to the next chain.
 *
 * Each router has 3 ports: in, out, local. An additional router at the center, numbered the last, which has \c chains ports.
 */
class StarRoutingAlgorithm : public RoutingAlgorithm {
    private:
        const uint32_t chains;
        const uint32_t length;

    public:
        const uint32_t PortI = 0;
        const uint32_t PortO = 1;
        const uint32_t PortL = 2;
        const uint32_t PortNum = 3;

    public:
        StarRoutingAlgorithm(uint32_t _chains, uint32_t _length)
            : chains(_chains), length(_length) {}

        uint32_t getNumTerminals() const { return chains * length; }

        uint32_t getNumRouters() const { return chains * length + 1; }

        uint32_t getNumPorts() const { return MAX(PortNum, chains); }

        uint32_t getCenterRouterId() const { return chains * length; }

        void nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId) {
            uint32_t dstChain = destinationId / length;
            if (currentId == getCenterRouterId()) {
                // At the center router, go to the destination chain.
                assert(dstChain < chains);
                *nextId = dstChain * length;
                *portId = dstChain;
                return;
            }
            uint32_t curChain = currentId / length;
            uint32_t curDist = currentId % length;
            if (curChain == dstChain) {
                // At the same chain, go in or out.
                uint32_t dstDist = destinationId % length;
                if (curDist > dstDist) {
                    // Go in.
                    *nextId = currentId - 1;
                    *portId = PortI;
                } else {
                    assert(curDist < dstDist);
                    // Go out.
                    *nextId = currentId + 1;
                    *portId = PortO;
                }
            } else {
                // At different chains, or destination at the center, go in.
                *nextId = curDist == 0 ? getCenterRouterId() : currentId - 1;
                *portId = PortI;
            }
        }
};

/**
 * Tree-topology routing, with the given number of nodes at each level from leaf to root.
 *
 * Terminals may reside only at the leaf level, or at all nodes across the tree. The terminals are numbered by levels
 * from leaf to root.
 *
 * If there are more than one routers at the root (maximum) level, they will be directly fully connected. The center
 * router is the middle one at the root.
 */
class TreeRoutingAlgorithm : public RoutingAlgorithm {
    public:
        // Notice level sizes are given from leaf to root.
        TreeRoutingAlgorithm(const g_vector<uint32_t>& levelSizes, bool onlyLeafTerminals);

        uint32_t getNumTerminals() const { return numTerminals; }

        uint32_t getNumRouters() const { return numRouters; }

        uint32_t getNumPorts() const { return rootNumRouters + maxFanout; }

        uint32_t getCenterRouterId() const { return rootNumRouters / 2; }

        void nextHop(uint32_t currentId, uint32_t destinationId, uint32_t* nextId, uint32_t* portId);

    private:
        struct RouterTuple {
            uint32_t level;
            uint32_t parentId;
            uint32_t firstChildId;

            RouterTuple(uint32_t _level, uint32_t _parentId, uint32_t _firstChildId)
                : level(_level), parentId(_parentId), firstChildId(_firstChildId) {}
        };
        g_vector<RouterTuple> tuples;

        uint32_t numRouters;
        uint32_t numTerminals;

        uint32_t rootLevel;
        uint32_t rootNumRouters;

        uint32_t maxFanout;

    private:
        // Uptrace to the common ancestor of the two.
        uint32_t uptrace(uint32_t& id1, uint32_t& id2) const;

        /* Ports.
         *
         * The first few ports are for up (non-root) or horizontal (root) transfers, with the number equal to MAX(1,
         * rootNumRouters) = rootNumRouters.
         *
         * The remaining ports are for down transfers, with the number equal to maxFanout.
         */

        static const uint32_t PortUp = 0;

        uint32_t portHorizontal(uint32_t i) const {
            assert(i < rootNumRouters);
            return i;
        }

        uint32_t portDown(uint32_t i) const {
            assert(i < maxFanout);
            return rootNumRouters + i;
        }
};

#endif  // ROUTING_ALGORITHM_H_

