#! /bin/sh

# This script generates and sends to stdout a set of udev.rules for use
# with all scsi block devices on your system. It creates a udev key NAME
# with prefix defaulting to "disk-", and appends the current kernel name
# and the udev kernel number (the partition number, empty for the entire
# disk).
#
# Managing these is probably better done via a gui interface.
#
# You can edit and append the output to your /etc/udev/udev.rules file.
# You probably want to to change names to be non-kernel defaults, so as to
# avoid confusion if a configuration change modifies /sys/block/sd*
# naming.
#
# /etc/scsi_id.config must be properly configured. If you are using this
# script, you probably want a single line enabling scsi_id for all
# devices as follows:
#
# options=-g
#
# The above assumes you will not attach block devices that do not
# properly support the page codes used by scsi_id, this is especially true
# of many USB mass storage devices (mainly flash card readers).
#

prefix=disk-
scsi_id=/sbin/scsi_id

dump_ids()
{
	cd ${sysfs_dir}/block
	for b in sd*
	do
		echo -n "$b "
		$scsi_id -s /block/$b
		if [ $? != 0 ]
		then
			echo $0 failed for device $b >&2
			exit 1
		fi
	done
}

sysfs_dir=$(mount | awk '$5 == "sysfs" {print $3}')

c=$(ls /${sysfs_dir}/block/sd* 2>/dev/null | wc -l)
if [ $c = 0 ]
then
	echo $0 no block devices present >&2
	exit 1
fi

echo "#"
echo "# Start of autogenerated scsi_id rules. Edit the NAME portions of these"
echo "# rules to your liking."
echo "#"
first_line=yes
dump_ids | while read in
do
	set $in
	name=$1
	shift
	id="$*"
	if [ $first_line = "yes" ]
	then
		first_line=no
		echo "BUS=\"scsi\", PROGRAM=\"${scsi_id}\", RESULT=\"${id}\", NAME=\"${prefix}${name}%n\""
		echo
		echo "# Further RESULT keys use the result of the last PROGRAM rule."
		echo "# Be careful not to add any rules containing PROGRAM key between here"
		echo "# and the end of this section"
		echo
	else
		# No PROGRAM, so just use the last result of PROGRAM. The
		# following is the same as the above without the PROGRAM
		# key.
		echo "BUS=\"scsi\", RESULT=\"${id}\", NAME=\"${prefix}${name}%n\""
	fi

done

echo "#"
echo "# End of autogenerated scsi_id rules"
echo "#"
