#include "log.h"

// #define DEBUG_PHASE 1
// #define DEBUG_LB_OUTPUT 1
// #define DEBUG_GATHER_STATE 1
// #define DEBUG_TASK_BEHAVIOR 1
// #define DEBUG_UNIT_BEHAVIOR 1
// #define DEBUG_SKETCH 1
// #define DEBUG_SCHED_META 1

#ifdef DEBUG_PHASE
#define DEBUG_PHASE_O(args...) info(args)
#else
#define DEBUG_PHASE_O(args...) 
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


