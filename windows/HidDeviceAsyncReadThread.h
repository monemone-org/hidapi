#pragma once

//#include "stdafx.h"
#include "IN_CS_block.h"
#include <string>
#include "hid_internal.h"

typedef struct ReadDataRecord {

    unsigned char* data;
    size_t cbData;

    ReadDataRecord(unsigned char* data_, size_t cbData_) :
        data(data_),
        cbData(cbData_)
    {
    }

    ~ReadDataRecord()
    {
        if (data) 
        {
            free(data);
        }
    }
} ReadDataRecord;

// interface
class IHidDeviceAsyncReadThreadMonitor
{
public:
    virtual void OnDataRead(const std::wstring& devID) = 0;
};

class HidDeviceAsyncReadThread
{
private:
    CRITICAL_SECTION m_cs;
    
    // use for dev lookup in multi-thread situation, in case dev is already freed.
    std::wstring m_devID;
    hid_device* m_dev;

    IHidDeviceAsyncReadThreadMonitor* m_pReadThreadMonitor;

    enum ReadMonitoringThreadState
    {
        threadState_Null,
        threadState_Running,
        threadState_Stopping,
        threadState_Stopped
    };
    ReadMonitoringThreadState m_readMonitoringThreadState;
    HANDLE m_hReadMonitoringThread;
    HANDLE m_hReadMonitoringThreadExitEvent;

    std::wstring m_sLastErrMsg;

    std::list<ReadDataRecord*> m_readDataList;

public:
    HidDeviceAsyncReadThread(hid_device* dev, IHidDeviceAsyncReadThreadMonitor* pReadThreadMonitor)
    {
        m_pReadThreadMonitor = pReadThreadMonitor;
        m_dev = dev;
        m_devID = dev->id; // use for dev lookup, in case dev is already freed.
        m_readMonitoringThreadState = threadState_Null;
        m_hReadMonitoringThread = NULL;
        m_hReadMonitoringThreadExitEvent = NULL;
        InitializeCriticalSection(&m_cs);
    }

    ~HidDeviceAsyncReadThread()
    {
        Stop(TRUE);

        DeleteCriticalSection(&m_cs);
    }

    BOOL Start();
    void Stop(BOOL bWait);

    HANDLE GetThreadHandle() const {
        return this->m_hReadMonitoringThread;
    }
    const std::wstring& GetDevID() const {
        return this->m_devID;
    }
    hid_device* GetDev() const {
        return this->m_dev;
    }

    BOOL PushReadData(unsigned char* data, size_t cbData);
    /**
    * Callers need to free the returned pData using free(data)
    */
    BOOL PopReadData(__out unsigned char** pData, __out size_t* pcbData);

    DWORD AsyncReadThreadProc();

protected:

    ReadMonitoringThreadState GetReadMonitoringThreadState() 
    {
        CSLock lock(m_cs);
        return m_readMonitoringThreadState;
    }
    void SetReadMonitoringThreadState(ReadMonitoringThreadState newValue)
    {
        CSLock lock(m_cs);
        m_readMonitoringThreadState = newValue;
    }
    BOOL CreatReadMonitoringThread();

    void FreeAllReadData();

    void SetLastError(const WCHAR* errMsg)
    {
        CSLock lock(m_cs);
        m_sLastErrMsg = errMsg;
    }

    void SetWinApiLastError(const WCHAR* operation)
    {
        WCHAR* errMsg = NULL;
        register_winapi_error_to_buffer(&errMsg, operation);

        {
            CSLock lock(m_cs);
            m_sLastErrMsg = errMsg;
        }

        free(errMsg);
    }
};


