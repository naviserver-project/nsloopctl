#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
# 

#
# all.tcl --
#
#       This file contains a top-level script to run all of the tests.
#       Execute it by invoking "source all.tcl" when running nsd in
#       command mode in this directory.
#

package require Tcl 8.4
package require tcltest 2.2
namespace import tcltest::*
eval configure $argv -singleproc true -testdir [file dirname [info script]]


runAllTests
