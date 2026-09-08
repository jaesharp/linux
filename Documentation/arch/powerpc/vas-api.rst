.. SPDX-License-Identifier: GPL-2.0
.. _VAS-API:

===================================================
Virtual Accelerator Switchboard (VAS) userspace API
===================================================

Introduction
============

Power9 processor introduced Virtual Accelerator Switchboard (VAS) which
allows both userspace and kernel communicate to co-processor
(hardware accelerator) referred to as the Nest Accelerator (NX). The NX
unit comprises of one or more hardware engines or co-processor types
such as 842 compression, GZIP compression and encryption. On power9,
userspace applications will have access to only GZIP Compression engine
which supports ZLIB and GZIP compression algorithms in the hardware.

To communicate with NX, kernel has to establish a channel or window and
then requests can be submitted directly without kernel involvement.
Requests to the GZIP engine must be formatted as a co-processor Request
Block (CRB) and these CRBs must be submitted to the NX using COPY/PASTE
instructions to paste the CRB to hardware address that is associated with
the engine's request queue.

The GZIP engine provides two priority levels of requests: Normal and
High. On PowerNV, only Normal requests are available from userspace. On
PowerVM, an application can ask for a window backed by the partition's
quality-of-service credits, which use the high priority queue; see the
flags field of the VAS_TX_WIN_OPEN ioctl and the "Credits and windows"
section below.

This document explains userspace API that is used to interact with
kernel to setup channel / window which can be used to send compression
requests directly to NX accelerator.


Overview
========

Application access to the GZIP engine is provided through
/dev/crypto/nx-gzip device node implemented by the VAS/NX device driver.
An application must open the /dev/crypto/nx-gzip device to obtain a file
descriptor (fd). Then should issue VAS_TX_WIN_OPEN ioctl with this fd to
establish connection to the engine. It means send window is opened on GZIP
engine for this process. Once a connection is established, the application
should use the mmap() system call to map the hardware address of engine's
request queue into the application's virtual address space.

The application can then submit one or more requests to the engine by
using copy/paste instructions and pasting the CRBs to the virtual address
(aka paste_address) returned by mmap(). User space can close the
established connection or send window by closing the file descriptor
(close(fd)) or upon the process exit.

Note that applications can send several requests with the same window or
can establish multiple windows, but one window for each file descriptor.

Following sections provide additional details and references about the
individual steps.

NX-GZIP Device Node
===================

There is one /dev/crypto/nx-gzip node in the system and it provides
access to all GZIP engines in the system. The only valid operations on
/dev/crypto/nx-gzip are:

	* open() the device for read and write.
	* issue VAS_TX_WIN_OPEN ioctl
	* mmap() the engine's request queue into application's virtual
	  address space (i.e. get a paste_address for the co-processor
	  engine).
	* close the device node.

Other file operations on this device node are undefined.

Note that the copy and paste operations go directly to the hardware and
do not go through this device. Refer COPY/PASTE document for more
details.

Although a system may have several instances of the NX co-processor
engines (typically, one per P9 chip) there is just one
/dev/crypto/nx-gzip device node in the system. When the nx-gzip device
node is opened, Kernel opens send window on a suitable instance of NX
accelerator. It finds CPU on which the user process is executing and
determine the NX instance for the corresponding chip on which this CPU
belongs.

Applications may chose a specific instance of the NX co-processor using
the vas_id field in the VAS_TX_WIN_OPEN ioctl as detailed below.

A userspace library libnxz is available here but still in development:

	 https://github.com/abalib/power-gzip

Applications that use inflate / deflate calls can link with libnxz
instead of libz and use NX GZIP compression without any modification.

Open /dev/crypto/nx-gzip
========================

The nx-gzip device should be opened for read and write. No special
privileges are needed to open the device. Each window corresponds to one
file descriptor. So if the userspace process needs multiple windows,
several open calls have to be issued.

See open(2) system call man pages for other details such as return values,
error codes and restrictions.

VAS_TX_WIN_OPEN ioctl
=====================

Applications should use the VAS_TX_WIN_OPEN ioctl as follows to establish
a connection with NX co-processor engine:

	::

		struct vas_tx_win_open_attr {
			__u32   version;
			__s16   vas_id; /* specific instance of vas or -1
						for default */
			__u16   reserved1;
			__u64   flags;
			__u64   reserved2[6];
		};

	version:
		1 or 2.

		Version 1 is the original interface. It does not check the
		reserved fields or reject undefined flag bits, and cannot
		start doing so without breaking programs that have relied
		on that since it shipped -- which is also why no new
		meaning can ever be given to those bits under version 1.

		Version 2 is the same structure with the rules below
		enforced: reserved fields must be zero and every flag bit
		set must be one the kernel defines. New features are
		offered under version 2 only, so a kernel that does not
		have a feature refuses the version that carries it and the
		application can fall back, rather than being given a window
		that silently lacks what it asked for.

	vas_id:
		If '-1' is passed, kernel will make a best-effort attempt
		to assign an optimal instance of NX for the process. To
		select the specific VAS instance, refer
		"Discovery of available VAS engines" section below.

	flags:
		VAS_TX_WIN_FLAG_QOS_CREDIT requests a window backed by the
		partition's quality-of-service credits instead of the
		default credits. Only meaningful on PowerVM, where the two
		pools exist; see "Credits and windows" below. All other
		bits are reserved and must be set to 0. Under version 2 a
		bit the kernel does not define is rejected with EINVAL;
		under version 1 it is ignored.

	reserved1 and reserved2[6] fields are for future extension and
	must be set to 0. Under version 2 a non-zero value in either is
	rejected with EINVAL; under version 1 both are ignored, which is
	what prevents them from carrying anything new.

	The attributes attr for the VAS_TX_WIN_OPEN ioctl are defined as
	follows::

		#define VAS_MAGIC 'v'
		#define VAS_TX_WIN_OPEN _IOW(VAS_MAGIC, 1,
						struct vas_tx_win_open_attr)

		struct vas_tx_win_open_attr attr;
		rc = ioctl(fd, VAS_TX_WIN_OPEN, &attr);

	The VAS_TX_WIN_OPEN ioctl returns 0 on success. On errors, it
	returns -1 and sets the errno variable to indicate the error.

	Error conditions:

		======	================================================
		EINVAL	fd does not refer to a valid VAS device.
		EINVAL	Invalid vas ID
		EINVAL	version is not set with proper value
		EEXIST	Window is already opened for the given fd
		ENOMEM	Memory is not available to allocate window
		EAGAIN	Every window id on the chip is in use (PowerNV)
		EINVAL	reserved fields are not 0, or a flag bit is not one
			this kernel defines (version 2 only).
		EBUSY	No credit is available for the window: on PowerVM
			the partition's credits for the requested type are
			all in use, or windows lost to a dynamic
			reconfiguration have not been reopened yet. Also
			returned when the caller's cgroup is at its window
			limit; see "Resource limits".
		======	================================================

	See the ioctl(2) man page for more details, error codes and
	restrictions.

mmap() NX-GZIP device
=====================

The mmap() system call for a NX-GZIP device fd returns a paste_address
that the application can use to copy/paste its CRB to the hardware engines.

	::

		paste_addr = mmap(addr, size, prot, flags, fd, offset);

	Only restrictions on mmap for a NX-GZIP device fd are:

		* size should be PAGE_SIZE
		* offset parameter should be 0ULL

	Refer to mmap(2) man page for additional details/restrictions.
	In addition to the error conditions listed on the mmap(2) man
	page, can also fail with one of the following error codes:

		======	=============================================
		EINVAL	fd is not associated with an open window
			(i.e mmap() does not follow a successful call
			to the VAS_TX_WIN_OPEN ioctl).
		EINVAL	offset field is not 0ULL.
		======	=============================================

Discovery of available VAS engines
==================================

Each available VAS instance in the system will have a device tree node
like /proc/device-tree/vas@* or /proc/device-tree/xscom@*/vas@*.
Determine the chip or VAS instance and use the corresponding ibm,vas-id
property value in this node to select specific VAS instance.

Credits and windows
===================

Two different things are called a credit, and they limit different
stages of a request's life.

A window credit is the depth of one window's request queue: one credit
is consumed when a request is pasted and returned when the engine has
processed it. A paste to a window with no free credit fails -- the
paste instruction itself reports the failure, CR0 does not indicate
success -- and the application retries or backs off; nothing is queued
and nothing is lost. On PowerNV a window carries 1024 credits, so up to
1024 requests can be outstanding on one window. On PowerVM a window
carries one credit by default, one request at a time.

On PowerVM the window credits themselves come from partition-wide
pools, and those pools are the second meaning. The hypervisor assigns
the partition default credits (in proportion to its cores) and,
optionally, quality-of-service credits an administrator configured
through the management console; each window takes its credit from the
pool the flags field selected when it was opened. When a pool is
exhausted, VAS_TX_WIN_OPEN fails with EBUSY until a window closes or
the pool grows. PowerNV has no partition pools; the corresponding
limit is the number of window ids per chip, and exhausting those
returns EAGAIN.

The pools are visible in sysfs, per type::

	/sys/devices/virtual/misc/vas/vas0/gzip/default_capabilities/nr_total_credits
	/sys/devices/virtual/misc/vas/vas0/gzip/default_capabilities/nr_used_credits
	/sys/devices/virtual/misc/vas/vas0/gzip/qos_capabilities/nr_total_credits
	/sys/devices/virtual/misc/vas/vas0/gzip/qos_capabilities/nr_used_credits

qos_capabilities also contains update_total_credits, which the
management console tooling writes after changing the partition's
quality-of-service assignment; it is not for applications.

No privilege beyond opening the device node is required for either
credit type. What a group of processes may consume can be bounded with
the miscellaneous cgroup controller; see "Resource limits" below.

Window lifecycle
================

A window is active from the ioctl until it is closed by the last
close() of the descriptor or by process exit. Two things can interrupt
it in between, both on PowerVM:

* A dynamic reconfiguration that removes cores can take the partition's
  credits with them. The kernel then closes enough windows to fit the
  new total, most recently opened first, and unmaps the paste address
  of every window it closes.

* A partition migration closes every window on the source system and
  reopens them on the destination, whose credit assignment may differ.

In both cases the application is not told; its next paste takes a page
fault on the unmapped paste address, and the kernel makes the paste
report failure exactly as an out-of-credit paste would. The
application's existing retry loop is the recovery: when credits return
(cores come back, migration completes), the kernel reopens the window,
the retried paste faults once more, the fault handler maps the new
paste address, and the request goes through. An application that wants
to distinguish "retry until it works" from "the credits are gone" can
compare nr_used_credits with nr_total_credits in sysfs: used above
total means windows are waiting for credits to come back.

Closing a window waits for the hardware to finish: for the window to
go quiet and for every credit to be returned, faults included. The
kernel bounds that wait at one minute; a window whose hardware never
drains -- or, on PowerVM, one the hypervisor refuses to deallocate --
is abandoned rather than freed, with a message in the kernel log
naming the window and owning pid, and its resources are deliberately
retained so that nothing the hardware can still touch is reused. Such
a window stays charged to its owner in every accounting described
here until reboot.

Copy/Paste operations
=====================

Applications should use the copy and paste instructions to send CRB to NX.
Refer section 4.4 in PowerISA for Copy/Paste instructions:
https://openpowerfoundation.org/?resource_lib=power-isa-version-3-0

CRB Specification and use NX
============================

Applications should format requests to the co-processor using the
co-processor Request Block (CRBs). Refer NX-GZIP user's manual for the format
of CRB and use NX from userspace such as sending requests and checking
request status.

NX Fault handling
=================

Applications send requests to NX and wait for the status by polling on
co-processor Status Block (CSB) flags. NX updates status in CSB after each
request is processed. Refer NX-GZIP user's manual for the format of CSB and
status flags.

In case if NX encounters translation error (called NX page fault) on CSB
address or any request buffer, raises an interrupt on the CPU to handle the
fault. Page fault can happen if an application passes invalid addresses or
request buffers are not in memory. The engine terminates the request when it
faults, so nothing can rescue that request; the operating system makes the
faulting address resident if it can, and reports the termination by updating
CSB with the following data::

	csb.flags = CSB_V;
	csb.cc = CSB_CC_FAULT_ADDRESS;
	csb.ce = CSB_CE_TERMINATION;
	csb.address = fault_address;

When an application receives translation error, it can touch or access
the page that has a fault address so that this page will be in memory. Then
the application can resend this request to NX. Retrying is the application's
responsibility: unlike an OpenCAPI adapter, which the kernel acknowledges
with RESTART so that the operation is reissued in hardware, VAS has no way to
restart a request the engine has already terminated. Touching the page is
usually not even necessary: before reporting the fault, the kernel resolves
the reported address and the run of request buffer it belongs to, so the
plain retry finds its pages resident. A request can still fault more than
once -- it can have more than one non-resident buffer, and memory pressure
can take pages back -- so the retry loop, not any single retry, is the
contract.

On a hash MMU kernel there is one more consequence an application can
observe but never has to handle. The accelerator's MMU translates through
per-process segment tables that the kernel builds, and a change of page
size in part of the address space -- a huge page mapping where none was
before is the common cause -- invalidates them wholesale. The affected
window's next request may then report a translation fault that a radix
kernel would not have produced; the kernel rebuilds the entries during
fault handling and the ordinary retry succeeds. Diagnostics for this
machinery -- counters, a dump of a process's segment table and of its
page-size layout -- live under /sys/kernel/debug/powerpc/nmmu_* on
kernels built with CONFIG_DEBUG_FS, readable by root.

If the OS can not update CSB due to invalid CSB address, sends SEGV signal
to the process who opened the send window on which the original request was
issued. This signal returns with the following siginfo struct::

	siginfo.si_signo = SIGSEGV;
	siginfo.si_errno = EFAULT;
	siginfo.si_code = SEGV_MAPERR;
	siginfo.si_addr = CSB address;

In the case of multi-thread applications, NX send windows can be shared
across all threads. For example, a child thread can open a send window,
but other threads can send requests to NX using this window. These
requests will be successful even in the case of OS handling faults as long
as CSB address is valid. If the NX request contains an invalid CSB address,
the signal will be sent to the child thread that opened the window. But if
the thread is exited without closing the window and the request is issued
using this window. the signal will be issued to the thread group leader
(tgid). It is up to the application whether to ignore or handle these
signals.

No CSB update is made and no signal is sent if the process is no longer
running in the address space that issued the request. A process that calls
execve() with requests still in flight keeps the window, because the file
descriptor survives the exec, but the CSB address those requests carry
belongs to the address space execve() replaced. There is nothing to write
and no correct process to notify -- the pid now names a different program
-- so the update is dropped. The same applies once a process has exited.
An application that wants its results must therefore consume them before
replacing its address space; a window inherited across an exec is usable
for new requests, but the results of requests issued before it are not
recoverable.

NX-GZIP User's Manual:
https://github.com/libnxz/power-gzip/blob/master/doc/power_nx_gzip_um.pdf

Simple example
==============

	::

		int use_nx_gzip()
		{
			int rc, fd;
			void *addr;
			struct vas_setup_attr txattr;

			fd = open("/dev/crypto/nx-gzip", O_RDWR);
			if (fd < 0) {
				fprintf(stderr, "open nx-gzip failed\n");
				return -1;
			}
			memset(&txattr, 0, sizeof(txattr));
			txattr.version = 1;
			txattr.vas_id = -1
			rc = ioctl(fd, VAS_TX_WIN_OPEN,
					(unsigned long)&txattr);
			if (rc < 0) {
				fprintf(stderr, "ioctl() n %d, error %d\n",
						rc, errno);
				return rc;
			}
			addr = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
					MAP_SHARED, fd, 0ULL);
			if (addr == MAP_FAILED) {
				fprintf(stderr, "mmap() failed, errno %d\n",
						errno);
				return -errno;
			}
			do {
				//Format CRB request with compression or
				//uncompression
				// Refer tests for vas_copy/vas_paste
				vas_copy(&crb, 0, 1);
				vas_paste(addr, 0, 1);
				// Poll on csb.flags with timeout
				// csb address is listed in CRB
			} while (true)
			close(fd) or window can be closed upon process exit
		}

	Refer https://github.com/libnxz/power-gzip for tests or more
	use cases.

Resource limits
===============

The credits described in "Credits and windows" are what the platform
has; the miscellaneous cgroup controller is how an administrator
divides them. Windows are charged as two resources, ``vas_windows``
for default windows and ``vas_qos_windows`` for quality-of-service
windows, mirroring the two pools. The root ``misc.capacity`` reports
what the platform has -- window ids per chip on PowerNV, the
partition's credits per pool on PowerVM, moving when dynamic
reconfiguration or migration moves them -- and ``misc.current`` what
each group holds.

A window is one unit, charged to the cgroup of the process whose ioctl
opened it and uncharged to that same cgroup when the window is finally
closed, however many processes the descriptor visited in between; a
charge follows the opener, as miscellaneous resources are specified to.
A window the kernel abandoned (see "Window lifecycle") stays charged,
because it is still consumed. When a cgroup is at its ``misc.max`` for
the resource, VAS_TX_WIN_OPEN fails with EBUSY, the same error as an
exhausted pool: from the application's side, its group's share ran out
either way. After a reconfiguration shrinks a pool, ``misc.current``
can legitimately exceed ``misc.capacity`` until windows close; new
charges fail in the meantime.

No limit is imposed by default: the capacities only describe the
platform, and ``misc.max`` starts at ``max``. In particular, a group
that should not compete for the administrator-assigned
quality-of-service credits can be confined by setting its
``vas_qos_windows`` limit to 0 while leaving ``vas_windows`` alone.
Without CONFIG_CGROUP_MISC there is no accounting and, as before,
nothing bounds how many windows a user may open beyond the file
descriptor limit.

Either cgroup hierarchy version will do -- the miscellaneous controller
offers the same files to both. What it cannot do is appear in two at
once, so on a system whose init has already given ``misc`` to a v1
hierarchy the files are there and not in the v2 mount; look in
/proc/cgroups for the hierarchy it belongs to, or boot with
``cgroup_no_v1=misc`` to leave it for v2.
