/*
 * Linux 2.6.32 and later Kernel module for VMware MVP Guest Communications
 *
 * Copyright (C) 2010-2013 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#line 5

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)
#error "MVP requires a host kernel newer than 3.0.0"
#endif

#ifndef CONFIG_MODULES
#error "MVP requires kernel loadable module support be enabled " \
	"(CONFIG_MODULES)"
#endif
#ifndef CONFIG_MODULE_UNLOAD
#error "MVP requires kernel module unload support be enabled " \
	"(CONFIG_MODULE_UNLOAD)"
#endif

#ifndef CONFIG_SYSFS
#error "MVP requires sysfs support (CONFIG_SYSFS)"
#endif

#ifndef CONFIG_NAMESPACES
#error "MVP requires namespace support (CONFIG_NAMESPACES)"
#endif
#ifndef CONFIG_NET_NS
#error "MVP requires network namespace support (CONFIG_NET_NS)"
#endif

#ifndef CONFIG_INET
#error "MVP requires IPv4 support (CONFIG_INET)"
#endif
#ifndef CONFIG_IPV6
#error "MVP requires IPv6 support (CONFIG_IPV6)"
#endif

#if !defined(CONFIG_TUN) && !defined(CONFIG_TUN_MODULE)
#error "MVP VPN support requires TUN device support (CONFIG_TUN)"
#endif

#if !defined(CONFIG_NETFILTER) && !defined(PVTCP_DISABLE_NETFILTER)
#error "MVP requires netfilter support (CONFIG_NETFILTER)"
#endif

#ifdef MVP_DEBUG
#if !defined(CONFIG_IKCONFIG) || !defined(CONFIG_IKCONFIG_PROC)
#error "MVP_DEBUG requires /proc/config.gz support (CONFIG_IKCONFIG_PROC)"
#endif
#endif

#ifdef CONFIG_MIGRATION
#if defined(CONFIG_NUMA) || \
	defined(CONFIG_MEMORY_FAILURE)
#error "MVP not tested with migration features other than " \
	"CONFIG_MEMORY_HOTPLUG and CONFIG_COMPACTION"
#endif
#endif
