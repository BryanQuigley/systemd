/*
 * namedev.h
 *
 * Userspace devfs
 *
 * Copyright (C) 2003,2004 Greg Kroah-Hartman <greg@kroah.com>
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 * 
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 * 
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef NAMEDEV_H
#define NAMEDEV_H

#include "udev.h"
#include "list.h"

struct sysfs_class_device;

#define FIELD_KERNEL		"KERNEL"
#define FIELD_SUBSYSTEM		"SUBSYSTEM"
#define FIELD_BUS		"BUS"
#define FIELD_SYSFS		"SYSFS"
#define FIELD_ID		"ID"
#define FIELD_PLACE		"PLACE"
#define FIELD_PROGRAM		"PROGRAM"
#define FIELD_RESULT		"RESULT"
#define FIELD_DRIVER		"DRIVER"
#define FIELD_NAME		"NAME"
#define FIELD_SYMLINK		"SYMLINK"
#define FIELD_OWNER		"OWNER"
#define FIELD_GROUP		"GROUP"
#define FIELD_MODE		"MODE"
#define FIELD_OPTIONS		"OPTIONS"

#define OPTION_IGNORE_DEVICE	"ignore_device"
#define OPTION_IGNORE_REMOVE	"ignore_remove"
#define OPTION_PARTITIONS	"all_partitions"

#define MAX_SYSFS_PAIRS		5

#define RULEFILE_SUFFIX		".rules"

struct sysfs_pair {
	char file[PATH_SIZE];
	char value[VALUE_SIZE];
};

struct config_device {
	struct list_head node;

	char kernel[NAME_SIZE];
	char subsystem[NAME_SIZE];
	char bus[NAME_SIZE];
	char id[NAME_SIZE];
	char place[NAME_SIZE];
	struct sysfs_pair sysfs_pair[MAX_SYSFS_PAIRS];
	char program[PATH_SIZE];
	char result[PATH_SIZE];
	char driver[NAME_SIZE];
	char name[PATH_SIZE];
	char symlink[PATH_SIZE];

	char owner[USER_SIZE];
	char group[USER_SIZE];
	mode_t mode;

	int partitions;
	int ignore_device;
	int ignore_remove;

	char config_file[PATH_SIZE];
	int config_line;
};

extern struct list_head config_device_list;

extern int namedev_init(void);
extern int namedev_name_device(struct udevice *udev, struct sysfs_class_device *class_dev);
extern void namedev_close(void);

extern void dump_config_dev(struct config_device *dev);
extern void dump_config_dev_list(void);

#endif
