/* 
 * Copyright 2003-2007, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <pthread.h>
#include "pthread_private.h"

#include <stdlib.h>


int 
pthread_mutexattr_init(pthread_mutexattr_t *_mutexAttr)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL)
		return EINVAL;

	attr = (pthread_mutexattr *)malloc(sizeof(pthread_mutexattr));
	if (attr == NULL)
		return ENOMEM;

	attr->type = PTHREAD_MUTEX_DEFAULT;
	attr->process_shared = false;

	*_mutexAttr = attr;
	return 0;
}


int 
pthread_mutexattr_destroy(pthread_mutexattr_t *_mutexAttr)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL)
		return EINVAL;

	*_mutexAttr = NULL;
	free(attr);

	return 0;
}


int 
pthread_mutexattr_gettype(const pthread_mutexattr_t *_mutexAttr, int *_type)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL || _type == NULL)
		return EINVAL;

	*_type = attr->type;
	return 0;
}


int 
pthread_mutexattr_settype(pthread_mutexattr_t *_mutexAttr, int type)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL
		|| type < PTHREAD_MUTEX_DEFAULT
		|| type > PTHREAD_MUTEX_RECURSIVE)
		return EINVAL;

	attr->type = type;
	return 0;
}


int 
pthread_mutexattr_getpshared(const pthread_mutexattr_t *_mutexAttr,
	int *_processShared)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL
		|| _processShared == NULL) {
		return EINVAL;
	}

	*_processShared = attr->process_shared ? PTHREAD_PROCESS_SHARED
		: PTHREAD_PROCESS_PRIVATE;
	return 0;
}


int 
pthread_mutexattr_setpshared(pthread_mutexattr_t *_mutexAttr,
	int processShared)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL
		|| processShared < PTHREAD_PROCESS_PRIVATE
		|| processShared > PTHREAD_PROCESS_SHARED) {
		return EINVAL;
	}

	attr->process_shared = processShared == PTHREAD_PROCESS_SHARED;
	return 0;
}


int 
pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *_mutexAttr,
	int *_priorityCeiling)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL
		|| _priorityCeiling == NULL) {
		return EINVAL;
	}

	*_priorityCeiling = 0;
		// not implemented

	return 0;
}


int 
pthread_mutexattr_setprioceiling(pthread_mutexattr_t *_mutexAttr,
	int priorityCeiling)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL)
		return EINVAL;

	// not implemented
	return EPERM;
}


int 
pthread_mutexattr_getprotocol(const pthread_mutexattr_t *_mutexAttr,
	int *_protocol)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL
		|| _protocol == NULL) {
		return EINVAL;
	}

	*_protocol = 0;
		// not implemented

	return 0;
}


int 
pthread_mutexattr_setprotocol(pthread_mutexattr_t *_mutexAttr, int protocol)
{
	pthread_mutexattr *attr;

	if (_mutexAttr == NULL || (attr = *_mutexAttr) == NULL)
		return EINVAL;

	// not implemented
	return EPERM;
}
