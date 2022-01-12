#!/bin/bash

mkdir -p build

# This is generas more or less the same command line as gimptool-2.0, except for the fact that we don't use pango
cc \
	-Wno-deprecated-declarations \
	-O2 \
	src/file-qoi.c \
	`pkg-config --cflags --libs glib-2.0 gtk+-2.0 gimp-2.0 gimpui-2.0` \
	-o build/file-qoi

DIR="/usr/lib/gimp/2.0/plug-in/file-qoi"
mkdir -p $DIR
cp build/file-qoi "${DIR}/file-qoi"
