#pragma once

#include "hidapi.h"
#include <windows.h>


// hid_internal functions.
bool IsMainWindowThread();
void SetMainWindowThread();
void AssertMainWindowThread();

typedef void (*on_read_callback)(hid_device*, unsigned char*, size_t);
typedef void (*on_disconnected_callback)(hid_device*);

class HidDeviceAsyncReadThread;

struct hid_device_ {
	WCHAR *id;
	HANDLE device_handle;
	BOOL blocking;
	USHORT output_report_length;
	unsigned char* write_buf;
	size_t input_report_length;
	USHORT feature_report_length;
	unsigned char* feature_buf;
	wchar_t* last_error_str;

	BOOL read_pending;
	char* read_buf;
	DWORD read_buf_bytes_read;
	OVERLAPPED ol;
	
	OVERLAPPED write_ol;

	struct hid_device_info* device_info;

	CRITICAL_SECTION cs; //to protect the access of on_read and on_disconnected
	on_read_callback on_read;
	on_disconnected_callback on_disconnected;

};


HANDLE open_device(const wchar_t* path, BOOL open_rw);

struct hid_device_info* create_hid_device_info_from_device_interface_name(
	wchar_t* device_interface,
	unsigned short if_match_vendor_id,
	unsigned short if_match_product_id,
	unsigned short if_match_usage_page,
	unsigned short if_match_usage
);
void free_hid_device_info(struct hid_device_info* d);

struct hid_device_info* hid_internal_get_device_info(const wchar_t* path, HANDLE handle);

wchar_t* hid_internal_UTF8toUTF16(const char* src);

// if dev is NULL, set the global last error string
void register_winapi_error(hid_device* dev, const WCHAR* op);
void register_string_error(hid_device* dev, const WCHAR* string_error);


enum hid_device_read_ret {
	hid_device_read_failed = -1,
	hid_device_read_succeeded = 0,
	hid_device_read_pending = 1
};
// Step 1. attempt to read from dev
// Returns: 
//		hid_device_read_pending - if an async read op has started.  Caller should use dev->ol.hEvent to check
//		                           if pending data ia ready to be read.
hid_device_read_ret hid_internal_dev_async_read_nowait(hid_device* dev);

// Step 2.  read ready pending data to dev->read_buf .
BOOL hid_internal_dev_async_read_pending_data(hid_device* dev);

// copy dev->read_buf to the provided data.
size_t hid_internal_dev_copy_read_buf(hid_device* dev, unsigned char* data, size_t length);

// Return the description of Windows' GetLastError() 
void register_winapi_error_to_buffer(wchar_t** error_buffer, const WCHAR* op);

#include <string>

static std::wstring g_last_error_str;

inline
void mone_set_global_error(const wchar_t* last_error_str)
{
	// TODO: this is not thread safe
	g_last_error_str = (last_error_str != NULL ? last_error_str : L"");
}

inline
const wchar_t* mone_get_global_error()
{
	// TODO: this is not thread safe
	return g_last_error_str.c_str();
}


