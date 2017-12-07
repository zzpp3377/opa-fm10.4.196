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
# Fast Fabric Support tools for OFED (oftools) installation

# autostart functions are per subcomponent
sub start_oftools
{
}

sub stop_oftools
{
}

sub available_oftools
{
	my $srcdir=$ComponentInfo{'oftools'}{'SrcDir'};
	return (rpm_resolve("$srcdir/RPMS/*/", "any", "opa-basic-tools") ne "" );
}

sub installed_oftools
{
	return(system("rpm -q --quiet opa-basic-tools") == 0)
}

# only called if installed_oftools is true
sub installed_version_oftools
{
	if ( -e "$ROOT$BASE_DIR/version_ff" ) {
		return `cat $ROOT$BASE_DIR/version_ff`;
	} else {
		return "";
	}
}

# only called if available_oftools is true
sub media_version_oftools
{
	my $srcdir=$ComponentInfo{'oftools'}{'SrcDir'};
	return `cat "$srcdir/version"`;
}

sub build_oftools
{
	my $osver = $_[0];
	my $debug = $_[1];	# enable extra debug of build itself
	my $build_temp = $_[2];	# temp area for use by build
	my $force = $_[3];	# force a rebuild
	return 0;	# success
}

sub need_reinstall_oftools($$)
{
	my $install_list = shift();	# total that will be installed when done
	my $installing_list = shift();	# what items are being installed/reinstalled

	return "no";
}

sub preinstall_oftools
{
	my $install_list = $_[0];	# total that will be installed when done
	my $installing_list = $_[1];	# what items are being installed/reinstalled

	return 0;	# success
}

sub install_oftools
{
	my $install_list = $_[0];	# total that will be installed when done
	my $installing_list = $_[1];	# what items are being installed/reinstalled

	my $srcdir=$ComponentInfo{'oftools'}{'SrcDir'};

	my $version=media_version_oftools();
	chomp $version;
	printf("Installing $ComponentInfo{'oftools'}{'Name'} $version $DBG_FREE...\n");
	#LogPrint "Installing $ComponentInfo{'oftools'}{'Name'} $version $DBG_FREE for $CUR_OS_VER\n";
	LogPrint "Installing $ComponentInfo{'oftools'}{'Name'} $version $DBG_FREE for $CUR_DISTRO_VENDOR $CUR_VENDOR_VER\n";

	# Check $BASE_DIR directory ...exist 
	check_config_dirs();

	if ( -e "$srcdir/comp.pl" ) {
		check_dir("/usr/lib/opa");
		copy_systool_file("$srcdir/comp.pl", "/usr/lib/opa/.comp_oftools.pl");
	}
	check_dir("/usr/lib/opa/tools");
	check_dir("/usr/share/opa/samples");

	my $rpmfile = rpm_resolve("$srcdir/RPMS/*/", "any", "opa-basic-tools");
	rpm_run_install($rpmfile, "any", " -U ");

	$rpmfile = rpm_resolve("$srcdir/RPMS/*/", "any", "opa-address-resolution");
	rpm_run_install($rpmfile, "any", " -U ");
	check_rpm_config_file("/etc/rdma/dsap.conf");

	# we copy version file here for both fastfabric and oftools
	copy_data_file("$srcdir/version", "$BASE_DIR/version_ff");

	## Install OPA_SA_DB library, headers.
	check_dir("/usr/include/infiniband");
	copy_shlib("$srcdir/bin/$ARCH/$CUR_DISTRO_VENDOR.$CUR_VENDOR_VER/lib/$DBG_FREE/libopasadb", "$LIB_DIR/libopasadb", "1.0.0");

	$ComponentWasInstalled{'oftools'}=1;
}

sub postinstall_oftools
{
	my $install_list = $_[0];	# total that will be installed when done
	my $installing_list = $_[1];	# what items are being installed/reinstalled
}

sub uninstall_oftools
{
	my $install_list = $_[0];	# total that will be left installed when done
	my $uninstalling_list = $_[1];	# what items are being uninstalled

	NormalPrint("Uninstalling $ComponentInfo{'oftools'}{'Name'}...\n");

	rpm_uninstall_list("any", "verbose", ("opa-basic-tools", "opa-address-resolution") );

	# remove LSF and Moab related files
	system("rm -rf $ROOT/usr/lib/opa/LSF_scripts");
	system("rm -rf $ROOT/usr/lib/opa/Moab_scripts");

	# may be created by opaverifyhosts
	system("rm -rf $ROOT/usr/lib/opa/tools/nodescript.sh");
	system("rm -rf $ROOT/usr/lib/opa/tools/nodeverify.sh");

	system "rmdir $ROOT/usr/lib/opa/tools 2>/dev/null";	# remove only if empty

	# oftools is a prereq of fastfabric can cleanup shared files here
	system("rm -rf $ROOT$BASE_DIR/version_ff");
	system "rmdir $ROOT$BASE_DIR 2>/dev/null";	# remove only if empty
	system "rmdir $ROOT$OPA_CONFIG_DIR 2>/dev/null";	# remove only if empty
	system("rm -rf $ROOT/usr/lib/opa/.comp_oftools.pl");
	system "rmdir $ROOT/usr/lib/opa 2>/dev/null";	# remove only if empty
	$ComponentWasInstalled{'oftools'}=0;
}

sub check_os_prereqs_oftools
{
	return rpm_check_os_prereqs("oftools", "user");
}
