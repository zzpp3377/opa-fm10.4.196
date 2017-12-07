#!/usr/bin/perl
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
use strict;
#use Term::ANSIColor;
#use Term::ANSIColor qw(:constants);
#use File::Basename;
#use Math::BigInt;

# ==========================================================================
# Fast Fabric installation

my $FF_CONF_FILE = "/usr/lib/opa/tools/opafastfabric.conf";
my $FF_TLS_CONF_FILE = "/etc/opa/opaff.xml";
sub available_fastfabric
{
	my $srcdir=$ComponentInfo{'fastfabric'}{'SrcDir'};
	return ((rpm_resolve("$srcdir/RPMS/*/", "any", "opa-basic-tools") ne "") &&
			(rpm_resolve("$srcdir/RPMS/*/", "any", "opa-fastfabric") ne ""));
}

sub installed_fastfabric
{
	return(system("rpm -q --quiet opa-fastfabric") == 0)
}

# only called if installed_fastfabric is true
sub installed_version_fastfabric
{
	my $version_file="$BASE_DIR/version_ff";

	if ( -e "$version_file" ) {
		return `cat $ROOT$version_file`;
	} else {
		return "";
	}
}

# only called if available_fastfabric is true
sub media_version_fastfabric
{
	my $srcdir=$ComponentInfo{'fastfabric'}{'SrcDir'};
	return `cat "$srcdir/version"`;
}

sub build_fastfabric
{
	my $osver = $_[0];
	my $debug = $_[1];	# enable extra debug of build itself
	my $build_temp = $_[2];	# temp area for use by build
	my $force = $_[3];	# force a rebuild
	return 0;	# success
}

sub need_reinstall_fastfabric($$)
{
	my $install_list = shift();	# total that will be installed when done
	my $installing_list = shift();	# what items are being installed/reinstalled

	return "no";
}

sub check_os_prereqs_fastfabric
{	
	return rpm_check_os_prereqs("fastfabric", "user");
}

sub preinstall_fastfabric
{
	my $install_list = $_[0];	# total that will be installed when done
	my $installing_list = $_[1];	# what items are being installed/reinstalled

	return 0;	# success
}

sub install_fastfabric
{
	my $install_list = $_[0];	# total that will be installed when done
	my $installing_list = $_[1];	# what items are being installed/reinstalled

	my $srcdir=$ComponentInfo{'fastfabric'}{'SrcDir'};
	my $depricated_dir = "/etc/sysconfig/opa";

	my $version=media_version_fastfabric();
	chomp $version;
	printf("Installing $ComponentInfo{'fastfabric'}{'Name'} $version $DBG_FREE...\n");
		LogPrint "Installing $ComponentInfo{'fastfabric'}{'Name'} $version $DBG_FREE for $CUR_DISTRO_VENDOR $CUR_VENDOR_VER\n";
	check_config_dirs();
	if ( -e "$srcdir/comp.pl" ) {
		check_dir("/usr/lib/opa");
		copy_systool_file("$srcdir/comp.pl", "/usr/lib/opa/.comp_fastfabric.pl");
	}

	my $rpmfile = rpm_resolve("$srcdir/RPMS/*/", "any", "opa-fastfabric");
	rpm_run_install($rpmfile, "any", " -U ");

	check_dir("/usr/lib/opa/tools");
	check_dir("/usr/share/opa/samples");
	system "chmod ug+x $ROOT/usr/share/opa/samples/hostverify.sh";
	system "rm -f $ROOT/usr/share/opa/samples/nodeverify.sh";

	check_rpm_config_file("$FF_TLS_CONF_FILE");
	printf("Default opaff.xml can be found in '/usr/share/opa/samples/opaff.xml-sample'\n");
	check_rpm_config_file("$CONFIG_DIR/opa/opamon.conf", $depricated_dir);
	check_rpm_config_file("$CONFIG_DIR/opa/opafastfabric.conf", $depricated_dir);
	check_rpm_config_file("$CONFIG_DIR/opa/allhosts", $depricated_dir);
	check_rpm_config_file("$CONFIG_DIR/opa/chassis", $depricated_dir);
	check_rpm_config_file("$CONFIG_DIR/opa/hosts", $depricated_dir);
	check_rpm_config_file("$CONFIG_DIR/opa/ports", $depricated_dir);
	check_rpm_config_file("$CONFIG_DIR/opa/switches", $depricated_dir);
	check_rpm_config_file("/usr/lib/opa/tools/osid_wrapper");

	#install_conf_file("$ComponentInfo{'fastfabric'}{'Name'}", "$FF_TLS_CONF_FILE", "$srcdir/fastfabric/tools/tls");
	#remove_conf_file("$ComponentInfo{'fastfabric'}{'Name'}", "$OPA_CONFIG_DIR/iba_stat.conf");
	system("rm -rf $ROOT$OPA_CONFIG_DIR/iba_stat.conf");	# old config

	install_shmem_apps($srcdir);

	$rpmfile = rpm_resolve("$srcdir/RPMS/*/", "any", "opa-mpi-apps");
	rpm_run_install($rpmfile, "any", " -U ");



	$ComponentWasInstalled{'fastfabric'}=1;
}

sub postinstall_fastfabric
{
	my $install_list = $_[0];	# total that will be installed when done
	my $installing_list = $_[1];	# what items are being installed/reinstalled
}

sub uninstall_fastfabric
{
	my $install_list = $_[0];	# total that will be left installed when done
	my $uninstalling_list = $_[1];	# what items are being uninstalled


	rpm_uninstall_list("any", "verbose", ("opa-mpi-apps", "opa-fastfabric") );

	NormalPrint("Uninstalling $ComponentInfo{'fastfabric'}{'Name'}...\n");
	remove_conf_file("$ComponentInfo{'fastfabric'}{'Name'}", "$FF_CONF_FILE");
	remove_conf_file("$ComponentInfo{'fastfabric'}{'Name'}", "$OPA_CONFIG_DIR/iba_stat.conf");
	remove_conf_file("$ComponentInfo{'fastfabric'}{'Name'}", "$FF_TLS_CONF_FILE");
	

	uninstall_shmem_apps;

	# remove samples we installed (or user compiled), however do not remove
	# any logs or other files the user may have created
	remove_installed_files "/usr/share/opa/samples";
	system "rmdir $ROOT/usr/share/opa/samples 2>/dev/null";	# remove only if empty

	system("rm -rf $ROOT/usr/lib/opa/.comp_fastfabric.pl");
	system "rmdir $ROOT/usr/lib/opa 2>/dev/null";	# remove only if empty
	system "rmdir $ROOT$BASE_DIR 2>/dev/null";	# remove only if empty
	system "rmdir $ROOT$OPA_CONFIG_DIR 2>/dev/null";	# remove only if empty
	$ComponentWasInstalled{'fastfabric'}=0;
}

