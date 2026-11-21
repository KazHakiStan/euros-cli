#include "input.h"
#include "platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_BYTES 8192
#define QUEUE_SIZE 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *queue[QUEUE_SIZE];
static size_t head = 0;
static size_t count = 0;
static bool eof = false;

static void *reader(void *arg)
{
    char line[LINE_MAX_BYTES];
    while (platform_read_line(line, sizeof line))
    {
        char *copy = strdup(line);
        pthread_mutex_lock(&lock);
        if (copy != NULL && count < QUEUE_SIZE)
        {
            queue[(head + count) % QUEUE_SIZE] = copy;
            ++count;
            copy = NULL;
        }
        pthread_mutex_unlock(&lock);
        free(copy); // dropped if the queue is full
    }
    pthread_mutex_lock(&lock);
    eof = true;
    pthread_mutex_unlock(&lock);
    return NULL;
}

bool input_start(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, reader, NULL) != 0)
    {
        return false;
    }
    pthread_detach(thread);
    return true;
}

bool input_next(char *out, size_t size)
{
    pthread_mutex_lock(&lock);
    char *line = NULL;
    if (count > 0)
    {
        line = queue[head];
        head = (head + 1) % QUEUE_SIZE;
        --count;
    }
    pthread_mutex_unlock(&lock);
    if (line == NULL)
    {
        return false;
    }
    snprintf(out, size, "%s", line);
    free(line);
    return true;
}

bool input_finished(void)
{
    pthread_mutex_lock(&lock);
    bool done = eof && count == 0;
    pthread_mutex_unlock(&lock);
    return done;
}
