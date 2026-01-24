/*
 * Copyright 2002-2014, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <StorageDefs.h>

#include <errno_private.h>
#include <find_directory_private.h>


static status_t
get_settings_path(char *path, const char* leaf, bool create)
{
	status_t status = __find_directory(B_SYSTEM_SETTINGS_DIRECTORY, -1, create,
		path, B_PATH_NAME_LENGTH);
	if (status != B_OK)
		return status;

	strlcat(path, "/network", B_PATH_NAME_LENGTH);
	if (create)
		mkdir(path, 0755);
	strlcat(path, "/", B_PATH_NAME_LENGTH);
	strlcat(path, leaf, B_PATH_NAME_LENGTH);
	return B_OK;
}


extern "C" int
sethostname(const char *hostName, size_t nameSize)
{
	char path[B_PATH_NAME_LENGTH];
	if (get_settings_path(path, "hostname", false) != B_OK) {
		__set_errno(B_ERROR);
		return -1;
	}

	int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file < 0)
		return -1;

	nameSize = min_c(nameSize, MAXHOSTNAMELEN);

	if (write(file, hostName, nameSize) != (ssize_t)nameSize
		|| write(file, "\n", 1) != 1) {
		close(file);
		return -1;
	}

	close(file);
	return 0;
}


extern "C" int
gethostname(char *hostName, size_t nameSize)
{
	// look up hostname from network settings hostname file

	char path[B_PATH_NAME_LENGTH];
	if (get_settings_path(path, "hostname", false) != B_OK) {
		__set_errno(B_ERROR);
		return -1;
	}

	int file = open(path, O_RDONLY);
	if (file < 0)
		return -1;

	nameSize = min_c(nameSize, MAXHOSTNAMELEN);

	int length = read(file, hostName, nameSize - 1);
	close(file);

	if (length < 0)
		return -1;

	hostName[length] = '\0';

	char *end = strpbrk(hostName, "\r\n\t");
	if (end != NULL)
		end[0] = '\0';

	return 0;
}


extern "C" long
gethostid(void)
{
	char path[B_PATH_NAME_LENGTH];
	if (get_settings_path(path, "hostid", false) != B_OK)
		return 0;

	int file = open(path, O_RDONLY);
	if (file < 0)
		return 0;

	char buffer[32];
	ssize_t length = read(file, buffer, sizeof(buffer) - 1);
	close(file);

	if (length <= 0)
		return 0;

	buffer[length] = '\0';
	return strtol(buffer, NULL, 16);
}


extern "C" int
sethostid(long hostid)
{
	if (geteuid() != 0) {
		__set_errno(EPERM);
		return -1;
	}

	char path[B_PATH_NAME_LENGTH];
	if (get_settings_path(path, "hostid", false) != B_OK) {
		__set_errno(B_ERROR);
		return -1;
	}

	int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file < 0)
		return -1;

	char buffer[32];
	int length = snprintf(buffer, sizeof(buffer), "%lx\n", hostid);

	if (write(file, buffer, length) != length) {
		close(file);
		return -1;
	}

	close(file);
	return 0;
}
