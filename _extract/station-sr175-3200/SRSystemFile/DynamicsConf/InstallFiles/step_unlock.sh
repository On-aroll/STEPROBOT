#!/bin/sh

####################################################################################################
#	Author: STEP_XK
#	Copy Rights: Shanghai STEP Robots Company
####################################################################################################

#####################################################################
#  Call REPLACE to uncrypted the whole package
#####################################################################

FILEPATH="/mnt/step/"
FILEPATH1="/mnt/step.zip"
FILEPATH2="/mnt/.backup.zip"
MAPPATH="/dev/mapper/disk1"
MNTPATH="/mnt"

encrypt(){
cd /
umount $MNTPATH
replace <<EOT 1>>/dev/null 2>/dev/null
4
6
EOT
}

foo(){

## call replace to uncrypted the disk
#echo START
replace <<EOT 1>>/dev/null 2>/dev/null
3
6
EOT
#echo FINISH
#exit 1

## mount the package
mount $MAPPATH $MNTPATH

## unzip

if [ ! -f "$FILEPATH1" ]; then
	if [ ! -f "$FILEPATH2" ]; then
		echo "2"
		exit 0
	fi
	echo "1"
#	encrypt
	cp $FILEPATH2 $FILEPATH1
	exit 0
fi

cd $MNTPATH

if [ ! -f "$FILEPATH2" ]; then
	cp $FILEPATH1 $FILEPATH2
fi

rm -rf $FILEPATH

unzip -q -P STEPROBOTSRH step.zip

echo "0"
## 
}

foo
