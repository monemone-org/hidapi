
static struct hid_device_info *create_device_info_with_usage(IOHIDDeviceRef dev, int32_t usage_page, int32_t usage)
{
	unsigned short dev_vid;
	unsigned short dev_pid;
	int BUF_LEN = 256;
	wchar_t buf[BUF_LEN];

	struct hid_device_info *cur_dev;
	io_object_t iokit_dev;
	kern_return_t res;
	uint64_t entry_id = 0;

	if (dev == NULL) {
		return NULL;
	}

	cur_dev = (struct hid_device_info *)calloc(1, sizeof(struct hid_device_info));
	if (cur_dev == NULL) {
		return NULL;
	}

	dev_vid = get_vendor_id(dev);
	dev_pid = get_product_id(dev);

	cur_dev->usage_page = usage_page;
	cur_dev->usage = usage;

	/* Fill out the record */
	cur_dev->next = NULL;

	/* Fill in the path (as a unique ID of the service entry) */
	cur_dev->path = NULL;
	iokit_dev = IOHIDDeviceGetService(dev);
	if (iokit_dev != MACH_PORT_NULL) {
		res = IORegistryEntryGetRegistryEntryID(iokit_dev, &entry_id);
	}
	else {
		res = KERN_INVALID_ARGUMENT;
	}

	if (res == KERN_SUCCESS) {
		/* max value of entry_id(uint64_t) is 18446744073709551615 which is 20 characters long,
		   so for (max) "path" string 'DevSrvsID:18446744073709551615' we would need
		   9+1+20+1=31 bytes byffer, but allocate 32 for simple alignment */
		cur_dev->path = calloc(1, 32);
		if (cur_dev->path != NULL) {
			sprintf(cur_dev->path, "DevSrvsID:%llu", entry_id);
		}
	}

	if (cur_dev->path == NULL) {
		/* for whatever reason, trying to keep it a non-NULL string */
		cur_dev->path = strdup("");
	}

	/* Serial Number */
	get_serial_number(dev, buf, BUF_LEN);
	cur_dev->serial_number = dup_wcs(buf);

	/* Manufacturer and Product strings */
	get_manufacturer_string(dev, buf, BUF_LEN);
	cur_dev->manufacturer_string = dup_wcs(buf);
	get_product_string(dev, buf, BUF_LEN);
	cur_dev->product_string = dup_wcs(buf);

	/* VID/PID */
	cur_dev->vendor_id = dev_vid;
	cur_dev->product_id = dev_pid;

	/* Release Number */
	cur_dev->release_number = get_int_property(dev, CFSTR(kIOHIDVersionNumberKey));

	/* Interface Number */
	/* We can only retrieve the interface number for USB HID devices.
	 * IOKit always seems to return 0 when querying a standard USB device
	 * for its interface. */
	int is_usb_hid = get_int_property(dev, CFSTR(kUSBInterfaceClass)) == kUSBHIDClass;
	if (is_usb_hid) {
		/* Get the interface number */
		cur_dev->interface_number = get_int_property(dev, CFSTR(kUSBInterfaceNumber));
	} else {
		cur_dev->interface_number = -1;
	}

	return cur_dev;
}

static struct hid_device_info *create_device_info(IOHIDDeviceRef device)
{
	const int32_t primary_usage_page = get_int_property(device, CFSTR(kIOHIDPrimaryUsagePageKey));
	const int32_t primary_usage = get_int_property(device, CFSTR(kIOHIDPrimaryUsageKey));

	/* Primary should always be first, to match previous behavior. */
	struct hid_device_info *root = create_device_info_with_usage(device, primary_usage_page, primary_usage);
	struct hid_device_info *cur = root;

	if (!root)
		return NULL;

	CFArrayRef usage_pairs = get_usage_pairs(device);

	if (usage_pairs != NULL) {
		struct hid_device_info *next = NULL;
		for (CFIndex i = 0; i < CFArrayGetCount(usage_pairs); i++) {
			CFTypeRef dict = CFArrayGetValueAtIndex(usage_pairs, i);
			if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
				continue;
			}

			CFTypeRef usage_page_ref, usage_ref;
			int32_t usage_page, usage;

			if (!CFDictionaryGetValueIfPresent((CFDictionaryRef)dict, CFSTR(kIOHIDDeviceUsagePageKey), &usage_page_ref) ||
			    !CFDictionaryGetValueIfPresent((CFDictionaryRef)dict, CFSTR(kIOHIDDeviceUsageKey), &usage_ref) ||
					CFGetTypeID(usage_page_ref) != CFNumberGetTypeID() ||
					CFGetTypeID(usage_ref) != CFNumberGetTypeID() ||
					!CFNumberGetValue((CFNumberRef)usage_page_ref, kCFNumberSInt32Type, &usage_page) ||
					!CFNumberGetValue((CFNumberRef)usage_ref, kCFNumberSInt32Type, &usage)) {
					continue;
			}
			if (usage_page == primary_usage_page && usage == primary_usage)
				continue; /* Already added. */

			next = create_device_info_with_usage(device, usage_page, usage);
			cur->next = next;
			if (next != NULL) {
				cur = next;
			}
		}
	}

	return root;
}

static void free_hid_device_info(struct hid_device_info *d)
{
	free(d->path);
	free(d->serial_number);
	free(d->manufacturer_string);
	free(d->product_string);
	free(d);
}


