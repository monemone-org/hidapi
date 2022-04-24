//hid_device.c

struct hid_device_ {
	IOHIDDeviceRef device_handle;
    IOOptionBits open_options;
	int blocking;
	//int uses_numbered_reports;
	volatile int disconnected;

	uint8_t *input_report_buf;
	CFIndex max_input_report_len;
	struct input_report *input_reports;

	pthread_mutex_t mutex; /* Protects input_reports */
	pthread_cond_t condition;

};

static hid_device *new_hid_device(void)
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	dev->device_handle = NULL;
    dev->open_options = device_open_options;
	dev->blocking = 1;
	//dev->uses_numbered_reports = 0;
	dev->disconnected = 0;

	dev->input_report_buf = NULL;
	dev->max_input_report_len =0;
	dev->input_reports = NULL;

	/* Thread objects */
	pthread_mutex_init(&dev->mutex, NULL);
	pthread_cond_init(&dev->condition, NULL);

	return dev;
}

static void free_hid_device(hid_device *dev)
{
	if (!dev)
		return;

	/* Delete any input reports still left over. */
	struct input_report *rpt = dev->input_reports;
	while (rpt) {
		struct input_report *next = rpt->next;
		free(rpt->data);
		free(rpt);
		rpt = next;
	}
	free(dev->input_report_buf);

	pthread_cond_destroy(&dev->condition);
	pthread_mutex_destroy(&dev->mutex);

	/* Free the structure itself. */
	free(dev);
}

