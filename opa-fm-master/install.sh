cd /root/rpmbuild/RPMS/x86_64
rpm -e opa-fm-debuginfo-10.4.0.0-196.el7.centos.x86_64
rpm -e opa-fm-10.4.0.0-196.el7.centos.x86_64

rpm -i opa-fm-debuginfo-10.4.0.0-196.el7.centos.x86_64.rpm
rpm -i opa-fm-10.4.0.0-196.el7.centos.x86_64.rpm

cd /etc/opa-fm
rm -rf opafm.xml
mv opafm.xml.rpmsave opafm.xml
