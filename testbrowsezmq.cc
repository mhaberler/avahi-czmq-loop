//   Michael Haberler, 2014
//   rehashed from client-browse-services.c from the avahi distribution

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <net/if.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>

#include <avahi-common/timeval.h>
#include <avahi-common/watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#define MACHINEKIT_DNS_SERVICE_TYPE "_machinekit._tcp"

#include "czmq.h"
#include "czmq-watch.h"

static AvahiCzmqPoll *czmq_poll = NULL;

static void resolve_callback(AvahiServiceResolver *r,
			     AVAHI_GCC_UNUSED AvahiIfIndex interface,
			     AVAHI_GCC_UNUSED AvahiProtocol protocol,
			     AvahiResolverEvent event,
			     const char *name,
			     const char *type,
			     const char *domain,
			     const char *host_name,
			     const AvahiAddress *address,
			     uint16_t port,
			     AvahiStringList *txt,
			     AvahiLookupResultFlags flags,
			     AVAHI_GCC_UNUSED void* userdata)
{
    assert(r);

    // Called whenever a service has been resolved successfully or timed out
    switch (event) {

    case AVAHI_RESOLVER_FAILURE:
	fprintf(stderr,"%s: (Resolver) Failed to resolve service"
		" '%s' of type '%s' in domain '%s': %s\n",
		__func__, name, type, domain,
		avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(r))));
	break;

    case AVAHI_RESOLVER_FOUND: {

	char a[AVAHI_ADDRESS_STR_MAX], *t;

	avahi_address_snprint(a, sizeof(a), address);
	t = avahi_string_list_to_string(txt);
	fprintf(stderr,"%s: Service '%s' of type '%s' in domain '%s' %s:%u %s TXT=%s\n",
		__func__, name, type, domain,  host_name, port, a,  t);

	// !!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
	// !!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
	// !!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
	// !!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
	// !!(flags & AVAHI_LOOKUP_RESULT_CACHED));
	avahi_free(t);
    }
    }

    avahi_service_resolver_free(r);
}

static void browse_callback(AvahiServiceBrowser *b,
			    AvahiIfIndex interface,
			    AvahiProtocol protocol,
			    AvahiBrowserEvent event,
			    const char *name,
			    const char *type,
			    const char *domain,
			    AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
			    void* userdata)
{
    AvahiClient *c = (AvahiClient *)userdata;
    assert(b);
    char ifname[IF_NAMESIZE];

    // Called whenever a new services becomes available on the LAN or is removed from the LAN
    switch (event) {
    case AVAHI_BROWSER_FAILURE:
	fprintf(stderr,"%s: (Browser) %s\n",
		__func__,
		avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));


	avahi_czmq_poll_quit(czmq_poll);
	return;

    case AVAHI_BROWSER_NEW:
	fprintf(stderr,"%s: (Browser) NEW: service '%s' of type '%s' in domain '%s' ifindex=%d protocol=%d\n",
		__func__,name, type, domain, interface, protocol);

	// We ignore the returned resolver object. In the callback
	// function we free it. If the server is terminated before
	// the callback function is called the server will free
	// the resolver for us.
	if (!(avahi_service_resolver_new(c,
					 interface,
					 protocol,
					 name,
					 type,
					 domain,
					 AVAHI_PROTO_UNSPEC,
					 (AvahiLookupFlags)0,
					 resolve_callback, c)))
	    fprintf(stderr,"%s: Failed to resolve service '%s': %s\n",
		    __func__, name, avahi_strerror(avahi_client_errno(c)));
	break;

    case AVAHI_BROWSER_REMOVE:
	memset(ifname, 0, sizeof(ifname));
	if_indextoname(interface, ifname);
	fprintf(stderr,"%s: (Browser) REMOVE: service '%s' of type '%s' in domain '%s' if=%s\n",
		__func__, name, type, domain, ifname);
	break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
	fprintf(stderr,"%s: (Browser) %s\n",
		__func__, event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
	break;
    }
}

static void client_callback(AvahiClient *c, AvahiClientState state,
			    AVAHI_GCC_UNUSED void * userdata)
{
    assert(c);

    // Called whenever the client or server state changes
    if (state == AVAHI_CLIENT_FAILURE) {
	fprintf(stderr,"%s: Server connection failure: %s\n",
		__func__, avahi_strerror(avahi_client_errno(c)));
	avahi_czmq_poll_quit(czmq_poll);
    }
}

int main(AVAHI_GCC_UNUSED int argc, AVAHI_GCC_UNUSED char*argv[])
{
    AvahiClient *client = NULL;
    AvahiServiceBrowser *sb = NULL;
    zctx_t *context = zctx_new ();
    assert(context);
    zloop_t *loop= zloop_new();
    assert(loop);
    zloop_set_verbose (loop, argc > 1);

    int error;
    int ret = 1;

    /* Allocate main loop object */
    if (!(czmq_poll = avahi_czmq_poll_new(loop))) {
        fprintf(stderr, "Failed to create za poll object.\n");
        goto fail;
    }

    /* Allocate a new client */
    client = avahi_client_new(avahi_czmq_poll_get(czmq_poll),
			      (AvahiClientFlags)0,
			      client_callback,
			      loop,
			      &error);

    /* Check wether creating the client object succeeded */
    if (!client) {
        fprintf(stderr, "Failed to create client: %s\n", avahi_strerror(error));
        goto fail;
    }

    /* Create the service browser */
    if (!(sb = avahi_service_browser_new(client,
					 AVAHI_IF_UNSPEC,
					 AVAHI_PROTO_UNSPEC,
					 MACHINEKIT_DNS_SERVICE_TYPE,
					 NULL,
					 (AvahiLookupFlags)0,
					 browse_callback,
					 client))) {
        fprintf(stderr, "Failed to create service browser: %s\n",
		avahi_strerror(avahi_client_errno(client)));
        goto fail;
    }


    do {
	ret = zloop_start(loop);
    } while  (!(ret || zctx_interrupted));

 fail:

    /* Cleanup things */
    if (sb)
        avahi_service_browser_free(sb);

    if (client)
        avahi_client_free(client);

    if (czmq_poll)
        avahi_czmq_poll_free(czmq_poll);

    return ret;
}
