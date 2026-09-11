#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#

ifndef NAVISERVER
	NAVISERVER = /usr/local/ns
endif

NSD        = $(NAVISERVER)/bin/nsd

MODNAME      = nsloopctl
MOD          = nsloopctl.so
MODOBJS      = nsloopctl.o


include  $(NAVISERVER)/include/Makefile.module



doc:
	$(MKDIR) doc/html doc/man

html-doc: doc
	dtplite -o doc/html html doc/src/mann/loopctl.man

man-doc: doc
	dtplite -o doc/man nroff doc/src/mann/loopctl.man



NS_TEST_CFG  = -c -d -t tests/config.tcl
NS_TEST_ALL  = tests/all.tcl $(TCLTESTARGS)

test: all
	$(NSD) $(NS_TEST_CFG) $(NS_TEST_ALL)

runtest: all
	$(NSD) $(NS_TEST_CFG)

gdbtest: all
	@echo set args $(NS_TEST_CFG) $(NS_TEST_ALL) > gdb.run
	gdb -x gdb.run $(NSD)
	rm gdb.run

gdbruntest: all
	@echo set args $(NS_TEST_CFG) > gdb.run
	gdb -x gdb.run $(NSD)
	rm gdb.run


.PHONY: doc html-doc man-doc
