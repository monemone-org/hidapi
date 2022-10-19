#include "stdafx.h"
#include "HidD_proxy.h"

#ifndef HIDAPI_USE_DDK
/* Since we're not building with the DDK, and the HID header
   files aren't part of the Windows SDK, we define what we need ourselves.
   In lookup_functions(), the function pointers
   defined below are set. */

//HidD_GetHidGuid_ HidD_GetHidGuid;
//HidD_GetAttributes_ HidD_GetAttributes;
//HidD_GetSerialNumberString_ HidD_GetSerialNumberString;
//HidD_GetManufacturerString_ HidD_GetManufacturerString;
//HidD_GetProductString_ HidD_GetProductString;
//HidD_SetFeature_ HidD_SetFeature;
//HidD_GetFeature_ HidD_GetFeature;
//HidD_GetInputReport_ HidD_GetInputReport;
//HidD_GetIndexedString_ HidD_GetIndexedString;
//HidD_GetPreparsedData_ HidD_GetPreparsedData;
//HidD_FreePreparsedData_ HidD_FreePreparsedData;
//HidP_GetCaps_ HidP_GetCaps;
//HidD_SetNumInputBuffers_ HidD_SetNumInputBuffers;
//
//CM_Locate_DevNodeW_ CM_Locate_DevNodeW = NULL;
//CM_Get_Parent_ CM_Get_Parent = NULL;
//CM_Get_DevNode_PropertyW_ CM_Get_DevNode_PropertyW = NULL;
//CM_Get_Device_Interface_PropertyW_ CM_Get_Device_Interface_PropertyW = NULL;
//CM_Get_Device_Interface_List_SizeW_ CM_Get_Device_Interface_List_SizeW = NULL;
//CM_Get_Device_Interface_ListW_ CM_Get_Device_Interface_ListW = NULL;

static HMODULE hid_lib_handle = NULL;
static HMODULE cfgmgr32_lib_handle = NULL;


void free_library_handles()
{
	if (hid_lib_handle)
		FreeLibrary(hid_lib_handle);
	hid_lib_handle = NULL;
	if (cfgmgr32_lib_handle)
		FreeLibrary(cfgmgr32_lib_handle);
	cfgmgr32_lib_handle = NULL;
}

int lookup_functions()
{
	hid_lib_handle = LoadLibraryW(L"hid.dll");
	if (hid_lib_handle == NULL) {
		goto err;
	}

	cfgmgr32_lib_handle = LoadLibraryW(L"cfgmgr32.dll");
	if (cfgmgr32_lib_handle == NULL) {
		goto err;
	}

#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#define RESOLVE(lib_handle, x) x = (x##_)GetProcAddress(lib_handle, #x); if (!x) goto err;

	RESOLVE(hid_lib_handle, HidD_GetHidGuid);
	RESOLVE(hid_lib_handle, HidD_GetAttributes);
	RESOLVE(hid_lib_handle, HidD_GetSerialNumberString);
	RESOLVE(hid_lib_handle, HidD_GetManufacturerString);
	RESOLVE(hid_lib_handle, HidD_GetProductString);
	RESOLVE(hid_lib_handle, HidD_SetFeature);
	RESOLVE(hid_lib_handle, HidD_GetFeature);
	RESOLVE(hid_lib_handle, HidD_GetInputReport);
	RESOLVE(hid_lib_handle, HidD_GetIndexedString);
	RESOLVE(hid_lib_handle, HidD_GetPreparsedData);
	RESOLVE(hid_lib_handle, HidD_FreePreparsedData);
	RESOLVE(hid_lib_handle, HidP_GetCaps);
	RESOLVE(hid_lib_handle, HidD_SetNumInputBuffers);

	RESOLVE(cfgmgr32_lib_handle, CM_Locate_DevNodeW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Parent);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_DevNode_PropertyW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Device_Interface_PropertyW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Device_Interface_List_SizeW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Device_Interface_ListW);

#undef RESOLVE
#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif

	return 0;

err:
	free_library_handles();
	return -1;
}

#endif /* HIDAPI_USE_DDK */

