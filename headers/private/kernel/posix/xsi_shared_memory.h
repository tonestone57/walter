/*
 * Copyright 2024, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_XSI_SHARED_MEMORY_H
#define KERNEL_XSI_SHARED_MEMORY_H

#include <sys/shm.h>
#include <sys/cdefs.h>

#include <OS.h>

#include <kernel.h>


struct Team;
struct xsi_shm_context;

__BEGIN_DECLS

extern void xsi_shm_init();
extern void xsi_shm_fork_team(struct Team* parent, struct Team* child);
extern void xsi_shm_exec_team(struct Team* team);
extern void xsi_shm_exit_team(struct Team* team);

/* user calls */
int _user_xsi_shmget(key_t key, size_t size, int shmflg);
void *_user_xsi_shmat(int shmid, const void *shmaddr, int shmflg);
int _user_xsi_shmdt(const void *shmaddr);
int _user_xsi_shmctl(int shmid, int cmd, struct shmid_ds *buf);

__END_DECLS

#endif	/* KERNEL_XSI_SHARED_MEMORY_H */
