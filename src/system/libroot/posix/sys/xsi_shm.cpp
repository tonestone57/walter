/*
 * Copyright 2024, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <sys/shm.h>

#include <errno.h>
#include <OS.h>

#include <errno_private.h>
#include <syscall_utils.h>
#include <syscalls.h>


int
shmget(key_t key, size_t size, int shmflg)
{
	RETURN_AND_SET_ERRNO(_kern_xsi_shmget(key, size, shmflg));
}


void *
shmat(int shmid, const void *shmaddr, int shmflg)
{
	void *address = _kern_xsi_shmat(shmid, shmaddr, shmflg);
	if ((status_t)(addr_t)address < 0) {
		__set_errno((status_t)(addr_t)address);
		return (void *)-1;
	}
	return address;
}


int
shmdt(const void *shmaddr)
{
	RETURN_AND_SET_ERRNO(_kern_xsi_shmdt(shmaddr));
}


int
shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	RETURN_AND_SET_ERRNO(_kern_xsi_shmctl(shmid, cmd, buf));
}
