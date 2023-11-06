#include "log.h"

// #define DEBUG_PHASE 1
// #define DEBUG_GATHER_STATE 1
// #define DEBUG_LB 1
// #define DEBUG_TASK_BEHAVIOR 1 // for task enqueue, dequeue, finish
// #define DEBUG_UNIT_BEHAVIOR 1 // for timestamp change, finish, restart
// #define DEBUG_SKETCH 1
// #define DEBUG_SCHED_META 1
// #define DEBUG_UBUNTU 1
// # define DEBUG_ADDR_RETURN 1

// #define DEBUG_CHECK_CORRECT 1

#ifdef DEBUG_PHASE
#define DEBUG_PHASE_O(args...) info(args)
#else
#define DEBUG_PHASE_O(args...) 
#endif

#ifdef DEBUG_GATHER_STATE
#define DEBUG_GATHER_STATE_O(args...) info(args)
#else
#define DEBUG_GATHER_STATE_O(args...) 
#endif

#ifdef DEBUG_LB
#define DEBUG_LB_O(args...) info(args)
#else
#define DEBUG_LB_O(args...) 
#endif

#ifdef DEBUG_TASK_BEHAVIOR
#define DEBUG_TASK_BEHAVIOR_O(args...) info(args)
#else
#define DEBUG_TASK_BEHAVIOR_O(args...)
#endif

#ifdef DEBUG_UNIT_BEHAVIOR
#define DEBUG_UNIT_BEHAVIOR_O(args...) info(args)
#else
#define DEBUG_UNIT_BEHAVIOR_O(args...)
#endif

#ifdef DEBUG_SKETCH
#define DEBUG_SKETCH_O(args...) info(args)
#else
#define DEBUG_SKETCH_O(args...)
#endif

#ifdef DEBUG_SCHED_META
#define DEBUG_SCHED_META_O(args...) info(args)
#else
#define DEBUG_SCHED_META_O(args...) 
#endif

#ifdef DEBUG_UBUNTU
#define DEBUG_UBUNTU_O(args...) info(args)
#else
#define DEBUG_UBUNTU_O(args...) 
#endif

#ifdef DEBUG_ADDR_RETURN
#define DEBUG_ADDR_RETURN_O(args...) info(args)
#else
#define DEBUG_ADDR_RETURN_O(args...) 
#endif



