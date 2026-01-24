/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <posix/xsi_shared_memory.h>

#include <new>

#include <sys/ipc.h>
#include <sys/types.h>

#include <OS.h>

#include <kernel.h>
#include <syscall_restart.h>
#include <team.h>
#include <thread.h>
#include <thread_types.h>

#include <util/atomic.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/OpenHashTable.h>

#include <vm/vm.h>
#include <vm/VMAddressSpace.h>
#include <vm/vm_types.h>


#define TRACE_XSI_SHARED_MEMORY
#ifdef TRACE_XSI_SHARED_MEMORY
#	define TRACE(x)			dprintf x
#	define TRACE_ERROR(x)	dprintf x
#else
#	define TRACE(x)			/* nothing */
#	define TRACE_ERROR(x)	dprintf x
#endif


namespace {

// Arbitrary limit
#define MAX_XSI_SHARED_MEMORY	1024

class XsiSharedMemory {
public:
	XsiSharedMemory(int flags, size_t size)
		:
		fID(-1),
		fAreaID(-1),
		fMarkedForDeletion(false),
		fLink(NULL),
		fAreaLink(NULL)
	{
		mutex_init(&fLock, "XsiSharedMemory private mutex");

		SetIpcKey((key_t)-1);
		SetPermissions(flags);

		memset((void *)&fSettings, 0, sizeof(struct shmid_ds));
		fSettings.shm_ctime = (time_t)real_time_clock();
		fSettings.shm_segsz = size;
		fSettings.shm_cpid = team_get_current_team_id();
		fSettings.shm_lpid = 0;
		fSettings.shm_nattch = 0;
		fSettings.shm_atime = 0;
		fSettings.shm_dtime = 0;
	}

	~XsiSharedMemory()
	{
		if (fAreaID >= 0)
			vm_delete_area(VMAddressSpace::KernelID(), fAreaID, true);
		mutex_destroy(&fLock);
	}

	void DoIpcSet(struct shmid_ds *result)
	{
		fSettings.shm_perm.uid = result->shm_perm.uid;
		fSettings.shm_perm.gid = result->shm_perm.gid;
		fSettings.shm_perm.mode = (fSettings.shm_perm.mode & ~0x01ff)
			| (result->shm_perm.mode & 0x01ff);
		fSettings.shm_ctime = (time_t)real_time_clock();
	}

	struct shmid_ds &GetShmDs()
	{
		return fSettings;
	}

	bool HasPermission() const
	{
		// Write permission check
		if ((fSettings.shm_perm.mode & S_IWOTH) != 0)
			return true;

		uid_t uid = team_geteuid(team_get_current_team_id());
		if (uid == 0 || (uid == fSettings.shm_perm.uid
			&& (fSettings.shm_perm.mode & S_IWUSR) != 0))
			return true;

		gid_t gid = team_get_effective_gid(team_get_current_team_id());
		if (gid == fSettings.shm_perm.gid
			&& (fSettings.shm_perm.mode & S_IWGRP) != 0)
			return true;

		return false;
	}

	bool HasReadPermission() const
	{
		if ((fSettings.shm_perm.mode & S_IROTH) != 0)
			return true;

		uid_t uid = team_geteuid(team_get_current_team_id());
		if (uid == 0 || (uid == fSettings.shm_perm.uid
			&& (fSettings.shm_perm.mode & S_IRUSR) != 0))
			return true;

		gid_t gid = team_get_effective_gid(team_get_current_team_id());
		if (gid == fSettings.shm_perm.gid
			&& (fSettings.shm_perm.mode & S_IRGRP) != 0)
			return true;

		return false;
	}

	bool IsOwner() const
	{
		uid_t uid = team_geteuid(team_get_current_team_id());
		if (uid == 0 || uid == fSettings.shm_perm.uid
			|| uid == fSettings.shm_perm.cuid)
			return true;
		return false;
	}

	int ID() const
	{
		return fID;
	}

	key_t IpcKey() const
	{
		return fSettings.shm_perm.key;
	}

	mutex &Lock()
	{
		return fLock;
	}

	void SetID(int id)
	{
		fID = id;
	}

	void SetIpcKey(key_t key)
	{
		fSettings.shm_perm.key = key;
	}

	void SetPermissions(int flags)
	{
		fSettings.shm_perm.uid = fSettings.shm_perm.cuid
			= team_geteuid(team_get_current_team_id());
		fSettings.shm_perm.gid = fSettings.shm_perm.cgid
			= team_get_effective_gid(team_get_current_team_id());
		fSettings.shm_perm.mode = (flags & 0x01ff);
	}

	area_id AreaID() const { return fAreaID; }
	void SetAreaID(area_id id) { fAreaID = id; }

	bool IsMarkedForDeletion() const { return fMarkedForDeletion; }
	void SetMarkedForDeletion(bool marked) { fMarkedForDeletion = marked; }

	XsiSharedMemory*& Link() { return fLink; }
	XsiSharedMemory*& AreaLink() { return fAreaLink; }

private:
	int					fID;
	area_id				fAreaID;
	bool				fMarkedForDeletion;
	mutex				fLock;
	struct shmid_ds		fSettings;
	XsiSharedMemory*	fLink;
	XsiSharedMemory*	fAreaLink;
};


// Shared Memory hash table definitions
struct SharedMemoryHashTableDefinition {
	typedef int					KeyType;
	typedef XsiSharedMemory		ValueType;

	size_t HashKey (const int key) const { return (size_t)key; }
	size_t Hash(XsiSharedMemory *variable) const { return (size_t)variable->ID(); }
	bool Compare(const int key, XsiSharedMemory *variable) const
		{ return (int)key == (int)variable->ID(); }
	XsiSharedMemory*& GetLink(XsiSharedMemory *variable) const
		{ return variable->Link(); }
};

struct AreaIdHashTableDefinition {
	typedef area_id				KeyType;
	typedef XsiSharedMemory		ValueType;

	size_t HashKey (const area_id key) const { return (size_t)key; }
	size_t Hash(XsiSharedMemory *variable) const { return (size_t)variable->AreaID(); }
	bool Compare(const area_id key, XsiSharedMemory *variable) const
		{ return key == variable->AreaID(); }
	XsiSharedMemory*& GetLink(XsiSharedMemory *variable) const
		{ return variable->AreaLink(); }
};


// IPC class
class Ipc {
public:
	Ipc(key_t key) : fKey(key), fSharedMemoryId(-1), fLink(NULL) {}

	key_t Key() const { return fKey; }
	int SharedMemoryID() const { return fSharedMemoryId; }
	void SetSharedMemoryID(XsiSharedMemory *shm) { fSharedMemoryId = shm->ID(); }
	Ipc*& Link() { return fLink; }

private:
	key_t				fKey;
	int					fSharedMemoryId;
	Ipc*				fLink;
};


struct IpcHashTableDefinition {
	typedef key_t	KeyType;
	typedef Ipc		ValueType;

	size_t HashKey (const key_t key) const { return (size_t)(key); }
	size_t Hash(Ipc *variable) const { return (size_t)HashKey(variable->Key()); }
	bool Compare(const key_t key, Ipc *variable) const
		{ return (key_t)key == (key_t)variable->Key(); }
	Ipc*& GetLink(Ipc *variable) const { return variable->Link(); }
};

} // namespace


static BOpenHashTable<IpcHashTableDefinition> sIpcHashTable;
static BOpenHashTable<SharedMemoryHashTableDefinition> sSharedMemoryHashTable;
static BOpenHashTable<AreaIdHashTableDefinition> sAreaIdHashTable;

static mutex sIpcLock;
static mutex sXsiSharedMemoryLock;

static int32 sXsiSharedMemoryCount = 0;


struct shm_attachment : DoublyLinkedListLinkImpl<shm_attachment> {
	addr_t address;
	XsiSharedMemory* shm;
};

struct xsi_shm_context {
	mutex lock;
	DoublyLinkedList<shm_attachment> attachments;

	xsi_shm_context()
	{
		mutex_init(&lock, "xsi shm context");
	}

	~xsi_shm_context()
	{
		mutex_destroy(&lock);
	}
};


//	#pragma mark - Team Functions


void
_DetachXsiShm(shm_attachment* attachment)
{
	XsiSharedMemory* shm = attachment->shm;
	MutexLocker hashLocker(sXsiSharedMemoryLock);
	// We don't check if shm exists in hash, we rely on the pointer being valid
	// because we hold a reference (logically) via nattch.

	MutexLocker objectLocker(shm->Lock());

	if (shm->GetShmDs().shm_nattch > 0)
		shm->GetShmDs().shm_nattch--;

	shm->GetShmDs().shm_dtime = real_time_clock();
	// We can't easily set lpid here as this might be called during team destruction

	if (shm->IsMarkedForDeletion() && shm->GetShmDs().shm_nattch == 0) {
		sAreaIdHashTable.Remove(shm);
		atomic_add(&sXsiSharedMemoryCount, -1);
		objectLocker.Unlock();
		hashLocker.Unlock();
		delete shm;
	} else {
		objectLocker.Unlock();
		hashLocker.Unlock();
	}
}


void
xsi_shm_fork_team(Team* parent, Team* child)
{
	if (parent->xsi_shm_context == NULL)
		return;

	xsi_shm_context* parentContext = parent->xsi_shm_context;
	MutexLocker parentLocker(parentContext->lock);

	if (parentContext->attachments.IsEmpty())
		return;

	xsi_shm_context* childContext = new(std::nothrow) xsi_shm_context;
	if (childContext == NULL)
		return; // ENOMEM, but we can't fail the fork easily here, just don't inherit

	for (DoublyLinkedList<shm_attachment>::Iterator it = parentContext->attachments.GetIterator();
			shm_attachment* attachment = it.Next();) {

		shm_attachment* newAttachment = new(std::nothrow) shm_attachment;
		if (newAttachment == NULL)
			continue;

		newAttachment->address = attachment->address;
		newAttachment->shm = attachment->shm;

		// Increment nattch
		XsiSharedMemory* shm = attachment->shm;
		MutexLocker objectLocker(shm->Lock());
		shm->GetShmDs().shm_nattch++;
		objectLocker.Unlock();

		childContext->attachments.Add(newAttachment);
	}

	child->xsi_shm_context = childContext;
}


void
xsi_shm_exec_team(Team* team)
{
	if (team->xsi_shm_context == NULL)
		return;

	xsi_shm_exit_team(team);
	team->xsi_shm_context = NULL;
}


void
xsi_shm_exit_team(Team* team)
{
	if (team->xsi_shm_context == NULL)
		return;

	xsi_shm_context* context = team->xsi_shm_context;
	MutexLocker locker(context->lock);

	while (shm_attachment* attachment = context->attachments.RemoveHead()) {
		_DetachXsiShm(attachment);
		delete attachment;
	}

	locker.Unlock();
	delete context;
	team->xsi_shm_context = NULL;
}


//	#pragma mark - Kernel exported API


void
xsi_shm_init()
{
	// Initialize hash tables
	status_t status = sIpcHashTable.Init();
	if (status != B_OK)
		panic("xsi_shm_init() failed to initialize ipc hash table\n");
	status =  sSharedMemoryHashTable.Init();
	if (status != B_OK)
		panic("xsi_shm_init() failed to initialize shared memory hash table\n");
	status =  sAreaIdHashTable.Init();
	if (status != B_OK)
		panic("xsi_shm_init() failed to initialize area id hash table\n");

	mutex_init(&sIpcLock, "global POSIX shared memory IPC table");
	mutex_init(&sXsiSharedMemoryLock, "global POSIX xsi shared memory table");
}


//	#pragma mark - Syscalls


int
_user_xsi_shmget(key_t key, size_t size, int flags)
{
	TRACE(("xsi_shmget: key = %d, size = %lu, flags = %d\n", (int)key, size, flags));
	XsiSharedMemory *shm = NULL;
	Ipc *ipcKey = NULL;
	bool isPrivate = true;
	bool create = true;

	if (key != IPC_PRIVATE) {
		isPrivate = false;
		MutexLocker _(sIpcLock);
		ipcKey = sIpcHashTable.Lookup(key);
		if (ipcKey == NULL || ipcKey->SharedMemoryID() == -1) {
			if (!(flags & IPC_CREAT)) {
				return -ENOENT;
			}
			if (ipcKey == NULL) {
				ipcKey = new(std::nothrow) Ipc(key);
				if (ipcKey == NULL) {
					return -ENOMEM;
				}
				sIpcHashTable.Insert(ipcKey);
			}
		} else {
			if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) {
				return -EEXIST;
			}
			int shmId = ipcKey->SharedMemoryID();

			MutexLocker locker(sXsiSharedMemoryLock);
			shm = sSharedMemoryHashTable.Lookup(shmId);
			if (shm == NULL) {
				return -EINVAL;
			}
			if (!shm->HasPermission()) {
				return -EACCES;
			}
			if (size > shm->GetShmDs().shm_segsz) {
				return -EINVAL;
			}
			create = false;
		}
	}

	if (create) {
		if (atomic_get(&sXsiSharedMemoryCount) >= MAX_XSI_SHARED_MEMORY) {
			return -ENOSPC;
		}

		// Create kernel area
		void* address = NULL;
		area_id areaID = vm_create_anonymous_area(VMAddressSpace::KernelID(),
			"xsi_shm_seg", size, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ | B_KERNEL_WRITE, 0, 0, NULL, NULL, true, &address);

		if (areaID < 0) {
			TRACE_ERROR(("xsi_shmget: failed to create kernel area: %s\n", strerror(areaID)));
			return areaID;
		}

		shm = new(std::nothrow) XsiSharedMemory(flags, size);
		if (shm == NULL) {
			vm_delete_area(VMAddressSpace::KernelID(), areaID, true);
			return -ENOMEM;
		}
		shm->SetAreaID(areaID);
		atomic_add(&sXsiSharedMemoryCount, 1);

		MutexLocker locker(sXsiSharedMemoryLock);
		int id = (int)real_time_clock();
		while (true) {
			if (sSharedMemoryHashTable.Lookup(id) == NULL)
				break;
			id++;
		}
		shm->SetID(id);

		if (isPrivate)
			shm->SetIpcKey((key_t)-1);
		else {
			shm->SetIpcKey(key);
			MutexLocker ipcLocker(sIpcLock);
			ipcKey = sIpcHashTable.Lookup(key);
			if (ipcKey) {
				ipcKey->SetSharedMemoryID(shm);
			}
		}
		sSharedMemoryHashTable.Insert(shm);
		sAreaIdHashTable.Insert(shm);
	}

	return shm->ID();
}


void *
_user_xsi_shmat(int shmid, const void *shmaddr, int shmflg)
{
	TRACE(("xsi_shmat: shmid = %d, shmaddr = %p, flags = %d\n", shmid, shmaddr, shmflg));

	MutexLocker hashLocker(sXsiSharedMemoryLock);
	XsiSharedMemory *shm = sSharedMemoryHashTable.Lookup(shmid);
	if (shm == NULL) {
		return (void *)EINVAL;
	}

	// We need to lock the object to update stats safely
	MutexLocker objectLocker(shm->Lock());
	hashLocker.Unlock();

	bool hasPermission = (shmflg & SHM_RDONLY)
		? shm->HasReadPermission()
		: (shm->HasReadPermission() && shm->HasPermission());

	if (!hasPermission) {
		return (void *)EACCES;
	}

	area_id sourceArea = shm->AreaID();

	// Determine protection
	uint32 protection = B_READ_AREA | B_WRITE_AREA;
	if (shmflg & SHM_RDONLY)
		protection = B_READ_AREA;

	void* address = (void*)shmaddr;
	uint32 addressSpec = B_ANY_ADDRESS;
	if (address != NULL) {
		if (shmflg & SHM_RND) {
			address = (void*)((addr_t)address & ~(B_PAGE_SIZE - 1));
		}
		addressSpec = B_EXACT_ADDRESS;
	}

	area_id newArea = vm_clone_area(VMAddressSpace::CurrentID(), "xsi_shm",
		&address, addressSpec, protection, REGION_NO_PRIVATE_MAP, sourceArea,
		false);

	if (newArea < 0) {
		return (void *)newArea;
	}

	// Register attachment
	Team* team = thread_get_current_thread()->team;

	// Check if context exists
	if (team->xsi_shm_context == NULL) {
		team->xsi_shm_context = new(std::nothrow) xsi_shm_context;
		if (team->xsi_shm_context == NULL) {
			vm_delete_area(VMAddressSpace::CurrentID(), newArea, true);
			return (void*)ENOMEM;
		}
	}

	xsi_shm_context* context = team->xsi_shm_context;
	MutexLocker contextLocker(context->lock);

	shm_attachment* attachment = new(std::nothrow) shm_attachment;
	if (attachment == NULL) {
		vm_delete_area(VMAddressSpace::CurrentID(), newArea, true);
		return (void*)ENOMEM;
	}
	attachment->address = (addr_t)address;
	attachment->shm = shm;
	context->attachments.Add(attachment);

	contextLocker.Unlock();

	shm->GetShmDs().shm_nattch++;
	shm->GetShmDs().shm_atime = real_time_clock();
	shm->GetShmDs().shm_lpid = team_get_current_team_id();

	return address;
}


int
_user_xsi_shmdt(const void *shmaddr)
{
	TRACE(("xsi_shmdt: shmaddr = %p\n", shmaddr));

	// Find area at address
	area_id area = _user_area_for((void*)shmaddr);
	if (area < 0) {
		return -EINVAL;
	}

	area_info info;
	status_t status = _user_get_area_info(area, &info);
	if (status != B_OK) {
		return -EINVAL;
	}

	// Lock global hash to lookup by source area
	MutexLocker hashLocker(sXsiSharedMemoryLock);
	XsiSharedMemory *shm = sAreaIdHashTable.Lookup(info.source_area);

	if (shm == NULL) {
		return -EINVAL;
	}

	MutexLocker objectLocker(shm->Lock());

	// Unregister attachment first
	Team* team = thread_get_current_thread()->team;
	if (team->xsi_shm_context != NULL) {
		xsi_shm_context* context = team->xsi_shm_context;
		MutexLocker contextLocker(context->lock);

		for (DoublyLinkedList<shm_attachment>::Iterator it = context->attachments.GetIterator();
				shm_attachment* attachment = it.Next();) {
			if (attachment->address == (addr_t)shmaddr) {
				context->attachments.Remove(attachment);
				delete attachment;
				break;
			}
		}
	}

	// Detach
	status = vm_delete_area(VMAddressSpace::CurrentID(), area, true);
	if (status != B_OK)
		return status;

	shm->GetShmDs().shm_nattch--;
	shm->GetShmDs().shm_dtime = real_time_clock();
	shm->GetShmDs().shm_lpid = team_get_current_team_id();

	if (shm->IsMarkedForDeletion() && shm->GetShmDs().shm_nattch == 0) {
		sAreaIdHashTable.Remove(shm);
		atomic_add(&sXsiSharedMemoryCount, -1);

		objectLocker.Unlock();
		hashLocker.Unlock();
		delete shm;
	} else {
		objectLocker.Unlock();
		hashLocker.Unlock();
	}

	return B_OK;
}


int
_user_xsi_shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	TRACE(("xsi_shmctl: shmid = %d, cmd = %d\n", shmid, cmd));

	MutexLocker hashLocker(sXsiSharedMemoryLock);
	XsiSharedMemory *shm = sSharedMemoryHashTable.Lookup(shmid);
	if (shm == NULL) {
		return -EINVAL;
	}

	MutexLocker objectLocker(shm->Lock());
	// We can unlock hash table for most ops, but for RMID we need it.
	// For STAT/SET we can unlock.
	if (cmd != IPC_RMID) {
		hashLocker.Unlock();
	}

	switch (cmd) {
		case IPC_STAT: {
			if (!shm->HasReadPermission()) {
				return -EACCES;
			}
			struct shmid_ds data = shm->GetShmDs();
			if (user_memcpy(buf, &data, sizeof(struct shmid_ds)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			break;
		}

		case IPC_SET: {
			if (!shm->IsOwner()) {
				return -EPERM;
			}
			struct shmid_ds data;
			if (user_memcpy(&data, buf, sizeof(struct shmid_ds)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			shm->DoIpcSet(&data);
			break;
		}

		case IPC_RMID: {
			if (!shm->IsOwner()) {
				hashLocker.Unlock();
				return -EPERM;
			}

			// Remove from IPC hash
			MutexLocker ipcLocker(sIpcLock);
			key_t key = shm->IpcKey();
			if (key != -1) {
				Ipc *ipc = sIpcHashTable.Lookup(key);
				if (ipc) {
					sIpcHashTable.Remove(ipc);
					delete ipc;
				}
			}
			ipcLocker.Unlock();

			// Remove from ID hash
			sSharedMemoryHashTable.Remove(shm);

			shm->SetMarkedForDeletion(true);

			if (shm->GetShmDs().shm_nattch == 0) {
				sAreaIdHashTable.Remove(shm);
				atomic_add(&sXsiSharedMemoryCount, -1);

				objectLocker.Unlock();
				hashLocker.Unlock();
				delete shm;
			} else {
				hashLocker.Unlock();
			}
			break;
		}

		default:
			return -EINVAL;
	}

	return B_OK;
}
