#pragma once

#include "hidapi_cfgmgr32.h"
#include "hidapi_hidsdi.h"

#ifndef HIDAPI_USE_DDK

int lookup_functions();
void free_library_handles();

static HidD_GetHidGuid_ HidD_GetHidGuid = NULL;
static HidD_GetAttributes_ HidD_GetAttributes = NULL;
static HidD_GetSerialNumberString_ HidD_GetSerialNumberString = NULL;
static HidD_GetManufacturerString_ HidD_GetManufacturerString = NULL;
static HidD_GetProductString_ HidD_GetProductString = NULL;
static HidD_SetFeature_ HidD_SetFeature = NULL;
static HidD_GetFeature_ HidD_GetFeature = NULL;
static HidD_GetInputReport_ HidD_GetInputReport = NULL;
static HidD_GetIndexedString_ HidD_GetIndexedString = NULL;
static HidD_GetPreparsedData_ HidD_GetPreparsedData = NULL;
static HidD_FreePreparsedData_ HidD_FreePreparsedData = NULL;
static HidP_GetCaps_ HidP_GetCaps = NULL;
static HidD_SetNumInputBuffers_ HidD_SetNumInputBuffers = NULL;

static CM_Locate_DevNodeW_ CM_Locate_DevNodeW = NULL;
static CM_Get_Parent_ CM_Get_Parent = NULL;
static CM_Get_DevNode_PropertyW_ CM_Get_DevNode_PropertyW = NULL;
static CM_Get_Device_Interface_PropertyW_ CM_Get_Device_Interface_PropertyW = NULL;
static CM_Get_Device_Interface_List_SizeW_ CM_Get_Device_Interface_List_SizeW = NULL;
static CM_Get_Device_Interface_ListW_ CM_Get_Device_Interface_ListW = NULL;

#endif  // HIDAPI_USE_DDK