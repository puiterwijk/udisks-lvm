
NULL =

dbusservicedir       = $(datadir)/dbus-1/system-services
dbusservice_in_files = com.redhat.lvm2.service.in
dbusservice_DATA     = $(dbusservice_in_files:.service.in=.service)

$(dbusservice_DATA): $(dbusservice_in_files) Makefile
	@sed -e "s|\@udisksdprivdir\@|$(prefix)/lib/udisks2|" $< > $@

dbusconfdir = $(sysconfdir)/dbus-1/system.d
dbusconf_in_files = com.redhat.lvm2.conf.in
dbusconf_DATA = $(dbusconf_in_files:.conf.in=.conf)

$(dbusconf_DATA): $(dbusconf_in_files) Makefile
	cp $< $@

systemdservice_in_files = udisks-lvm.service.in

if HAVE_SYSTEMD
systemdservicedir       = $(systemdsystemunitdir)
systemdservice_DATA     = $(systemdservice_in_files:.service.in=.service)

$(systemdservice_DATA): $(systemdservice_in_files) Makefile
	@sed -e "s|\@udisksdprivdir\@|$(prefix)/lib/udisks2|" $< > $@
endif

polkitdir        = $(datadir)/polkit-1/actions
polkit_in_files  = com.redhat.lvm2.policy.in
polkit_DATA      = $(polkit_in_files:.policy.in=.policy)

@INTLTOOL_POLICY_RULE@

dbusinterfacedir = $(datadir)/dbus-1/interfaces
dbusinterface_DATA = com.redhat.lvm2.xml

EXTRA_DIST = 						\
	com.redhat.lvm2.xml				\
	$(systemdservice_in_files)			\
	$(dbusservice_in_files)				\
	$(dbusconf_in_files)				\
	$(polkit_in_files)				\
	$(NULL)

clean-local :
	rm -f *~ $(dbusservice_DATA) $(dbusconf_DATA) $(systemdservice_DATA) $(polkit_DATA)
