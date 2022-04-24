
/* Barrier implementation because Mac OSX doesn't have pthread_barrier.
   It also doesn't have clock_gettime(). So much for POSIX and SUSv2.
   This implementation came from Brent Priddy and was posted on
   StackOverflow. It is used with his permission. */
typedef int pthread_barrierattr_t;
typedef struct pthread_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int trip_count;
} pthread_barrier_t;

static int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
    (void) attr;

    if(count == 0) {
        errno = EINVAL;
        return -1;
    }

    if(pthread_mutex_init(&barrier->mutex, 0) < 0) {
        return -1;
    }
    if(pthread_cond_init(&barrier->cond, 0) < 0) {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }
    barrier->trip_count = count;
    barrier->count = 0;

    return 0;
}

static int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

static int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    ++(barrier->count);
    if(barrier->count >= barrier->trip_count)
    {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &(barrier->mutex));
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}
