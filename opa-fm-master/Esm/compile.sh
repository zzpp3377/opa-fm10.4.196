./Esm/update_opa-fm_spec.sh opa-fm.spec.in opa-fm.spec
tar czf $HOME/rpmbuild/SOURCES/opa-fm.tar.gz --exclude-vcs .
rpmbuild -ba ./opa-fm.spec
