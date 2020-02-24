/* iface, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2002, 2013,2016 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012-2017 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Wolfgang Nothdurft <wolfgang@linogate.de>
 * Copyright (C) 2016-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 D. Hugh Redelmeier <hugh@mimosa.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <unistd.h>

#include "defs.h"

#include "iface.h"
#include "log.h"
#include "hostpair.h"			/* for release_dead_interfaces() */
#include "state.h"			/* for delete_states_dead_interfaces() */
#include "server.h"			/* for *_pluto_event() */
#include "kernel.h"
#include "demux.h"
#include "iface_udp.h"
#include "ip_info.h"

struct iface_port  *interfaces = NULL;  /* public interfaces */

/*
 * The interfaces - eth0 ...
 */

static void jam_iface_dev(jambuf_t *buf, const void *data)
{
	const struct iface_dev *ifd = data;
	jam_string(buf, ifd->id_rname);
}

static const struct list_info iface_dev_info = {
	.name = "interface_dev",
	.jam = jam_iface_dev,
};

static struct list_head interface_dev = INIT_LIST_HEAD(&interface_dev, &iface_dev_info);

static void add_iface_dev(const struct raw_iface *ifp)
{
	struct iface_dev *ifd = alloc_thing(struct iface_dev,
					    "struct iface_dev");
	init_ref(ifd);
	ifd->ifd_entry = list_entry(&iface_dev_info, ifd);
	insert_list_entry(&interface_dev, &ifd->ifd_entry);
	ifd->id_rname = clone_str(ifp->name,
				 "real device name");
	ifd->id_nic_offload = kernel_ops->detect_offload(ifp);
	ifd->id_address = ifp->addr;
	dbg("iface: marking %s add", ifd->id_rname);
	ifd->ifd_change = IFD_ADD;
}

static void mark_ifaces_dead(void)
{
	struct iface_dev *ifd;
	FOR_EACH_LIST_ENTRY_OLD2NEW(&interface_dev, ifd) {
		dbg("iface: marking %s dead", ifd->id_rname);
		ifd->ifd_change = IFD_DELETE;
	}
}

void add_or_keep_iface_dev(struct raw_iface *ifp)
{
	/* find the iface */
	struct iface_dev *ifd;
	FOR_EACH_LIST_ENTRY_OLD2NEW(&interface_dev, ifd) {
		if (streq(ifd->id_rname, ifp->name) &&
		    sameaddr(&ifd->id_address, &ifp->addr)) {
			dbg("iface: marking %s keep", ifd->id_rname);
			ifd->ifd_change = IFD_KEEP;
			return;
		}
	}
	add_iface_dev(ifp);
}

static void free_iface_dev(struct iface_dev **ifd,
			   where_t where UNUSED)
{
	remove_list_entry(&(*ifd)->ifd_entry);
	pfree((*ifd)->id_rname);
	pfree((*ifd));
	*ifd = NULL;
}

void release_iface_dev(struct iface_dev **id)
{
	delete_ref(id, free_iface_dev);
}

static void free_dead_ifaces(void)
{
	struct iface_port *p;
	bool some_dead = false;
	bool some_new = false;

	/*
	 * XXX: this iterates over the interface, and not the
	 * interface_devs, so that it can log what will be shutdown
	 * before shutting it down.
	 */
	for (p = interfaces; p != NULL; p = p->next) {
		if (p->ip_dev->ifd_change == IFD_DELETE) {
			endpoint_buf b;
			libreswan_log("shutting down interface %s %s",
				      p->ip_dev->id_rname,
				      str_endpoint(&p->local_endpoint, &b));
			some_dead = TRUE;
		} else if (p->ip_dev->ifd_change == IFD_ADD) {
			some_new = TRUE;
		}
	}

	if (some_dead) {
		struct iface_port **pp;

		release_dead_interfaces();
		delete_states_dead_interfaces();
		for (pp = &interfaces; (p = *pp) != NULL; ) {
			if (p->ip_dev->ifd_change == IFD_DELETE) {
				*pp = p->next; /* advance *pp */
				delete_pluto_event(&p->pev);
				close(p->fd);
				release_iface_dev(&p->ip_dev);
				pfree(p);
			} else {
				pp = &p->next; /* advance pp */
			}
		}
	}

	/* this must be done after the release_dead_interfaces
	 * in case some to the newly unoriented connections can
	 * become oriented here.
	 */
	if (some_dead || some_new)
		check_orientations();
}

void free_ifaces(void)
{
	mark_ifaces_dead();
	free_dead_ifaces();
}

/*
 * Open new interfaces.
 */
static void add_new_ifaces(void)
{
	struct iface_dev *ifd;
	FOR_EACH_LIST_ENTRY_OLD2NEW(&interface_dev, ifd) {
		if (ifd->ifd_change != IFD_ADD)
			continue;

		struct iface_port *q = udp_iface_port(ifd, pluto_port,
						      false/*ike_float*/);
		if (q == NULL) {
			ifd->ifd_change = IFD_DELETE;
			continue;
		}

		endpoint_buf b;
		libreswan_log("adding interface %s %s",
			      q->ip_dev->id_rname,
			      str_endpoint(&q->local_endpoint, &b));

		/*
		 * From linux's xfrm: right now, we do not support
		 * NAT-T on IPv6, because the kernel did not support
		 * it, and gave an error it one tried to turn it on.
		 *
		 * From bsd's kame: right now, we do not support NAT-T
		 * on IPv6, because the kernel did not support it, and
		 * gave an error it one tried to turn it on.
		 *
		 * Who should we believe?
		 */
		if (address_type(&ifd->id_address) == &ipv4_info) {
			q = udp_iface_port(ifd, pluto_nat_port,
					   true/*ike_float*/);
			if (q == NULL) {
				continue;
			}
			endpoint_buf b;
			libreswan_log("adding interface %s %s",
				      q->ip_dev->id_rname,
				      str_endpoint(&q->local_endpoint, &b));
		}
	}
}

static void handle_udp_packet_cb(evutil_socket_t unused_fd UNUSED,
				 const short unused_event UNUSED,
				 void *arg)
{
	const struct iface_port *ifp = arg;
	handle_packet_cb(ifp);
}

void find_ifaces(bool rm_dead)
{
	/*
	 * Sweep the interfaces, after this each is either KEEP, DEAD,
	 * or ADD.
	 */
	mark_ifaces_dead();
	kernel_ops->process_raw_ifaces(find_raw_ifaces4());
	kernel_ops->process_raw_ifaces(find_raw_ifaces6());
	add_new_ifaces();

	if (rm_dead)
		free_dead_ifaces(); /* ditch remaining old entries */

	if (interfaces == NULL)
		loglog(RC_LOG_SERIOUS, "no public interfaces found");

	if (listening) {
		struct iface_port *ifp;

		for (ifp = interfaces; ifp != NULL; ifp = ifp->next) {
			delete_pluto_event(&ifp->pev);
			ifp->pev = add_fd_read_event_handler(ifp->fd,
							     handle_udp_packet_cb,
							     ifp, "ethX");
			endpoint_buf b;
			dbg("setup callback for interface %s %s fd %d",
			    ifp->ip_dev->id_rname,
			    str_endpoint(&ifp->local_endpoint, &b),
			    ifp->fd);
		}
	}
}

struct iface_port *find_iface_port_by_local_endpoint(ip_endpoint *local_endpoint)
{
	for (struct iface_port *p = interfaces; p != NULL; p = p->next) {
		if (endpoint_eq(*local_endpoint, p->local_endpoint)) {
			return p;
		}
	}
	return NULL;
}

void show_ifaces_status(const struct fd *whackfd)
{
	struct iface_port *p;

	for (p = interfaces; p != NULL; p = p->next) {
		endpoint_buf b;
		whack_comment(whackfd, "interface %s %s",
			  p->ip_dev->id_rname,
			  str_endpoint(&p->local_endpoint, &b));
	}
}
