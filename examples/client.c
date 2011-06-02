/* coap-client -- simple CoAP client
 *
 * Copyright (C) 2010,2011 Olaf Bergmann <bergmann@tzi.org>
 *
 * This file is part of the CoAP library libcoap. Please see
 * README for terms of use. 
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "coap.h"

static coap_list_t *optlist = NULL;
/* Request URI.
 * TODO: associate the resources with transaction id and make it expireable */
static coap_uri_t uri;
static str proxy = { 0, NULL };

/* reading is done when this flag is set */
static int ready = 0;
static FILE *file = NULL;	/* output file name */

static str payload = { 0, NULL }; /* optional payload to send */

typedef unsigned char method_t;
method_t method = 1;		/* the method we are using in our requests */

extern unsigned int
print_readable( const unsigned char *data, unsigned int len,
		unsigned char *result, unsigned int buflen );

int
append_to_file(const char *filename, const unsigned char *data, size_t len) {
  size_t written;

  if ( !file && !(file = fopen(filename, "w")) ) {
    perror("append_to_file: fopen");
    return -1;
  }

  do {
    written = fwrite(data, 1, len, file);
    len -= written;
    data += written;
  } while ( written && len );

  return 0;
}

coap_pdu_t *
new_ack( coap_context_t  *ctx, coap_queue_t *node ) {
  coap_pdu_t *pdu = coap_new_pdu();

  if (pdu) {
    pdu->hdr->type = COAP_MESSAGE_ACK;
    pdu->hdr->code = 0;
    pdu->hdr->id = node->pdu->hdr->id;
  }

  return pdu;
}

coap_pdu_t *
new_response( coap_context_t  *ctx, coap_queue_t *node, unsigned int code ) {
  coap_pdu_t *pdu = new_ack(ctx, node);

  if (pdu)
    pdu->hdr->code = code;

  return pdu;
}

coap_pdu_t *
coap_new_request( method_t m, coap_list_t *options ) {
  coap_pdu_t *pdu;
  coap_list_t *opt;
  int res;
#define BUFSIZE 40
  unsigned char _buf[BUFSIZE];
  unsigned char *buf = _buf;

  if ( ! ( pdu = coap_new_pdu() ) )
    return NULL;

  pdu->hdr->type = COAP_MESSAGE_CON;
  pdu->hdr->id = rand();	/* use a random transaction id */
  pdu->hdr->code = m;

  for (opt = options; opt; opt = opt->next) {
    switch (COAP_OPTION_KEY(*(coap_option *)opt->data)) {
      case COAP_OPTION_URI_PATH:
	res = coap_split_path(COAP_OPTION_DATA(*(coap_option *)opt->data),
			      COAP_OPTION_LENGTH(*(coap_option *)opt->data),
			      buf, BUFSIZE);

	while (res--) {
	  printf("add option %d\n", COAP_OPTION_KEY(*(coap_option *)opt->data));
	  
	  coap_add_option(pdu, COAP_OPTION_KEY(*(coap_option *)opt->data),
			  COAP_OPT_LENGTH(*(coap_opt_t *)buf),
			  COAP_OPT_VALUE(*(coap_opt_t *)buf));

	  buf += COAP_OPT_SIZE(*(coap_opt_t *)buf);      
	}
	break;
      case COAP_OPTION_URI_QUERY:
	res = coap_split_query(COAP_OPTION_DATA(*(coap_option *)opt->data),
			       COAP_OPTION_LENGTH(*(coap_option *)opt->data),
			       buf, BUFSIZE);

	while (res--) {
	  printf("add option %d\n", COAP_OPTION_KEY(*(coap_option *)opt->data));
	  
	  coap_add_option(pdu, COAP_OPTION_KEY(*(coap_option *)opt->data),
			  COAP_OPT_LENGTH(*(coap_opt_t *)buf),
			  COAP_OPT_VALUE(*(coap_opt_t *)buf));

	  buf += COAP_OPT_SIZE(*(coap_opt_t *)buf);      
	}
	break;
      default:
	printf("add option %d\n", COAP_OPTION_KEY(*(coap_option *)opt->data));
	coap_add_option(pdu, COAP_OPTION_KEY(*(coap_option *)opt->data),
			COAP_OPTION_LENGTH(*(coap_option *)opt->data),
			COAP_OPTION_DATA(*(coap_option *)opt->data));
      }
  }

  if (payload.length) {
    /* TODO: must handle block */

    coap_add_data(pdu, payload.length, payload.s);
  }

  return pdu;
}

int 
resolve_address(const str *server, struct sockaddr *dst) {
  
  struct addrinfo *res, *ainfo;
  struct addrinfo hints;
  static char addrstr[256];
  int error;

  memset(addrstr, 0, sizeof(addrstr));
  memcpy(addrstr, server->s, server->length);

  memset ((char *)&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_family = AF_UNSPEC;

  error = getaddrinfo(addrstr, "", &hints, &res);

  if (error != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
    return error;
  }

  for (ainfo = res; ainfo != NULL; ainfo = ainfo->ai_next) {

    switch (ainfo->ai_family) {
    case AF_INET6:
    case AF_INET:

      memcpy(dst, ainfo->ai_addr, ainfo->ai_addrlen);
      return ainfo->ai_addrlen;
    default:
      ;
    }
  }

  freeaddrinfo(res);
  return -1;
}

#define COAP_OPT_BLOCK_LAST(opt) ( COAP_OPT_VALUE(*block) + (COAP_OPT_LENGTH(*block) - 1) )
#define COAP_OPT_BLOCK_MORE(opt) ( *COAP_OPT_LAST(*block) & 0x08 )
#define COAP_OPT_BLOCK_SIZE(opt) ( *COAP_OPT_LAST(*block) & 0x07 )

unsigned int
_read_blk_nr(coap_opt_t *opt) {
  unsigned int i, nr=0;
  for ( i = COAP_OPT_LENGTH(*opt); i; --i) {
    nr = (nr << 8) + COAP_OPT_VALUE(*opt)[i-1];
  }
  return nr >> 4;
}
#define COAP_OPT_BLOCK_NR(opt)   _read_blk_nr(&opt)

void
message_handler( coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_pdu_t *pdu = NULL;
  coap_opt_t *block, *ct, *sub;
  unsigned int blocknr;
  unsigned char buf[4];
  coap_list_t *option;
  unsigned int len;
  unsigned char *databuf;

#ifndef NDEBUG
  printf("** process pdu: ");
  coap_show_pdu( node->pdu );
#endif

  if ( node->pdu->hdr->version != COAP_DEFAULT_VERSION ) {
#ifndef NDEBUG
    debug("dropped packet with unknown version %u\n", node->pdu->hdr->version);
#endif
    return;
  }

  if ( node->pdu->hdr->code < COAP_RESPONSE_200 && node->pdu->hdr->type == COAP_MESSAGE_CON ) {
    /* send 500 response */
    pdu = new_response( ctx, node, COAP_RESPONSE_500 );
    goto finish;
  }

  switch (node->pdu->hdr->code) {
  case COAP_RESPONSE_200:
    /* got some data, check if block option is set */
    block = coap_check_option( node->pdu, COAP_OPTION_BLOCK );
    if ( !block ) {
      /* There is no block option set, just read the data and we are done. */
      if ( coap_get_data( node->pdu, &len, &databuf ) ) {
	/*path = coap_check_option( node->pdu, COAP_OPTION_URI_PATH );*/
	append_to_file( "coap.out", databuf, len );
      }
    } else {
      blocknr = coap_decode_var_bytes( COAP_OPT_VALUE(*block), COAP_OPT_LENGTH(*block) );

      /* TODO: check if we are looking at the correct block number */
      if ( coap_get_data( node->pdu, &len, &databuf ) ) {
	/*path = coap_check_option( node->pdu, COAP_OPTION_URI_PATH );*/
	append_to_file( "coap.out", databuf, len );
      }

      if ( (blocknr & 0x08) ) {
	/* more bit is set */
	printf("found the M bit, block size is %u, block nr. %u\n",
	       blocknr & 0x07,
	       (blocknr & 0xf0) << blocknr & 0x07);

	/* need to acknowledge if message was asyncronous */
	if ( node->pdu->hdr->type == COAP_MESSAGE_CON ) {
	  pdu = new_ack( ctx, node );

	  if (pdu && coap_send(ctx, 
			       &node->remote.addr.sa, node->remote.size, pdu) 
	      == COAP_INVALID_TID) {
#ifndef NDEBUG
	    debug("message_handler: error sending reponse");
#endif
	    coap_delete_pdu(pdu);
	    return;
	  }
	}

	/* create pdu with request for next block */
	pdu = coap_new_request( method, NULL ); /* first, create bare PDU w/o any option  */
	if ( pdu ) {
	  pdu->hdr->id = node->pdu->hdr->id; /* copy transaction id from response */

	  /* get content type from response */
	  ct = coap_check_option( node->pdu, COAP_OPTION_CONTENT_TYPE );
	  if ( ct ) {
	    coap_add_option( pdu, COAP_OPTION_CONTENT_TYPE,
			     COAP_OPT_LENGTH(*ct),COAP_OPT_VALUE(*ct) );
	  }

	  /* add URI components from optlist */
	  for (option = optlist; option; option = option->next ) {
	    switch (COAP_OPTION_KEY(*(coap_option *)option->data)) {
	    case COAP_OPTION_URI_HOST :
	    case COAP_OPTION_URI_PATH :
	    case COAP_OPTION_URI_QUERY :
	      coap_add_option ( pdu, COAP_OPTION_KEY(*(coap_option *)option->data),
				COAP_OPTION_LENGTH(*(coap_option *)option->data),
				COAP_OPTION_DATA(*(coap_option *)option->data) );
	      break;
	    default:
	      ;			/* skip other options */
	    }
	  }

	  /* finally add updated block option from response */
	  coap_add_option ( pdu, COAP_OPTION_BLOCK,
			    coap_encode_var_bytes(buf, blocknr + ( 1 << 4) ), buf);

	  if (coap_send_confirmed(ctx, 
				  &node->remote.addr.sa, node->remote.size, pdu) 
	      == COAP_INVALID_TID) {
#ifndef NDEBUG
	    debug("message_handler: error sending reponse");
#endif
	    coap_delete_pdu(pdu);
	  }
	  return;
	}
      }
    }

    break;
  default:
    ;
  }

  /* acknowledge if requested */
  if ( !pdu && node->pdu->hdr->type == COAP_MESSAGE_CON ) {
    pdu = new_ack( ctx, node );
  }

  finish:
  if (pdu && coap_send(ctx, &node->remote.addr.sa, node->remote.size, pdu) 
      == COAP_INVALID_TID) {
#ifndef NDEBUG
    debug("message_handler: error sending reponse");
#endif
    coap_delete_pdu(pdu);
  }

  /* our job is done, we can exit at any time */
  sub = coap_check_option( node->pdu, COAP_OPTION_SUBSCRIPTION );
#ifndef NDEBUG
  if ( sub ) {
    debug("message_handler: Subscription-Lifetime is %d\n",
	  COAP_PSEUDOFP_DECODE_8_4(*COAP_OPT_VALUE(*sub)));
  }
#endif
  ready = !sub || COAP_PSEUDOFP_DECODE_8_4(*COAP_OPT_VALUE(*sub)) == 0;
}

void
usage( const char *program, const char *version) {
  const char *p;

  p = strrchr( program, '/' );
  if ( p )
    program = ++p;

  fprintf( stderr, "%s v%s -- a small CoAP implementation\n"
	   "(c) 2010 Olaf Bergmann <bergmann@tzi.org>\n\n"
	   "usage: %s [-b num] [-g group] [-m method] [-p port] [-s num] [-t type...] [-T string] URI\n\n"
	   "\tURI can be an absolute or relative coap URI,\n"
	   "\t-b size\t\tblock size to be used in GET/PUT/POST requests\n"
	   "\t       \t\t(value must be a multiple of 16 not larger than 2048)\n"
	   "\t-f file\t\tfile to send with PUT/POST (use '-' for STDIN)\n"
	   "\t-g group\tjoin the given multicast group\n"
	   "\t-m method\trequest method (get|put|post|delete)\n"
	   "\t-p port\t\tlisten on specified port\n"
	   "\t-s duration\tsubscribe for given duration [s]\n"
	   "\t-A types\taccepted content for GET (comma-separated list)\n"
	   "\t-t type\t\tcontent type for given resource for PUT/POST\n"
	   "\t-P addr[:port]\tuse proxy (automatically adds Proxy-Uri option to request)\n"
	   "\t-T token\tinclude specified token\n",
	   program, version, program );
}

int
join( coap_context_t *ctx, char *group_name ){
  struct ipv6_mreq mreq;
  struct addrinfo   *reslocal = NULL, *resmulti = NULL, hints, *ainfo;
  int result = -1;

  /* we have to resolve the link-local interface to get the interface id */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  result = getaddrinfo("::", NULL, &hints, &reslocal);
  if ( result < 0 ) {
    fprintf(stderr, "join: cannot resolve link-local interface: %s\n", 
	    gai_strerror(result));
    goto finish;
  }

  /* get the first suitable interface identifier */
  for (ainfo = reslocal; ainfo != NULL; ainfo = ainfo->ai_next) {
    if ( ainfo->ai_family == AF_INET6 ) {
      mreq.ipv6mr_interface =
	      ((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_scope_id;
      break;
    }
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  /* resolve the multicast group address */
  result = getaddrinfo(group_name, NULL, &hints, &resmulti);

  if ( result < 0 ) {
    fprintf(stderr, "join: cannot resolve multicast address: %s\n", 
	    gai_strerror(result));
    goto finish;
  }

  for (ainfo = resmulti; ainfo != NULL; ainfo = ainfo->ai_next) {
    if ( ainfo->ai_family == AF_INET6 ) {
      mreq.ipv6mr_multiaddr =
	((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_addr;
      break;
    }
  }

  result = setsockopt( ctx->sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
		       (char *)&mreq, sizeof(mreq) );
  if ( result < 0 )
    perror("join: setsockopt");

 finish:
  freeaddrinfo(resmulti);
  freeaddrinfo(reslocal);

  return result;
}

int
order_opts(void *a, void *b) {
  if (!a || !b)
    return a < b ? -1 : 1;

  if (COAP_OPTION_KEY(*(coap_option *)a) < COAP_OPTION_KEY(*(coap_option *)b))
    return -1;

  return COAP_OPTION_KEY(*(coap_option *)a) == COAP_OPTION_KEY(*(coap_option *)b);
}


coap_list_t *
new_option_node(unsigned short key, unsigned int length, unsigned char *data) {
  coap_option *option;
  coap_list_t *node;

  option = coap_malloc(sizeof(coap_option) + length);
  if ( !option )
    goto error;

  COAP_OPTION_KEY(*option) = key;
  COAP_OPTION_LENGTH(*option) = length;
  memcpy(COAP_OPTION_DATA(*option), data, length);

  /* we can pass NULL here as delete function since option is released automatically  */
  node = coap_new_listnode(option, NULL);

  if ( node )
    return node;

 error:
  perror("new_option_node: malloc");
  coap_free( option );
  return NULL;
}

void
cmdline_content_type(char *arg, unsigned short key) {
  static char *content_types[] =
    { "plain", "xml", "csv", "html", "","","","","","","","","","","","","","","","","",
      "gif", "jpeg", "png", "tiff", "audio", "video", "","","","","","","","","","","","","",
      "link", "axml", "binary", "rdf", "soap", "atom", "xmpp", "exi",
      "bxml", "infoset", "json", 0};
  coap_list_t *node;
  unsigned char i, value[10];
  int valcnt = 0;
  char *p, *q = arg;

  while (q && *q) {
    p = strchr(q, ',');

    for (i=0; content_types[i] &&
	   strncmp(q,content_types[i], p ? p-q : strlen(q)) != 0 ;
	 ++i)
      ;

    if (content_types[i]) {
      value[valcnt] = i;
      valcnt++;
    } else {
      fprintf(stderr, "W: unknown content-type '%s'\n",arg);
    }

    if (!p || key == COAP_OPTION_CONTENT_TYPE)
      break;

    q = p+1;
  }

  if (valcnt) {
    node = new_option_node(key, valcnt, value);
    if (node)
      coap_insert( &optlist, node, order_opts );
  }
}

void
cmdline_uri(char *arg) {
  unsigned char portbuf[2];

  if (proxy.length) {		/* create Proxy-Uri from argument */

    coap_insert( &optlist, 
		 new_option_node(COAP_OPTION_PROXY_URI,
				 strlen(arg), (unsigned char *)arg),
		 order_opts);

  } else {			/* split arg into Uri-* options */
    coap_split_uri((unsigned char *)arg, strlen(arg), &uri );

    if (uri.port != COAP_DEFAULT_PORT) {
      coap_insert( &optlist, 
		   new_option_node(COAP_OPTION_URI_PORT,
				   coap_encode_var_bytes(portbuf, uri.port),
				 portbuf),
		   order_opts);    
    }

    if (uri.path.length)
      coap_insert( &optlist, new_option_node(COAP_OPTION_URI_PATH,
					     uri.path.length, uri.path.s),
		   order_opts);
    
    if (uri.query.length)
      coap_insert( &optlist, new_option_node(COAP_OPTION_URI_QUERY,
					     uri.query.length, uri.query.s),
		   order_opts);
  }
}

void
cmdline_blocksize(char *arg) {
  static unsigned char buf[4];	/* hack: temporarily take encoded bytes */
  unsigned int blocksize = atoi(arg);

  if ( COAP_MAX_PDU_SIZE < blocksize + sizeof(coap_hdr_t) ) {
    fprintf(stderr, "W: skipped invalid blocksize\n");
    return;
  }

  /* use only last three bits and clear M-bit */
  blocksize = (coap_fls(blocksize >> 4) - 1) & 0x07;
  coap_insert( &optlist, new_option_node(COAP_OPTION_BLOCK,
					 coap_encode_var_bytes(buf, blocksize), buf),
					 order_opts);
}

void
cmdline_subscribe(char *arg) {
  unsigned int ls, s;
  unsigned char duration = COAP_PSEUDOFP_ENCODE_8_4_UP(atoi(arg), ls, s);

  coap_insert( &optlist, new_option_node(COAP_OPTION_SUBSCRIPTION,
					 1, &duration), order_opts );
}

void
cmdline_proxy(char *arg) {
  proxy.length = strlen(arg);
  if ( (proxy.s = coap_malloc(proxy.length + 1)) == NULL) {
    proxy.length = 0;
    return;
  }

  memcpy(proxy.s, arg, proxy.length+1);
}

void
cmdline_token(char *arg) {
  coap_insert( &optlist, new_option_node(COAP_OPTION_TOKEN,
					 strlen(arg),
					 (unsigned char *)arg), order_opts);
}

int
cmdline_input_from_file(char *filename, str *buf) {
  FILE *inputfile = NULL;
  ssize_t len;
  int result = 1;
  struct stat statbuf;

  if (!filename || !buf)
    return 0;

  if (filename[0] == '-' && !filename[1]) { /* read from stdin */
    buf->length = 20000;
    buf->s = (unsigned char *)coap_malloc(buf->length);
    if (!buf->s)
      return 0;

    inputfile = stdin;
  } else {
    /* read from specified input file */
    if (stat(filename, &statbuf) < 0) {
      perror("cmdline_input_from_file: stat");
      return 0;
    }

    buf->length = statbuf.st_size;
    buf->s = (unsigned char *)coap_malloc(buf->length);
    if (!buf->s)
      return 0;

    inputfile = fopen(filename, "r");
    if ( !inputfile ) {
      perror("cmdline_input_from_file: fopen");
      coap_free(buf->s);
      return 0;
    }
  }

  len = fread(buf->s, 1, buf->length, inputfile);

  if (len < buf->length) {
    if (ferror(inputfile) != 0) {
      perror("cmdline_input_from_file: fread");
      coap_free(buf->s);
      buf->length = 0;
      buf->s = NULL;
      result = 0;
    } else {
      buf->length = len;
    }
  }

  if (inputfile != stdin)
    fclose(inputfile);

  return result;
}

method_t
cmdline_method(char *arg) {
  static char *methods[] =
    { 0, "get", "post", "put", "delete", 0};
  unsigned char i;

  for (i=1; methods[i] && strcasecmp(arg,methods[i]) != 0 ; ++i)
    ;

  return i;	     /* note that we do not prevent illegal methods */
}

coap_context_t *
get_context(const char *node, const char *port) {
  coap_context_t *ctx = NULL;  
  int s;
  struct addrinfo hints;
  struct addrinfo *result, *rp;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
  hints.ai_socktype = SOCK_DGRAM; /* Coap uses UDP */
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV | AI_ALL;
  
  s = getaddrinfo(node, port, &hints, &result);
  if ( s != 0 ) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
    return NULL;
  } 

  /* iterate through results until success */
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    ctx = coap_new_context(rp->ai_addr, rp->ai_addrlen);
    if (ctx) {
      /* TODO: output address:port for successful binding */
      goto finish;
    }
  }
  
  fprintf(stderr, "no context available for interface '%s'\n", node);

 finish:
  freeaddrinfo(result);
  return ctx;
}

int
main(int argc, char **argv) {
  coap_context_t  *ctx;
  coap_address_t dst;
  static char addr[INET6_ADDRSTRLEN];
  void *addrptr = NULL;
  fd_set readfds;
  struct timeval tv, *timeout;
  int result;
  coap_tick_t now;
  coap_queue_t *nextpdu;
  coap_pdu_t  *pdu;
  static str server;
  unsigned short port = COAP_DEFAULT_PORT;
  char port_str[NI_MAXSERV] = "0";
  int opt, res;
  char *group = NULL;

  while ((opt = getopt(argc, argv, "b:f:g:m:p:s:t:A:P:T:")) != -1) {
    switch (opt) {
    case 'b' :
      cmdline_blocksize(optarg);
      break;
    case 'f' :
      cmdline_input_from_file(optarg,&payload);
      break;
    case 'g' :
      group = optarg;
      break;
    case 'p' :
      strncpy(port_str, optarg, NI_MAXSERV-1);
      port_str[NI_MAXSERV - 1] = '\0';
      break;
    case 'm' :
      method = cmdline_method(optarg);
      break;
    case 's' :
      cmdline_subscribe(optarg);
      break;
    case 'A' :
      cmdline_content_type(optarg,COAP_OPTION_ACCEPT);
      break;
    case 't' :
      cmdline_content_type(optarg,COAP_OPTION_CONTENT_TYPE);
      break;
    case 'P' :
      cmdline_proxy(optarg);
      break;
    case 'T' :
      cmdline_token(optarg);
      break;
    default:
      usage( argv[0], PACKAGE_VERSION );
      exit( 1 );
    }
  }

  ctx = get_context("::", port_str);
  if ( !ctx )
    return -1;

  coap_register_message_handler( ctx, message_handler );

  if ( optind < argc )
    cmdline_uri( argv[optind] );
  else {
    usage( argv[0], PACKAGE_VERSION );
    exit( 1 );
  }

  if ( group )
    join( ctx, group );

  server = proxy.length ? proxy : uri.host;

  /* resolve destination address where server should be sent */
  res = resolve_address(&server, &dst.addr.sa);

  if (res < 0) {
    fprintf(stderr, "failed to resolve address\n");
    exit(-1);
  }

  dst.size = res;
  dst.addr.sin.sin_port = htons(port);

  /* add Uri-Host if server address differs from uri.host */
  
  switch (dst.addr.sa.sa_family) {
  case AF_INET: 
    addrptr = &dst.addr.sin.sin_addr;
    break;
  case AF_INET6:
    addrptr = &dst.addr.sin6.sin6_addr;
    break;
  default:
    ;
  }

  if (addrptr
      && (inet_ntop(dst.addr.sa.sa_family, addrptr, addr, sizeof(addr)) != 0)
      && (strlen(addr) != uri.host.length 
	  || memcmp(addr, uri.host.s, uri.host.length) != 0)) {
      /* add Uri-Host */

    coap_insert(&optlist, new_option_node(COAP_OPTION_URI_HOST,
					  uri.host.length, uri.host.s),
		order_opts);
  }

  if (! (pdu = coap_new_request( method, optlist ) ) )
    return -1;

#ifndef NDEBUG
  {
    unsigned char buf[COAP_MAX_PDU_SIZE];
    print_readable( (unsigned char *)pdu->hdr, pdu->length, buf, COAP_MAX_PDU_SIZE);
    printf("%s\n",buf);
  }
#endif

  coap_send_confirmed( ctx, &dst.addr.sa, dst.size, pdu );

  while ( !(ready && coap_can_exit(ctx)) ) {
    FD_ZERO(&readfds);
    FD_SET( ctx->sockfd, &readfds );

    nextpdu = coap_peek_next( ctx );

    coap_ticks(&now);
    while ( nextpdu && nextpdu->t <= now ) {
      coap_retransmit( ctx, coap_pop_next( ctx ) );
      nextpdu = coap_peek_next( ctx );
    }

    if ( nextpdu ) {	        /* set timeout if there is a pdu to send */
      tv.tv_usec = ((nextpdu->t - now) % COAP_TICKS_PER_SECOND) << 10;
      tv.tv_sec = (nextpdu->t - now) / COAP_TICKS_PER_SECOND;
      timeout = &tv;
    } else
      timeout = NULL;		/* no timeout otherwise */

    result = select( ctx->sockfd + 1, &readfds, 0, 0, timeout );

    if ( result < 0 ) {		/* error */
      perror("select");
    } else if ( result > 0 ) {	/* read from socket */
      if ( FD_ISSET( ctx->sockfd, &readfds ) ) {
	coap_read( ctx );	/* read received data */
	coap_dispatch( ctx );	/* and dispatch PDUs from receivequeue */
      }
    }
  }

  if ( file ) {
    fflush( file );
    fclose( file );
  }
  coap_free_context( ctx );

  return 0;
}