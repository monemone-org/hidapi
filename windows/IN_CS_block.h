#pragma once

#include "stdafx.h"
#include <functional>

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
