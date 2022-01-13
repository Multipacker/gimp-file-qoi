#!/bin/bash

# If no operation is specified, default to building
operation=${1:-build}

if [ "$operation" == "build" ]; then
	mkdir --parents build

	# This is generas more or less the same command line as gimptool-2.0, except for the fact that we don't use pango
	cc \
		-Wno-deprecated-declarations \
		-O2 \
		src/file-qoi.c \
		`pkg-config --cflags --libs glib-2.0 gtk+-2.0 gimp-2.0 gimpui-2.0` \
		-o build/file-qoi
elif [ "$operation" == "install" ]; then
	DIR="/usr/lib/gimp/2.0/plug-ins/file-qoi"
	sudo mkdir --parents $DIR
	sudo cp build/file-qoi "${DIR}/file-qoi"
else
	echo "$0: Unknown operation '$operation'"
fi
