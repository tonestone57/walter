/*
 * Copyright 2024, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/ipc.h>
#include <sys/types.h>
#include <time.h>
#include <OS.h>

/* Operation flags for shmget() */
#define SHM_R		0400		/* Read permission */
#define SHM_W		0200		/* Write permission */

/* Operation flags for shmat() */
#define SHM_RDONLY	010000		/* Attach read-only */
#define SHM_RND		020000		/* Round attach address to SHMLBA */
#define SHM_REMAP	040000		/* Take-over region on attach */
#define SHM_EXEC	0100000		/* Execution access */

/* Commands for shmctl() */
#define SHM_LOCK	11			/* Lock segment (root only) */
#define SHM_UNLOCK	12			/* Unlock segment (root only) */

/* Segment low boundary address multiple */
#define SHMLBA		B_PAGE_SIZE	/* Attach address must be page-aligned */

typedef unsigned long shmatt_t;

struct shmid_ds {
	struct ipc_perm		shm_perm;	/* Operation permission structure */
	size_t				shm_segsz;	/* Size of segment in bytes */
	pid_t				shm_lpid;	/* Process ID of last shared memory op */
	pid_t				shm_cpid;	/* Process ID of creator */
	shmatt_t			shm_nattch;	/* Number of current attaches */
	time_t				shm_atime;	/* Time of last shmat() */
	time_t				shm_dtime;	/* Time of last shmdt() */
	time_t				shm_ctime;	/* Time of last change by shmctl() */
};

__BEGIN_DECLS

void* shmat(int shmId, const void* shmAddr, int shmFlg);
int shmctl(int shmId, int command, struct shmid_ds* buf);
int shmdt(const void* shmAddr);
int shmget(key_t key, size_t size, int shmFlg);

__END_DECLS

#endif	/* _SYS_SHM_H */
