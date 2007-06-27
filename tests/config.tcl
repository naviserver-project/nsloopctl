#
# nsdbi configuration example.
#


set homedir   [pwd]
set bindir    [file dirname [ns_info nsd]]



ns_section "ns/parameters"
ns_param   home           $homedir
ns_param   tcllibrary     $bindir/../tcl
ns_param   logdebug       false
ns_param   logdevel       false

ns_section "ns/servers"
ns_param   server1         "Server One"

ns_section "ns/server/server1/modules"
ns_param   nsloop          $homedir/nsloopctl.so

ns_section "ns/server/server1/tcl"
ns_param   initfile        ${bindir}/init.tcl
ns_param   library         $homedir/tests/testserver/modules

