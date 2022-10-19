/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022, All Rights Reserved.

 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU General Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
        https://github.com/libusb/hidapi .
********************************************************/

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
/* Do not warn about wcsncpy usage.
   https://docs.microsoft.com/cpp/c-runtime-library/security-features-in-the-crt */
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "stdafx.h"

//#ifdef __cplusplus
//extern "C" {
//#endif

#include "hidapi.h"
#include "hid_internal.h"
#include "HidDeviceConnectionMonitor.hpp"
#include "HidDeviceAsyncReadThread.h"
#include "HidD_proxy.h"

#include <windows.h>

//#ifndef _NTDEF_
//typedef LONG NTSTATUS;
//#endif

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

/*#define HIDAPI_USE_DDK*/

#include "hidapi_hidclass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MIN
#undef MIN
#endif
#define MIN(x,y) ((x) < (y)? (x): (y))

/* MAXIMUM_USB_STRING_LENGTH from usbspec.h is 255 */
/* BLUETOOTH_DEVICE_NAME_SIZE from bluetoothapis.h is 256 */
#define MAX_STRING_WCHARS 256

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

static BOOLEAN hidapi_initialized = FALSE;

static HidDeviceConnectionMonitor g_DeviceMonitor;

static hid_device *new_hid_device()
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	if (dev == NULL)
	{
		register_string_error(NULL, L"calloc");
		return NULL;
	}
	
	GUID guid = { 0 };
	HRESULT hr = CoCreateGuid(&guid);
	if (FAILED(hr))
	{
		register_winapi_error(NULL, L"CoCreateGuid");
		return NULL;
	}
	WCHAR* pszGuid = NULL;
	hr = StringFromCLSID(guid, &pszGuid);
	if (FAILED(hr))
	{
		register_winapi_error(NULL, L"StringFromCLSID");
		return NULL;
	}
	dev->id = pszGuid;

	dev->device_handle = INVALID_HANDLE_VALUE;
	dev->blocking = TRUE;
	dev->output_report_length = 0;
	dev->write_buf = NULL;
	dev->input_report_length = 0;
	dev->feature_report_length = 0;
	dev->feature_buf = NULL;
	dev->last_error_str = NULL;
	dev->read_pending = FALSE;
	dev->read_buf = NULL;
	dev->read_buf_bytes_read = 0;
	memset(&dev->ol, 0, sizeof(dev->ol));
	dev->ol.hEvent = CreateEvent(NULL, FALSE, FALSE /*initial state f=nonsignaled*/, NULL);
	if (dev->ol.hEvent == NULL)
	{
		register_winapi_error(NULL, L"CreateEvent");
		return NULL;
	}
	memset(&dev->write_ol, 0, sizeof(dev->write_ol));
	dev->write_ol.hEvent = CreateEvent(NULL, FALSE, FALSE /*inital state f=nonsignaled*/, NULL);
	if (dev->write_ol.hEvent == NULL)
	{
		register_winapi_error(NULL, L"CreateEvent");
		return NULL;
	}
	dev->on_read = NULL;
	dev->on_disconnected = NULL;
	dev->device_info = NULL;

	return dev;
}

static void free_hid_device(hid_device *dev)
{
	if (dev->id)
	{
		CoTaskMemFree(dev->id);
	}
	CloseHandle(dev->ol.hEvent);
	CloseHandle(dev->write_ol.hEvent);
	CloseHandle(dev->device_handle);
	free(dev->last_error_str);
	dev->last_error_str = NULL;
	free(dev->write_buf);
	free(dev->feature_buf);
	free(dev->read_buf);
	hid_free_enumeration(dev->device_info);
	free(dev);

}


static on_read_callback hid_device_get_on_read(hid_device *dev)
{
	return dev->on_read;
}

static void hid_device_set_on_read(hid_device *dev, on_read_callback on_read)
{
	if (on_read == hid_device_get_on_read(dev))
	{
		return;
	}

	dev->on_read = on_read;
	if (on_read)
	{
		g_DeviceMonitor.StartMonitoringOnReadForDevice(dev);
	}
	else
	{
		g_DeviceMonitor.StopMonitoringOnReadForDevice(dev);
	}
	
}

static on_disconnected_callback hid_device_get_on_disconnect(hid_device *dev)
{
	return dev->on_disconnected;
}

static void hid_device_set_on_disconnected(hid_device *dev, on_disconnected_callback on_disconnected)
{
	dev->on_disconnected = on_disconnected;
	if (on_disconnected)
	{
		g_DeviceMonitor.StartMonitoringDisconnectionForDevice(dev);
	}
	else
	{
		g_DeviceMonitor.StopMonitoringDisconnectionForDevice(dev);
	}
}



void register_winapi_error(hid_device *dev, const WCHAR *op)
{
	if (!dev)
	{
		wchar_t* global_error = NULL;
		register_winapi_error_to_buffer(&global_error, op);
		mone_set_global_error(global_error);
		free(global_error);		
		return;
	}

	register_winapi_error_to_buffer(&dev->last_error_str, op);
}

static void register_string_error_to_buffer(wchar_t **error_buffer, const WCHAR *string_error)
{
	if (!error_buffer)
		return;

	if (*error_buffer)
	{
		free(*error_buffer);
		*error_buffer = NULL;
	}

	if (string_error) {
		*error_buffer = _wcsdup(string_error);
	}
}

void register_string_error(hid_device *dev, const WCHAR *string_error)
{
	if (!dev)
	{
		mone_set_global_error(string_error);
		return;
	}

	register_string_error_to_buffer(&dev->last_error_str, string_error);
}

HANDLE open_device(const wchar_t *path, BOOL open_rw)
{
	HANDLE handle;
	DWORD desired_access = (open_rw)? (GENERIC_WRITE | GENERIC_READ): 0;
	DWORD share_mode = FILE_SHARE_READ|FILE_SHARE_WRITE;

	handle = CreateFileW(path,
		desired_access,
		share_mode,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,/*FILE_ATTRIBUTE_NORMAL,*/
		0);

	return handle;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version()
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str()
{
	return HID_API_VERSION_STR;
}

int HID_API_EXPORT hid_init(void)
{
#ifndef HIDAPI_USE_DDK
	if (!hidapi_initialized) {
		if (lookup_functions() < 0) {
			return -1;
		}
		hidapi_initialized = TRUE;
	}
#endif
	return 0;
}

int HID_API_EXPORT hid_exit(void)
{
#ifndef HIDAPI_USE_DDK
	free_library_handles();
	hidapi_initialized = FALSE;
#endif
	return 0;
}

static void* hid_internal_get_devnode_property(DEVINST dev_node, const DEVPROPKEY* property_key, DEVPROPTYPE expected_property_type)
{
	ULONG len = 0;
	CONFIGRET cr;
	DEVPROPTYPE property_type;
	PBYTE property_value = NULL;

	cr = CM_Get_DevNode_PropertyW(dev_node, property_key, &property_type, NULL, &len, 0);
	if (cr != CR_BUFFER_SMALL || property_type != expected_property_type)
		return NULL;

	property_value = (PBYTE)calloc(len, sizeof(BYTE));
	cr = CM_Get_DevNode_PropertyW(dev_node, property_key, &property_type, property_value, &len, 0);
	if (cr != CR_SUCCESS) {
		free(property_value);
		return NULL;
	}

	return property_value;
}

static void* hid_internal_get_device_interface_property(const wchar_t* interface_path, const DEVPROPKEY* property_key, DEVPROPTYPE expected_property_type)
{
	ULONG len = 0;
	CONFIGRET cr;
	DEVPROPTYPE property_type;
	PBYTE property_value = NULL;

	cr = CM_Get_Device_Interface_PropertyW(interface_path, property_key, &property_type, NULL, &len, 0);
	if (cr != CR_BUFFER_SMALL || property_type != expected_property_type)
		return NULL;

	property_value = (PBYTE)calloc(len, sizeof(BYTE));
	cr = CM_Get_Device_Interface_PropertyW(interface_path, property_key, &property_type, property_value, &len, 0);
	if (cr != CR_SUCCESS) {
		free(property_value);
		return NULL;
	}

	return property_value;
}

static void hid_internal_get_ble_info(struct hid_device_info* dev, DEVINST dev_node)
{
	wchar_t *manufacturer_string, *serial_number, *product_string;
	/* Manufacturer String */
	manufacturer_string = (wchar_t*)hid_internal_get_devnode_property(dev_node, (const DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_Manufacturer, DEVPROP_TYPE_STRING);
	if (manufacturer_string) {
		free(dev->manufacturer_string);
		dev->manufacturer_string = manufacturer_string;
	}

	/* Serial Number String (MAC Address) */
	serial_number = (wchar_t*)hid_internal_get_devnode_property(dev_node, (const DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_DeviceAddress, DEVPROP_TYPE_STRING);
	if (serial_number) {
		free(dev->serial_number);
		dev->serial_number = serial_number;
	}

	/* Get devnode grandparent to reach out Bluetooth LE device node */
	if (CM_Get_Parent(&dev_node, dev_node, 0) != CR_SUCCESS)
		return;

	/* Product String */
	product_string = (wchar_t*)hid_internal_get_devnode_property(dev_node, &DEVPKEY_NAME, DEVPROP_TYPE_STRING);
	if (product_string) {
		free(dev->product_string);
		dev->product_string = product_string;
	}
}

/* USB Device Interface Number.
   It can be parsed out of the Hardware ID if a USB device is has multiple interfaces (composite device).
   See https://docs.microsoft.com/windows-hardware/drivers/hid/hidclass-hardware-ids-for-top-level-collections
   and https://docs.microsoft.com/windows-hardware/drivers/install/standard-usb-identifiers

   hardware_id is always expected to be uppercase.
*/
static int hid_internal_get_interface_number(const wchar_t* hardware_id)
{
	int interface_number;
	const wchar_t* startptr;
	wchar_t* endptr;
	const wchar_t *interface_token = L"&MI_";

	startptr = wcsstr(hardware_id, interface_token);
	if (!startptr)
		return -1;

	startptr += wcslen(interface_token);
	interface_number = wcstol(startptr, &endptr, 16);
	if (endptr == startptr)
		return -1;

	return interface_number;
}

static void hid_internal_get_info(const wchar_t* interface_path, struct hid_device_info* dev)
{
	wchar_t *device_id = NULL, *compatible_ids = NULL, *hardware_ids = NULL;
	CONFIGRET cr;
	DEVINST dev_node;

	/* Get the device id from interface path */
	device_id = (wchar_t*)hid_internal_get_device_interface_property(interface_path, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
	if (!device_id)
		goto end;

	/* Open devnode from device id */
	cr = CM_Locate_DevNodeW(&dev_node, (DEVINSTID_W)device_id, CM_LOCATE_DEVNODE_NORMAL);
	if (cr != CR_SUCCESS)
		goto end;

	/* Get the hardware ids from devnode */
	hardware_ids = (wchar_t*)hid_internal_get_devnode_property(dev_node, &DEVPKEY_Device_HardwareIds, DEVPROP_TYPE_STRING_LIST);
	if (!hardware_ids)
		goto end;

	/* Search for interface number in hardware ids */
	for (wchar_t* hardware_id = hardware_ids; *hardware_id; hardware_id += wcslen(hardware_id) + 1) {
		/* Normalize to upper case */
		for (wchar_t* p = hardware_id; *p; ++p) *p = towupper(*p);

		dev->interface_number = hid_internal_get_interface_number(hardware_id);

		if (dev->interface_number != -1)
			break;
	}

	/* Get devnode parent */
	cr = CM_Get_Parent(&dev_node, dev_node, 0);
	if (cr != CR_SUCCESS)
		goto end;

	/* Get the compatible ids from parent devnode */
	compatible_ids = (wchar_t*)hid_internal_get_devnode_property(dev_node, &DEVPKEY_Device_CompatibleIds, DEVPROP_TYPE_STRING_LIST);
	if (!compatible_ids)
		goto end;

	/* Now we can parse parent's compatible IDs to find out the device bus type */
	for (wchar_t* compatible_id = compatible_ids; *compatible_id; compatible_id += wcslen(compatible_id) + 1) {
		/* Normalize to upper case */
		for (wchar_t* p = compatible_id; *p; ++p) *p = towupper(*p);

		/* Bluetooth LE devices */
		if (wcsstr(compatible_id, L"BTHLEDEVICE") != NULL) {
			/* HidD_GetProductString/HidD_GetManufacturerString/HidD_GetSerialNumberString is not working for BLE HID devices
			   Request this info via dev node properties instead.
			   https://docs.microsoft.com/answers/questions/401236/hidd-getproductstring-with-ble-hid-device.html */
			hid_internal_get_ble_info(dev, dev_node);
			break;
		}
	}
end:
	free(device_id);
	free(hardware_ids);
	free(compatible_ids);
}

static char *hid_internal_UTF16toUTF8(const wchar_t *src)
{
	char *dst = NULL;
	int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, NULL, 0, NULL, NULL);
	if (len) {
		dst = (char*)calloc(len, sizeof(char));
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst, len, NULL, NULL);
	}

	return dst;
}

wchar_t *hid_internal_UTF8toUTF16(const char *src)
{
	wchar_t *dst = NULL;
	int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
	if (len) {
		dst = (wchar_t*)calloc(len, sizeof(wchar_t));
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, len);
	}

	return dst;
}

struct hid_device_info *hid_internal_get_device_info(const wchar_t *path, HANDLE handle)
{
	struct hid_device_info *dev = NULL; /* return object */
	HIDD_ATTRIBUTES attrib = { 0 };
	PHIDP_PREPARSED_DATA pp_data = NULL;
	HIDP_CAPS caps;
	wchar_t string[MAX_STRING_WCHARS] = { 0 };

	/* Create the record. */
	dev = (struct hid_device_info*)calloc(1, sizeof(struct hid_device_info));
	if (dev == NULL)
	{
		register_string_error(NULL, L"calloc(1, sizeof(struct hid_device_info)) failed");
		return NULL;
	}

	/* Fill out the record */
	dev->next = NULL;
	dev->path = hid_internal_UTF16toUTF8(path);

	attrib.Size = sizeof(HIDD_ATTRIBUTES);
	if (HidD_GetAttributes(handle, &attrib)) {
		/* VID/PID */
		dev->vendor_id = attrib.VendorID;
		dev->product_id = attrib.ProductID;

		/* Release Number */
		dev->release_number = attrib.VersionNumber;
	}

	/* Get the Usage Page and Usage for this device. */
	if (HidD_GetPreparsedData(handle, &pp_data)) {
		if (HidP_GetCaps(pp_data, &caps) == HIDP_STATUS_SUCCESS) {
			dev->usage_page = caps.UsagePage;
			dev->usage = caps.Usage;
		}

		HidD_FreePreparsedData(pp_data);
	}

	/* Serial Number */
	string[0] = L'\0';
	HidD_GetSerialNumberString(handle, string, sizeof(string));
	string[MAX_STRING_WCHARS - 1] = L'\0';
	dev->serial_number = _wcsdup(string);

	/* Manufacturer String */
	string[0] = L'\0';
	HidD_GetManufacturerString(handle, string, sizeof(string));
	string[MAX_STRING_WCHARS - 1] = L'\0';
	dev->manufacturer_string = _wcsdup(string);

	/* Product String */
	string[0] = L'\0';
	HidD_GetProductString(handle, string, sizeof(string));
	string[MAX_STRING_WCHARS - 1] = L'\0';
	dev->product_string = _wcsdup(string);

	hid_internal_get_info(path, dev);

	return dev;
}

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
    return hid_enumerate_ex(vendor_id, product_id, 0, 0, NULL);
}

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate_ex(unsigned short vendor_id,
                                                                      unsigned short product_id,
                                                                      unsigned short usage_page,
                                                                      unsigned short usage,
                                                                      void (*on_added_device)(struct hid_device_info *))
{
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;
	GUID interface_class_guid;
	CONFIGRET cr;
	wchar_t* device_interface_list = NULL;
	DWORD len;

	if (hid_init() < 0)
		return NULL;

	/* Retrieve HID Interface Class GUID
	   https://docs.microsoft.com/windows-hardware/drivers/install/guid-devinterface-hid */
	HidD_GetHidGuid(&interface_class_guid);

	if (on_added_device != NULL)
	{
		//Mone: create temp top level HWND to listen to new devices

	}

	/* Get the list of all device interfaces belonging to the HID class. */
	/* Retry in case of list was changed between calls to
	  CM_Get_Device_Interface_List_SizeW and CM_Get_Device_Interface_ListW */
	do {
		cr = CM_Get_Device_Interface_List_SizeW(&len, &interface_class_guid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
		if (cr != CR_SUCCESS) {
			break;
		}

//		if (device_interface_list != NULL) {
//			free(device_interface_list);
//		}

		device_interface_list = (wchar_t*)calloc(len, sizeof(wchar_t));
		if (device_interface_list == NULL) {
			return NULL;
		}
		cr = CM_Get_Device_Interface_ListW(&interface_class_guid, NULL, device_interface_list, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
	} while (cr == CR_BUFFER_SMALL);

	if (cr != CR_SUCCESS) {
		goto end_of_function;
	}

	/* Iterate over each device interface in the HID class, looking for the right one. */
	for (wchar_t* device_interface = device_interface_list; *device_interface; device_interface += wcslen(device_interface) + 1) {

		struct hid_device_info* tmp = create_hid_device_info_from_device_interface_name(
			device_interface,
			vendor_id,
			product_id,
			usage_page,
			usage);

		if (tmp != NULL) 
		{
			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;
		}
	}

end_of_function:
	free(device_interface_list);

	return root;
}

void  HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info *devs)
{
	/* TODO: Merge this with the Linux version. This function is platform-independent. */
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}

HID_API_EXPORT hid_device * HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	/* TODO: Merge this functions with the Linux version. This function should be platform independent. */
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;

	devs = hid_enumerate(vendor_id, product_id);
	if (!devs) {
		return NULL;
	}

	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (cur_dev->serial_number && wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* Open the device */
		handle = hid_open_path(path_to_open);
	}

	hid_free_enumeration(devs);

	return handle;
}

HID_API_EXPORT hid_device * HID_API_CALL hid_open_path(const char *path)
{
	hid_device *dev = NULL;
	wchar_t* interface_path = NULL;
	HANDLE device_handle = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA pp_data = NULL;
	HIDP_CAPS caps;

	if (hid_init() < 0)
		goto end_of_function;

	interface_path = hid_internal_UTF8toUTF16(path);
	if (!interface_path)
		goto end_of_function;

	/* Open a handle to the device */
	device_handle = open_device(interface_path, TRUE);

	/* Check validity of write_handle. */
	if (device_handle == INVALID_HANDLE_VALUE) {
		/* System devices, such as keyboards and mice, cannot be opened in
		   read-write mode, because the system takes exclusive control over
		   them.  This is to prevent keyloggers.  However, feature reports
		   can still be sent and received.  Retry opening the device, but
		   without read/write access. */
		device_handle = open_device(interface_path, FALSE);

		/* Check the validity of the limited device_handle. */
		if (device_handle == INVALID_HANDLE_VALUE)
			goto end_of_function;
	}

	/* Set the Input Report buffer size to 64 reports. */
	if (!HidD_SetNumInputBuffers(device_handle, 64))
		goto end_of_function;

	/* Get the Input Report length for the device. */
	if (!HidD_GetPreparsedData(device_handle, &pp_data))
		goto end_of_function;

	if (HidP_GetCaps(pp_data, &caps) != HIDP_STATUS_SUCCESS)
		goto end_of_function;

	dev = new_hid_device();
	if (dev)
	{
		dev->device_handle = device_handle;
		device_handle = INVALID_HANDLE_VALUE;

		dev->output_report_length = caps.OutputReportByteLength;
		dev->input_report_length = caps.InputReportByteLength;
		dev->feature_report_length = caps.FeatureReportByteLength;
		dev->read_buf = (char*)malloc(dev->input_report_length);
		dev->device_info = hid_internal_get_device_info(interface_path, dev->device_handle);
	}

end_of_function:
	free(interface_path);
	if (device_handle != INVALID_HANDLE_VALUE && device_handle != NULL)
	{
		CloseHandle(device_handle);
		device_handle = INVALID_HANDLE_VALUE;
	}

	if (pp_data) {
		HidD_FreePreparsedData(pp_data);
	}

	return dev;
}

int HID_API_EXPORT HID_API_CALL hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	DWORD bytes_written = 0;
	int function_result = -1;
	BOOL res;
	BOOL overlapped = FALSE;

	unsigned char *buf;

	if (!data || (length==0)) {
		register_string_error(dev, L"Zero buffer/length");
		return function_result;
	}

	/* Make sure the right number of bytes are passed to WriteFile. Windows
	   expects the number of bytes which are in the _longest_ report (plus
	   one for the report number) bytes even if the data is a report
	   which is shorter than that. Windows gives us this value in
	   caps.OutputReportByteLength. If a user passes in fewer bytes than this,
	   use cached temporary buffer which is the proper size. */
	if (length >= dev->output_report_length) {
		/* The user passed the right number of bytes. Use the buffer as-is. */
		buf = (unsigned char *) data;
	} else {
		if (dev->write_buf == NULL)
			dev->write_buf = (unsigned char *) malloc(dev->output_report_length);
		buf = dev->write_buf;
		if (buf == NULL)
		{
			register_winapi_error(dev, L"malloc");
			return -1;
		}

		memcpy(buf, data, length);
		memset(buf + length, 0, dev->output_report_length - length);
		length = dev->output_report_length;
	}

	res = WriteFile(dev->device_handle, buf, (DWORD) length, NULL, &dev->write_ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* WriteFile() failed. Return error. */
			register_winapi_error(dev, L"WriteFile");
			goto end_of_function;
		}
		overlapped = TRUE;
	}

	if (overlapped) {
		/* Wait for the transaction to complete. This makes
		   hid_write() synchronous. */
		res = WaitForSingleObject(dev->write_ol.hEvent, 1000);
		if (res != WAIT_OBJECT_0) {
			/* There was a Timeout. */
			register_winapi_error(dev, L"hid_write/WaitForSingleObject");
			goto end_of_function;
		}

		/* Get the result. */
		res = GetOverlappedResult(dev->device_handle, &dev->write_ol, &bytes_written, FALSE/*wait*/);
		if (res) {
			function_result = bytes_written;
		}
		else {
			/* The Write operation failed. */
			register_winapi_error(dev, L"hid_write/GetOverlappedResult");
			goto end_of_function;
		}
	}

end_of_function:
	return function_result;
}

int HID_API_EXPORT HID_API_CALL hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	if (hid_device_get_on_read(dev)) 
	{
		register_string_error(dev, L"hid_device has been registered to use on_read callback to read data.");
		return -1;
	}

	BOOL res = FALSE;
	int copy_len = 0;
	
	res = hid_internal_dev_async_read_nowait(dev);
	if (!res)
	{
		goto end_of_function;
	}

	if (dev->read_pending) 
	{
		if (milliseconds >= 0) {
			/* See if there is any data yet. */
			res = WaitForSingleObject(dev->ol.hEvent, milliseconds);
			if (res != WAIT_OBJECT_0) {
				/* There was no data this time. Return zero bytes available,
				   but leave the Overlapped I/O running. */
				return 0;
			}
		}

		res = hid_internal_dev_async_read_pending_data(dev);
	}

	if (res)
	{
		copy_len = (int)hid_internal_dev_copy_read_buf(dev, data, length);
	}


end_of_function:
	if (!res) {
		return -1;
	}

	return copy_len;
}

int HID_API_EXPORT HID_API_CALL hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device *dev, int nonblock)
{
	dev->blocking = nonblock ? FALSE : TRUE;
	return 0; /* Success */
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	BOOL res = FALSE;
	unsigned char *buf;
	size_t length_to_send = 0;

	/* Windows expects at least caps.FeatureReportByteLength bytes passed
	   to HidD_SetFeature(), even if the report is shorter. Any less sent and
	   the function fails with error ERROR_INVALID_PARAMETER set. Any more
	   and HidD_SetFeature() silently truncates the data sent in the report
	   to caps.FeatureReportByteLength. */
	if (length >= dev->feature_report_length) {
		buf = (unsigned char *) data;
		length_to_send = length;
	} else {
		if (dev->feature_buf == NULL)
			dev->feature_buf = (unsigned char *) malloc(dev->feature_report_length);
		buf = dev->feature_buf;
		if (buf == NULL)
		{
			register_winapi_error(dev, L"malloc");
			return -1;
		}
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->feature_report_length - length);
		length_to_send = dev->feature_report_length;		
	}

	res = HidD_SetFeature(dev->device_handle, (PVOID)buf, (DWORD) length_to_send);

	if (!res) {
		register_winapi_error(dev, L"HidD_SetFeature");
		return -1;
	}

	return (int) length;
}

static int hid_get_report(hid_device *dev, DWORD report_type, unsigned char *data, size_t length)
{
	BOOL res;
	DWORD bytes_returned = 0;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	res = DeviceIoControl(dev->device_handle,
		report_type,
		data, (DWORD) length,
		data, (DWORD) length,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* DeviceIoControl() failed. Return error. */
			register_winapi_error(dev, L"Get Input/Feature Report DeviceIoControl");
			return -1;
		}
	}

	/* Wait here until the write is done. This makes
	   hid_get_feature_report() synchronous. */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		/* The operation failed. */
		register_winapi_error(dev, L"Get Input/Feature Report GetOverLappedResult");
		return -1;
	}

	/* When numbered reports aren't used,
	   bytes_returned seem to include only what is actually received from the device
	   (not including the first byte with 0, as an indication "no numbered reports"). */
	if (data[0] == 0x0) {
		bytes_returned++;
	}

	return bytes_returned;
}

int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	/* We could use HidD_GetFeature() instead, but it doesn't give us an actual length, unfortunately */
	return hid_get_report(dev, IOCTL_HID_GET_FEATURE, data, length);
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	/* We could use HidD_GetInputReport() instead, but it doesn't give us an actual length, unfortunately */
	return hid_get_report(dev, IOCTL_HID_GET_INPUT_REPORT, data, length);
}

void HID_API_EXPORT HID_API_CALL hid_close(hid_device *dev)
{
	if (!dev)
		return;

	CancelIo(dev->device_handle);

	g_DeviceMonitor.StopMonitoringDisconnectionForDevice(dev);
	g_DeviceMonitor.StopMonitoringOnReadForDevice(dev);

	free_hid_device(dev);
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!dev->device_info)
	{
		register_string_error(dev, L"NULL device/info");
		return -1;
	}

	if (!string || !maxlen)
	{
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	wcsncpy(string, dev->device_info->manufacturer_string, maxlen);
	string[maxlen] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!dev->device_info)
	{
		register_string_error(dev, L"NULL device/info");
		return -1;
	}

	if (!string || !maxlen)
	{
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}


	wcsncpy(string, dev->device_info->product_string, maxlen);
	string[maxlen] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!dev->device_info)
	{
		register_string_error(dev, L"NULL device/info");
		return -1;
	}

	if (!string || !maxlen)
	{
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}


	wcsncpy(string, dev->device_info->serial_number, maxlen);
	string[maxlen] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetIndexedString(dev->device_handle, string_index, string, sizeof(wchar_t) * (DWORD) MIN(maxlen, MAX_STRING_WCHARS));
	if (!res) {
		register_winapi_error(dev, L"HidD_GetIndexedString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_max_report_length(hid_device* dev)
{
	return (int)dev->input_report_length;
}

int HID_API_EXPORT_CALL hid_winapi_get_container_id(hid_device *dev, GUID *container_id)
{
	wchar_t *interface_path = NULL, *device_id = NULL;
	CONFIGRET cr = CR_FAILURE;
	DEVINST dev_node;
	DEVPROPTYPE property_type;
	ULONG len;

	if (!container_id)
	{
		register_string_error(dev, L"Invalid Container ID");
		return -1;
	}

	interface_path = hid_internal_UTF8toUTF16(dev->device_info->path);
	if (!interface_path)
	{
		register_string_error(dev, L"Path conversion failure");
		goto end;
	}

	/* Get the device id from interface path */
	device_id = (wchar_t*)hid_internal_get_device_interface_property(interface_path, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
	if (!device_id)
	{
		register_string_error(dev, L"Failed to get device interface property InstanceId");
		goto end;
	}

	/* Open devnode from device id */
	cr = CM_Locate_DevNodeW(&dev_node, (DEVINSTID_W)device_id, CM_LOCATE_DEVNODE_NORMAL);
	if (cr != CR_SUCCESS)
	{
		register_string_error(dev, L"Failed to locate device node");
		goto end;
	}

	/* Get the container id from devnode */
	len = sizeof(*container_id);
	cr = CM_Get_DevNode_PropertyW(dev_node, &DEVPKEY_Device_ContainerId, &property_type, (PBYTE)container_id, &len, 0);
	if (cr == CR_SUCCESS && property_type != DEVPROP_TYPE_GUID)
		cr = CR_FAILURE;

	if (cr != CR_SUCCESS)
		register_string_error(dev, L"Failed to read ContainerId property from device node");

end:
	free(interface_path);
	free(device_id);

	return cr == CR_SUCCESS ? 0 : -1;
}


HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	if (dev) {
		if (dev->last_error_str == NULL)
			return L"Success";
		return (wchar_t*)dev->last_error_str;
	}

	/* Global error messages are not (yet) implemented on Windows. */
	return L"hid_error for global errors is not implemented yet";
}


int HID_API_EXPORT HID_API_CALL hid_register_read_callback(hid_device *dev, void (*on_read)(hid_device *, unsigned char *, size_t))
{
	hid_device_set_on_read(dev, on_read);
    return 0;
}

void HID_API_EXPORT HID_API_CALL hid_unregister_read_callback(hid_device *dev)
{
	hid_device_set_on_read(dev, NULL);
}

int HID_API_EXPORT hid_register_disconnected_callback(hid_device *dev, void (*on_disconnected)(hid_device *))
{
	hid_device_set_on_disconnected(dev, on_disconnected);
    return 0;
}
    
void HID_API_EXPORT hid_unregister_disconnected_callback(hid_device *dev)
{
	hid_device_set_on_disconnected(dev, NULL);
}


//#ifdef __cplusplus
//} /* extern "C" */
//#endif
