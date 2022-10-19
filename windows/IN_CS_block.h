#pragma once

#include "stdafx.h"
#include <functional>

//template<typename T>
//T IN_CS_blockT(CRITICAL_SECTION& cs, std::function<T(void)>  block)
//{
//    T ret;
//    EnterCriticalSection(&cs);
//    ret = block();
//    LeaveCriticalSection(&cs);
//    return ret;
//}
//
//inline
//void IN_CS_block(CRITICAL_SECTION& cs, std::function<void(void)>  block)
//{
//    EnterCriticalSection(&cs);
//    block();
//    LeaveCriticalSection(&cs);
//}
//

struct CSLock
{
    CRITICAL_SECTION& cs;

    CSLock(CRITICAL_SECTION& cs_) :
        cs(cs_)
    {
        EnterCriticalSection(&cs);
    }

    ~CSLock()
    {
        LeaveCriticalSection(&cs);
    }
};
