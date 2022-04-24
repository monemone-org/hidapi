// get_property.c

static CFArrayRef get_array_property(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref != NULL && CFGetTypeID(ref) == CFArrayGetTypeID()) {
        return (CFArrayRef)ref;
    } else {
        return NULL;
    }
}

static int32_t get_int_property(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef ref;
    int32_t value;

    ref = IOHIDDeviceGetProperty(device, key);
    if (ref) {
        if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef) ref, kCFNumberSInt32Type, &value);
            return value;
        }
    }
    return 0;
}

static CFArrayRef get_usage_pairs(IOHIDDeviceRef device)
{
    return get_array_property(device, CFSTR(kIOHIDDeviceUsagePairsKey));
}

static unsigned short get_vendor_id(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDVendorIDKey));
}

static unsigned short get_product_id(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDProductIDKey));
}

static int32_t get_max_report_length(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDMaxInputReportSizeKey));
}

static int get_string_property(IOHIDDeviceRef device, CFStringRef prop, wchar_t *buf, size_t len)
{
    CFStringRef str;

    if (!len)
        return 0;

    str = (CFStringRef) IOHIDDeviceGetProperty(device, prop);

    buf[0] = 0;

    if (str) {
        CFIndex str_len = CFStringGetLength(str);
        CFRange range;
        CFIndex used_buf_len;
        CFIndex chars_copied;

        len --;

        range.location = 0;
        range.length = ((size_t) str_len > len)? len: (size_t) str_len;
        chars_copied = CFStringGetBytes(str,
            range,
            kCFStringEncodingUTF32LE,
            (char) '?',
            FALSE,
            (UInt8*)buf,
            len * sizeof(wchar_t),
            &used_buf_len);

        if (chars_copied <= 0)
            buf[0] = 0;
        else
            buf[chars_copied] = 0;

        return 0;
    }
    else
        return -1;

}

static int get_serial_number(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDSerialNumberKey), buf, len);
}

static int get_manufacturer_string(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDManufacturerKey), buf, len);
}

static int get_product_string(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDProductKey), buf, len);
}


/* Implementation of wcsdup() for Mac. */
static wchar_t *dup_wcs(const wchar_t *s)
{
    size_t len = wcslen(s);
    wchar_t *ret = (wchar_t*) malloc((len+1)*sizeof(wchar_t));
    wcscpy(ret, s);

    return ret;
}


