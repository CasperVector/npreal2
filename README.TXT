npreal2: Moxa Linux Real TTY Driver
===================================

Email: support@moxa.com.tw


Background
----------

Using this driver, you can use NPort serial ports as local tty ports.
This driver works with Linux kernel 3.x, 4.x, 5.x and 6.x.  Before using
this software, make sure the IP configuration of the NPort is correct and
you can access (ping, telnet, ...) it successfully.  For the Real COM Mode
and the Redundant COM Mode, you need to set the operation mode of your
NPort correspondingly; the latter mode is only available with certain
series of products.


Preparation
-----------

npreal2 has the following dependencies:
- kernel 3.0 or above and the kernel headers
- gcc, binutils, make, coreutils, awk
- openssl (optional, for Secure Real COM Mode only)

With them available, run the following command to build npreal2 (to enable
the Secure Real COM Mode, append the argument `SSL=1' to `make'; to enable
background polling, append the argument `POLLING=1'):
$ make
Then run the following command as root to install npreal2:
# make install

After installation, run the following command as root (if it has not been done
before) to create the low-privilege user for running `npreal2d' and `redund':
# useradd -r npreal2
Then build the npreal2 kernel module (this needs to be done again after every
kernel upgrade; to enable concurrent open in the Secure Real COM Mode, append
the argument `CONCURRENT=1' to `mxmkdrv'):
# mxmkdrv


Configuration
-------------

Use `mxaddsvr' as root to map NPort serial ports to the host tty ports:
# mxaddsvr server_ip num_port [data_port] [cmd port] [interface]
For redundant mode, use the `-r' option:
# mxaddsvr -r server_ip backup_ip num_port [data_port] [cmd port] [interface]
To unmap an NPort server, use `mxdelsvr' as root:
# mxdelsvr server_ip
The mapping configuration can be seen in `/etc/npreal2d.cf'.

For example, the following command
# mxaddsvr 192.168.3.4 16 4001 966
will add 16 tty ports, all with IP address `192.168.3.4', with data ports
4001, 4002, ..., 4016 and command ports 966, 967, ..., 981, respectively.
The following command
# mxaddsvr 192.168.3.4 16 4001 966
will remove all tty ports associated with `192.168.3.4'.


Port mapping
------------

After the serial ports have been mapped, run the following command as root to
load the npreal2 kernel module and set up the tty devices `/dev/ttyr*':
# mxloadsvr
After this, start the npreal2 daemon(s) to actually map the ports (`npreal2d'
for the Real COM Mode, `redund' for the Redundant COM Mode; to prevent the
daemons from double forking and let them log to stderr, append the `-f' option;
for better security, the daemons can also be run as the `npreal2' user):
# npreal2d -t 1
# redund -t 1

NOTE: If you have modified the port mapping with `mxaddsvr' and `mxdelsvr',
for the changes to take effect immediately, you need to stop the npreal2
daemon(s), run `mxloadsvr' again to reset the `/dev/ttyr*' devices, and
then restart the daemon(s).


Misc notes
----------

Huge efforts have been made to clean up the codebase while keeping the interface
nearly intact, by only eliminating obviously unnecessary code.  Nevertheless,
the codebase is still extremely crufty and ugly, and cannot be considered
trustable.  Even though the crappy user space daemons can be run without root
privilege, the source code of the kernel module is just a little less crappy.

Access to `/dev/ttyr*' is controlled with the `dialout' group: only root and
users in the `dialout' group can read/write these devices.  The callout devices
`/dev/ttycur*' have been obsoleted in Linux 2.6.  `/dev/ttyr0' to `/dev/ttyr9'
and some tty names represented by decimal numbers are created for compatibility
reasons.  Only use `/dev/ttyr*' configured in `/etc/npreal2d.cf'.

To enable or disable Secure Real COM Mode for certain NPort servers, set the
6th field ("ssl") of the corresponding lines in `/etc/npreal2d.cf' to 1 or 0,
respectively.  (And you should first pass `SSL=1' to `make'.)

If you don't want your program to be blocked when opening tty ports with NPort
not yet existing, you should pass `POLLING=1' to `make'.  This feature will
poll NPort in the background to eliminate the time your program is blocked.

Since openssl causes internal system call error when opening multiple tty ports
concurrently, the opening might be failed.  If your application needs to open
tty ports concurrently, you should pass `CONCURRENT=1' to `mxmkdrv'.

NPort driver needs up to one minute to synchronize with the NPort after the
network is down and up again.  Therefore, if connection loss occurs again
during this period, data loss might occur.

The default IP address for NPort is 192.168.127.254.  For NPort Wireless series,
the default IP address of the Ethernet interface is 192.168.126.254, and the
default IP address of the Wireless LAN interface is 192.168.127.254.


