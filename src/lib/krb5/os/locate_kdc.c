/*
 * lib/krb5/os/locate_kdc.c
 *
 * Copyright 1990,2000,2001 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 *
 * get socket addresses for KDC.
 */

#define NEED_SOCKETS
#include "k5-int.h"
#include "os-proto.h"
#include <stdio.h>
#ifdef KRB5_DNS_LOOKUP
#ifdef WSHELPER
#include <wshelper.h>
#else /* WSHELPER */
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#endif /* WSHELPER */
#ifndef T_SRV
#define T_SRV 33
#endif /* T_SRV */

/* for old Unixes and friends ... */
#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif

#define MAX_DNS_NAMELEN (15*(MAXHOSTNAMELEN + 1)+1)

#if KRB5_DNS_LOOKUP_KDC
#define DEFAULT_LOOKUP_KDC 1
#else
#define DEFAULT_LOOKUP_KDC 0
#endif
#if KRB5_DNS_LOOKUP_REALM
#define DEFAULT_LOOKUP_REALM 1
#else
#define DEFAULT_LOOKUP_REALM 0
#endif

static int
maybe_use_dns (context, name, defalt)
     krb5_context context;
     const char *name;
     int defalt;
{
    krb5_error_code code;
    char * value = NULL;
    int use_dns = 0;

    code = profile_get_string(context->profile, "libdefaults",
                              name, 0, 0, &value);
    if (value == 0 && code == 0)
	code = profile_get_string(context->profile, "libdefaults",
				  "dns_fallback", 0, 0, &value);
    if (code)
        return defalt;

    if (value == 0)
	return defalt;

    use_dns = _krb5_conf_boolean(value);
    profile_release_string(value);
    return use_dns;
}

int
_krb5_use_dns_kdc(context)
    krb5_context context;
{
    return maybe_use_dns (context, "dns_lookup_kdc", DEFAULT_LOOKUP_KDC);
}

int
_krb5_use_dns_realm(context)
    krb5_context context;
{
    return maybe_use_dns (context, "dns_lookup_realm", DEFAULT_LOOKUP_REALM);
}

#endif /* KRB5_DNS_LOOKUP */

static int get_port (const char *service, int stream, int defalt)
{
#ifdef HAVE_GETADDRINFO
    struct addrinfo hints = { 0 };
    struct addrinfo *ai;
    int err;

    hints.ai_family = PF_INET;
    hints.ai_socktype = stream ? SOCK_STREAM : SOCK_DGRAM;
    err = getaddrinfo (NULL, service, &hints, &ai);
    if (err == 0 && ai != 0) {
	if (ai->ai_addr->sa_family == AF_INET) {
	    int port = ((struct sockaddr_in *)ai->ai_addr)->sin_port;
	    freeaddrinfo (ai);
	    return port;
	}
	freeaddrinfo (ai);
    }
    /* Any error - don't complain, just use default.  */
    return htons (defalt);
#else
    struct servent *sp;
    sp = getservbyname (service, stream ? "tcp" : "udp"); /* NOT THREAD SAFE */
    if (sp)
	return sp->s_port;
    return htons (defalt);
#endif
}

struct addrlist {
    struct sockaddr **addrs;
    int naddrs;
    int space;
};
#define ADDRLIST_INIT { 0, 0, 0 }

static int
grow_list (struct addrlist *lp, int nmore)
{
    int i;
    int newspace = lp->space + nmore;
    size_t newsize = newspace * sizeof (struct addrlist);
    struct sockaddr **newaddrs;

    /* NULL check a concession to SunOS4 compatibility for now; not
       required for pure ANSI support.  */
    if (lp->addrs)
	newaddrs = realloc (lp->addrs, newsize);
    else
	newaddrs = malloc (newsize);

    if (newaddrs == NULL)
	return errno;
    for (i = lp->space; i < newspace; i++)
	newaddrs[i] = NULL;
    lp->addrs = newaddrs;
    lp->space = newspace;
    return 0;
}

static void
free_list (struct addrlist *lp)
{
    int i;
    for (i = 0; i < lp->naddrs; i++)
	free (lp->addrs[i]);
    free (lp->addrs);
    lp->addrs = NULL;
    lp->naddrs = lp->space = 0;
}

static int
add_sockaddr_to_list (struct addrlist *lp, const struct sockaddr *addr,
		      size_t len)
{
    struct sockaddr *copy;

#ifdef TEST
    fprintf (stderr, "\tadding sockaddr family %2d, len %d", addr->sa_family,
	     len);
#ifdef HAVE_GETNAMEINFO
    {
	char name[NI_MAXHOST];
	int err;

	err = getnameinfo (addr, len, name, sizeof (name), NULL, 0,
			   NI_NUMERICHOST | NI_NUMERICSERV);
	if (err == 0)
	    fprintf (stderr, "\t%s", name);
    }
#else
    if (addr->sa_family == AF_INET)
	fprintf (stderr, "\t%s",
		 inet_ntoa (((const struct sockaddr_in *)addr)->sin_addr));
#endif
    fprintf (stderr, "\n");
#endif

    if (lp->naddrs == lp->space) {
	int err = grow_list (lp, 1);
	if (err) {
#ifdef TEST
	    fprintf (stderr, "grow_list failed %d\n", err);
#endif
	    return err;
	}
    }
    copy = malloc (len);
    if (copy == NULL) {
#ifdef TEST
	perror ("malloc");
#endif
	return errno;
    }
    memcpy (copy, addr, len);
    lp->addrs[lp->naddrs++] = copy;
#ifdef TEST
    fprintf (stderr, "count is now %d\n", lp->naddrs);
#endif
    return 0;
}

#ifdef HAVE_GETADDRINFO
static int translate_ai_error (int err)
{
    switch (err) {
    case 0:
	return 0;
    case EAI_ADDRFAMILY:
    case EAI_BADFLAGS:
    case EAI_FAMILY:
    case EAI_SOCKTYPE:
    case EAI_SERVICE:
	/* All of these indicate bad inputs to getaddrinfo.  */
	return EINVAL;
    case EAI_AGAIN:
	/* Translate to standard errno code.  */
	return EAGAIN;
    case EAI_MEMORY:
	/* Translate to standard errno code.  */
	return ENOMEM;
    case EAI_NODATA:
    case EAI_NONAME:
	/* Name not known or no address data, but no error.  Do
	   nothing more.  */
	return 0;
    case EAI_SYSTEM:
	/* System error, obviously.  */
	return errno;
    default:
	/* An error code we haven't handled?  */
	return EINVAL;
    }
}

static int add_addrinfo_to_list (struct addrlist *lp, struct addrinfo *a)
{
    return add_sockaddr_to_list (lp, a->ai_addr, a->ai_addrlen);
}

static void set_port_num (struct sockaddr *addr, int num)
{
    switch (addr->sa_family) {
    case AF_INET:
	((struct sockaddr_in *)addr)->sin_port = num;
	break;
    case AF_INET6:
	((struct sockaddr_in6 *)addr)->sin6_port = num;
	break;
    }
}
#endif

static int
add_host_to_list (struct addrlist *lp, const char *hostname,
		  int port, int secport)
{
#ifdef HAVE_GETADDRINFO
    struct addrinfo *addrs, *a;
#else
    struct hostent *hp;
#endif
    int err;

#ifdef TEST
    fprintf (stderr, "adding hostname %s, ports %d,%d\n", hostname,
	     ntohs (port), ntohs (secport));
#endif

#ifdef HAVE_GETADDRINFO
    err = getaddrinfo (hostname, NULL, NULL, &addrs);
    if (err)
	return translate_ai_error (err);
    for (a = addrs; a; a = a->ai_next) {
	set_port_num (a->ai_addr, port);
	err = add_addrinfo_to_list (lp, a);
	if (err)
	    break;

	if (secport == 0)
	    continue;

	set_port_num (a->ai_addr, secport);
	err = add_addrinfo_to_list (lp, a);
	if (err)
	    break;
    }
    freeaddrinfo (addrs);
#else
    hp = gethostbyname (hostname);
    if (hp != NULL) {
	int i;
	for (i = 0; hp->h_addr_list[i] != 0; i++) {
	    struct sockaddr_in sin4;

	    memset (&sin4, 0, sizeof (sin4));
	    memcpy (&sin4.sin_addr, hp->h_addr_list[i],
		    sizeof (sin4.sin_addr));
	    sin4.sin_family = AF_INET;
	    sin4.sin_port = port;
	    err = add_sockaddr_to_list (lp, (struct sockaddr *) &sin4,
					sizeof (sin4));
	    if (err)
		break;
	    if (secport != 0) {
		sin4.sin_port = secport;
		err = add_sockaddr_to_list (lp, (struct sockaddr *) &sin4,
					    sizeof (sin4));
	    }

	    if (err)
		break;
	}
    }
#endif
    return err;
}

/*
 * returns count of number of addresses found
 * if master is non-NULL, it is filled in with the index of
 * the master kdc
 */

static krb5_error_code
krb5_locate_srv_conf_1(krb5_context context, const krb5_data *realm,
		       const char * name, struct addrlist *addrlist,
		       int get_masters, int udpport, int sec_udpport)
{
    const char	*realm_srv_names[4];
    char **masterlist, **hostlist, *host, *port, *cp;
    krb5_error_code code;
    int i, j, count, ismaster;

#ifdef TEST
    fprintf (stderr,
	     "looking in krb5.conf for realm %s entry %s; ports %d,%d\n",
	     realm->data, name, ntohs (udpport), ntohs (sec_udpport));
#endif

    if ((host = malloc(realm->length + 1)) == NULL) 
	return ENOMEM;

    strncpy(host, realm->data, realm->length);
    host[realm->length] = '\0';
    hostlist = 0;

    masterlist = NULL;

    realm_srv_names[0] = "realms";
    realm_srv_names[1] = host;
    realm_srv_names[2] = name;
    realm_srv_names[3] = 0;

    code = profile_get_values(context->profile, realm_srv_names, &hostlist);

    if (code) {
        if (code == PROF_NO_SECTION || code == PROF_NO_RELATION)
            code = KRB5_REALM_UNKNOWN;
 	krb5_xfree(host);
  	return code;
     }

    count = 0;
    while (hostlist && hostlist[count])
	    count++;
    
    if (count == 0) {
        profile_free_list(hostlist);
	krb5_xfree(host);
	addrlist->naddrs = 0;
	return 0;
    }
    
    if (get_masters) {
	realm_srv_names[0] = "realms";
	realm_srv_names[1] = host;
	realm_srv_names[2] = "admin_server";
	realm_srv_names[3] = 0;

	code = profile_get_values(context->profile, realm_srv_names,
				  &masterlist);

	krb5_xfree(host);

	if (code == 0) {
	    for (i=0; masterlist[i]; i++) {
		host = masterlist[i];

		/*
		 * Strip off excess whitespace
		 */
		cp = strchr(host, ' ');
		if (cp)
		    *cp = 0;
		cp = strchr(host, '\t');
		if (cp)
		    *cp = 0;
		cp = strchr(host, ':');
		if (cp)
		    *cp = 0;
	    }
	}
    } else {
	krb5_xfree(host);
    }

    /* at this point, if master is non-NULL, then either the master kdc
       is required, and there is one, or the master kdc is not required,
       and there may or may not be one. */

#ifdef HAVE_NETINET_IN_H
    if (sec_udpport)
	    count = count * 2;
#endif

    for (i=0; hostlist[i]; i++) {
	int p1, p2;

	host = hostlist[i];
	/*
	 * Strip off excess whitespace
	 */
	cp = strchr(host, ' ');
	if (cp)
	    *cp = 0;
	cp = strchr(host, '\t');
	if (cp)
	    *cp = 0;
	port = strchr(host, ':');
	if (port) {
	    *port = 0;
	    port++;
	}

	ismaster = 0;
	if (masterlist) {
	    for (j=0; masterlist[j]; j++) {
		if (strcasecmp(hostlist[i], masterlist[j]) == 0) {
		    ismaster = 1;
		}
	    }
	}

	if (get_masters && !ismaster)
	    continue;

	if (port) {
	    unsigned long l;
#ifdef HAVE_STROUL
	    char *endptr;
	    l = strtoul (port, &endptr, 10);
	    if (endptr == NULL || *endptr != 0)
		return EINVAL;
#else
	    l = atoi (port);
#endif
	    /* L is unsigned, don't need to check <0.  */
	    if (l > 65535)
		return EINVAL;
	    p1 = htons (l);
	    p2 = 0;
	} else {
	    p1 = udpport;
	    p2 = sec_udpport;
	}

	code = add_host_to_list (addrlist, hostlist[i], p1, p2);
	if (code) {
#ifdef TEST
	    fprintf (stderr, "error %d returned from add_host_to_list\n", code);
#endif
	    if (hostlist)
		profile_free_list (hostlist);
	    if (masterlist)
		profile_free_list (masterlist);
	    return code;
	}
    }

    if (hostlist)
        profile_free_list(hostlist);
    if (masterlist)
        profile_free_list(masterlist);

    return 0;
}

#ifdef TEST
static krb5_error_code
krb5_locate_srv_conf(context, realm, name, addr_pp, naddrs, get_masters,
		     udpport, sec_udpport)
    krb5_context context;
    const krb5_data *realm;
    const char * name;
    struct sockaddr ***addr_pp;
    int *naddrs;
    int get_masters;
    int udpport, sec_udpport;
{
    krb5_error_code ret;
    struct addrlist al = ADDRLIST_INIT;

    ret = krb5_locate_srv_conf_1 (context, realm, name, &al,
				  get_masters, udpport, sec_udpport);
    if (ret) {
	free_list (&al);
	return ret;
    }
    if (al.naddrs == 0)		/* Couldn't resolve any KDC names */
	return KRB5_REALM_CANT_RESOLVE;
    *addr_pp = al.addrs;
    *naddrs = al.naddrs;
    return 0;
}
#endif

#ifdef KRB5_DNS_LOOKUP

/*
 * Lookup a KDC via DNS SRV records
 */

static krb5_error_code
krb5_locate_srv_dns_1 (const krb5_data *realm,
		       const char *service,
		       const char *protocol,
		       struct addrlist *addrlist)
{
    union {
        unsigned char bytes[2048];
        HEADER hdr;
    } answer;
    unsigned char *p=NULL;
    char host[MAX_DNS_NAMELEN], *h;
    int type, class;
    int priority, weight, size, len, numanswers, numqueries, rdlen;
    unsigned short port;
    const int hdrsize = sizeof(HEADER);
    struct srv_dns_entry {
	struct srv_dns_entry *next;
	int priority;
	int weight;
	unsigned short port;
	char *host;
    };

    struct srv_dns_entry *head = NULL;
    struct srv_dns_entry *srv = NULL, *entry = NULL;
    krb5_error_code code = 0;

    /*
     * First off, build a query of the form:
     *
     * service.protocol.realm
     *
     * which will most likely be something like:
     *
     * _kerberos._udp.REALM
     *
     */

    if ( strlen(service) + strlen(protocol) + realm->length + 6 
         > MAX_DNS_NAMELEN )
        goto out;
    sprintf(host, "%s.%s.%.*s", service, protocol, (int) realm->length,
	    realm->data);

    /* Realm names don't (normally) end with ".", but if the query
       doesn't end with "." and doesn't get an answer as is, the
       resolv code will try appending the local domain.  Since the
       realm names are absolutes, let's stop that.  

       But only if a name has been specified.  If we are performing
       a search on the prefix alone then the intention is to allow
       the local domain or domain search lists to be expanded.  */

    h = host + strlen (host);
    if ((h > host) && (h[-1] != '.') && ((h - host + 1) < sizeof(host)))
        strcpy (h, ".");

#ifdef TEST
    fprintf (stderr, "sending DNS SRV query for %s\n", host);
#endif

    size = res_search(host, C_IN, T_SRV, answer.bytes, sizeof(answer.bytes));

    if (size < hdrsize)
	goto out;

    /*
     * We got an answer!  First off, parse the header and figure out how
     * many answers we got back.
     */

    p = answer.bytes;

    numqueries = ntohs(answer.hdr.qdcount);
    numanswers = ntohs(answer.hdr.ancount);

    p += sizeof(HEADER);

    /*
     * We need to skip over all of the questions, so we have to iterate
     * over every query record.  dn_expand() is able to tell us the size
     * of compress DNS names, so we use it.
     */

#define INCR_CHECK(x,y) x += y; if (x > size + answer.bytes) goto out
#define CHECK(x,y) if (x + y > size + answer.bytes) goto out
#define NTOHSP(x,y) x[0] << 8 | x[1]; x += y

    while (numqueries--) {
	len = dn_expand(answer.bytes, answer.bytes + size, p, host, sizeof(host));
	if (len < 0)
	    goto out;
	INCR_CHECK(p, len + 4);
    }

    /*
     * We're now pointing at the answer records.  Only process them if
     * they're actually T_SRV records (they might be CNAME records,
     * for instance).
     *
     * But in a DNS reply, if you get a CNAME you always get the associated
     * "real" RR for that CNAME.  RFC 1034, 3.6.2:
     *
     * CNAME RRs cause special action in DNS software.  When a name server
     * fails to find a desired RR in the resource set associated with the
     * domain name, it checks to see if the resource set consists of a CNAME
     * record with a matching class.  If so, the name server includes the CNAME
     * record in the response and restarts the query at the domain name
     * specified in the data field of the CNAME record.  The one exception to
     * this rule is that queries which match the CNAME type are not restarted.
     *
     * In other words, CNAMEs do not need to be expanded by the client.
     */

    while (numanswers--) {

	/* First is the name; use dn_expand to get the compressed size */
	len = dn_expand(answer.bytes, answer.bytes + size, p, host, sizeof(host));
	if (len < 0)
	    goto out;
	INCR_CHECK(p, len);

	/* Next is the query type */
        CHECK(p, 2);
	type = NTOHSP(p,2);

	/* Next is the query class; also skip over 4 byte TTL */
        CHECK(p, 6);
	class = NTOHSP(p,6);

	/* Record data length */

        CHECK(p,2);
	rdlen = NTOHSP(p,2);

	/*
	 * If this is an SRV record, process it.  Record format is:
	 *
	 * Priority
	 * Weight
	 * Port
	 * Server name
	 */

	if (class == C_IN && type == T_SRV) {
            CHECK(p,2);
	    priority = NTOHSP(p,2);
	    CHECK(p, 2);
	    weight = NTOHSP(p,2);
	    CHECK(p, 2);
	    port = NTOHSP(p,2);
	    len = dn_expand(answer.bytes, answer.bytes + size, p, host, sizeof(host));
	    if (len < 0)
		goto out;
	    INCR_CHECK(p, len);

	    /*
	     * We got everything!  Insert it into our list, but make sure
	     * it's in the right order.  Right now we don't do anything
	     * with the weight field
	     */

	    srv = (struct srv_dns_entry *) malloc(sizeof(struct srv_dns_entry));
	    if (srv == NULL)
		goto out;
	
	    srv->priority = priority;
	    srv->weight = weight;
	    srv->port = port;
	    srv->host = strdup(host);

	    if (head == NULL || head->priority > srv->priority) {
		srv->next = head;
		head = srv;
	    } else
		/*
		 * This is confusing.  Only insert an entry into this
		 * spot if:
		 * The next person has a higher priority (lower priorities
		 * are preferred).
		 * Or
		 * There is no next entry (we're at the end)
		 */
		for (entry = head; entry != NULL; entry = entry->next)
		    if ((entry->next &&
			 entry->next->priority > srv->priority) ||
			entry->next == NULL) {
			srv->next = entry->next;
			entry->next = srv;
			break;
		    }
	} else
	    INCR_CHECK(p, rdlen);
    }
	
    /*
     * Okay!  Now we've got a linked list of entries sorted by
     * priority.  Start looking up A records and returning
     * addresses.
     */

    if (head == NULL)
	goto out;

#ifdef TEST
    fprintf (stderr, "walking answer list:\n");
#endif
    for (entry = head; entry != NULL; entry = entry->next) {
#ifdef TEST
	fprintf (stderr, "\tport=%d host=%s\n", entry->port, entry->host);
#endif
	code = add_host_to_list (addrlist, entry->host, htons (entry->port), 0);
	if (code)
	    break;
    }
#ifdef TEST
    fprintf (stderr, "[end]\n");
#endif

    for (entry = head; entry != NULL; ) {
	free(entry->host);
        entry->host = NULL;
	srv = entry;
	entry = entry->next;
	free(srv);
        srv = NULL;
    }

  out:
    if (srv)
        free(srv);

    return code;
}

#ifdef TEST
static krb5_error_code
krb5_locate_srv_dns(const krb5_data *realm,
		    const char *service, const char *protocol,
		    struct sockaddr ***addr_pp, int *naddrs)
{
    struct addrlist al = ADDRLIST_INIT;
    krb5_error_code code;

    code = krb5_locate_srv_dns_1 (realm, service, protocol, &al);
    *addr_pp = al.addrs;
    *naddrs = al.naddrs;
    return code;
}
#endif
#endif /* KRB5_DNS_LOOKUP */

/*
 * Wrapper function for the two backends
 */

krb5_error_code
krb5int_locate_server (krb5_context context, const krb5_data *realm,
		       struct sockaddr ***addr_pp, int *naddrs,
		       int get_masters,
		       const char *profname, const char *dnsname,
		       int is_stream,
		       /* network order port numbers! */
		       int dflport1, int dflport2)
{
    krb5_error_code code;
    struct addrlist al = ADDRLIST_INIT;

    /*
     * We always try the local file first
     */

    code = krb5_locate_srv_conf_1(context, realm, profname, &al, get_masters,
				  dflport1, dflport2);

#ifdef KRB5_DNS_LOOKUP
    if (code && dnsname != 0) {
	int use_dns = _krb5_use_dns_kdc(context);
	if (use_dns)
	    code = krb5_locate_srv_dns_1(realm, dnsname,
					 is_stream ? "_tcp" : "_udp", &al);
    }
#endif /* KRB5_DNS_LOOKUP */
#ifdef TEST
    if (code == 0)
	fprintf (stderr, "krb5int_locate_server found %d addresses\n",
		 al.naddrs);
    else
	fprintf (stderr, "krb5int_locate_server returning error code %d\n",
		 code);
#endif
    if (code != 0) {
	if (al.space)
	    free_list (&al);
	return code;
    }
    if (al.naddrs == 0) {	/* No good servers */
	if (al.space)
	    free_list (&al);
	return KRB5_REALM_CANT_RESOLVE;
    }
    *addr_pp = al.addrs;
    *naddrs = al.naddrs;
    return 0;
}

krb5_error_code
krb5_locate_kdc(context, realm, addr_pp, naddrs, get_masters)
    krb5_context context;
    const krb5_data *realm;
    struct sockaddr ***addr_pp;
    int *naddrs;
    int get_masters;
{
    int udpport, sec_udpport;

    udpport = get_port (KDC_PORTNAME, 0, KRB5_DEFAULT_PORT);
    sec_udpport = get_port (KDC_SECONDARY_PORTNAME, 0,
			    (udpport == htons (KRB5_DEFAULT_PORT)
			     ? KRB5_DEFAULT_SEC_PORT
			     : KRB5_DEFAULT_PORT));
    if (sec_udpport == udpport)
	sec_udpport = 0;

    return krb5int_locate_server (context, realm, addr_pp, naddrs, get_masters,
				  "kdc",
				  (get_masters
				   ? "_kerberos-master"
				   : "_kerberos"),
				  0, udpport, sec_udpport);
}
