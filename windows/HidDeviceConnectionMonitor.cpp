
#include "stdafx.h"
#include "HidDeviceConnectionMonitor.hpp"
#include "hid_internal.h"
#include "HidDeviceAsyncReadThread.h"

#ifdef __MINGW32__
#include <ntdef.h>
#include <winbase.h>
#define WC_ERR_INVALID_CHARS 0x00000080
#endif

#ifdef __CYGWIN__
#include <ntdef.h>
#include <wctype.h>
#define _wcsdup wcsdup
#endif

#include <initguid.h>
#include <windows.h>
#include <WinUser.h>
#include <dbt.h>
#include <usbiodef.h>
#include <hidsdi.h>

#include <list>
#include <vector>
#include <iterator>
#include <algorithm>
#include <assert.h>

static wchar_t const szWindowClass[] = L"HidDeviceConnectionMonitorHWND";

// WPARAM: WCHAR* devID.  Message handler needs to free(devID)
// LPARAM: 0
//      
UINT const WMAPP_ON_READ_DATA = WM_APP + 1;


LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    HidDeviceConnectionMonitor* pMonitor = (HidDeviceConnectionMonitor*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (pMonitor == NULL)
    {
        return 0;
    }

    return pMonitor->MonitorWndProc(hwnd, message, wParam, lParam);
}

     
BOOL HidDeviceConnectionMonitor::IsInitialized() {
    return m_bInitialized;
}

// clean up all the global variables.  Should be called on the main thread.
void HidDeviceConnectionMonitor::Uninitialize()
{
    AssertMainWindowThread();

    m_bInitialized = FALSE;

    for (auto iter = m_devIdToAsyncReadThreadMap.begin(); iter != m_devIdToAsyncReadThreadMap.end(); ++iter)
    {
        HidDeviceAsyncReadThread* pAsyncReadThread = iter->second;
        pAsyncReadThread->Stop(TRUE);
        delete pAsyncReadThread;
    }
    m_devIdToAsyncReadThreadMap.clear();

    if (m_hDeviceNotify)
    {
        UnregisterDeviceNotification(m_hDeviceNotify);
        m_hDeviceNotify = NULL;
    }

    if (m_pHidEnumOnAddDeviceNotify)
    {
        free(m_pHidEnumOnAddDeviceNotify);
        m_pHidEnumOnAddDeviceNotify = NULL;
    }

    m_monitoringDisconnectionDeviceList.clear();

    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = NULL;
    }

    m_sLastErrMsg.clear();
}

BOOL HidDeviceConnectionMonitor::Initialize()
{
    AssertMainWindowThread();

    if (IsInitialized()) {
        return TRUE;
    }

    RegisterWindowClass(szWindowClass);

    // Create the main window. This could be a hidden window if you don't need
    // any UI other than the notification icon.
    m_hwnd = CreateWindowW(szWindowClass, L"Monitor Window", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 250, 200, NULL, NULL, g_hinstDLL, NULL);
    if (m_hwnd == NULL)
    {
        return FALSE;
    }
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;
    ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
    m_hDeviceNotify = RegisterDeviceNotificationW(
        m_hwnd,                       // events recipient
        &NotificationFilter,        // type of device
        DEVICE_NOTIFY_WINDOW_HANDLE // type of recipient handle
    );
    if (m_hDeviceNotify == NULL)
    {
        return FALSE;
    }

    m_bInitialized = TRUE;
    return TRUE;
}




void HidDeviceConnectionMonitor::RegisterWindowClass(PCWSTR pszClassName)
{
    AssertMainWindowThread();

    WNDCLASSEX wcex = { sizeof(wcex) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = g_hinstDLL;
    wcex.hIcon = NULL;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = pszClassName;
    RegisterClassEx(&wcex);
}

LRESULT HidDeviceConnectionMonitor::MonitorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    AssertMainWindowThread();

    switch (message)
    {
    case WM_CREATE:
        break;

    case WMAPP_ON_READ_DATA:
        {
            WCHAR* pszDevID = (WCHAR *)wParam;
            
            HidDeviceAsyncReadThread* pReadThread = FindMonitoringAsyncReadThreadByDeviceID(pszDevID);
            if (pReadThread)
            {
                hid_device* dev = pReadThread->GetDev();
                if (dev->on_read)
                {
                    unsigned char* data = NULL;
                    size_t cbData = 0;
                    while (pReadThread->PopReadData(&data, &cbData))
                    {
                        dev->on_read(dev, data, cbData);
                        
                        free(data);
                        data = NULL;
                    }
                }
            }

            free(pszDevID);
            break;
        }

    case WM_DEVICECHANGE:
    {
        switch (wParam)
        {
            /*case DBT_DEVNODES_CHANGED:
            {
                ATLTRACE("DBT_DEVNODES_CHANGED");
                break;
            }*/

        case DBT_DEVICEARRIVAL:
        {
            DEV_BROADCAST_HDR* pDevHDR = (DEV_BROADCAST_HDR*)lParam;
            if (pDevHDR->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            {
                DEV_BROADCAST_DEVICEINTERFACE* pDevInterface = (DEV_BROADCAST_DEVICEINTERFACE*)pDevHDR;
                this->OnDBTDeviceArrival(pDevInterface->dbcc_name);
            }

            break;
        }

        case DBT_DEVICEREMOVECOMPLETE:
        {
            DEV_BROADCAST_HDR* pDevHDR = (DEV_BROADCAST_HDR*)lParam;
            if (pDevHDR->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            {
                DEV_BROADCAST_DEVICEINTERFACE* pDevInterface = (DEV_BROADCAST_DEVICEINTERFACE*)pDevHDR;
                this->OnDBTDeviceRemoveComplete(pDevInterface->dbcc_name);
            }
            break;
        }

        default:
        {
            break;
        }

        }
        break;
    }

    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}


/*
* After receiving DBT_DEVICEARRIVAL, notify hid caller via the provided on_added_device function to hid_enumerate().
*/
void HidDeviceConnectionMonitor::OnDBTDeviceArrival(wchar_t* device_interface_name)
{
    AssertMainWindowThread();

    if (m_pHidEnumOnAddDeviceNotify)
    {
        struct hid_device_info* dev_info = create_hid_device_info_from_device_interface_name(device_interface_name,
            m_pHidEnumOnAddDeviceNotify->if_match_vendor_id,
            m_pHidEnumOnAddDeviceNotify->if_match_product_id,
            m_pHidEnumOnAddDeviceNotify->if_match_usage_page,
            m_pHidEnumOnAddDeviceNotify->if_match_usage
            );
        if (dev_info)
        {
            if (m_pHidEnumOnAddDeviceNotify->g_on_added_device)
            {
                m_pHidEnumOnAddDeviceNotify->g_on_added_device(dev_info);
            }
            free(dev_info);
            dev_info = NULL;
        }
    }
}

/*
* After receiving DBT_DEVICEREMOVECOMPLETE from the Windows System, notify the opened hid_device_* 's on_disconnected() function
*/
void HidDeviceConnectionMonitor::OnDBTDeviceRemoveComplete(wchar_t* device_interface_name)
{
    AssertMainWindowThread();

    // on_disconnect should only be accessed through the caller app's main thread 
    // because HidDeviceConnectionMonitor uses WndProc to notify on device disconnection
    //
    struct hid_device_* dev = FindMonitoringDisconnectionDeviceByDeviceInterfaceName(device_interface_name);
    if (dev)
    {
        if (dev->on_disconnected)
        {
            dev->on_disconnected(dev);
        }
    }
}


void HidDeviceConnectionMonitor::StartMonitoringNewDevices(
    unsigned short if_match_vendor_id,
    unsigned short if_match_product_id,
    unsigned short if_match_usage_page,
    unsigned short if_match_usage,
    void (*on_added_device)(struct hid_device_info*))
{
    AssertMainWindowThread();

    if (m_pHidEnumOnAddDeviceNotify == NULL)
    {
        m_pHidEnumOnAddDeviceNotify = (HidEnumOnAddDeviceNotify*)calloc(1, sizeof(HidEnumOnAddDeviceNotify));
        if (m_pHidEnumOnAddDeviceNotify == NULL)
        {
            // out of memory
            SetLastError(L"calloc(1, sizeof(HidEnumOnAddDeviceNotify)) failed");
            return;
        }
    }
    m_pHidEnumOnAddDeviceNotify->if_match_vendor_id = if_match_vendor_id;
    m_pHidEnumOnAddDeviceNotify->if_match_product_id = if_match_product_id;
    m_pHidEnumOnAddDeviceNotify->if_match_usage_page = if_match_usage_page;
    m_pHidEnumOnAddDeviceNotify->if_match_usage = if_match_usage;
        
}

BOOL HidDeviceConnectionMonitor::StartMonitoringDisconnectionForDevice(hid_device* dev)
{
    AssertMainWindowThread();
    m_monitoringDisconnectionDeviceList.push_back(dev);
    return TRUE;
}

void HidDeviceConnectionMonitor::StopMonitoringDisconnectionForDevice(hid_device* dev)
{
    AssertMainWindowThread();
    m_monitoringDisconnectionDeviceList.remove(dev);
}


hid_device* HidDeviceConnectionMonitor::FindMonitoringDisconnectionDeviceByDeviceInterfaceName(wchar_t* device_interface_name)
{
    AssertMainWindowThread();

    if (device_interface_name == NULL)
    {
        return NULL;
    }

    hid_device* ret_dev = NULL;

    for (auto iter = m_monitoringDisconnectionDeviceList.begin();
        iter != m_monitoringDisconnectionDeviceList.end();
        ++iter)
    {
        hid_device* dev = *iter;
        wchar_t* interface_path = hid_internal_UTF8toUTF16(dev->device_info->path);
        if (interface_path)
        {
            BOOL same_path = (wcscmp(interface_path, device_interface_name) == 0);
            free(interface_path);
            interface_path = NULL;

            if (same_path)
            {
                ret_dev = dev;
                break;
            }
        }

    }

    return ret_dev;
}


BOOL HidDeviceConnectionMonitor::StartMonitoringOnReadForDevice(hid_device* dev)
{
    AssertMainWindowThread();

    std::wstring devId = dev->id;

    if (this->m_devIdToAsyncReadThreadMap.find(devId) == this->m_devIdToAsyncReadThreadMap.end())
    {
        this->SetLastError(L"HidDeviceAsyncReadThread already created for hid_device.");
        return FALSE;
    }

    HidDeviceAsyncReadThread* pThread = new HidDeviceAsyncReadThread(dev, this);
    if (pThread == NULL || !pThread->Start())
    {
        this->SetLastError(L"Failed to create HidDeviceAsyncReadThread.");
        return FALSE;
    }

    this->m_devIdToAsyncReadThreadMap[devId] = pThread;
    return TRUE;

}

void HidDeviceConnectionMonitor::StopMonitoringOnReadForDevice(hid_device* dev)
{
    AssertMainWindowThread();

    std::wstring devID = dev->id;
    auto threadIter = this->m_devIdToAsyncReadThreadMap.find(devID);
    if (threadIter != this->m_devIdToAsyncReadThreadMap.end())
    {
        HidDeviceAsyncReadThread* pThread = threadIter->second;
        pThread->Stop(TRUE);
        delete pThread;
        m_devIdToAsyncReadThreadMap.erase(threadIter);
    }
}

HidDeviceAsyncReadThread* HidDeviceConnectionMonitor::FindMonitoringAsyncReadThreadByDeviceID(wchar_t* pszDevID)
{
    AssertMainWindowThread();

    std::wstring devID = pszDevID;

    auto threadIter = this->m_devIdToAsyncReadThreadMap.find(devID);
    if (threadIter != this->m_devIdToAsyncReadThreadMap.end())
    {
        return threadIter->second;
    }
    else
    {
        return NULL;
    }    
}


//IHidDeviceAsyncReadThreadMonitor

// this method should be called from background read threads.
void HidDeviceConnectionMonitor::OnDataRead(const std::wstring& devID)
{
    WCHAR* pszDevID = _wcsdup(devID.c_str());
    PostMessage(m_hwnd, WMAPP_ON_READ_DATA, (WPARAM)pszDevID, 0);
}


