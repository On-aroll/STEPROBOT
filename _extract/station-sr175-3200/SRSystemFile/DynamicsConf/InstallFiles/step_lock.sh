#!/bin/sh

FILEPATH="/mnt/step/"
FILEPATH1="/mnt/step.zip"
FILEPATH2="/mnt/.backup.zip"
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
##判断step文件夹是否存在，若不存在，继续判断是否存在step.zip
if [ ! -d "$FILEPATH" ]; then
##若step.zip不存在，返回错误，严重退出
	if [ ! -f "$FILEPATH1" ]; then
		echo "1"
		exit 0
##若存在step.zip，则直接加密退出
	else
		cd $MNTPATH

		./encryptZip && rm -rf $FILEPATH

		encrypt
		echo "0"
	fi
else

	cd $MNTPATH

	rm -rf $FILEPATH1

	./encryptZip && rm -rf $FILEPATH

	encrypt
	echo "0"
fi

}

foo
