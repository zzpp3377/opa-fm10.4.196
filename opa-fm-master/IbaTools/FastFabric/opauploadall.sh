#!/bin/bash
# BEGIN_ICS_COPYRIGHT8 ****************************************
# 
# Copyright (c) 2015, Intel Corporation
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of Intel Corporation nor the names of its contributors
#       may be used to endorse or promote products derived from this software
#       without specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# 
# END_ICS_COPYRIGHT8   ****************************************

# [ICS VERSION STRING: unknown]

# optional override of defaults
if [ -f /etc/opa/opafastfabric.conf ]
then
	. /etc/opa/opafastfabric.conf
fi

. /usr/lib/opa/tools/opafastfabric.conf.def

. /usr/lib/opa/tools/ff_funcs

trap "exit 1" SIGHUP SIGTERM SIGINT

Usage_full()
{
	echo "Usage: opauploadall [-rp] [-f hostfile] [-d upload_dir] [-h 'hosts'] [-u user]" >&2
	echo "                       source_file ... dest_file" >&2
	echo "              or" >&2
	echo "       opauploadall --help" >&2
	echo "   --help - produce full help text" >&2
	echo "   -p - perform copy in parallel on all hosts" >&2
	echo "   -r - recursive upload of directories" >&2
	echo "   -f hostfile - file with hosts in cluster, default is $CONFIG_DIR/opa/hosts" >&2
	echo "   -h hosts - list of hosts to upload from" >&2
	echo "   -u user - user to perform copy to, default is current user code" >&2
	echo "   -d upload_dir - directory to upload to, default is uploads" >&2
	echo "   source_file - list of source files to copy" >&2
	echo "   dest_file - destination for copy" >&2
	echo "        If more than 1 source file, this must be a directory" >&2
	echo " Environment:" >&2
	echo "   HOSTS - list of hosts, used if -h option not supplied" >&2
	echo "   HOSTS_FILE - file containing list of hosts, used in absence of -f and -h" >&2
	echo "   UPLOADS_DIR - directory to upload to, used in absence of -d" >&2
	echo "   FF_MAX_PARALLEL - when -p option is used, maximum concurrent operations" >&2
	echo "example:">&2
	echo "   opauploadall -h 'arwen elrond' capture.tgz /etc/init.d/ipoib.cfg ." >&2
	echo "   opauploadall -p capture.tgz /etc/init.d/ipoib.cfg ." >&2
	echo "   opauploadall capture.tgz /etc/init.d/ipoib.cfg pre-install" >&2
	echo "user@ syntax cannot be used in filenames specified" >&2
	echo "A local directory within upload_dir/ will be created for each hostname." >&2
	echo "Destination file will be upload_dir/hostname/dest_file within the local system." >&2
	echo "If copying multiple files, dest_file directory will be created." >&2
	echo "To copy files from this host to hosts in the cluster use opascpall or" >&2
	echo "   opadownloadall." >&2
	exit 0
}

Usage()
{
	echo "Usage: opauploadall [-rp] [-f hostfile] source_file ... dest_file" >&2
	echo "              or" >&2
	echo "       opauploadall --help" >&2
	echo "   --help - produce full help text" >&2
	echo "   -p - perform copy in parallel on all hosts" >&2
	echo "   -r - recursive upload of directories" >&2
	echo "   -f hostfile - file with hosts in cluster, default is $CONFIG_DIR/opa/hosts" >&2
	echo "   source_file - list of source files to copy" >&2
	echo "   dest_file - destination for copy" >&2
	echo "        If more than 1 source file, this must be a directory" >&2
	echo "example:">&2
	echo "   opauploadall -p capture.tgz /etc/init.d/ipoib.cfg ." >&2
	echo "   opauploadall capture.tgz /etc/init.d/ipoib.cfg pre-install" >&2
	echo "user@ syntax cannot be used in filenames specified" >&2
	echo "A local directory within uploads/ will be created for each hostname." >&2
	echo "Destination file will be uploads/hostname/dest_file within the local system." >&2
	echo "If copying multiple files, dest_file directory will be created." >&2
	echo "To copy files from this host to hosts in the cluster use opascpall or" >&2
	echo "   opadownloadall." >&2
	exit 2
}

if [ x"$1" = "x--help" ]
then
	Usage_full
fi

user=`id -u -n`
opts=
popt=n
while getopts f:d:h:u:rp param
do
	case $param in
	f)
		HOSTS_FILE="$OPTARG";;
	d)
		UPLOADS_DIR="$OPTARG";;
	h)
		HOSTS="$OPTARG";;
	u)
		user="$OPTARG";;
	r)
		opts="$opts -r";;
	p)
		opts="$opts -q"
		popt=y;;
	?)
		Usage;;
	esac
done
shift $((OPTIND -1))
if [ $# -lt 2 ]
then
	Usage
fi

check_host_args opauploadall

# remove last name from the list
files=
dest=
file_count=0
for file in "$@"
do
	if [ ! -z "$dest" ]
	then
		files="$files $dest"
		file_count=`expr $file_count + 1`
	fi
	dest="$file"
done

running=0
for hostname in $HOSTS
do
	src_files=
	for file in $files
	do
		src_files="$src_files $user@[$hostname]:$file"
	done
	mkdir -p $UPLOADS_DIR/$hostname
	if [ $file_count -gt 1 ]
	then
		mkdir -p $UPLOADS_DIR/$hostname/$dest
	fi

	if [ "$popt" = "y" ]
	then
		if [ $running -ge $FF_MAX_PARALLEL ]
		then
			wait
			running=0
		fi
		echo "scp $opts $src_files $UPLOADS_DIR/$hostname/$dest"
		scp $opts $src_files $UPLOADS_DIR/$hostname/$dest &
		running=$(( $running + 1))
	else
		echo "scp $opts $src_files $UPLOADS_DIR/$hostname/$dest"
		scp $opts $src_files $UPLOADS_DIR/$hostname/$dest
	fi
done
wait
