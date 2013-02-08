/*-
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$KAME: in6_src.c,v 1.132 2003/08/26 04:42:27 keiichi Exp $
 */

/*-
 * Copyright (c) 1982, 1986, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)in_pcb.c	8.2 (Berkeley) 1/4/94
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_mpath.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/priv.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/jail.h>
#include <sys/kernel.h>
#include <sys/sx.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <net/if_llatbl.h>
#ifdef RADIX_MPATH
#include <net/radix_mpath.h>
#endif

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/ip_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/in6_pcb.h>
#include <netinet6/ip6_var.h>
#include <netinet6/scope6_var.h>
#include <netinet6/nd6.h>

static struct mtx addrsel_lock;
#define	ADDRSEL_LOCK_INIT()	mtx_init(&addrsel_lock, "addrsel_lock", NULL, MTX_DEF)
#define	ADDRSEL_LOCK()		mtx_lock(&addrsel_lock)
#define	ADDRSEL_UNLOCK()	mtx_unlock(&addrsel_lock)
#define	ADDRSEL_LOCK_ASSERT()	mtx_assert(&addrsel_lock, MA_OWNED)

static struct sx addrsel_sxlock;
#define	ADDRSEL_SXLOCK_INIT()	sx_init(&addrsel_sxlock, "addrsel_sxlock")
#define	ADDRSEL_SLOCK()		sx_slock(&addrsel_sxlock)
#define	ADDRSEL_SUNLOCK()	sx_sunlock(&addrsel_sxlock)
#define	ADDRSEL_XLOCK()		sx_xlock(&addrsel_sxlock)
#define	ADDRSEL_XUNLOCK()	sx_xunlock(&addrsel_sxlock)

#define ADDR_LABEL_NOTAPP (-1)
static VNET_DEFINE(struct in6_addrpolicy, defaultaddrpolicy);
#define	V_defaultaddrpolicy		VNET(defaultaddrpolicy)

VNET_DEFINE(int, ip6_prefer_tempaddr) = 0;

static int selectroute(struct sockaddr_in6 *, struct ip6_pktopts *,
	struct ip6_moptions *, struct route_in6 *, struct ifnet **,
	struct rtentry **, int, u_int);
static int in6_selectif(struct sockaddr_in6 *, struct ip6_pktopts *,
	struct ip6_moptions *, struct route_in6 *ro, struct ifnet **,
	struct ifnet *, u_int);

static struct in6_addrpolicy *lookup_addrsel_policy(struct sockaddr_in6 *);
static int lookup_policy_label(const struct sockaddr_in6 *);

static void init_policy_queue(void);
static int add_addrsel_policyent(struct in6_addrpolicy *);
static int delete_addrsel_policyent(struct in6_addrpolicy *);
static int walk_addrsel_policy(int (*)(struct in6_addrpolicy *, void *),
	void *);
static int dump_addrsel_policyent(struct in6_addrpolicy *, void *);
static struct in6_addrpolicy *match_addrsel_policy(struct sockaddr_in6 *);
static int in6_srcaddrscope(const struct in6_addr *);

/*
 * Return an IPv6 address, which is the most appropriate for a given
 * destination and user specified options.
 * If necessary, this function lookups the routing table and returns
 * an entry to the caller for later use.
 */

struct srcaddr_choice {
	struct in6_ifaddr *ia;
	int scope;
	int label;
	int rule;
};

struct dstaddr_props {
	struct ifnet *ifp;
	struct in6_addr *addr;
	int scope;
	int label;
	int prefixlen;
};

#define	REPLACE(r)	{ rule = r; goto replace; }
#define	NEXT(r)		{ rule = r; goto next; }

static int
srcaddrcmp(struct srcaddr_choice *c, const struct in6_ifaddr *ia,
    struct dstaddr_props *dst, const struct ucred *cred,
    const struct ip6_pktopts *opts)
{
	int srcscope, rule, label, prefer_tempaddr, prefixlen;

	/* Avoid unusable addresses */
	if ((ia->ia6_flags & (IN6_IFF_NOTREADY | IN6_IFF_DETACHED)) ||
	    (ia->ia_ifp->if_flags & IFF_UP) == 0)
		return (-1);
	/*
	 * In any case, multicast addresses and the unspecified address
	 * MUST NOT be included in a candidate set.
	 */
	if (IN6_IS_ADDR_MULTICAST(IA6_IN6(ia)) ||
	    IN6_IS_ADDR_UNSPECIFIED(IA6_IN6(ia)))
		return (-1);
	if (!V_ip6_use_deprecated && IFA6_IS_DEPRECATED(ia))
		return (-1);
	/* If jailed, only take addresses of the jail into account. */
	if (cred != NULL && prison_check_ip6(cred, IA6_IN6(ia)) != 0)
		return (-1);
	/* Check scope zones */
	scope = in6_srcaddrscope(IA6_IN6(ia));
	if (in6_getscopezone(ia->ia_ifp, scope) !=
	    in6_getscopezone(dst->ifp, dst->scope))
		return (-1)
	label = ADDR_LABEL_NOTAPP;
	prefixlen = -1;
	/* Rule 1: Prefer same address. */
	if (IN6_ARE_ADDR_EQUAL(IA6_IN6(ia), dst->addr))
		REPLACE(1);
	/* Rule 2: Prefer appropriate scope. */
	srcscope = in6_srcaddrscope(IA6_IN6(ia));
	if (c->ia == NULL) {
		dst->label = lookup_policy_label(dst->addr);
		REPLACE(0);
	}
	if (IN6_ARE_SCOPE_CMP(c->scope, srcscope) < 0) {
		if (IN6_ARE_SCOPE_CMP(c->scope, dst->scope) < 0)
			REPLACE(2);
		NEXT(2);
	} else if (IN6_ARE_SCOPE_CMP(srcscope, c->scope) < 0) {
		if (IN6_ARE_SCOPE_CMP(srcscope, dst->scope) < 0)
			NEXT(2);
		REPLACE(2);
	}
	/* Rule 3: Avoid deprecated addresses. */
	if (!IFA6_IS_DEPRECATED(c->ia) && IFA6_IS_DEPRECATED(ia))
		NEXT(3);
	if (IFA6_IS_DEPRECATED(c->ia) && !IFA6_IS_DEPRECATED(ia))
		REPLACE(3);
	/*
	 * Rule 4: Prefer home addresses.
	 * XXX: This is a TODO.
	 */
	/* Rule 5: Prefer outgoing interface. */
	if (c->ia->ia_ifp == dst->ifp && ia->ia_ifp != dst->ifp)
		NEXT(5);
	if (c->ia->ia_ifp != dst->ifp && ia->ia_ifp == dst->ifp)
		REPLACE(5);
	/*
	 * Rule 5.5: Prefer addresses in a prefix advertised by
	 * the next-hop.
	 * XXX: not yet.
	 */
	/* Rule 6: Prefer matching label. */
	if (dst->label != ADDR_LABEL_NOTAPP) {
		c->label = lookup_policy_label(IA6_IN6(c->ia));
		label = lookup_policy_label(IA6_IN6(ia));
		if (c->label == dst->label && label != dst->label)
			NEXT(6);
		if (label == dst->label && c->label != dst->label)
			REPLACE(6);
	}
	/* Rule 7: Prefer temporary addresses. */
	if (opts == NULL ||
	    opts->ip6po_prefer_tempaddr == IP6PO_TEMPADDR_SYSTEM)
		prefer_tempaddr = V_ip6_prefer_tempaddr;
	else if (opts->ip6po_prefer_tempaddr == IP6PO_TEMPADDR_NOTPREFER)
		prefer_tempaddr = 0;
	else
		prefer_tempaddr = 1;
	if ((c->ia->ia6_flags & IN6_IFF_TEMPORARY) != 0 &&
	    (ia->ia6_flags & IN6_IFF_TEMPORARY) == 0) {
		if (prefer_tempaddr)
			NEXT(7);
		REPLACE(7);
	}
	if ((c->ia->ia6_flags & IN6_IFF_TEMPORARY) == 0 &&
	    (ia->ia6_flags & IN6_IFF_TEMPORARY) != 0) {
		if (prefer_tempaddr)
			REPLACE(7);
		NEXT(7);
	}
	/* Rule 8: Use longest matching prefix. */
	if (c->prefixlen < 0)
		c->prefixlen = in6_matchlen(IA6_IN6(c->ia), dst->addr);
	prefixlen = in6_matchlen(IA6_IN6(ia), dst->addr);
	if (c->prefixlen > prefixlen)
		NEXT(8);
	if (prefixlen > c->prefixlen)
		REPLACE(8);
	return (-1);
replace:
	/* debug output */
	c->ia = ia;
	c->label = label;
	c->scope = srcscope;
	c->rule = rule;
	c->prefixlen = prefixlen;
	return (rule);
next:
	/* debug output */
	return (rule);
}
#undef	REPLACE
#undef	NEXT

int
in6_selectsrc(struct sockaddr_in6 *dstsock, struct ip6_pktopts *opts,
    struct inpcb *inp, struct route_in6 *ro, struct ucred *cred,
    struct ifnet **ifpp, struct in6_addr *srcp)
{
	struct dstaddr_props dstprops;
	struct srcaddr_choice best;
	struct in6_addr tmp;
	struct ip6_moptions *mopts;
	struct in6_pktinfo *pi;
	struct in6_ifaddr *ia;
	struct ifaddr *ifa;
	struct ifnet *ifp, *oifp;
	int error;

	KASSERT(srcp != NULL, ("%s: srcp is NULL", __func__));

	ifp = oifp = NULL;
	if (ifpp) {
		/*
		 * Save a possibly passed in ifp for in6_selectsrc. Only
		 * neighbor discovery code should use this feature, where
		 * we may know the interface but not the FIB number holding
		 * the connected subnet in case someone deleted it from the
		 * default FIB and we need to check the interface.
		 */
		if (*ifpp != NULL)
			oifp = *ifpp;
		*ifpp = NULL;
	}
	if (inp != NULL) {
		INP_LOCK_ASSERT(inp);
		mopts = inp->in6p_moptions;
	} else {
		mopts = NULL;
	}

	/*
	 * If the source address is explicitly specified by the caller,
	 * check if the requested source address is indeed a unicast address
	 * assigned to the node, and can be used as the packet's source
	 * address.  If everything is okay, use the address as source.
	 */
	if (opts && (pi = opts->ip6po_pktinfo) &&
	    !IN6_IS_ADDR_UNSPECIFIED(&pi->ipi6_addr)) {
		/* get the outgoing interface */
		if ((error = in6_selectif(dstsock, opts, mopts, ro, &ifp, oifp,
		    (inp != NULL) ? inp->inp_inc.inc_fibnum : RT_DEFAULT_FIB))
		    != 0)
			return (error);
		KASSERT(ifp != NULL, ("%s: ifp is NULL", __func__));
		/* XXX: prison_local_ip6 can override pi->ipi6_addr */
		if (cred != NULL && (error = prison_local_ip6(cred,
		    &pi->ipi6_addr, (inp != NULL &&
		    (inp->inp_flags & IN6P_IPV6_V6ONLY) != 0))) != 0)
			return (error);
		/*
		 * Determine the appropriate zone id of the source based on
		 * the zone of the destination and the outgoing interface.
		 * Even if source address is explicitly specified, it can
		 * not break the destination zone.
		 */
		ia = in6ifa_ifwithaddr(&pi->ipi6_addr, in6_getscopezone(
		    ifp, in6_srcaddrscope(&dstsock.sin6_addr)));
		if (ia == NULL ||
		    (ia->ia6_flags & (IN6_IFF_ANYCAST | IN6_IFF_NOTREADY))) {
			if (ia != NULL)
				ifa_free(&ia->ia_ifa);
			return (EADDRNOTAVAIL);
		}
		if (ifpp)
			*ifpp = ifp;
		bcopy(IA6_IN6(ia), srcp, sizeof(*srcp));
		ifa_free(&ia->ia_ifa);
		return (0);
	}
	/*
	 * Otherwise, if the socket has already bound the source, just use it.
	 */
	if (inp != NULL && !IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr)) {
		if (cred != NULL &&
		    (error = prison_local_ip6(cred, &inp->in6p_laddr,
		    ((inp->inp_flags & IN6P_IPV6_V6ONLY) != 0))) != 0)
			return (error);
		bcopy(&inp->in6p_laddr, srcp, sizeof(*srcp));
		return (0);
	}
	/*
	 * Bypass source address selection and use the primary jail IP
	 * if requested.
	 */
	if (cred != NULL && !prison_saddrsel_ip6(cred, srcp))
		return (0);
	/*
	 * If the address is not specified, choose the best one based on
	 * the outgoing interface and the destination address.
	 */
	/* get the outgoing interface */
	if ((error = in6_selectif(dstsock, opts, mopts, ro, &ifp, oifp,
	    (inp != NULL) ? inp->inp_inc.inc_fibnum : RT_DEFAULT_FIB)) != 0)
		return (error);
	KASSERT(ifp != NULL, ("%s: ifp is NULL", __func__));
	/*
	 * RFC 6724 (section 4):
	 * For all multicast and link-local destination addresses, the set of
	 * candidate source addresses MUST only include addresses assigned to
	 * interfaces belonging to the same link as the outgoing interface.
	 */
	dstprops.ifp = ifp;
	dstprops.addr = &dstsock.sin6_addr;
	dstprops.scope = in6_srcaddrscope(&dstsock.sin6_addr);
	best.rule = -1;
	best.ia = NULL;
	if (IN6_IS_ADDR_MULTICAST(&dstsock.sin6_addr) ||
	    dstprops.scope == IPV6_ADDR_SCOPE_LINKLOCAL) {
		IF_ADDR_RLOCK(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr->sa_family != AF_INET6)
				continue;
			ret = srcaddrcmp(&best, (struct in6_ifaddr*)ifa,
			    &dstprops, cred, opts);
			if (ret == 1)
				break;
		}
		if (best.rule >= 0)
			ifa_ref(&best.ia->ia_ifa);
		IF_ADDR_RUNLOCK(ifp);
	} else {
		IN6_IFADDR_RLOCK();
		TAILQ_FOREACH(ia, &V_in6_ifaddrhead, ia_link) {
			ret = srcaddrcmp(&best, ia, &dstprops, cred, opts);
			if (ret == 1)
				break;
		}
		if (best.rule >= 0)
			ifa_ref(&best.ia->ia_ifa);
		IN6_IFADDR_RUNLOCK();
	}
	if (best.rule < 0)
		return (EADDRNOTAVAIL);
	/*
	 * At this point at least one of the addresses belonged to the jail
	 * but it could still be, that we want to further restrict it, e.g.
	 * theoratically IN6_IS_ADDR_LOOPBACK.
	 * It must not be IN6_IS_ADDR_UNSPECIFIED anymore.
	 * prison_local_ip6() will fix an IN6_IS_ADDR_LOOPBACK but should
	 * let all others previously selected pass.
	 * Use tmp to not change ::1 on lo0 to the primary jail address.
	 */
	tmp = ia->ia_addr.sin6_addr;
	ifa_free(&ia->ia_ifa);
	if (cred != NULL && prison_local_ip6(cred, &tmp, (inp != NULL &&
	    (inp->inp_flags & IN6P_IPV6_V6ONLY) != 0)) != 0) {
		return (EADDRNOTAVAIL);
	}
	if (ifpp)
		*ifpp = ifp;
	V_ip6stat.ip6s_sources_rule[best.rule]++;
	bcopy(&tmp, srcp, sizeof(*srcp));
	return (0);
}

/*
 * clone - meaningful only for bsdi and freebsd
 */
static int
selectroute(struct sockaddr_in6 *dstsock, struct ip6_pktopts *opts,
    struct ip6_moptions *mopts, struct route_in6 *ro,
    struct ifnet **retifp, struct rtentry **retrt, int norouteok, u_int fibnum)
{
	int error = 0;
	struct ifnet *ifp = NULL;
	struct rtentry *rt = NULL;
	struct sockaddr_in6 *sin6_next;
	struct in6_pktinfo *pi = NULL;
	struct in6_addr *dst = &dstsock->sin6_addr;
#if 0
	char ip6buf[INET6_ADDRSTRLEN];

	if (dstsock->sin6_addr.s6_addr32[0] == 0 &&
	    dstsock->sin6_addr.s6_addr32[1] == 0 &&
	    !IN6_IS_ADDR_LOOPBACK(&dstsock->sin6_addr)) {
		printf("in6_selectroute: strange destination %s\n",
		       ip6_sprintf(ip6buf, &dstsock->sin6_addr));
	} else {
		printf("in6_selectroute: destination = %s%%%d\n",
		       ip6_sprintf(ip6buf, &dstsock->sin6_addr),
		       dstsock->sin6_scope_id); /* for debug */
	}
#endif

	/* If the caller specify the outgoing interface explicitly, use it. */
	if (opts && (pi = opts->ip6po_pktinfo) != NULL && pi->ipi6_ifindex) {
		/* XXX boundary check is assumed to be already done. */
		ifp = ifnet_byindex(pi->ipi6_ifindex);
		if (ifp != NULL &&
		    (norouteok || retrt == NULL ||
		    IN6_IS_ADDR_MULTICAST(dst))) {
			/*
			 * we do not have to check or get the route for
			 * multicast.
			 */
			goto done;
		} else
			goto getroute;
	}

	/*
	 * If the destination address is a multicast address and the outgoing
	 * interface for the address is specified by the caller, use it.
	 */
	if (IN6_IS_ADDR_MULTICAST(dst) &&
	    mopts != NULL && (ifp = mopts->im6o_multicast_ifp) != NULL) {
		goto done; /* we do not need a route for multicast. */
	}

  getroute:
	/*
	 * If the next hop address for the packet is specified by the caller,
	 * use it as the gateway.
	 */
	if (opts && opts->ip6po_nexthop) {
		struct route_in6 *ron;
		struct llentry *la;
	    
		sin6_next = satosin6(opts->ip6po_nexthop);
		
		/* at this moment, we only support AF_INET6 next hops */
		if (sin6_next->sin6_family != AF_INET6) {
			error = EAFNOSUPPORT; /* or should we proceed? */
			goto done;
		}

		/*
		 * If the next hop is an IPv6 address, then the node identified
		 * by that address must be a neighbor of the sending host.
		 */
		ron = &opts->ip6po_nextroute;
		/*
		 * XXX what do we do here?
		 * PLZ to be fixing
		 */


		if (ron->ro_rt == NULL) {
			in6_rtalloc(ron, fibnum); /* multi path case? */
			if (ron->ro_rt == NULL) {
				/* XXX-BZ WT.? */
				if (ron->ro_rt) {
					RTFREE(ron->ro_rt);
					ron->ro_rt = NULL;
				}
				error = EHOSTUNREACH;
				goto done;
			} 
		}

		rt = ron->ro_rt;
		ifp = rt->rt_ifp;
		IF_AFDATA_RLOCK(ifp);
		la = lla_lookup(LLTABLE6(ifp), 0, (struct sockaddr *)&sin6_next->sin6_addr);
		IF_AFDATA_RUNLOCK(ifp);
		if (la != NULL) 
			LLE_RUNLOCK(la);
		else {
			error = EHOSTUNREACH;
			goto done;
		}
#if 0
		if ((ron->ro_rt &&
		     (ron->ro_rt->rt_flags & (RTF_UP | RTF_LLINFO)) !=
		     (RTF_UP | RTF_LLINFO)) ||
		    !IN6_ARE_ADDR_EQUAL(&satosin6(&ron->ro_dst)->sin6_addr,
		    &sin6_next->sin6_addr)) {
			if (ron->ro_rt) {
				RTFREE(ron->ro_rt);
				ron->ro_rt = NULL;
			}
			*satosin6(&ron->ro_dst) = *sin6_next;
		}
		if (ron->ro_rt == NULL) {
			in6_rtalloc(ron, fibnum); /* multi path case? */
			if (ron->ro_rt == NULL ||
			    !(ron->ro_rt->rt_flags & RTF_LLINFO)) {
				if (ron->ro_rt) {
					RTFREE(ron->ro_rt);
					ron->ro_rt = NULL;
				}
				error = EHOSTUNREACH;
				goto done;
			}
		}
#endif

		/*
		 * When cloning is required, try to allocate a route to the
		 * destination so that the caller can store path MTU
		 * information.
		 */
		goto done;
	}

	/*
	 * Use a cached route if it exists and is valid, else try to allocate
	 * a new one.  Note that we should check the address family of the
	 * cached destination, in case of sharing the cache with IPv4.
	 */
	if (ro) {
		if (ro->ro_rt &&
		    (!(ro->ro_rt->rt_flags & RTF_UP) ||
		     ((struct sockaddr *)(&ro->ro_dst))->sa_family != AF_INET6 ||
		     !IN6_ARE_ADDR_EQUAL(&satosin6(&ro->ro_dst)->sin6_addr,
		     dst))) {
			RTFREE(ro->ro_rt);
			ro->ro_rt = (struct rtentry *)NULL;
		}
		if (ro->ro_rt == (struct rtentry *)NULL) {
			struct sockaddr_in6 *sa6;

			/* No route yet, so try to acquire one */
			bzero(&ro->ro_dst, sizeof(struct sockaddr_in6));
			sa6 = (struct sockaddr_in6 *)&ro->ro_dst;
			*sa6 = *dstsock;
#ifdef RADIX_MPATH
				rtalloc_mpath_fib((struct route *)ro,
				    ntohl(sa6->sin6_addr.s6_addr32[3]), fibnum);
#else			
				ro->ro_rt = in6_rtalloc1((struct sockaddr *)
				    &ro->ro_dst, 0, 0UL, fibnum);
				if (ro->ro_rt)
					RT_UNLOCK(ro->ro_rt);
#endif
		}
				
		/*
		 * do not care about the result if we have the nexthop
		 * explicitly specified.
		 */
		if (opts && opts->ip6po_nexthop)
			goto done;

		if (ro->ro_rt) {
			ifp = ro->ro_rt->rt_ifp;

			if (ifp == NULL) { /* can this really happen? */
				RTFREE(ro->ro_rt);
				ro->ro_rt = NULL;
			}
		}
		if (ro->ro_rt == NULL)
			error = EHOSTUNREACH;
		rt = ro->ro_rt;

		/*
		 * Check if the outgoing interface conflicts with
		 * the interface specified by ipi6_ifindex (if specified).
		 * Note that loopback interface is always okay.
		 * (this may happen when we are sending a packet to one of
		 *  our own addresses.)
		 */
		if (ifp && opts && opts->ip6po_pktinfo &&
		    opts->ip6po_pktinfo->ipi6_ifindex) {
			if (!(ifp->if_flags & IFF_LOOPBACK) &&
			    ifp->if_index !=
			    opts->ip6po_pktinfo->ipi6_ifindex) {
				error = EHOSTUNREACH;
				goto done;
			}
		}
	}

  done:
	if (ifp == NULL && rt == NULL) {
		/*
		 * This can happen if the caller did not pass a cached route
		 * nor any other hints.  We treat this case an error.
		 */
		error = EHOSTUNREACH;
	}
	if (error == EHOSTUNREACH)
		V_ip6stat.ip6s_noroute++;

	if (retifp != NULL) {
		*retifp = ifp;

		/*
		 * Adjust the "outgoing" interface.  If we're going to loop 
		 * the packet back to ourselves, the ifp would be the loopback 
		 * interface. However, we'd rather know the interface associated 
		 * to the destination address (which should probably be one of 
		 * our own addresses.)
		 */
		if (rt) {
			if ((rt->rt_ifp->if_flags & IFF_LOOPBACK) &&
			    (rt->rt_gateway->sa_family == AF_LINK))
				*retifp = 
					ifnet_byindex(((struct sockaddr_dl *)
						       rt->rt_gateway)->sdl_index);
		}
	}

	if (retrt != NULL)
		*retrt = rt;	/* rt may be NULL */

	return (error);
}

static int
in6_selectif(struct sockaddr_in6 *dstsock, struct ip6_pktopts *opts,
    struct ip6_moptions *mopts, struct route_in6 *ro, struct ifnet **retifp,
    struct ifnet *oifp, u_int fibnum)
{
	int error;
	struct route_in6 sro;
	struct rtentry *rt = NULL;

	KASSERT(retifp != NULL, ("%s: retifp is NULL", __func__));

	if (ro == NULL) {
		bzero(&sro, sizeof(sro));
		ro = &sro;
	}

	if ((error = selectroute(dstsock, opts, mopts, ro, retifp,
	    &rt, 1, fibnum)) != 0) {
		if (ro == &sro && rt && rt == sro.ro_rt)
			RTFREE(rt);
		/* Help ND. See oifp comment in in6_selectsrc(). */
		if (oifp != NULL && fibnum == RT_DEFAULT_FIB) {
			*retifp = oifp;
			error = 0;
		}
		return (error);
	}

	/*
	 * do not use a rejected or black hole route.
	 * XXX: this check should be done in the L2 output routine.
	 * However, if we skipped this check here, we'd see the following
	 * scenario:
	 * - install a rejected route for a scoped address prefix
	 *   (like fe80::/10)
	 * - send a packet to a destination that matches the scoped prefix,
	 *   with ambiguity about the scope zone.
	 * - pick the outgoing interface from the route, and disambiguate the
	 *   scope zone with the interface.
	 * - ip6_output() would try to get another route with the "new"
	 *   destination, which may be valid.
	 * - we'd see no error on output.
	 * Although this may not be very harmful, it should still be confusing.
	 * We thus reject the case here.
	 */
	if (rt && (rt->rt_flags & (RTF_REJECT | RTF_BLACKHOLE))) {
		int flags = (rt->rt_flags & RTF_HOST ? EHOSTUNREACH : ENETUNREACH);

		if (ro == &sro && rt && rt == sro.ro_rt)
			RTFREE(rt);
		return (flags);
	}

	if (ro == &sro && rt && rt == sro.ro_rt)
		RTFREE(rt);
	return (0);
}

/*
 * Public wrapper function to selectroute().
 *
 * XXX-BZ in6_selectroute() should and will grow the FIB argument. The
 * in6_selectroute_fib() function is only there for backward compat on stable.
 */
int
in6_selectroute(struct sockaddr_in6 *dstsock, struct ip6_pktopts *opts,
    struct ip6_moptions *mopts, struct route_in6 *ro,
    struct ifnet **retifp, struct rtentry **retrt)
{

	return (selectroute(dstsock, opts, mopts, ro, retifp,
	    retrt, 0, RT_DEFAULT_FIB));
}

#ifndef BURN_BRIDGES
int
in6_selectroute_fib(struct sockaddr_in6 *dstsock, struct ip6_pktopts *opts,
    struct ip6_moptions *mopts, struct route_in6 *ro,
    struct ifnet **retifp, struct rtentry **retrt, u_int fibnum)
{

	return (selectroute(dstsock, opts, mopts, ro, retifp,
	    retrt, 0, fibnum));
}
#endif

/*
 * Default hop limit selection. The precedence is as follows:
 * 1. Hoplimit value specified via ioctl.
 * 2. (If the outgoing interface is detected) the current
 *     hop limit of the interface specified by router advertisement.
 * 3. The system default hoplimit.
 */
int
in6_selecthlim(struct inpcb *in6p, struct ifnet *ifp)
{

	if (in6p && in6p->in6p_hops >= 0)
		return (in6p->in6p_hops);
	else if (ifp)
		return (ND_IFINFO(ifp)->chlim);
	else if (in6p && !IN6_IS_ADDR_UNSPECIFIED(&in6p->in6p_faddr)) {
		struct route_in6 ro6;
		struct ifnet *lifp;

		bzero(&ro6, sizeof(ro6));
		ro6.ro_dst.sin6_family = AF_INET6;
		ro6.ro_dst.sin6_len = sizeof(struct sockaddr_in6);
		ro6.ro_dst.sin6_addr = in6p->in6p_faddr;
		in6_rtalloc(&ro6, in6p->inp_inc.inc_fibnum);
		if (ro6.ro_rt) {
			lifp = ro6.ro_rt->rt_ifp;
			RTFREE(ro6.ro_rt);
			if (lifp)
				return (ND_IFINFO(lifp)->chlim);
		}
	}
	return (V_ip6_defhlim);
}

/*
 * XXX: this is borrowed from in6_pcbbind(). If possible, we should
 * share this function by all *bsd*...
 */
int
in6_pcbsetport(struct in6_addr *laddr, struct inpcb *inp, struct ucred *cred)
{
	struct socket *so = inp->inp_socket;
	u_int16_t lport = 0;
	int error, lookupflags = 0;
#ifdef INVARIANTS
	struct inpcbinfo *pcbinfo = inp->inp_pcbinfo;
#endif

	INP_WLOCK_ASSERT(inp);
	INP_HASH_WLOCK_ASSERT(pcbinfo);

	error = prison_local_ip6(cred, laddr,
	    ((inp->inp_flags & IN6P_IPV6_V6ONLY) != 0));
	if (error)
		return(error);

	/* XXX: this is redundant when called from in6_pcbbind */
	if ((so->so_options & (SO_REUSEADDR|SO_REUSEPORT)) == 0)
		lookupflags = INPLOOKUP_WILDCARD;

	inp->inp_flags |= INP_ANONPORT;

	error = in_pcb_lport(inp, NULL, &lport, cred, lookupflags);
	if (error != 0)
		return (error);

	inp->inp_lport = lport;
	if (in_pcbinshash(inp) != 0) {
		inp->in6p_laddr = in6addr_any;
		inp->inp_lport = 0;
		return (EAGAIN);
	}

	return (0);
}

void
addrsel_policy_init(void)
{

	init_policy_queue();

	/* initialize the "last resort" policy */
	bzero(&V_defaultaddrpolicy, sizeof(V_defaultaddrpolicy));
	V_defaultaddrpolicy.label = ADDR_LABEL_NOTAPP;

	if (!IS_DEFAULT_VNET(curvnet))
		return;

	ADDRSEL_LOCK_INIT();
	ADDRSEL_SXLOCK_INIT();
}

static struct in6_addrpolicy *
lookup_addrsel_policy(struct sockaddr_in6 *key)
{
	struct in6_addrpolicy *match = NULL;

	ADDRSEL_LOCK();
	match = match_addrsel_policy(key);

	if (match == NULL)
		match = &V_defaultaddrpolicy;
	else
		match->use++;
	ADDRSEL_UNLOCK();

	return (match);
}

/*
 * Subroutines to manage the address selection policy table via sysctl.
 */
struct walkarg {
	struct sysctl_req *w_req;
};

static int in6_src_sysctl(SYSCTL_HANDLER_ARGS);
SYSCTL_DECL(_net_inet6_ip6);
static SYSCTL_NODE(_net_inet6_ip6, IPV6CTL_ADDRCTLPOLICY, addrctlpolicy,
	CTLFLAG_RD, in6_src_sysctl, "");

static int
in6_src_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct walkarg w;

	if (req->newptr)
		return EPERM;

	bzero(&w, sizeof(w));
	w.w_req = req;

	return (walk_addrsel_policy(dump_addrsel_policyent, &w));
}

int
in6_src_ioctl(u_long cmd, caddr_t data)
{
	int i;
	struct in6_addrpolicy ent0;

	if (cmd != SIOCAADDRCTL_POLICY && cmd != SIOCDADDRCTL_POLICY)
		return (EOPNOTSUPP); /* check for safety */

	ent0 = *(struct in6_addrpolicy *)data;

	if (ent0.label == ADDR_LABEL_NOTAPP)
		return (EINVAL);
	/* check if the prefix mask is consecutive. */
	if (in6_mask2len(&ent0.addrmask.sin6_addr, NULL) < 0)
		return (EINVAL);
	/* clear trailing garbages (if any) of the prefix address. */
	for (i = 0; i < 4; i++) {
		ent0.addr.sin6_addr.s6_addr32[i] &=
			ent0.addrmask.sin6_addr.s6_addr32[i];
	}
	ent0.use = 0;

	switch (cmd) {
	case SIOCAADDRCTL_POLICY:
		return (add_addrsel_policyent(&ent0));
	case SIOCDADDRCTL_POLICY:
		return (delete_addrsel_policyent(&ent0));
	}

	return (0);		/* XXX: compromise compilers */
}

/*
 * The followings are implementation of the policy table using a
 * simple tail queue.
 * XXX such details should be hidden.
 * XXX implementation using binary tree should be more efficient.
 */
struct addrsel_policyent {
	TAILQ_ENTRY(addrsel_policyent) ape_entry;
	struct in6_addrpolicy ape_policy;
};

TAILQ_HEAD(addrsel_policyhead, addrsel_policyent);

static VNET_DEFINE(struct addrsel_policyhead, addrsel_policytab);
#define	V_addrsel_policytab		VNET(addrsel_policytab)

static void
init_policy_queue(void)
{

	TAILQ_INIT(&V_addrsel_policytab);
}

static int
add_addrsel_policyent(struct in6_addrpolicy *newpolicy)
{
	struct addrsel_policyent *new, *pol;

	new = malloc(sizeof(*new), M_IFADDR,
	       M_WAITOK);
	ADDRSEL_XLOCK();
	ADDRSEL_LOCK();

	/* duplication check */
	TAILQ_FOREACH(pol, &V_addrsel_policytab, ape_entry) {
		if (IN6_ARE_ADDR_EQUAL(&newpolicy->addr.sin6_addr,
				       &pol->ape_policy.addr.sin6_addr) &&
		    IN6_ARE_ADDR_EQUAL(&newpolicy->addrmask.sin6_addr,
				       &pol->ape_policy.addrmask.sin6_addr)) {
			ADDRSEL_UNLOCK();
			ADDRSEL_XUNLOCK();
			free(new, M_IFADDR);
			return (EEXIST);	/* or override it? */
		}
	}

	bzero(new, sizeof(*new));

	/* XXX: should validate entry */
	new->ape_policy = *newpolicy;

	TAILQ_INSERT_TAIL(&V_addrsel_policytab, new, ape_entry);
	ADDRSEL_UNLOCK();
	ADDRSEL_XUNLOCK();

	return (0);
}

static int
delete_addrsel_policyent(struct in6_addrpolicy *key)
{
	struct addrsel_policyent *pol;

	ADDRSEL_XLOCK();
	ADDRSEL_LOCK();

	/* search for the entry in the table */
	TAILQ_FOREACH(pol, &V_addrsel_policytab, ape_entry) {
		if (IN6_ARE_ADDR_EQUAL(&key->addr.sin6_addr,
		    &pol->ape_policy.addr.sin6_addr) &&
		    IN6_ARE_ADDR_EQUAL(&key->addrmask.sin6_addr,
		    &pol->ape_policy.addrmask.sin6_addr)) {
			break;
		}
	}
	if (pol == NULL) {
		ADDRSEL_UNLOCK();
		ADDRSEL_XUNLOCK();
		return (ESRCH);
	}

	TAILQ_REMOVE(&V_addrsel_policytab, pol, ape_entry);
	ADDRSEL_UNLOCK();
	ADDRSEL_XUNLOCK();

	return (0);
}

static int
walk_addrsel_policy(int (*callback)(struct in6_addrpolicy *, void *), void *w)
{
	struct addrsel_policyent *pol;
	int error = 0;

	ADDRSEL_SLOCK();
	TAILQ_FOREACH(pol, &V_addrsel_policytab, ape_entry) {
		if ((error = (*callback)(&pol->ape_policy, w)) != 0) {
			ADDRSEL_SUNLOCK();
			return (error);
		}
	}
	ADDRSEL_SUNLOCK();
	return (error);
}

static int
dump_addrsel_policyent(struct in6_addrpolicy *pol, void *arg)
{
	int error = 0;
	struct walkarg *w = arg;

	error = SYSCTL_OUT(w->w_req, pol, sizeof(*pol));

	return (error);
}

static struct in6_addrpolicy *
match_addrsel_policy(struct sockaddr_in6 *key)
{
	struct addrsel_policyent *pent;
	struct in6_addrpolicy *bestpol = NULL, *pol;
	int matchlen, bestmatchlen = -1;
	u_char *mp, *ep, *k, *p, m;

	TAILQ_FOREACH(pent, &V_addrsel_policytab, ape_entry) {
		matchlen = 0;

		pol = &pent->ape_policy;
		mp = (u_char *)&pol->addrmask.sin6_addr;
		ep = mp + 16;	/* XXX: scope field? */
		k = (u_char *)&key->sin6_addr;
		p = (u_char *)&pol->addr.sin6_addr;
		for (; mp < ep && *mp; mp++, k++, p++) {
			m = *mp;
			if ((*k & m) != *p)
				goto next; /* not match */
			if (m == 0xff) /* short cut for a typical case */
				matchlen += 8;
			else {
				while (m >= 0x80) {
					matchlen++;
					m <<= 1;
				}
			}
		}

		/* matched.  check if this is better than the current best. */
		if (bestpol == NULL ||
		    matchlen > bestmatchlen) {
			bestpol = pol;
			bestmatchlen = matchlen;
		}

	  next:
		continue;
	}

	return (bestpol);
}

/*
 * This function is similar to in6_addrscope, but has some difference,
 * specific for the source address selection algorithm (RFC 6724).
 */
static int
in6_srcaddrscope(const struct in6_addr *addr)
{

	/* 169.254/16 and 127/8 have link-local scope */
	if (IN6_IS_ADDR_V4MAPPED(addr)) {
		if (addr->s6_addr[12] == 127 || (
		    addr->s6_addr[12] == 169 && addr->s6_addr[13] == 254))
			return (IPV6_ADDR_SCOPE_LINKLOCAL);
	}
	return (in6_addrscope(addr));
}

static int
lookup_policy_label(const struct sockaddr_in6 *sa6)
{

	return (lookup_addrsel_policy(sa6)->label);
}
