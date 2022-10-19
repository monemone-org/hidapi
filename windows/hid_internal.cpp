#include "stdafx.h"
#include "hid_internal.h"
#include "HidD_proxy.h"
#include "hidapi_hidsdi.h"


/*
    * return null if fails to create hid_device_info*
    */
struct hid_device_info* create_hid_device_info_from_device_interface_name(
    wchar_t* device_interface,
    unsigned short if_match_vendor_id,
    unsigned short if_match_product_id,
    unsigned short if_match_usage_page,
    unsigned short if_match_usage
)
{
    struct hid_device_info* ret_device_info = NULL;

    HANDLE device_handle = INVALID_HANDLE_VALUE;
    HIDD_ATTRIBUTES attrib = { 0 };

    /* Open read-only handle to the device */
    device_handle = open_device(device_interface, FALSE);

    /* Check validity of device_handle. */
    if (device_handle == INVALID_HANDLE_VALUE) {
        /* Unable to open the device. */
        return NULL;
    }

    /* Get the Vendor ID and Product ID for this device. */
    attrib.Size = sizeof(HIDD_ATTRIBUTES);
    if (!HidD_GetAttributes(device_handle, &attrib)) {
        goto cont_close;
    }

    /* Check the VID/PID to see if we should add this
       device to the enumeration list. */
    if ((if_match_vendor_id == 0x0 || attrib.VendorID == if_match_vendor_id) &&
        (if_match_product_id == 0x0 || attrib.ProductID == if_match_product_id)) {

        /* VID/PID match. Create the record. */
        struct hid_device_info* tmp = hid_internal_get_device_info(device_interface, device_handle);

        // Mone: Skip devices that do not match the specified usage and usage_page.
        if ((if_match_usage_page != 0x0 && if_match_usage_page != tmp->usage_page) &&
            (if_match_usage != 0x0 && if_match_usage != tmp->usage))
        {
            free(tmp);
            tmp = NULL;
        }

        ret_device_info = tmp;
    }

cont_close:
    CloseHandle(device_handle);

    return ret_device_info;
}

void free_hid_device_info(struct hid_device_info* d)
{
    free(d->path);
    free(d->serial_number);
    free(d->manufacturer_string);
    free(d->product_string);
    free(d);
}






/**
* Read data from HID device but don't wait for pending data.
* There are 3 possible outcomes:
* 1. ReadFile reads data sucessfully.  Function returns hid_device_read_succeeded with dev->read_pending = FALSE.  Read data is in dev->read_buf.
* 2. ReadFile finds out ther is async data incoming.  Function returns TRUE with *pbOverlapped = FALSE.
*    Caller needs to wait for dev->ol.hEvent and call GetOverlappedResult later to get the read data.
* 3. ReadFile fails. Function returns hid_device_read_failed and dev->LastError is set.
*
* Thread safety:
*  if on_read callback is not used, function should only be called in main caller thread.
*  if on_read callback is used, function should only be called by the HidDeviceConnectionMonitor's m_hReadMonitoringThread.
*
*/
hid_device_read_ret hid_internal_dev_async_read_nowait(hid_device* dev)
{
	hid_device_read_ret ret = hid_device_read_succeeded;

	if (!dev->read_pending)  //already waiting for data
	{
		/* Start an Overlapped I/O read. */
		memset(dev->read_buf, 0, dev->input_report_length);
		dev->read_buf_bytes_read = 0;
		ResetEvent(dev->ol.hEvent);

		BOOL res = ReadFile(dev->device_handle, dev->read_buf, (DWORD)dev->input_report_length, &dev->read_buf_bytes_read, &dev->ol);
		if (!res)
		{
			if (GetLastError() == ERROR_IO_PENDING)
			{
				dev->read_pending = TRUE;
				ret = hid_device_read_pending;
			}
			else
			{
				/* ReadFile() has failed.
				   Clean up and return error. */
				register_winapi_error(dev, L"ReadFile");
				CancelIo(dev->device_handle);
				ret = hid_device_read_failed;
			}
		}
		else
		{
			ret = hid_device_read_succeeded;
		}
	}
	else {
		ret = hid_device_read_pending;
	}

	return ret;
}

/**
* Copies dev->read_buf to data.
* returns the num of bytes coped to data.
*
* Thread safety:
*  if on_read callback is not used, function should only be called in main caller thread.
*  if on_read callback is used, function should only be called by the HidDeviceAsyncReadThread's m_hReadMonitoringThread.

*/
size_t hid_internal_dev_copy_read_buf(hid_device* dev, unsigned char* data, size_t length)
{
	size_t copy_len = 0;

	if (dev->read_buf_bytes_read > 0)
	{
		if (dev->read_buf[0] == 0x0) {
			/* If report numbers aren't being used, but Windows sticks a report
			   number (0x0) on the beginning of the report anyway. To make this
			   work like the other platforms, and to make it work more like the
			   HID spec, we'll skip over this byte. */
			(dev->read_buf_bytes_read)--;
			copy_len = length > dev->read_buf_bytes_read ? dev->read_buf_bytes_read : length;
			memcpy(data, dev->read_buf + 1, copy_len);
		}
		else {
			/* Copy the whole buffer, report number and all. */
			copy_len = length > dev->read_buf_bytes_read ? dev->read_buf_bytes_read : length;
			memcpy(data, dev->read_buf, copy_len);
		}
	}

	return copy_len;
}

/*
* Should only be called after hid_internal_dev_async_read_nowait(dev) returns hid_device_read_pending
* and dev->ol.hEvent is set.
*
* Thread safety:
*  if on_read callback is not used, function should only be called in main caller thread.
*  if on_read callback is used, function should only be called by the HidDeviceAsyncReadThread's m_hReadMonitoringThread.
*
*/
BOOL hid_internal_dev_async_read_pending_data(hid_device* dev)
{
	DWORD bytes_read = 0;

	if (!dev->read_pending) //There is no pending read event.
	{
		register_string_error(dev, L"hid_device->read_pending == FALSE. There is no pending data.");
		return FALSE;
	}

	/* Either WaitForSingleObject() told us that ReadFile has completed, or
	   we are in non-blocking mode. Get the number of bytes read. The actual
	   data has been copied to the data[] array which was passed to ReadFile(). */
	BOOL res = GetOverlappedResult(dev->device_handle, &dev->ol, &bytes_read, TRUE/*wait*/);
	if (!res)
	{
		register_winapi_error(dev, L"hid_read_timeout/GetOverlappedResult");
		return FALSE;
	}

	/* Set pending back to false, even if GetOverlappedResult() returned error. */
	dev->read_pending = FALSE;
	return TRUE;
}


/*
* Caller needs to free() the returned error_buffer.
*/
void register_winapi_error_to_buffer(wchar_t** error_buffer, const WCHAR* op)
{
	if (!error_buffer)
		return;

	if (*error_buffer)
	{
		free(*error_buffer);
		*error_buffer = NULL;
	}

	/* Only clear out error messages if NULL is passed into op */
	if (!op) {
		return;
	}

	WCHAR system_err_buf[1024];
	DWORD error_code = GetLastError();

	DWORD system_err_len = FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		error_code,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		system_err_buf, ARRAYSIZE(system_err_buf),
		NULL);

	size_t op_len = wcslen(op);

	size_t op_prefix_len =
		op_len
		+ 15 /*: (0x00000000) */
		;
	size_t msg_len =
		+op_prefix_len
		+ system_err_len
		;

	*error_buffer = (WCHAR*)calloc(msg_len + 1, sizeof(WCHAR));
	WCHAR* msg = *error_buffer;

	if (!msg)
		return;

	int printf_written = swprintf(msg, msg_len + 1, L"%.*ls: (0x%08X) %.*ls", (int)op_len, op, error_code, (int)system_err_len, system_err_buf);

	if (printf_written < 0)
	{
		/* Highly unlikely */
		msg[0] = L'\0';
		return;
	}

	/* Get rid of the CR and LF that FormatMessage() sticks at the
	   end of the message. Thanks Microsoft! */
	while (msg[msg_len - 1] == L'\r' || msg[msg_len - 1] == L'\n' || msg[msg_len - 1] == L' ')
	{
		msg[msg_len - 1] = L'\0';
		msg_len--;
	}
}

