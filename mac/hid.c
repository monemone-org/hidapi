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

/* See Apple Technical Note TN2187 for details on IOHidManager. */

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/USBSpec.h>
#include <CoreFoundation/CoreFoundation.h>
#include <wchar.h>
#include <locale.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <dlfcn.h>

#include "hidapi_darwin.h"

/* As defined in AppKit.h, but we don't need the entire AppKit for a single constant. */
extern const double NSAppKitVersionNumber;

#include "pthread_barrier.c"
#include "thread_object.c"
#include "get_property.c"
#include "hid_device_info.c"
#include "hid_device_list_node.c"

static int return_data(hid_device *dev, unsigned char *data, size_t length);

/* Linked List of input reports received from the device. */
struct input_report {
	uint8_t *data;
	size_t len;
	struct input_report *next;
};

static struct hid_api_version api_version = {
    .major = HID_API_VERSION_MAJOR,
    .minor = HID_API_VERSION_MINOR,
    .patch = HID_API_VERSION_PATCH
};

static    int is_macos_10_10_or_greater = 0;
static  IOOptionBits device_open_options = 0;

static  thread_object        *hid_daemon_thread_object  = NULL;
static	IOHIDManagerRef       hid_main_mgr = 0x0;
static  hid_device_list_node *hid_device_list = NULL;

typedef void (on_added_device_func)(struct hid_device_info *);
static on_added_device_func *the_on_added_device = NULL;

#include "hid_device.c"

static void hid_daemon_thread_object_starting(thread_object *thread_object);
static void hid_daemon_thread_object_exiting(thread_object *thread_object);
static void matchingCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device);
    //static void removalCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device);
static void hid_mgr_set_matching(IOHIDManagerRef hid_mgr,
                                 unsigned short vendor_id,
                                 unsigned short product_id,
                                 unsigned short usage_page,
                                 unsigned short usage);


/* Initialize the IOHIDManager. Return 0 for success and -1 for failure. */
static IOHIDManagerRef init_hid_manager(void)
{
    /* Initialize all the HID Manager Objects */
    IOHIDManagerRef hid_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (hid_mgr)
    {
        IOHIDManagerSetDeviceMatching(hid_mgr, NULL);
        IOHIDManagerScheduleWithRunLoop(hid_mgr, hid_daemon_thread_object->run_loop, hid_daemon_thread_object->run_loop_mode);
        return hid_mgr;
    }
    
    return NULL;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version()
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str()
{
	return HID_API_VERSION_STR;
}

static void matchingCallback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    struct hid_device_info *dev = create_device_info(device);
    if (the_on_added_device != NULL)
    {
        the_on_added_device(dev);
        free_hid_device_info(dev);
    }
}



/* Initialize the IOHIDManager if necessary. This is the public function, and
   it is safe to call this function repeatedly. Return 0 for success and -1
   for failure. */
int HID_API_EXPORT hid_init(void)
{
    if (hid_daemon_thread_object == NULL)
    {
        //create deamon thread for add/remove device and read notification
        hid_daemon_thread_object = init_thread_object();
        if (hid_daemon_thread_object == NULL)
            goto return_error;
        
        hid_daemon_thread_object->on_thread_starting = &hid_daemon_thread_object_starting;
        hid_daemon_thread_object->on_thread_exiting = &hid_daemon_thread_object_exiting;
        start_thread_object(hid_daemon_thread_object);
    }

    if (!hid_main_mgr) {
        is_macos_10_10_or_greater = (NSAppKitVersionNumber >= 1343); /* NSAppKitVersionNumber10_10 */
		hid_darwin_set_open_exclusive(1); /* Backward compatibility */
        hid_main_mgr = init_hid_manager();
        if (hid_main_mgr == NULL)
            goto return_error;
    }

	/* Already initialized. */
	return 0;
    
return_error:
    
    hid_exit();
    
    return -1;
}

int HID_API_EXPORT hid_exit(void)
{
	if (hid_main_mgr) {
		/* Close the HID manager. */
        IOHIDManagerRegisterDeviceMatchingCallback(hid_main_mgr, NULL, hid_main_mgr);
		IOHIDManagerClose(hid_main_mgr, kIOHIDOptionsTypeNone);
		CFRelease(hid_main_mgr);
        hid_main_mgr = NULL;
    }

    the_on_added_device = NULL;

    if (hid_daemon_thread_object)
    {
		stop_thread_object(hid_daemon_thread_object);
		free_thread_object(hid_daemon_thread_object);
        hid_daemon_thread_object = NULL;
	}

	return 0;
}

static void hid_daemon_thread_object_starting(thread_object *thread_object)
{
}

static void hid_daemon_thread_object_exiting(thread_object *thread_object)
{
	pthread_mutex_lock(&hid_daemon_thread_object->mutex);

	hid_device_list_node *curr = hid_device_list;
	while (curr != NULL)
	{

		/* Now that the read thread is stopping, Wake any threads which are
		   waiting on data (in hid_read_timeout()). Do this under a mutex to
		   make sure that a thread which is about to go to sleep waiting on
		   the condition actually will go to sleep before the condition is
		   signaled. */
		pthread_mutex_lock(&curr->dev->mutex);
		pthread_cond_broadcast(&curr->dev->condition);
		pthread_mutex_unlock(&curr->dev->mutex);

		curr = curr->next;
	}

	pthread_mutex_unlock(&hid_daemon_thread_object->mutex);

}

// static void process_pending_events(void) {
// 	SInt32 res;
// 	do {
// 		res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, FALSE);
// 	} while(res != kCFRunLoopRunFinished && res != kCFRunLoopRunTimedOut);
// }




static void hid_mgr_set_matching(IOHIDManagerRef hid_mgr,
                                 unsigned short vendor_id,
                                 unsigned short product_id,
                                 unsigned short usage_page,
                                 unsigned short usage)
{
	/* Get a list of the Devices */
	CFMutableDictionaryRef matching = NULL;
	if (vendor_id != 0 || product_id != 0) {
		matching = CFDictionaryCreateMutable(kCFAllocatorDefault, kIOHIDOptionsTypeNone, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

		if (matching && vendor_id != 0) {
			CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &vendor_id);
			CFDictionarySetValue(matching, CFSTR(kIOHIDVendorIDKey), v);
			CFRelease(v);
		}

		if (matching && product_id != 0) {
			CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &product_id);
			CFDictionarySetValue(matching, CFSTR(kIOHIDProductIDKey), p);
			CFRelease(p);
		}

		if (matching && usage_page != 0) {
			CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &usage_page);
			CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsagePageKey), p);
			CFRelease(p);
		}

		if (matching && usage != 0) {
			CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &usage);
			CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsageKey), p);
			CFRelease(p);
		}
	}
	IOHIDManagerSetDeviceMatching(hid_mgr, matching);
	if (matching != NULL) {
		CFRelease(matching);
	}	
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id,
                                                       unsigned short product_id)
{
    return hid_enumerate_ex(vendor_id, product_id, 0, 0, NULL);
}


struct hid_device_info  HID_API_EXPORT *hid_enumerate_ex(unsigned short vendor_id,
	                                                     unsigned short product_id,
	                                                     unsigned short usage_page,
	                                                     unsigned short usage,
                                                         void (*on_added_device)(struct hid_device_info *))
{
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;
	CFIndex num_devices;
	int i;

	/* Set up the HID Manager if it hasn't been done */
	if (hid_init() < 0)
		return NULL;

    /* give the IOHIDManager a chance to update itself */
	//process_pending_events();

    // stop receiving matching callback so we can safely update the_on_added_device
    IOHIDManagerRegisterDeviceMatchingCallback(hid_main_mgr, NULL, hid_main_mgr);

    hid_mgr_set_matching(hid_main_mgr, vendor_id, product_id, usage_page, usage);

    // update matching callback after set_matching, so we only get update callback
    // after our enumerations
    if (on_added_device != NULL)
    {
        the_on_added_device = on_added_device;
        IOHIDManagerRegisterDeviceMatchingCallback(hid_main_mgr, matchingCallback, hid_main_mgr);
    }

	CFSetRef device_set = IOHIDManagerCopyDevices(hid_main_mgr);
	if (device_set == NULL) {
		return NULL;
	}

	/* Convert the list into a C array so we can iterate easily. */
	num_devices = CFSetGetCount(device_set);
	IOHIDDeviceRef *device_array = (IOHIDDeviceRef*) calloc(num_devices, sizeof(IOHIDDeviceRef));
	CFSetGetValues(device_set, (const void **) device_array);

	/* Iterate over each device, making an entry for it. */
	for (i = 0; i < num_devices; i++) {

		IOHIDDeviceRef dev = device_array[i];
		if (!dev) {
			continue;
		}

		struct hid_device_info *tmp = create_device_info(dev);
		if (tmp == NULL) {
			continue;
		}

		if (cur_dev) {
			cur_dev->next = tmp;
		}
		else {
			root = tmp;
		}
		cur_dev = tmp;

		/* move the pointer to the tail of returnd list */
		while (cur_dev->next != NULL) {
			cur_dev = cur_dev->next;
		}
	}

	free(device_array);
	CFRelease(device_set);

	return root;
}


void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
	/* This function is identical to the Linux version. Platform independent. */
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free_hid_device_info(d);
		d = next;
	}
}

hid_device * HID_API_EXPORT hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	/* This function is identical to the Linux version. Platform independent. */
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device * handle = NULL;

	devs = hid_enumerate(vendor_id, product_id);
	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (wcscmp(serial_number, cur_dev->serial_number) == 0) {
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

static void hid_device_removal_callback(void *context, IOReturn result,
                                        void *sender)
{
	(void) result;
	(void) sender;

	/* Stop the Run Loop for this device. */
	hid_device *d = (hid_device*) context;

	d->disconnected = 1;
	//CFRunLoopStop(d->run_loop);
}

/* The Run Loop calls this function for each input report received.
   This function puts the data into a linked list to be picked up by
   hid_read(). */
static void hid_report_callback(void *context, IOReturn result, void *sender,
                         IOHIDReportType report_type, uint32_t report_id,
                         uint8_t *report, CFIndex report_length)
{
	(void) result;
	(void) sender;
	(void) report_type;
	(void) report_id;

	struct input_report *rpt;
	hid_device *dev = (hid_device*) context;

	/* Make a new Input Report object */
	rpt = (struct input_report*) calloc(1, sizeof(struct input_report));
	rpt->data = (uint8_t*) calloc(1, report_length);
	memcpy(rpt->data, report, report_length);
	rpt->len = report_length;
	rpt->next = NULL;

    // get dev->on_read safely
    void (*the_on_read)(unsigned char *, size_t) = NULL;
    pthread_mutex_lock(&dev->mutex);
    the_on_read = dev->on_read;
    pthread_mutex_unlock(&dev->mutex);
    
    if (the_on_read != NULL)
    {
        the_on_read(rpt->data, rpt->len);
        return;
    }
    
	/* Lock this section */
	pthread_mutex_lock(&dev->mutex);

	/* Attach the new report object to the end of the list. */
	if (dev->input_reports == NULL) {
		/* The list is empty. Put it at the root. */
		dev->input_reports = rpt;
	}
	else {
		/* Find the end of the list and attach. */
		struct input_report *cur = dev->input_reports;
		int num_queued = 0;
		while (cur->next != NULL) {
			cur = cur->next;
			num_queued++;
		}
		cur->next = rpt;

		/* Pop one off if we've reached 30 in the queue. This
		   way we don't grow forever if the user never reads
		   anything from the device. */
		if (num_queued > 30) {
			return_data(dev, NULL, 0);
		}
	}

	/* Signal a waiting thread that there is data. */
	pthread_cond_signal(&dev->condition);

	/* Unlock */
	pthread_mutex_unlock(&dev->mutex);

}



/* \p path must be one of:
     - in format 'DevSrvsID:<RegistryEntryID>' (as returned by hid_enumerate);
     - a valid path to an IOHIDDevice in the IOService plane (as returned by IORegistryEntryGetPath,
       e.g.: "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/EHC1@1D,7/AppleUSBEHCI/PLAYSTATION(R)3 Controller@fd120000/IOUSBInterface@0/IOUSBHIDDriver");
   Second format is for compatibility with paths accepted by older versions of HIDAPI.
*/
static io_registry_entry_t hid_open_service_registry_from_path(const char *path)
{
	if (path == NULL)
		return MACH_PORT_NULL;

	/* Get the IORegistry entry for the given path */
	if (strncmp("DevSrvsID:", path, 10) == 0) {
		char *endptr;
		uint64_t entry_id = strtoull(path + 10, &endptr, 10);
		if (*endptr == '\0') {
			return IOServiceGetMatchingService(NULL, IORegistryEntryIDMatching(entry_id));
		}
	}
	else {
		/* Fallback to older format of the path */
		return IORegistryEntryFromPath(NULL, path);
	}

	return MACH_PORT_NULL;
}

hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
	hid_device *dev = NULL;
	io_registry_entry_t entry = MACH_PORT_NULL;
	IOReturn ret = kIOReturnInvalid;

	/* Set up the HID Manager if it hasn't been done */
	if (hid_init() < 0)
		goto return_error;

	dev = new_hid_device();

	/* Get the IORegistry entry for the given path */
	entry = hid_open_service_registry_from_path(path);
	if (entry == MACH_PORT_NULL) {
		/* Path wasn't valid (maybe device was removed?) */
		goto return_error;
	}

	/* Create an IOHIDDevice for the entry */
	dev->device_handle = IOHIDDeviceCreate(kCFAllocatorDefault, entry);
	if (dev->device_handle == NULL) {
		/* Error creating the HID device */
		goto return_error;
	}

	/* Open the IOHIDDevice */
	ret = IOHIDDeviceOpen(dev->device_handle, dev->open_options);
	if (ret == kIOReturnSuccess) {

		/* Create the buffers for receiving data */
		dev->max_input_report_len = (CFIndex) get_max_report_length(dev->device_handle);
		dev->input_report_buf = (uint8_t*) calloc(dev->max_input_report_len, sizeof(uint8_t));

		/* Move the device's run loop to this thread. */
		IOHIDDeviceScheduleWithRunLoop(dev->device_handle, hid_daemon_thread_object->run_loop, hid_daemon_thread_object->run_loop_mode);

		/* Attach the device to a Run Loop */
		IOHIDDeviceRegisterInputReportCallback(
			dev->device_handle, dev->input_report_buf, dev->max_input_report_len,
			&hid_report_callback, dev);
		IOHIDDeviceRegisterRemovalCallback(dev->device_handle, hid_device_removal_callback, dev);

		IOObjectRelease(entry);

		pthread_mutex_lock(&hid_daemon_thread_object->mutex);
		hid_device_list = add_hid_device_to_list(dev, hid_device_list);
		pthread_mutex_unlock(&hid_daemon_thread_object->mutex);

		return dev;
	}
	else {
		goto return_error;
	}

return_error:
	if (dev->device_handle != NULL)
		CFRelease(dev->device_handle);

	if (entry != MACH_PORT_NULL)
		IOObjectRelease(entry);

	free_hid_device(dev);
	return NULL;
}

void HID_API_EXPORT hid_close(hid_device *dev)
{
	if (!dev)
		return;

	/* Disconnect the report callback before close.
	   See comment below.
	*/
	if (is_macos_10_10_or_greater || !dev->disconnected) {
		IOHIDDeviceRegisterInputReportCallback(
			dev->device_handle, dev->input_report_buf, dev->max_input_report_len,
			NULL, dev);
		IOHIDDeviceRegisterRemovalCallback(dev->device_handle, NULL, dev);
		IOHIDDeviceUnscheduleFromRunLoop(dev->device_handle, hid_daemon_thread_object->run_loop, hid_daemon_thread_object->run_loop_mode);
		IOHIDDeviceScheduleWithRunLoop(dev->device_handle, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
	}

	/* Close the OS handle to the device, but only if it's not
	   been unplugged. If it's been unplugged, then calling
	   IOHIDDeviceClose() will crash.

	   UPD: The crash part was true in/until some version of macOS.
	   Starting with macOS 10.15, there is an opposite effect in some environments:
	   crash happenes if IOHIDDeviceClose() is not called.
	   Not leaking a resource in all tested environments.
	*/
	if (is_macos_10_10_or_greater || !dev->disconnected) {
		IOHIDDeviceClose(dev->device_handle, dev->open_options);
	}

	/* Clear out the queue of received reports. */
	pthread_mutex_lock(&dev->mutex);
	while (dev->input_reports) {
		return_data(dev, NULL, 0);
	}
	pthread_mutex_unlock(&dev->mutex);
	CFRelease(dev->device_handle);

    pthread_mutex_lock(&hid_daemon_thread_object->mutex);
	hid_device_list = remove_hid_device_from_list(dev, hid_device_list);
	pthread_mutex_unlock(&hid_daemon_thread_object->mutex);

	free_hid_device(dev);
}

static int set_report(hid_device *dev, IOHIDReportType type, const unsigned char *data, size_t length)
{
	const unsigned char *data_to_send = data;
	CFIndex length_to_send = length;
	IOReturn res;
	unsigned char report_id;

	if (!data || (length == 0)) {
        printf("IOHIDDeviceSetReport - return -1 because if (!data || (length == 0))\n");
		return -1;
	}

	report_id = data[0];

	if (report_id == 0x0) {
        printf("IOHIDDeviceSetReport - report_id == 0x0\n");
		/* Not using numbered Reports.
		   Don't send the report number. */
		data_to_send = data+1;
		length_to_send = length-1;
	}

	/* Avoid crash if the device has been unplugged. */
	if (dev->disconnected) {
        printf("IOHIDDeviceSetReport - return -1 because if dev->disconnected\n");
		return -1;
	}

	res = IOHIDDeviceSetReport(dev->device_handle,
	                           type,
	                           report_id,
	                           data_to_send, length_to_send);
    //printf("IOHIDDeviceSetReport - res = %d\n", (int)res);

	if (res == kIOReturnSuccess) {
		return (int) length;
	}

	return -1;
}

static int get_report(hid_device *dev, IOHIDReportType type, unsigned char *data, size_t length)
{
	unsigned char *report = data;
	CFIndex report_length = length;
	IOReturn res = kIOReturnSuccess;
	const unsigned char report_id = data[0];

	if (report_id == 0x0) {
		/* Not using numbered Reports.
		   Don't send the report number. */
		report = data+1;
		report_length = length-1;
	}

	/* Avoid crash if the device has been unplugged. */
	if (dev->disconnected) {
		return -1;
	}

	res = IOHIDDeviceGetReport(dev->device_handle,
	                           type,
	                           report_id,
	                           report, &report_length);

	if (res == kIOReturnSuccess) {
		if (report_id == 0x0) { /* 0 report number still present at the beginning */
			report_length++;
		}
		return (int) report_length;
	}

	return -1;
}

int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	return set_report(dev, kIOHIDReportTypeOutput, data, length);
}

/* Helper function, so that this isn't duplicated in hid_read(). */
static int return_data(hid_device *dev, unsigned char *data, size_t length)
{
	/* Copy the data out of the linked list item (rpt) into the
	   return buffer (data), and delete the liked list item. */
	struct input_report *rpt = dev->input_reports;
	size_t len = (length < rpt->len)? length: rpt->len;
	memcpy(data, rpt->data, len);
	dev->input_reports = rpt->next;
	free(rpt->data);
	free(rpt);
	return (int) len;
}

static int cond_wait(const hid_device *dev, pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	while (!dev->input_reports) {
		int res = pthread_cond_wait(cond, mutex);
		if (res != 0)
			return res;

		/* A res of 0 means we may have been signaled or it may
		   be a spurious wakeup. Check to see that there's actually
		   data in the queue before returning, and if not, go back
		   to sleep. See the pthread_cond_timedwait() man page for
		   details. */

		if (hid_daemon_thread_object == NULL || hid_daemon_thread_object->shutdown_thread || dev->disconnected)
			return -1;
	}

	return 0;
}

static int cond_timedwait(const hid_device *dev, pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
	while (!dev->input_reports) {
		int res = pthread_cond_timedwait(cond, mutex, abstime);
		if (res != 0)
			return res;

		/* A res of 0 means we may have been signaled or it may
		   be a spurious wakeup. Check to see that there's actually
		   data in the queue before returning, and if not, go back
		   to sleep. See the pthread_cond_timedwait() man page for
		   details. */

		if (hid_daemon_thread_object == NULL || hid_daemon_thread_object->shutdown_thread || dev->disconnected)
			return -1;
	}

	return 0;

}

int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	int bytes_read = -1;

	/* Lock the access to the report list. */
	pthread_mutex_lock(&dev->mutex);

	/* There's an input report queued up. Return it. */
	if (dev->input_reports) {
		/* Return the first one */
		bytes_read = return_data(dev, data, length);
		goto ret;
	}

	/* Return if the device has been disconnected. */
	if (dev->disconnected) {
		bytes_read = -1;
		goto ret;
	}

	if (hid_daemon_thread_object == NULL || hid_daemon_thread_object->shutdown_thread) {
		/* This means the device has been closed (or there
		   has been an error. An error code of -1 should
		   be returned. */
		bytes_read = -1;
		goto ret;
	}

	/* There is no data. Go to sleep and wait for data. */

	if (milliseconds == -1) {
		/* Blocking */
		int res;
		res = cond_wait(dev, &dev->condition, &dev->mutex);
		if (res == 0)
			bytes_read = return_data(dev, data, length);
		else {
			/* There was an error, or a device disconnection. */
			bytes_read = -1;
		}
	}
	else if (milliseconds > 0) {
		/* Non-blocking, but called with timeout. */
		int res;
		struct timespec ts;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		TIMEVAL_TO_TIMESPEC(&tv, &ts);
		ts.tv_sec += milliseconds / 1000;
		ts.tv_nsec += (milliseconds % 1000) * 1000000;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}

		res = cond_timedwait(dev, &dev->condition, &dev->mutex, &ts);
		if (res == 0)
			bytes_read = return_data(dev, data, length);
		else if (res == ETIMEDOUT)
			bytes_read = 0;
		else
			bytes_read = -1;
	}
	else {
		/* Purely non-blocking */
		bytes_read = 0;
	}

ret:
	/* Unlock */
	pthread_mutex_unlock(&dev->mutex);
	return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}


int HID_API_EXPORT hid_register_read_callback(hid_device *dev, void (*on_read)(unsigned char *, size_t))
{
    pthread_mutex_lock(&dev->mutex);
    dev->on_read = on_read;
    pthread_mutex_unlock(&dev->mutex);
    return 0;
}

void HID_API_EXPORT hid_unregister_read_callback(hid_device *dev)
{
    pthread_mutex_lock(&dev->mutex);
    dev->on_read = NULL;
    pthread_mutex_unlock(&dev->mutex);
    return;
}


int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
	/* All Nonblocking operation is handled by the library. */
	dev->blocking = !nonblock;

	return 0;
}

int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	return set_report(dev, kIOHIDReportTypeFeature, data, length);
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	return get_report(dev, kIOHIDReportTypeFeature, data, length);
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{	
	return get_report(dev, kIOHIDReportTypeInput, data, length);
}

int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	return get_manufacturer_string(dev->device_handle, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	return get_product_string(dev->device_handle, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	return get_serial_number(dev->device_handle, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	(void) dev;
	(void) string_index;
	(void) string;
	(void) maxlen;

	/* TODO: */

	return 0;
}

int HID_API_EXPORT_CALL hid_get_max_report_length(hid_device *dev)
{
    return get_max_report_length(dev->device_handle);
}

int HID_API_EXPORT_CALL hid_darwin_get_location_id(hid_device *dev, uint32_t *location_id)
{
	int res = get_int_property(dev->device_handle, CFSTR(kIOHIDLocationIDKey));
	if (res != 0) {
		*location_id = (uint32_t) res;
		return 0;
	} else {
		return -1;
	}
}

void HID_API_EXPORT_CALL hid_darwin_set_open_exclusive(int open_exclusive)
{
	device_open_options = (open_exclusive == 0) ? kIOHIDOptionsTypeNone : kIOHIDOptionsTypeSeizeDevice;
}

int HID_API_EXPORT_CALL hid_darwin_get_open_exclusive(void)
{
	return (device_open_options == kIOHIDOptionsTypeSeizeDevice) ? 1 : 0;
}

int HID_API_EXPORT_CALL hid_darwin_is_device_open_exclusive(hid_device *dev)
{
	if (!dev)
		return -1;

	return (dev->open_options == kIOHIDOptionsTypeSeizeDevice) ? 1 : 0;
}

HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	(void) dev;
	/* TODO: */

	return L"hid_error is not implemented yet";
}




