## ACPIBatteryManager by RehabMan


### How to Install:

Install the kext using your favorite kext installer utility, such as Kext Wizard.  The Debug director is for troubleshooting only, in normal "working" installs, you should install the Release version.

Please read this post if you need to make DSDT edits: http://www.tonymacx86.com/mavericks-laptop-support/116102-how-patch-dsdt-working-battery-status.html


### Downloads:

Downloads are available on Bitbucket:

https://bitbucket.org/RehabMan/os-x-acpi-battery-driver/downloads


Archived builds are available on Google Code:

https://code.google.com/p/os-x-acpi-battery-driver/downloads/list


### Build Environment

My build environment is currently Xcode 6.1, using SDK 10.8, targeting OS X 10.6.

No other build environment is supported.


### 32-bit Builds

Currently, builds are provided only for 64-bit systems.  32-bit/64-bit FAT binaries are not provided.  But you can build your own should you need them.  I do not test 32-bit, and there may be times when the repo is broken with respect to 32-bit builds, but I do check on major releases to see if the build still works for 32-bit.

Here's how to build 32-bit (universal):

- xcode 4.6.3
- open ACPIBatteryManager.xcodeproj
- click on ACPIBatteryManager at the top of the project tree
- select ACPIBatteryManager under Project
- change Architectures to 'Standard (32/64-bit Intel)'

probably not necessary, but a good idea to check that the targets don't have overrides:
- multi-select all the Targets
- check/change Architectures to 'Standard (32/64-bit Intel)'
- build (either w/ menu or with make)

Or, if you have the command line tools installed, just run:

- For FAT binary (32-bit and 64-bit in one binary)
make BITS=3264

- For 32-bit only
make BITS=32


### Source Code:

The source code is maintained at the following sites:

https://code.google.com/p/os-x-acpi-battery-driver/

https://github.com/RehabMan/OS-X-ACPI-Battery-Driver

https://bitbucket.org/RehabMan/os-x-acpi-battery-driver


### Feedback:

Please use this thread on tonymacx86.com for feedback, questions, and help:

http://www.tonymacx86.com/hp-probook/69472-battery-manager-fix-boot-without-batteries.html



### Known issues:

- Most DSDTs need to be patched in order to work properly with OS X.  


### Change Log:

2014-10-16 v1.53

- added debug output for _BIX


2014-02-07 v1.52

- Fix deadlock caused by changes made for multiple batteries.  See issue #3.

- When determining if other batteries are discharging, ignore batteries that are not connected.  See issue #2.

- To provide AC status changes quicker after an AC change, poll battery objects more often after such a change.  See issue #4.


2014-01-21 v1.51

- Some fixes related to multiple batteries.  Work in progress.


2013-12-04 v1.50

- Added ACPIACAdapter implementation, which implemeents an ACPI compliant object to track status changes of the AC adapter.  As the status change, the battery objects are notified.

prior fixes:

- I didn't really track a change log prior to now.  Read the threads linked or the commit log in git.


### History

See original post at:
http://www.insanelymac.com/forum/index.php?s=bfca1f05adde52f77c9d5c0caa1250f7&showtopic=264597&view=findpost&p=1729132

See updated wiki documentation at:
https://github.com/gsly/OS-X-ACPI-Battery-Driver/wiki


### Original Credits

Most of the base code for AppleSmartBatteryManager and AppleSmartBattery created by gsly on insanelymac.com, no doubt with influences from the many other ACPI battery implementations that were out there.

Other ideas brought in by myself, some borrowed from zprood on insanelymac.com.

RehabMan - recent enhancements/bug fixes
Zprood - cycle count hack in _BIF
gsly - base code for RehabMan enhancements
