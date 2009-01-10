/*
 * kdc/main.c
 *
 * Copyright 1990,2001,2008 by the Massachusetts Institute of Technology.
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
 * Main procedure body for the KDC server process.
 */
/*
 * Copyright (c) 2006-2008, Novell, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *   * The copyright holder's name is not used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <netdb.h>

#include "k5-int.h"
#include "com_err.h"
#include "adm.h"
#include "adm_proto.h"
#include "kdc_util.h"
#include "extern.h"
#include "kdc5_err.h"
#include "kdb_kt.h"
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#if defined(NEED_DAEMON_PROTO)
extern int daemon(int, int);
#endif

void usage (char *);

krb5_sigtype request_exit (int);
krb5_sigtype request_hup  (int);

void setup_signal_handlers (void);

krb5_error_code setup_sam (void);

void initialize_realms (krb5_context, int, char **);

void finish_realms (char *);

static int nofork = 0;
static int rkey_init_done = 0;

#ifdef POSIX_SIGNALS
static struct sigaction s_action;
#endif /* POSIX_SIGNALS */

#define	KRB5_KDC_MAX_REALMS	32

/*
 * Find the realm entry for a given realm.
 */
kdc_realm_t *
find_realm_data(char *rname, krb5_ui_4 rsize)
{
    int i;
    for (i=0; i<kdc_numrealms; i++) {
	if ((rsize == strlen(kdc_realmlist[i]->realm_name)) &&
	    !strncmp(rname, kdc_realmlist[i]->realm_name, rsize))
	    return(kdc_realmlist[i]);
    }
    return((kdc_realm_t *) NULL);
}

krb5_error_code
setup_server_realm(krb5_principal sprinc)
{
    krb5_error_code	kret;
    kdc_realm_t		*newrealm;

    kret = 0;
    if (kdc_numrealms > 1) {
	if (!(newrealm = find_realm_data(sprinc->realm.data,
					 (krb5_ui_4) sprinc->realm.length)))
	    kret = ENOENT;
	else
	    kdc_active_realm = newrealm;
    }
    else
	kdc_active_realm = kdc_realmlist[0];
    return(kret);
}

static void
finish_realm(kdc_realm_t *rdp)
{
    if (rdp->realm_dbname)
	free(rdp->realm_dbname);
    if (rdp->realm_mpname)
	free(rdp->realm_mpname);
    if (rdp->realm_stash)
	free(rdp->realm_stash);
    if (rdp->realm_ports)
	free(rdp->realm_ports);
    if (rdp->realm_tcp_ports)
	free(rdp->realm_tcp_ports);
    if (rdp->realm_keytab)
	krb5_kt_close(rdp->realm_context, rdp->realm_keytab);
    if (rdp->realm_context) {
	if (rdp->realm_mprinc)
	    krb5_free_principal(rdp->realm_context, rdp->realm_mprinc);
	if (rdp->realm_mkey.length && rdp->realm_mkey.contents) {
	    memset(rdp->realm_mkey.contents, 0, rdp->realm_mkey.length);
	    free(rdp->realm_mkey.contents);
	}
	krb5_db_fini(rdp->realm_context);
	if (rdp->realm_tgsprinc)
	    krb5_free_principal(rdp->realm_context, rdp->realm_tgsprinc);
	krb5_free_context(rdp->realm_context);
    }
    memset((char *) rdp, 0, sizeof(*rdp));
    free(rdp);
}

/*
 * Initialize a realm control structure from the alternate profile or from
 * the specified defaults.
 *
 * After we're complete here, the essence of the realm is embodied in the
 * realm data and we should be all set to begin operation for that realm.
 */
static krb5_error_code
init_realm(char *progname, kdc_realm_t *rdp, char *realm, 
	   char *def_mpname, krb5_enctype def_enctype, char *def_udp_ports,
	   char *def_tcp_ports, krb5_boolean def_manual, char **db_args)
{
    krb5_error_code	kret;
    krb5_boolean	manual;
    krb5_realm_params	*rparams;
    int			kdb_open_flags;
    krb5_kvno       mkvno = IGNORE_VNO;

    memset((char *) rdp, 0, sizeof(kdc_realm_t));
    if (!realm) {
	kret = EINVAL;
	goto whoops;
    }
	
    rdp->realm_name = realm;
    kret = krb5int_init_context_kdc(&rdp->realm_context);
    if (kret) {
	com_err(progname, kret, "while getting context for realm %s",
		realm);
	goto whoops;
    }

    kret = krb5_read_realm_params(rdp->realm_context, rdp->realm_name,
				  &rparams);
    if (kret) {
	com_err(progname, kret, "while reading realm parameters");
	goto whoops;
    }
    
    /* Handle profile file name */
    if (rparams && rparams->realm_profile)
	rdp->realm_profile = strdup(rparams->realm_profile);

    /* Handle master key name */
    if (rparams && rparams->realm_mkey_name)
	rdp->realm_mpname = strdup(rparams->realm_mkey_name);
    else
	rdp->realm_mpname = (def_mpname) ? strdup(def_mpname) :
	    strdup(KRB5_KDB_M_NAME);

    /* Handle KDC ports */
    if (rparams && rparams->realm_kdc_ports)
	rdp->realm_ports = strdup(rparams->realm_kdc_ports);
    else
	rdp->realm_ports = strdup(def_udp_ports);
    if (rparams && rparams->realm_kdc_tcp_ports)
	rdp->realm_tcp_ports = strdup(rparams->realm_kdc_tcp_ports);
    else
	rdp->realm_tcp_ports = strdup(def_tcp_ports);

    /* Handle stash file */
    if (rparams && rparams->realm_stash_file) {
	rdp->realm_stash = strdup(rparams->realm_stash_file);
	manual = FALSE;
    } else
	manual = def_manual;

    /* Handle master key type */
    if (rparams && rparams->realm_enctype_valid)
	rdp->realm_mkey.enctype = (krb5_enctype) rparams->realm_enctype;
    else
	rdp->realm_mkey.enctype = manual ? def_enctype : ENCTYPE_UNKNOWN;

    /* Handle reject-bad-transit flag */
    if (rparams && rparams->realm_reject_bad_transit_valid)
	rdp->realm_reject_bad_transit = rparams->realm_reject_bad_transit;
    else
	rdp->realm_reject_bad_transit = 1;

    /* Handle ticket maximum life */
    rdp->realm_maxlife = (rparams && rparams->realm_max_life_valid) ?
	rparams->realm_max_life : KRB5_KDB_MAX_LIFE;

    /* Handle ticket renewable maximum life */
    rdp->realm_maxrlife = (rparams && rparams->realm_max_rlife_valid) ?
	rparams->realm_max_rlife : KRB5_KDB_MAX_RLIFE;

    if (rparams)
	krb5_free_realm_params(rdp->realm_context, rparams);

    /*
     * We've got our parameters, now go and setup our realm context.
     */

    /* Set the default realm of this context */
    if ((kret = krb5_set_default_realm(rdp->realm_context, realm))) {
	com_err(progname, kret, "while setting default realm to %s",
		realm);
	goto whoops;
    }

    /* first open the database  before doing anything */
#ifdef KRBCONF_KDC_MODIFIES_KDB
    kdb_open_flags = KRB5_KDB_OPEN_RW | KRB5_KDB_SRV_TYPE_KDC;
#else
    kdb_open_flags = KRB5_KDB_OPEN_RO | KRB5_KDB_SRV_TYPE_KDC;
#endif
    if ((kret = krb5_db_open(rdp->realm_context, db_args, kdb_open_flags))) {
	com_err(progname, kret,
		"while initializing database for realm %s", realm);
	goto whoops;
    }

    /* Assemble and parse the master key name */
    if ((kret = krb5_db_setup_mkey_name(rdp->realm_context, rdp->realm_mpname,
					rdp->realm_name, (char **) NULL,
					&rdp->realm_mprinc))) {
	com_err(progname, kret,
		"while setting up master key name %s for realm %s",
		rdp->realm_mpname, realm);
	goto whoops;
    }

    /*
     * Get the master key (note, may not be the most current mkey).
     */
    if ((kret = krb5_db_fetch_mkey(rdp->realm_context, rdp->realm_mprinc,
				   rdp->realm_mkey.enctype, manual,
				   FALSE, rdp->realm_stash,
				   &mkvno, NULL, &rdp->realm_mkey))) {
	com_err(progname, kret,
		"while fetching master key %s for realm %s",
		rdp->realm_mpname, realm);
	goto whoops;
    }

#if 0 /************** Begin IFDEF'ed OUT *******************************/
    /*
     * Commenting krb5_db_verify_master_key out because it requires the most
     * current mkey which may not be the case here.  The call to
     * krb5_db_fetch_mkey_list() will end up verifying that the mkey is viable
     * anyway.
     */
    /* Verify the master key */
    if ((kret = krb5_db_verify_master_key(rdp->realm_context,
					  rdp->realm_mprinc,
                                          IGNORE_VNO,
					  &rdp->realm_mkey))) {
	com_err(progname, kret,
		"while verifying master key for realm %s", realm);
	goto whoops;
    }
#endif /**************** END IFDEF'ed OUT *******************************/

    if ((kret = krb5_db_fetch_mkey_list(rdp->realm_context, rdp->realm_mprinc,
				   &rdp->realm_mkey, mkvno, &rdp->mkey_list))) {
	com_err(progname, kret,
		"while fetching master keys list for realm %s", realm);
	goto whoops;
    }

    if ((kret = krb5_db_set_mkey(rdp->realm_context, &rdp->realm_mkey))) {
	com_err(progname, kret,
		"while setting master key for realm %s", realm);
	goto whoops;
    }

    /* Set up the keytab */
    if ((kret = krb5_ktkdb_resolve(rdp->realm_context, NULL,
				   &rdp->realm_keytab))) {
	com_err(progname, kret,
		"while resolving kdb keytab for realm %s", realm);
	goto whoops;
    }

    /* Preformat the TGS name */
    if ((kret = krb5_build_principal(rdp->realm_context, &rdp->realm_tgsprinc,
				     strlen(realm), realm, KRB5_TGS_NAME,
				     realm, (char *) NULL))) {
	com_err(progname, kret,
		"while building TGS name for realm %s", realm);
	goto whoops;
    }

    if (!rkey_init_done) {
	krb5_data seed;
	/*
	 * If all that worked, then initialize the random key
	 * generators.
	 */

	seed.length = rdp->realm_mkey.length;
	seed.data = (char *)rdp->realm_mkey.contents;

	if ((kret = krb5_c_random_add_entropy(rdp->realm_context,
					     KRB5_C_RANDSOURCE_TRUSTEDPARTY, &seed)))
	    goto whoops;

	rkey_init_done = 1;
    }
 whoops:
    /*
     * If we choked, then clean up any dirt we may have dropped on the floor.
     */
    if (kret) {
        
	finish_realm(rdp);
    }
    return(kret);
}

krb5_sigtype
request_exit(int signo)
{
    signal_requests_exit = 1;

#ifdef POSIX_SIGTYPE
    return;
#else
    return(0);
#endif
}

krb5_sigtype
request_hup(int signo)
{
    signal_requests_hup = 1;

#ifdef POSIX_SIGTYPE
    return;
#else
    return(0);
#endif
}

void
setup_signal_handlers(void)
{
#ifdef POSIX_SIGNALS
    (void) sigemptyset(&s_action.sa_mask);
    s_action.sa_flags = 0;
    s_action.sa_handler = request_exit;
    (void) sigaction(SIGINT, &s_action, (struct sigaction *) NULL);
    (void) sigaction(SIGTERM, &s_action, (struct sigaction *) NULL);
    s_action.sa_handler = request_hup;
    (void) sigaction(SIGHUP, &s_action, (struct sigaction *) NULL);
    s_action.sa_handler = SIG_IGN;
    (void) sigaction(SIGPIPE, &s_action, (struct sigaction *) NULL);
#else  /* POSIX_SIGNALS */
    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);
    signal(SIGHUP, request_hup);
    signal(SIGPIPE, SIG_IGN);
#endif /* POSIX_SIGNALS */

    return;
}

krb5_error_code
setup_sam(void)
{
    return krb5_c_make_random_key(kdc_context, ENCTYPE_DES_CBC_MD5, &psr_key);
}

void
usage(char *name)
{
    fprintf(stderr, "usage: %s [-x db_args]* [-d dbpathname] [-r dbrealmname]\n\t\t[-R replaycachename] [-m] [-k masterenctype] [-M masterkeyname]\n\t\t[-p port] [-n]\n"
	    "\nwhere,\n\t[-x db_args]* - Any number of database specific arguments.  Look at\n"
	    "\t\t\teach database module documentation for supported\n\t\t\targuments\n",
	    name);
    return;
}

void
initialize_realms(krb5_context kcontext, int argc, char **argv)
{
    int 		c;
    char		*db_name = (char *) NULL;
    char		*mkey_name = (char *) NULL;
    char		*rcname = KDCRCACHE;
    char		*lrealm;
    krb5_error_code	retval;
    krb5_enctype	menctype = ENCTYPE_UNKNOWN;
    kdc_realm_t		*rdatap;
    krb5_boolean	manual = FALSE;
    char		*default_udp_ports = 0;
    char		*default_tcp_ports = 0;
    krb5_pointer	aprof;
    const char		*hierarchy[3];
    char               **db_args      = NULL;
    int                  db_args_size = 0;

    extern char *optarg;

    if (!krb5_aprof_init(DEFAULT_KDC_PROFILE, KDC_PROFILE_ENV, &aprof)) {
	hierarchy[0] = "kdcdefaults";
	hierarchy[1] = "kdc_ports";
	hierarchy[2] = (char *) NULL;
	if (krb5_aprof_get_string(aprof, hierarchy, TRUE, &default_udp_ports))
	    default_udp_ports = 0;
	hierarchy[1] = "kdc_tcp_ports";
	if (krb5_aprof_get_string(aprof, hierarchy, TRUE, &default_tcp_ports))
	    default_tcp_ports = 0;
	hierarchy[1] = "kdc_max_dgram_reply_size";
	if (krb5_aprof_get_int32(aprof, hierarchy, TRUE, &max_dgram_reply_size))
	    max_dgram_reply_size = MAX_DGRAM_SIZE;

	/* aprof_init can return 0 with aprof == NULL */
	if (aprof)
	     krb5_aprof_finish(aprof);
    }
    if (default_udp_ports == 0)
	default_udp_ports = strdup(DEFAULT_KDC_UDP_PORTLIST);
    if (default_tcp_ports == 0)
	default_tcp_ports = strdup(DEFAULT_KDC_TCP_PORTLIST);
    /*
     * Loop through the option list.  Each time we encounter a realm name,
     * use the previously scanned options to fill in for defaults.
     */
    while ((c = getopt(argc, argv, "x:r:d:mM:k:R:e:p:s:n4:X3")) != -1) {
	switch(c) {
	case 'x':
	    db_args_size++;
	    {
		char **temp = realloc( db_args, sizeof(char*) * (db_args_size+1)); /* one for NULL */
		if( temp == NULL )
		{
		    fprintf(stderr,"%s: KDC cannot initialize. Not enough memory\n",
			    argv[0]);
		    exit(1);
		}

		db_args = temp;
	    }
	    db_args[db_args_size-1] = optarg;
	    db_args[db_args_size]   = NULL;
	  break;

	case 'r':			/* realm name for db */
	    if (!find_realm_data(optarg, (krb5_ui_4) strlen(optarg))) {
		if ((rdatap = (kdc_realm_t *) malloc(sizeof(kdc_realm_t)))) {
		    if ((retval = init_realm(argv[0], rdatap, optarg, 
					     mkey_name, menctype,
					     default_udp_ports,
					     default_tcp_ports, manual, db_args))) {
			fprintf(stderr,"%s: cannot initialize realm %s - see log file for details\n",
				argv[0], optarg);
			exit(1);
		    }
		    kdc_realmlist[kdc_numrealms] = rdatap;
		    kdc_numrealms++;
		    free(db_args), db_args=NULL, db_args_size = 0;
		}
		else
		{
			fprintf(stderr,"%s: cannot initialize realm %s. Not enough memory\n",
				argv[0], optarg);
			exit(1);
		}
	    }
	    break;
	case 'd':			/* pathname for db */
	    /* now db_name is not a seperate argument. It has to be passed as part of the db_args */
	    if( db_name == NULL ) {
		if (asprintf(&db_name, "dbname=%s", optarg) < 0) {
		    fprintf(stderr,
			    "%s: KDC cannot initialize. Not enough memory\n",
			    argv[0]);
		    exit(1);
		}
	    }

	    db_args_size++;
	    {
		char **temp = realloc( db_args, sizeof(char*) * (db_args_size+1)); /* one for NULL */
		if( temp == NULL )
		{
		    fprintf(stderr,"%s: KDC cannot initialize. Not enough memory\n",
			    argv[0]);
		    exit(1);
		}

		db_args = temp;
	    }
	    db_args[db_args_size-1] = db_name;
	    db_args[db_args_size]   = NULL;
	    break;
	case 'm':			/* manual type-in of master key */
	    manual = TRUE;
	    if (menctype == ENCTYPE_UNKNOWN)
		menctype = ENCTYPE_DES_CBC_CRC;
	    break;
	case 'M':			/* master key name in DB */
	    mkey_name = optarg;
	    break;
	case 'n':
	    nofork++;			/* don't detach from terminal */
	    break;
	case 'k':			/* enctype for master key */
	    if (krb5_string_to_enctype(optarg, &menctype))
		com_err(argv[0], 0, "invalid enctype %s", optarg);
	    break;
	case 'R':
	    rcname = optarg;
	    break;
	case 'p':
	    if (default_udp_ports)
		free(default_udp_ports);
	    default_udp_ports = strdup(optarg);
#if 0 /* not yet */
	    if (default_tcp_ports)
		free(default_tcp_ports);
	    default_tcp_ports = strdup(optarg);
#endif
	    break;
	case '4':
	    break;
	case 'X':
	    break;
	case '?':
	default:
	    usage(argv[0]);
	    exit(1);
	}
    }

    /*
     * Check to see if we processed any realms.
     */
    if (kdc_numrealms == 0) {
	/* no realm specified, use default realm */
	if ((retval = krb5_get_default_realm(kcontext, &lrealm))) {
	    com_err(argv[0], retval,
		    "while attempting to retrieve default realm");
	    fprintf (stderr, "%s: %s, attempting to retrieve default realm\n",
		     argv[0], krb5_get_error_message(kcontext, retval));
	    exit(1);
	}
	if ((rdatap = (kdc_realm_t *) malloc(sizeof(kdc_realm_t)))) {
	    if ((retval = init_realm(argv[0], rdatap, lrealm, 
				     mkey_name, menctype, default_udp_ports,
				     default_tcp_ports, manual, db_args))) {
		fprintf(stderr,"%s: cannot initialize realm %s - see log file for details\n",
			argv[0], lrealm);
		exit(1);
	    }
	    kdc_realmlist[0] = rdatap;
	    kdc_numrealms++;
	}
    }

#ifdef USE_RCACHE
    /*
     * Now handle the replay cache.
     */
    if ((retval = kdc_initialize_rcache(kcontext, rcname))) {
	com_err(argv[0], retval, "while initializing KDC replay cache '%s'",
		rcname);
	exit(1);
    }
#endif

    /* Ensure that this is set for our first request. */
    kdc_active_realm = kdc_realmlist[0];
    if (default_udp_ports)
	free(default_udp_ports);
    if (default_tcp_ports)
	free(default_tcp_ports);
    if (db_args)
	free(db_args);
    if (db_name)
	free(db_name);

    return;
}

void
finish_realms(char *prog)
{
    int i;

    for (i = 0; i < kdc_numrealms; i++) {
	finish_realm(kdc_realmlist[i]);
	kdc_realmlist[i] = 0;
    }
}

/*
 outline:

 process args & setup

 initialize database access (fetch master key, open DB)

 initialize network

 loop:
 	listen for packet

	determine packet type, dispatch to handling routine
		(AS or TGS (or V4?))

	reflect response

	exit on signal

 clean up secrets, close db

 shut down network

 exit
 */

int main(int argc, char **argv)
{
    krb5_error_code	retval;
    krb5_context	kcontext;
    int errout = 0;

    if (strrchr(argv[0], '/'))
	argv[0] = strrchr(argv[0], '/')+1;

    if (!(kdc_realmlist = (kdc_realm_t **) malloc(sizeof(kdc_realm_t *) * 
						  KRB5_KDC_MAX_REALMS))) {
	fprintf(stderr, "%s: cannot get memory for realm list\n", argv[0]);
	exit(1);
    }
    memset((char *) kdc_realmlist, 0,
	   (size_t) (sizeof(kdc_realm_t *) * KRB5_KDC_MAX_REALMS));

    /*
     * A note about Kerberos contexts: This context, "kcontext", is used
     * for the KDC operations, i.e. setup, network connection and error
     * reporting.  The per-realm operations use the "realm_context"
     * associated with each realm.
     */
    retval = krb5int_init_context_kdc(&kcontext);
    if (retval) {
	    com_err(argv[0], retval, "while initializing krb5");
	    exit(1);
    }
    krb5_klog_init(kcontext, "kdc", argv[0], 1);
    /* N.B.: After this point, com_err sends output to the KDC log
       file, and not to stderr.  */

    initialize_kdc5_error_table();

    /*
     * Scan through the argument list
     */
    initialize_realms(kcontext, argc, argv);

    setup_signal_handlers();

    load_preauth_plugins(kcontext);
    load_authdata_plugins(kcontext);

    retval = setup_sam();
    if (retval) {
	com_err(argv[0], retval, "while initializing SAM");
	finish_realms(argv[0]);
	return 1;
    }

    if ((retval = setup_network(argv[0]))) {
	com_err(argv[0], retval, "while initializing network");
	finish_realms(argv[0]);
	return 1;
    }
    if (!nofork && daemon(0, 0)) {
	com_err(argv[0], errno, "while detaching from tty");
	finish_realms(argv[0]);
	return 1;
    }
    krb5_klog_syslog(LOG_INFO, "commencing operation");
    if ((retval = listen_and_process(argv[0]))) {
	com_err(argv[0], retval, "while processing network requests");
	errout++;
    }
    if ((retval = closedown_network(argv[0]))) {
	com_err(argv[0], retval, "while shutting down network");
	errout++;
    }
    krb5_klog_syslog(LOG_INFO, "shutting down");
    unload_preauth_plugins(kcontext);
    unload_authdata_plugins(kcontext);
    krb5_klog_close(kdc_context);
    finish_realms(argv[0]);
    if (kdc_realmlist) 
      free(kdc_realmlist);
#ifdef USE_RCACHE
    (void) krb5_rc_close(kcontext, kdc_rcache);
#endif
#ifndef NOCACHE
    kdc_free_lookaside(kcontext);
#endif
    krb5_free_context(kcontext);
    return errout;
}




