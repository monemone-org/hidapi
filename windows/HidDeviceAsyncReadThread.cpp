
#include "stdafx.h"
#include "HidDeviceAsyncReadThread.h"

#include <windows.h>

//#include <usbiodef.h>
//#include <hidsdi.h>

//#include <list>
//#include <vector>
//#include <iterator>
//#include <algorithm>
#include <assert.h>

DWORD  CALLBACK ThreadProc(_In_ LPVOID lpParameter)
{
    HidDeviceAsyncReadThread* pThread = (HidDeviceAsyncReadThread*)lpParameter;
    if (pThread == NULL)
    {
        return 0;
    }

    return pThread->AsyncReadThreadProc();
}

// clean up all the global variables.  Should be called on the main thread.
void HidDeviceAsyncReadThread::Stop(BOOL bWait)
{
    AssertMainWindowThread();

    if (m_hReadMonitoringThread)
    {
        if (m_hReadMonitoringThreadExitEvent)
        {
            if (GetReadMonitoringThreadState() == threadState_Running)
            {
                SetReadMonitoringThreadState(threadState_Stopping);
                SetEvent(m_hReadMonitoringThreadExitEvent); //notify read monitoring thread to quit
            }
        }

        if (GetReadMonitoringThreadState() == threadState_Stopping)
        {
            if (bWait)
            {
                WaitForSingleObject(m_hReadMonitoringThread, INFINITE);
            }
            else
            {
                return;
            }
        }

        CloseHandle(m_hReadMonitoringThread);
        m_hReadMonitoringThread = NULL;
    }

    if (m_hReadMonitoringThreadExitEvent)
    {
        CloseHandle(m_hReadMonitoringThreadExitEvent);
        m_hReadMonitoringThreadExitEvent = NULL;
    }

    // free m_readDataList
    FreeAllReadData();

    SetReadMonitoringThreadState(threadState_Stopped);
}

BOOL HidDeviceAsyncReadThread::Start()
{
    AssertMainWindowThread();

    if (GetReadMonitoringThreadState() == threadState_Running) {
        return TRUE;
    }

    Stop(TRUE); //free previously created read monitoring thread resources

    if (!CreatReadMonitoringThread())
    {
        return FALSE;
    }

    if (-1 == ResumeThread(m_hReadMonitoringThread))
    {
        this->SetLastError(L"ResumeThread failed.");
        return FALSE;
    }

    SetReadMonitoringThreadState(threadState_Running);
    
    return TRUE;
}

BOOL HidDeviceAsyncReadThread::CreatReadMonitoringThread()
{
    AssertMainWindowThread();

    m_hReadMonitoringThreadExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (m_hReadMonitoringThreadExitEvent == NULL)
    {
        this->SetLastError(L"CreatEventW failed.");
        return FALSE;
    }

    m_hReadMonitoringThread = CreateThread(
        NULL,               //[in, optional]  LPSECURITY_ATTRIBUTES   lpThreadAttributes,
        0,                  //[in]            SIZE_T                  dwStackSize,
        ThreadProc,         //[in]            LPTHREAD_START_ROUTINE  lpStartAddress,
        this,               //[in, optional]  __drv_aliasesMem LPVOID lpParameter,
        CREATE_SUSPENDED,   //[in]            DWORD                   dwCreationFlags,
        NULL                //[out, optional] LPDWORD                 lpThreadId
    );
    if (m_hReadMonitoringThread == NULL)
    {
        this->SetLastError(L"CreateThread failed.");
        return FALSE;
    }

    return TRUE;
}

void HidDeviceAsyncReadThread::FreeAllReadData()
{
    AssertMainWindowThread();

    std::list<ReadDataRecord*> listCopy;

    {
        CSLock lock(m_cs);
        listCopy = m_readDataList;
        m_readDataList.clear();
    }

    for (auto iterReadData = listCopy.begin();
        iterReadData != listCopy.end();
        ++iterReadData)
    {
        ReadDataRecord* pReadData = *iterReadData;
        delete pReadData;
    }

}

BOOL HidDeviceAsyncReadThread::PushReadData(unsigned char* data, size_t cbData)
{
    ReadDataRecord* pReadData = (ReadDataRecord*)calloc(1, sizeof(ReadDataRecord));
    if (pReadData == NULL)
    {
        this->SetLastError(L"calloc failed.");
        return NULL;
    }
    pReadData->data = data;
    pReadData->cbData = cbData;
    
    {
        CSLock lock(m_cs);
        if (GetReadMonitoringThreadState() == threadState_Running)
        {
            m_readDataList.push_back(pReadData);
        }
    }
    return TRUE;
}

/**
* Callers need to free the returned pData using free(data)
*/
BOOL HidDeviceAsyncReadThread::PopReadData(__out unsigned char** pData, __out size_t* pcbData)
{
    AssertMainWindowThread();

    *pData = NULL;
    *pcbData = 0;

    ReadDataRecord* pReadData = NULL;

    {
        CSLock lock(m_cs);

        if (m_readDataList.empty())
        {
            return FALSE;
        }

        pReadData = m_readDataList.front();
        m_readDataList.pop_front();
    }

    //transfer the ownership of data to the caller.
    //The caller needs to call free() to free the data memory block.
    *pData = pReadData->data;    
    pReadData->data = NULL;
    *pcbData = pReadData->cbData;

    delete pReadData;

    return TRUE;
}

DWORD HidDeviceAsyncReadThread::AsyncReadThreadProc()
{
    const size_t waitObjectsCount = 2;
    const int exitEventIndex = 0;
    const int devEventIndex = 1;
    HANDLE waitObjects[2] = {
        m_hReadMonitoringThreadExitEvent,
        m_dev->ol.hEvent
    };

    auto copy_and_notify_read_buf = [this]() -> bool 
    {
        if (!hid_internal_dev_async_read_pending_data(m_dev))
        {
            return false;
        }
       
        size_t cbData = m_dev->input_report_length;
        unsigned char* data = (unsigned char*)calloc(cbData, 1);
        if (data == NULL)
        {
            SetLastError(L"calloc failed. Out of memory.");
            return false;
        }

        cbData = hid_internal_dev_copy_read_buf(m_dev, data, cbData);
        this->PushReadData(data, cbData);
        //Notify that new data has been read.
        m_pReadThreadMonitor->OnDataRead(m_devID);

        return true;
    };

    while (GetReadMonitoringThreadState() == threadState_Running)
    {
        hid_device_read_ret read_ret = hid_internal_dev_async_read_nowait(m_dev);
        switch (read_ret)
        {
            case hid_device_read_failed:
                SetLastError(L"hid_internal_dev_async_read_nowait failed.");
                continue;
                break;

            case hid_device_read_succeeded:
                if (!copy_and_notify_read_buf())
                {
                    //return -1;
                    continue;
                }
                break;

            case hid_device_read_pending:
                // Wait for a client to connect, or for a read or write 
                // operation to be completed, which causes a completion 
                // routine to be queued for execution. 
                DWORD dwWait = WaitForMultipleObjectsEx(
                    waitObjectsCount,
                    waitObjects,    // event objects to wait for 
                    FALSE,          // wait all
                    INFINITE,       // waits indefinitely 
                    TRUE);          // alertable wait enabled 

                if (dwWait == exitEventIndex + WAIT_OBJECT_0)
                {
                    CancelIo(m_dev->device_handle);
#ifdef DEBUG
                    {
                        auto state = GetReadMonitoringThreadState();
                        assert(state == threadState_Stopping);
                    }
#endif
                }
                else if (dwWait == devEventIndex + WAIT_OBJECT_0)
                {
                    if (!copy_and_notify_read_buf())
                    {
                        //return -1;
                        continue;
                    }
                }
                else
                {
                    SetWinApiLastError(L"WaitForMultipleObjectsEx");
                    //return -1;
                    continue;
                }

                break;

        }
    }

    return 0;
}

