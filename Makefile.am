## Process this file with automake to produce Makefile.in

NULL =

SUBDIRS = data src po doc

EXTRA_DIST = 			\
	HACKING 		\
	$(NULL)

DISTCHECK_CONFIGURE_FLAGS=							\
	--enable-gtk-doc 							\
	--disable-introspection							\
	--with-udevdir=$$dc_install_base/$(udevdir)				\
	--with-systemdsystemunitdir=$$dc_install_base/$(systemdsystemunitdir)	\
	$(NULL)

if WITH_COVERAGE
clean-local:
	find $(builddir) -name '*.gc??' -delete

coverage:
	mkdir -p .coverage
	lcov --directory . --zerocounters
	$(MAKE) check
	sleep 1 # Completing all gcov writes...
	lcov --directory . --capture --no-external --output-file .coverage/info
	genhtml --output-directory .coverage \
		--title "cockpit $(PACKAGE_VERSION)" \
		.coverage/info
	@echo "file://$(abs_top_builddir)/.coverage/index.html"
endif
