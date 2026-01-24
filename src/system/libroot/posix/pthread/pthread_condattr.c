/*
 * Copyright 2007, Ryan Leavengood, leavengood@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <pthread.h>
#include "pthread_private.h"

#include <stdlib.h>


int 
pthread_condattr_init(pthread_condattr_t *_condAttr)
{
	pthread_condattr *attr;

	if (_condAttr == NULL)
		return EINVAL;

	attr = (pthread_condattr *)malloc(sizeof(pthread_condattr));
	if (attr == NULL)
		return ENOMEM;

	attr->process_shared = false;
	attr->clock_id = CLOCK_REALTIME;

	*_condAttr = attr;
	return 0;
}


int 
pthread_condattr_destroy(pthread_condattr_t *_condAttr)
{
	pthread_condattr *attr;

	if (_condAttr == NULL || (attr = *_condAttr) == NULL)
		return EINVAL;

	*_condAttr = NULL;
	free(attr);

	return 0;
}


int 
pthread_condattr_getpshared(const pthread_condattr_t *_condAttr, int *_processShared)
{
	pthread_condattr *attr;

	if (_condAttr == NULL || (attr = *_condAttr) == NULL || _processShared == NULL)
		return EINVAL;

	*_processShared = attr->process_shared ? PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;
	return 0;
}


int 
pthread_condattr_setpshared(pthread_condattr_t *_condAttr, int processShared)
{
	pthread_condattr *attr;

	if (_condAttr == NULL || (attr = *_condAttr) == NULL
		|| processShared < PTHREAD_PROCESS_PRIVATE
		|| processShared > PTHREAD_PROCESS_SHARED)
		return EINVAL;

	attr->process_shared = processShared == PTHREAD_PROCESS_SHARED ? true : false;
	return 0;
}


int
pthread_condattr_getclock(const pthread_condattr_t *_condAttr, clockid_t *_clockID)
{
	pthread_condattr *attr;

	if (_condAttr == NULL || (attr = *_condAttr) == NULL || _clockID == NULL)
		return EINVAL;

	*_clockID = attr->clock_id;
	return 0;
}


int
pthread_condattr_setclock(pthread_condattr_t *_condAttr, clockid_t clockID)
{
	pthread_condattr *attr;

	if (_condAttr == NULL || (attr = *_condAttr) == NULL
		|| (clockID != CLOCK_REALTIME && clockID != CLOCK_MONOTONIC))
		return EINVAL;

	attr->clock_id = clockID;
	return 0;
}
