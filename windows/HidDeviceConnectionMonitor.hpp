#pragma once

#include "hidapi.h"
#include <list>
#include <map>
#include <string>

#include "HidDeviceAsyncReadThread.h"

/*
* monitor when a HID device is connected/disconnected to the Windows System
* monitor when there is data to be read for a HID device.
*/
class HidDeviceConnectionMonitor: 
    IHidDeviceAsyncReadThreadMonitor
{
protected:
    BOOL m_bInitialized;
    
    HWND m_hwnd;

    // For monitoring OnDisconnect
    HDEVNOTIFY m_hDeviceNotify;
    typedef struct {
        unsigned short if_match_vendor_id;
        unsigned short if_match_product_id;
        unsigned short if_match_usage_page;
        unsigned short if_match_usage;
        on_added_device_callback_entry on_added_device_callback;
    } HidEnumOnAddDeviceNotify;
    HidEnumOnAddDeviceNotify* m_pHidEnumOnAddDeviceNotify;
    std::list<hid_device*> m_monitoringDisconnectionDeviceList;

    // for Monitoring OnRead
    std::map<std::wstring, HidDeviceAsyncReadThread*> m_devIdToAsyncReadThreadMap;

    std::wstring m_sLastErrMsg;

public:
    HidDeviceConnectionMonitor()
    {
        m_bInitialized = FALSE;
        m_hwnd = NULL;
        m_hDeviceNotify = NULL;
        m_pHidEnumOnAddDeviceNotify = NULL;
    }

    ~HidDeviceConnectionMonitor()
    {
        Uninitialize();
    }

    BOOL Initialize();
    BOOL IsInitialized();
    void Uninitialize();

    BOOL StartMonitoringNewDevices(
        unsigned short if_match_vendor_id,
        unsigned short if_match_product_id,
        unsigned short if_match_usage_page,
        unsigned short if_match_usage,
        on_added_device_callback_entry on_added_device_callback);
    void StopMonitoringNewDevices();

    // hid_c needs to notify HidDeviceConnectionMonitor when a 
    // hid_device is opened/closed, so when it receives DBT_DEVICEREMOVECOMPLETE,
    // it can notify the caller via the registered disconnected callback. 
    BOOL StartMonitoringDisconnectionForDevice(hid_device* dev);
    void StopMonitoringDisconnectionForDevice(hid_device* dev);

    BOOL StartMonitoringOnReadForDevice(hid_device* dev);
    void StopMonitoringOnReadForDevice(hid_device* dev);

    LRESULT MonitorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

protected:
    void RegisterWindowClass(PCWSTR pszClassName);

    // call from WndProc
    void OnDBTDeviceArrival(wchar_t* device_interface_name);
    void OnDBTDeviceRemoveComplete(wchar_t* device_interface_name);

    hid_device* FindMonitoringDisconnectionDeviceByDeviceInterfaceName(wchar_t* device_interface_name);
    HidDeviceAsyncReadThread* FindMonitoringAsyncReadThreadByDeviceID(wchar_t* pszDevID);


    void SetLastError(const WCHAR* errMsg) 
    {
        m_sLastErrMsg = errMsg;
    }

public:
    //IHidDeviceAsyncReadThreadMonitor
    virtual void OnDataRead(const std::wstring& devID);
    virtual void OnDataReadFailure(const std::wstring& devID, const std::wstring& errMsg);
};


