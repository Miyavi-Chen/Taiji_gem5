#ifndef __MEM_ADDR_TYPE_HH__
#define __MEM_ADDR_TYPE_HH__
#include "base/statistics.hh"

typedef struct ChannelIdx { size_t val; } ChannelIdx;
typedef struct PhysAddr { Addr val; } PhysAddr;
typedef struct PageAddr { Addr val; } PageAddr;
typedef struct FrameAddr { Addr val; } FrameAddr;


#endif