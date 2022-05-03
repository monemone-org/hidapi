
struct thread_object_;

typedef struct thread_object_ {
	pthread_t thread;

	CFStringRef run_loop_mode;
	CFRunLoopRef run_loop;
	CFRunLoopSourceRef source;

	pthread_barrier_t barrier; /* Ensures correct startup sequence */
	pthread_barrier_t shutdown_barrier; /* Ensures correct shutdown sequence */
	volatile int shutdown_thread;

	pthread_mutex_t mutex;

	//run under thread's context
    void (* on_thread_starting)(struct thread_object_ *);
	void (* on_thread_exiting)(struct thread_object_ *);

} thread_object;


static thread_object* init_thread_object() {

	thread_object *thread_obj = (thread_object *)calloc(1, sizeof(thread_object));

    thread_obj->thread = NULL;
	
    thread_obj->run_loop_mode = NULL;
    thread_obj->run_loop = NULL;
    thread_obj->source = NULL;

    thread_obj->shutdown_thread = 0;
    thread_obj->on_thread_starting = NULL;
    thread_obj->on_thread_exiting = NULL;

	pthread_barrier_init(&thread_obj->barrier, NULL, 2);
	pthread_barrier_init(&thread_obj->shutdown_barrier, NULL, 2);

	pthread_mutex_init(&thread_obj->mutex, NULL);

	return thread_obj;

}

static void free_thread_object(thread_object *thread_obj)
{
	/* Free the string and the report buffer. The check for NULL
	   is necessary here as CFRelease() doesn't handle NULL like
	   free() and others do. */
	if (thread_obj->run_loop_mode)
		CFRelease(thread_obj->run_loop_mode);

    if (thread_obj->source)
		CFRelease(thread_obj->source);

	/* Clean up the thread objects */
	pthread_barrier_destroy(&thread_obj->barrier);
	pthread_barrier_destroy(&thread_obj->shutdown_barrier);

	pthread_mutex_destroy(&thread_obj->mutex);

	free(thread_obj);

}

static void *read_thread(void *param);

static void start_thread_object(thread_object *thread_obj)
{
    char str[32];
    
	/* Create the Run Loop Mode for this thread */
	sprintf(str, "HIDAPI_%p", (void*) thread_obj);
    thread_obj->run_loop_mode =
		CFStringCreateWithCString(NULL, str, kCFStringEncodingASCII);
    
	/* Start the read thread */
	pthread_create(&thread_obj->thread, NULL, read_thread, thread_obj);

	/* Wait here for the read thread to be initialized. */
	pthread_barrier_wait(&thread_obj->barrier);

}

static void stop_thread_object(thread_object *thread_obj)
{
	/* Cause read_thread() to stop. */
    thread_obj->shutdown_thread = 1;

	/* Wake up the run thread's event loop so that the thread can exit. */
	CFRunLoopSourceSignal(thread_obj->source);
	CFRunLoopWakeUp(thread_obj->run_loop);

	/* Notify the read thread that it can shut down now. */
	pthread_barrier_wait(&thread_obj->shutdown_barrier);

	/* Wait for read_thread() to end. */
	pthread_join(thread_obj->thread, NULL);

}

/* This gets called when the read_thread's run loop gets signaled by
   hid_close(), and serves to stop the read_thread's run loop. */
static void perform_signal_callback(void *context)
{
	thread_object *thread_obj = (thread_object*) context;
	CFRunLoopStop(thread_obj->run_loop); /*TODO: CFRunLoopGetCurrent()*/
}


static void *read_thread(void *param)
{
	thread_object *thread_obj = (thread_object *)param;

	SInt32 code;

	/* Create the RunLoopSource which is used to signal the
	   event loop to stop when hid_exit() is called. */
	CFRunLoopSourceContext ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.version = 0;
	ctx.info = thread_obj;
	ctx.perform = &perform_signal_callback;
    thread_obj->source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0/*order*/, &ctx);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), thread_obj->source, thread_obj->run_loop_mode);

	/* Store off the Run Loop so it can be stopped from hid_close(). */
    thread_obj->run_loop = CFRunLoopGetCurrent();
    
    if (thread_obj->on_thread_starting)
    {
        thread_obj->on_thread_starting(thread_obj);
    }

	/* Notify the main thread that the read thread is up and running. */
	pthread_barrier_wait(&thread_obj->barrier);
    
	/* Run the Event Loop. CFRunLoopRunInMode() will dispatch HID input
	   reports into the hid_report_callback(). */
	while (!thread_obj->shutdown_thread) {
		code = CFRunLoopRunInMode(thread_obj->run_loop_mode, 1000/*sec*/, FALSE);
		/* Return if the device has been disconnected */
		if (code == kCFRunLoopRunFinished) {
			break;
		}


		/* Break if The Run Loop returns Finished or Stopped. */
		if (code != kCFRunLoopRunTimedOut &&
		    code != kCFRunLoopRunHandledSource) {
			/* There was some kind of error. Setting
			   shutdown seems to make sense, but
			   there may be something else more appropriate */
			break;
		}
	}

    thread_obj->shutdown_thread = 1;
	if (thread_obj->on_thread_exiting)
	{
        thread_obj->on_thread_exiting(thread_obj);
	}


	/* Wait here until hid_close() is called and makes it past
	   the call to CFRunLoopWakeUp(). This thread still needs to
	   be valid when that function is called on the other thread. */
	pthread_barrier_wait(&thread_obj->shutdown_barrier);

	return NULL;
}




