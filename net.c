/* net.c -- CoAP network interface
 *
 * Copyright (C) 2010--2012 Olaf Bergmann <bergmann@tzi.org>
 *
 * This file is part of the CoAP library libcoap. Please see
 * README for terms of use. 
 */

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#elif HAVE_SYS_UNISTD_H
#include <sys/unistd.h>
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "debug.h"
#include "mem.h"
#include "str.h"
#include "async.h"
#include "resource.h"
#include "option.h"
#include "encode.h"
#include "net.h"
#include "asynchronous.h"
#include "subscribe.h"
#include "utlist.h"

#include <android/sensor.h>

#include "globals_test.h"


#ifndef WITH_CONTIKI

time_t clock_offset;

static inline coap_queue_t *
coap_malloc_node() {
  return (coap_queue_t *)coap_malloc(sizeof(coap_queue_t));
}

static inline void
coap_free_node(coap_queue_t *node) {
  coap_free(node);
}
#else /* WITH_CONTIKI */
# ifndef DEBUG
#  define DEBUG DEBUG_PRINT
# endif /* DEBUG */

#include "memb.h"
#include "net/uip-debug.h"

clock_time_t clock_offset;

#define UIP_IP_BUF   ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])
#define UIP_UDP_BUF  ((struct uip_udp_hdr *)&uip_buf[UIP_LLIPH_LEN])

void coap_resources_init();
void coap_pdu_resources_init();

unsigned char initialized = 0;
coap_context_t the_coap_context;

MEMB(node_storage, coap_queue_t, COAP_PDU_MAXCNT);

PROCESS(coap_retransmit_process, "message retransmit process");

static inline coap_queue_t *
coap_malloc_node() {
  return (coap_queue_t *)memb_alloc(&node_storage);
}

static inline void
coap_free_node(coap_queue_t *node) {
  memb_free(&node_storage, node);
}
#endif /* WITH_CONTIKI */

int print_wellknown(coap_context_t *, unsigned char *, size_t *, coap_opt_t *);

void
coap_handle_failed_notify(coap_context_t *context,  coap_registration_t *reg,
			  const coap_address_t *peer, const str *token);

coap_alive_mid_t *
coap_mid_alive_init();
void
coap_clean_expired_mids(coap_alive_mid_t *list);
coap_alive_mid_t *
mid_is_alive(coap_context_t *context, coap_queue_t *rcvd);

int
coap_insert_node(coap_queue_t **queue, coap_queue_t *node,
		 int (*order)(coap_queue_t *, coap_queue_t *node) ) {
  coap_queue_t *p, *q;
  if ( !queue || !node )
    return 0;

  /* set queue head if empty */
  if ( !*queue ) {
    *queue = node;
    return 1;
  }

  /* replace queue head if PDU's time is less than head's time */
  q = *queue;
  if ( order( node, q ) < 0) {
    node->next = q;
    *queue = node;
    return 1;
  }

  /* search for right place to insert */
  do {
    p = q;
    q = q->next;
  } while ( q && order( node, q ) >= 0 );

  /* insert new item */
  node->next = q;
  p->next = node;
  return 1;
}

int
coap_delete_node(coap_queue_t *node) {
  if ( !node )
    return 0;

  coap_delete_pdu(node->pdu);
  coap_free_node(node);

  return 1;
}

void
coap_delete_all(coap_queue_t *queue) {
  if ( !queue )
    return;

  coap_delete_all( queue->next );
  coap_delete_node( queue );
}

coap_queue_t *
coap_new_node() {
  coap_queue_t *node;
  node = coap_malloc_node();

  if ( ! node ) {
#ifndef NDEBUG
    coap_log(LOG_WARN, "coap_new_node: malloc");
#endif
    return NULL;
  }

  memset(node, 0, sizeof *node );

  /* Adding this to be sure */
  node->reg = NULL;

  return node;
}

coap_queue_t *
coap_peek_next( coap_context_t *context ) {
  if ( !context || !context->sendqueue )
    return NULL;

  return context->sendqueue;
}

coap_queue_t *
coap_pop_next( coap_context_t *context ) {
  coap_queue_t *next;

  if ( !context || !context->sendqueue )
    return NULL;

  next = context->sendqueue;
  context->sendqueue = context->sendqueue->next;
  next->next = NULL;
  return next;
}

#ifdef COAP_DEFAULT_WKC_HASHKEY
/** Checks if @p Key is equal to the pre-defined hash key for.well-known/core. */
#define is_wkc(Key)							\
  (memcmp((Key), COAP_DEFAULT_WKC_HASHKEY, sizeof(coap_key_t)) == 0)
#else
/* Implements a singleton to store a hash key for the .wellknown/core
 * resources. */
int
is_wkc(coap_key_t k) {
  static coap_key_t wkc;
  static unsigned char _initialized = 0;
  if (!_initialized) {
    _initialized = coap_hash_path((unsigned char *)COAP_DEFAULT_URI_WELLKNOWN, 
				 sizeof(COAP_DEFAULT_URI_WELLKNOWN) - 1, wkc);
  }
  return memcmp(k, wkc, sizeof(coap_key_t)) == 0;
}
#endif

coap_context_t *
coap_new_context(const coap_address_t *listen_addr) {
#ifndef WITH_CONTIKI
  coap_context_t *c = coap_malloc( sizeof( coap_context_t ) );
  int reuse = 1;
#else /* WITH_CONTIKI */
  coap_context_t *c;

  if (initialized)
    return NULL;
#endif /* WITH_CONTIKI */

  if (!listen_addr) {
    coap_log(LOG_EMERG, "no listen address specified\n");
    return NULL;
  }

  coap_clock_init();
  prng_init((unsigned long)listen_addr ^ clock_offset);

#ifndef WITH_CONTIKI
  if ( !c ) {
#ifndef NDEBUG
    coap_log(LOG_EMERG, "coap_init: malloc:");
#endif
    return NULL;
  }
#else /* WITH_CONTIKI */
  coap_resources_init();
  coap_pdu_resources_init();

  c = &the_coap_context;
  initialized = 1;
#endif /* WITH_CONTIKI */

  memset(c, 0, sizeof( coap_context_t ) );

  /* initialize message id */
  prng((unsigned char *)&c->message_id, sizeof(unsigned short));

  /* register the critical options that we know */
  coap_register_option(c, COAP_OPTION_CONTENT_TYPE);
  coap_register_option(c, COAP_OPTION_PROXY_URI);
  coap_register_option(c, COAP_OPTION_URI_HOST);
  coap_register_option(c, COAP_OPTION_URI_PORT);
  coap_register_option(c, COAP_OPTION_URI_PATH);
  coap_register_option(c, COAP_OPTION_TOKEN);
  coap_register_option(c, COAP_OPTION_URI_QUERY);

  /* Added.. make sure that pointers to the buffers are NULL
   * at startup.
   */
  c->notbuf = NULL;
  c->smreqbuf = NULL;

  /* Make sure the alive mids list is NULL at startup. */
  c->alive_mids = NULL;

#ifndef WITH_CONTIKI
  c->sockfd = socket(listen_addr->addr.sa.sa_family, SOCK_DGRAM, 0);
  if ( c->sockfd < 0 ) {
#ifndef NDEBUG
    coap_log(LOG_EMERG, "coap_new_context: socket");
#endif
    goto onerror;
  }

  if ( setsockopt( c->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse) ) < 0 ) {
#ifndef NDEBUG
    coap_log(LOG_WARN, "setsockopt SO_REUSEADDR");
#endif
  }

  if (bind(c->sockfd, &listen_addr->addr.sa, listen_addr->size) < 0) {
#ifndef NDEBUG
    coap_log(LOG_EMERG, "coap_new_context: bind");
#endif
    goto onerror;
  }

  return c;

 onerror:
  if ( c->sockfd >= 0 )
    close ( c->sockfd );
  coap_free( c );
  return NULL;

#else /* WITH_CONTIKI */
  c->conn = udp_new(NULL, 0, NULL);
  udp_bind(c->conn, listen_addr->port);
  
  process_start(&coap_retransmit_process, (char *)c);

  PROCESS_CONTEXT_BEGIN(&coap_retransmit_process);
#ifndef WITHOUT_OBSERVE
  etimer_set(&c->notify_timer, COAP_RESOURCE_CHECK_TIME * COAP_TICKS_PER_SECOND);
#endif /* WITHOUT_OBSERVE */
  /* the retransmit timer must be initialized to some large value */
  etimer_set(&the_coap_context.retransmit_timer, 0xFFFF);
  PROCESS_CONTEXT_END(&coap_retransmit_process);
  return c;
#endif /* WITH_CONTIKI */
}

void
coap_free_context( coap_context_t *context ) {
#ifndef WITH_CONTIKI
  coap_resource_t *res, *rtmp;
#endif /* WITH_CONTIKI */
  if ( !context )
    return;

  coap_delete_all(context->recvqueue);
  coap_delete_all(context->sendqueue);

#ifndef WITH_CONTIKI
  HASH_ITER(hh, context->resources, res, rtmp) {
    coap_delete_resource(context, res->key);
  }

  /* coap_delete_list(context->subscriptions); */
  close( context->sockfd );
  coap_free( context );
#else /* WITH_CONTIKI */
  memset(&the_coap_context, 0, sizeof(coap_context_t));
  initialized = 0;
#endif /* WITH_CONTIKI */
}

int
coap_option_check_critical(coap_context_t *ctx, 
			   coap_pdu_t *pdu,
			   coap_opt_filter_t unknown) {

  coap_opt_iterator_t opt_iter;
  int ok = 1;
  
  coap_option_iterator_init(pdu, &opt_iter, COAP_OPT_ALL);

  while (coap_option_next(&opt_iter)) {

    /* The following condition makes use of the fact that
     * coap_option_getb() returns -1 if type exceeds the bit-vector
     * filter. As the vector is supposed to be large enough to hold
     * the largest known option, we know that everything beyond is
     * bad.
     */
    if (opt_iter.type & 0x01 && 
	coap_option_getb(ctx->known_options, opt_iter.type) < 1) {
      debug("unknown critical option %d\n", opt_iter.type);
      
      ok = 0;

      /* When opt_iter.type is beyond our known option range,
       * coap_option_setb() will return -1 and we are safe to leave
       * this loop. */
      if (coap_option_setb(unknown, opt_iter.type) == -1)
	break;
    }
  }

  return ok;
}

void
coap_transaction_id(const coap_address_t *peer, const coap_pdu_t *pdu, 
		    coap_tid_t *id) {
  coap_key_t h;

  memset(h, 0, sizeof(coap_key_t));

  /* Compare the complete address structure in case of IPv4. For IPv6,
   * we need to look at the transport address only. */

#ifndef WITH_CONTIKI
  switch (peer->addr.sa.sa_family) {
  case AF_INET:
    coap_hash((const unsigned char *)&peer->addr.sa, peer->size, h);
    break;
  case AF_INET6:
    coap_hash((const unsigned char *)&peer->addr.sin6.sin6_port,
	      sizeof(peer->addr.sin6.sin6_port), h);
    coap_hash((const unsigned char *)&peer->addr.sin6.sin6_addr,
	      sizeof(peer->addr.sin6.sin6_addr), h);
    break;
  default:
    return;
  }
#else /* WITH_CONTIKI */
    coap_hash((const unsigned char *)&peer->port, sizeof(peer->port), h);
    coap_hash((const unsigned char *)&peer->addr, sizeof(peer->addr), h);  
#endif /* WITH_CONTIKI */

  coap_hash((const unsigned char *)&pdu->hdr->id, sizeof(unsigned short), h);

  *id = ((h[0] << 8) | h[1]) ^ ((h[2] << 8) | h[3]);
}

coap_tid_t
coap_send_ack(coap_context_t *context, 
	      const coap_address_t *dst,
	      coap_pdu_t *request) {
  coap_pdu_t *response;
  coap_tid_t result = COAP_INVALID_TID;
  
  if (request && request->hdr->type == COAP_MESSAGE_CON) {
    response = coap_pdu_init(COAP_MESSAGE_ACK, 0, request->hdr->id, 
			     sizeof(coap_pdu_t)); 
    if (response) {
      result = coap_send(context, dst, response);
      coap_delete_pdu(response);
    }
  }
  return result;
}

#ifndef WITH_CONTIKI
/* releases space allocated by PDU if free_pdu is set */
coap_tid_t
coap_send_impl(coap_context_t *context, 
	       const coap_address_t *dst,
	       coap_pdu_t *pdu) {

/*
	if (pdu!=NULL) {
		unsigned char ostr[50];
		coap_opt_iterator_t opt_iter;
		coap_opt_t *odb = coap_check_option(pdu,
				COAP_OPTION_TOKEN, &opt_iter);
		if (odb != NULL) {
			unsigned char *val = coap_opt_value(odb);
			if (val == NULL)
				LOGI("option val null");
			unsigned short len = coap_opt_length(odb);
			LOGI("Len%d", len);
			memcpy(ostr, val, len);
			ostr[len] = NULL;
			int yy = 0;
			while (ostr[yy]!=NULL) {
				LOGI("%c", ostr[yy]);
				yy++;
			}
		}
	}
*/

  ssize_t bytes_written;
  coap_tid_t id = COAP_INVALID_TID;

  if ( !context || !dst || !pdu )
    return id;

  bytes_written = sendto( context->sockfd, pdu->hdr, pdu->length, 0,
			  &dst->addr.sa, dst->size);

  if (bytes_written >= 0) {
    coap_transaction_id(dst, pdu, &id);


    LOGI("--- Sent packet -----------");
    printpdu(pdu);
    LOGI("---------------------------");


	UDP_OUT_counter++;
	UDP_OUT_octects += bytes_written;

	if (pdu->hdr->type == COAP_MESSAGE_NON) {
		OUT_NON_counter++;
		OUT_NON_octects += bytes_written;
	}
	else if (pdu->hdr->type == COAP_MESSAGE_CON) {
		OUT_CON_counter++;
		OUT_CON_octects += bytes_written;
	}
	else if (pdu->hdr->type == COAP_MESSAGE_ACK) {
		OUT_ACK_counter++;
		OUT_ACK_octects += bytes_written;
	}
	else if (pdu->hdr->type == COAP_MESSAGE_RST) {
		OUT_RST_counter++;
		OUT_RST_octects += bytes_written;
	}


  } else {
    coap_log(LOG_CRIT, "coap_send: sendto");
  }

  return id;
}
#else  /* WITH_CONTIKI */
/* releases space allocated by PDU if free_pdu is set */
coap_tid_t
coap_send_impl(coap_context_t *context, 
	       const coap_address_t *dst,
	       coap_pdu_t *pdu) {
  coap_tid_t id = COAP_INVALID_TID;

  if ( !context || !dst || !pdu )
    return id;

  /* FIXME: is there a way to check if send was successful? */
  uip_udp_packet_sendto(context->conn, pdu->hdr, pdu->length,
			&dst->addr, dst->port);

  coap_transaction_id(dst, pdu, &id);

  return id;
}
#endif /* WITH_CONTIKI */

coap_tid_t 
coap_send(coap_context_t *context, 
	  const coap_address_t *dst, 
	  coap_pdu_t *pdu) {
  return coap_send_impl(context, dst, pdu);
}

coap_tid_t
coap_send_error(coap_context_t *context, 
		coap_pdu_t *request,
		const coap_address_t *dst,
		unsigned char code,
		coap_opt_filter_t opts) {
  coap_pdu_t *response;
  coap_tid_t result = COAP_INVALID_TID;

  assert(request);
  assert(dst);

  response = coap_new_error_response(request, code, opts);
  if (response) {
    result = coap_send(context, dst, response);
    coap_delete_pdu(response);
  }
  
  return result;
}

coap_tid_t
coap_send_message_type(coap_context_t *context, 
		       const coap_address_t *dst, 
		       coap_pdu_t *request,
		       unsigned char type) {
  coap_pdu_t *response;
  coap_tid_t result = COAP_INVALID_TID;

  if (request) {
    response = coap_pdu_init(type, 0, request->hdr->id, sizeof(coap_pdu_t)); 
    if (response) {
      result = coap_send(context, dst, response);
      coap_delete_pdu(response);
    }
  }
  return result;
}

int
_order_timestamp( coap_queue_t *lhs, coap_queue_t *rhs ) {
  return lhs && rhs && ( lhs->t < rhs->t ) ? -1 : 1;
}

coap_tid_t
coap_send_confirmed(coap_context_t *context, 
		    const coap_address_t *dst,
		    coap_pdu_t *pdu) {
  coap_queue_t *node;
  coap_tick_t now;
  int r;

  node = coap_new_node();
  if (!node) {
    debug("coap_send_confirmed: insufficient memory\n");
    return COAP_INVALID_TID;
  }

  /* assigns a new transaction ID and sends, returns the transaction id */
  node->id = coap_send_impl(context, dst, pdu);
  if (COAP_INVALID_TID == node->id) {
    debug("coap_send_confirmed: error sending pdu\n");
    coap_free_node(node);
    return COAP_INVALID_TID;
  }

  LOGI("Sent CON mess id%d, new outstanding transaction id%d", pdu->hdr->id, node->id);
  
  prng((unsigned char *)&r,sizeof(r));
  coap_ticks(&now);
  node->t = now;

  /* add randomized RESPONSE_TIMEOUT to determine retransmission timeout */
  /*node->timeout = COAP_DEFAULT_RESPONSE_TIMEOUT * COAP_TICKS_PER_SECOND +
    (COAP_DEFAULT_RESPONSE_TIMEOUT >> 1) *
    ((COAP_TICKS_PER_SECOND * (r & 0xFF)) >> 8);*/
  /*
  int a = 0.150 * 1024;
  float y = rand()/(float)RAND_MAX;
  int b = a + (a/3)*y;
   */
  int a = 0.150 * 1024;
  float y = rand()/(float)RAND_MAX;
  node->timeout = a + (a/3)*y;
  LOGW("Timeout assigned to:%d coapclk:%d", node->timeout, now);
  node->t += node->timeout;

  memcpy(&node->remote, dst, sizeof(coap_address_t));
  node->pdu = pdu;

  /* Added to support observe registrations.
   * It is nulled also inside coap_new_node()
   * but let's be safe! */
  node->reg = NULL;

  assert(&context->sendqueue);
  coap_insert_node(&context->sendqueue, node, _order_timestamp);

#ifdef WITH_CONTIKI
  {			    /* (re-)initialize retransmission timer */
    coap_queue_t *nextpdu;

    nextpdu = coap_peek_next(context);
    assert(nextpdu);		/* we have just inserted a node */

    /* must set timer within the context of the retransmit process */
    PROCESS_CONTEXT_BEGIN(&coap_retransmit_process);
    etimer_set(&context->retransmit_timer, 
	       now < nextpdu->t ? nextpdu->t - now : 0);
    PROCESS_CONTEXT_END(&coap_retransmit_process);
  }
#endif /* WITH_CONTIKI */

  /* returns the transaction id */
  return node->id;
}

/* Implementation mimics coap_send_confirmed()
 * except for the assignment of coap_queue_t's reg field. */
coap_tid_t
coap_notify_confirmed(coap_context_t *context,
	    const coap_address_t *dst,
	    coap_pdu_t *pdu,
	    coap_registration_t *reg) {
		/* TODO: add deadline
		 * it can be set perfectly by imposing
		 * a random factor of 1.00 (the standard allows this)
		 * therefore the bound cases of ACK_TIMEOUT*1
		 * and ACK_TIMEOUT*ACK_RANDOM factor collide.
		 */

  coap_queue_t *node;
  coap_tick_t now;
  int r;

  node=coap_new_node();
  if (!node) {
    debug("coap_notify: insufficient memory\n");
    return COAP_INVALID_TID;
  }

  /* assigns a new transaction ID and sends, returns the transaction id */
  node->id = coap_send_impl(context, dst, pdu);
  if (COAP_INVALID_TID == node->id) {
	LOGI("Invalid TID, error sending PDU");
    debug("coap_notify: error sending pdu\n");
    coap_free_node(node);
    return COAP_INVALID_TID;
  }

  prng((unsigned char *)&r,sizeof(r));
  coap_ticks(&now);
  node->t = now;

  /* add randomized RESPONSE_TIMEOUT to determine retransmission timeout */
  /*node->timeout = COAP_DEFAULT_RESPONSE_TIMEOUT * COAP_TICKS_PER_SECOND +
    (COAP_DEFAULT_RESPONSE_TIMEOUT >> 1) *
    ((COAP_TICKS_PER_SECOND * (r & 0xFF)) >> 8);*/
  /*
  int a = 0.150 * 1024;
  float y = rand()/(float)RAND_MAX;
  int b = a + (a/3)*y;
   */
  int a = 0.150 * 1024;
  float y = rand()/(float)RAND_MAX;
  node->timeout = a + (a/3)*y;
  LOGW("Timeout assigned to:%d coapclk:%d", node->timeout, now);
  node->t += node->timeout;

  memcpy(&node->remote, dst, sizeof(coap_address_t));
  node->pdu = pdu;

  /* As a conceptual separation, we let the checkout be done when passing
   * reg as a parameter by the caller:
   * coap_notify_confirmed( , , , coap_registration_checkout(reg) ) */
  node->reg = reg;

  /* Insert the node into the sendqueue. */
  assert(&context->sendqueue);
  coap_insert_node(&context->sendqueue, node, _order_timestamp);

  LOGI("Sent CON notif, mess id:%d, new outstanding transaction id:%d", pdu->hdr->id, node->id);

  /* returns the transaction id */
  return node->id;
}


coap_tid_t
coap_retransmit( coap_context_t *context, coap_queue_t *node ) {
  if ( !context || !node )
    return COAP_INVALID_TID;

  /* re-initialize timeout when maximum number of retransmissions are not reached yet */
  if ( node->retransmit_cnt < COAP_DEFAULT_MAX_RETRANSMIT ) {
    node->retransmit_cnt++;
    node->t += ( node->timeout << node->retransmit_cnt );
    coap_insert_node( &context->sendqueue, node, _order_timestamp );

    RETR_counter++;

    //They'll all have our payload header..
	ze_payload_header_t *pay = (ze_payload_header_t *)node->pdu->data;
	if (pay->sensor_type == ASENSOR_TYPE_ACCELEROMETER)
		ACCEL_RETR_counter++;
	else if (pay->sensor_type == ASENSOR_TYPE_LIGHT)
		LIGHT_RETR_counter++;
	else if (pay->sensor_type == ASENSOR_TYPE_GYROSCOPE)
		GYRO_RETR_counter++;
	else if (pay->sensor_type == ASENSOR_TYPE_PROXIMITY)
		PROX_RETR_counter++;

	/* only for testing purposes in order to distinguish at the
	 * client side which are first-time or retransmitted packets. */
	if (pay->packet_type == DATAPOINT)
		pay->packet_type = DATAPOINT_RETRANSMITTED;

#ifndef WITH_CONTIKI
    debug("** retransmission #%d of transaction %d\n",
	  node->retransmit_cnt, ntohs(node->pdu->hdr->id));
    LOGI("** retransmission #%d of transaction %d nodeid%d\n",
	  node->retransmit_cnt, /*ntohs(*/node->pdu->hdr->id/*)*/, node->id);
#else /* WITH_CONTIKI */
    debug("** retransmission #%u of transaction %u\n",
	  node->retransmit_cnt, uip_ntohs(node->pdu->hdr->id));
#endif /* WITH_CONTIKI */

    node->id = coap_send_impl(context, &node->remote, node->pdu);
    return node->id;
  }

  /* no more retransmissions, remove node from system */

  debug("** transaction %d unsuccessful, removed\n", node->id);

#ifndef WITHOUT_OBSERVE
  /* Check if subscriptions exist that should be canceled after
     COAP_MAX_NOTIFY_FAILURES */
  if (node->pdu->hdr->code >= 64) {
    coap_opt_iterator_t opt_iter;
    str token = { 0, NULL };

    if (coap_check_option(node->pdu, COAP_OPTION_TOKEN, &opt_iter)) {
      token.length = COAP_OPT_LENGTH(opt_iter.option);
      token.s = COAP_OPT_VALUE(opt_iter.option);
    }

    if (node->reg != NULL)
    	/* This takes care of the resource-specific unregistration procedure
    	 * (if it's the first confirmable message that tops the fail count)
    	 * as well as releasing the registration.
    	 */
    	coap_handle_failed_notify(context, node->reg, &node->remote, &token);
  }
#endif /* WITHOUT_OBSERVE */

  /* And finally delete the node */
  coap_delete_node( node );

  return COAP_INVALID_TID;
}


int
_order_transaction_id( coap_queue_t *lhs, coap_queue_t *rhs ) {
  return ( lhs && rhs && lhs->pdu && rhs->pdu &&
	   ( lhs->id < rhs->id ) )
    ? -1
    : 1;
}

/** 
 * Checks if @p opt fits into the message that ends with @p maxpos.
 * This function returns @c 1 on success, or @c 0 if the option @p opt
 * would exceed @p maxpos.
 */
static inline int
check_opt_size(coap_opt_t *opt, unsigned char *maxpos) {
  if (opt && opt < maxpos) {
    if (((*opt & 0x0f) < 0x0f) || (opt + 1 < maxpos))
      return opt + COAP_OPT_SIZE(opt) < maxpos;
  }
  return 0;
}

/**
 * Advances *optp to next option if still in PDU.
 */
static int
next_option_safe(coap_opt_t **optp, unsigned char *endptr) {
  size_t length = 0;
  coap_opt_t *opt; /* local copy to advance optp only when everything is ok */

  assert(optp); assert(*optp);

  opt = *optp;

  if (endptr <= opt) {
    debug("opt exceeds endptr\n");
    return 0;
  }

  if ((*opt & 0xf0) == 0xf0) {
    /* skip option jump and end-of-options */
    switch (*opt) {
    case 0xf0:			/* end-of-options, return to caller */
      debug("unexpected end-of-options marker\n");
      return 0;
    case 0xf1:
    case 0xf2:
    case 0xf3:
      if (opt + (*opt & 0x03) < endptr)
	opt += *opt & 0x03;
      else {
	debug("broken option jump\n");
	return 0;
      }
      debug("handled option jump\n");
      break;			/* proceed with option */
    default:
      debug("found unknown special character %02x in option list\n", **optp);
      return 0;
    }
  }

  length = *opt & 0x0f;

  if (length == 15) {		/* extended length spec */

    while (++opt <= endptr && *opt == 0xff && length < 780)
      length += 255;

    if (endptr <= opt)
      return 0;
    
    length += *opt & 0xff;
  } else
    ++opt;			/* skip option type/length byte */
   
  opt += length;

  if (opt <= endptr) {
    *optp = opt;
    return 1;
  } else {
    debug("cannot advance opt (%p), endptr is %p\n", opt, endptr);
    return 0;
  }
}

int
coap_read( coap_context_t *ctx ) {
#ifndef WITH_CONTIKI
  static char buf[COAP_MAX_PDU_SIZE];
  coap_hdr_t *pdu = (coap_hdr_t *)buf;
#else /* WITH_CONTIKI */
  char *buf;
  coap_hdr_t *pdu;
#endif /* WITH_CONTIKI */
  ssize_t bytes_read = -1;
  coap_address_t src, dst;
  coap_queue_t *node;

#ifdef WITH_CONTIKI
  buf = uip_appdata;
  pdu = (coap_hdr_t *)buf;
#endif /* WITH_CONTIKI */

  coap_address_init(&src);

#ifndef WITH_CONTIKI
  bytes_read = recvfrom(ctx->sockfd, buf, sizeof(buf), 0,
			&src.addr.sa, &src.size);
#else /* WITH_CONTIKI */
  if(uip_newdata()) {
    uip_ipaddr_copy(&src.addr, &UIP_IP_BUF->srcipaddr);
    src.port = UIP_UDP_BUF->srcport;
    uip_ipaddr_copy(&dst.addr, &UIP_IP_BUF->destipaddr);
    dst.port = UIP_UDP_BUF->destport;

    bytes_read = uip_datalen();
    ((char *)uip_appdata)[bytes_read] = 0;
    PRINTF("Server received %d bytes from [", (int)bytes_read);
    PRINT6ADDR(&src.addr);
    PRINTF("]:%d\n", uip_ntohs(src.port));
  } 
#endif /* WITH_CONTIKI */

  if ( bytes_read < 0 ) {
    warn("coap_read: recvfrom");
    return -1;
  }

  if ( (size_t)bytes_read < sizeof(coap_hdr_t) ) {
    debug("coap_read: discarded invalid frame\n" );
    return -1;
  }

  if ( pdu->version != COAP_DEFAULT_VERSION ) {
    debug("coap_read: unknown protocol version\n" );
    return -1;
  }

  node = coap_new_node();
  if ( !node )
    return -1;

  node->pdu = coap_pdu_init(0, 0, 0, bytes_read);
  if (!node->pdu)
    goto error;

  coap_ticks( &node->t );
  memcpy(&node->local, &dst, sizeof(coap_address_t));
  memcpy(&node->remote, &src, sizeof(coap_address_t));

  node->pdu->hdr->version = buf[0] >> 6;
  node->pdu->hdr->type = (buf[0] >> 4) & 0x03;
  node->pdu->hdr->optcnt = buf[0] & 0x0f;
  node->pdu->hdr->code = buf[1];

  /* Copy message id in network byte order, so we can easily write the
   * response back to the network. */
  memcpy(&node->pdu->hdr->id, buf + 2, 2);

  /* append data to pdu structure */
  memcpy(node->pdu->hdr + 1, buf + 4, bytes_read - 4);
  node->pdu->length = bytes_read;

  /* Finally calculate beginning of data block and thereby check integrity
   * of the PDU structure. */
  {
    coap_opt_t *opt = options_start(node->pdu);
    unsigned char cnt = node->pdu->hdr->optcnt;

    /* Note that we cannot use the official options iterator here as
     * it relies on correct options and option jump encoding. */
    while (cnt && opt) {
      if ((unsigned char *)node->pdu->hdr + node->pdu->max_size <= opt) {
	/* !(node->pdu->hdr->type & 0x02) */
	if (node->pdu->hdr->type == COAP_MESSAGE_CON || 
	    node->pdu->hdr->type == COAP_MESSAGE_NON) {
	  coap_send_message_type(ctx, &node->remote, node->pdu, 
				 COAP_MESSAGE_RST);
	  debug("sent RST on malformed message\n");
	} else {
	  debug("dropped malformed message\n");
	}
      	goto error;
      }

      if (node->pdu->hdr->optcnt == COAP_OPT_LONG) {
	if (*opt == COAP_OPT_END) {
	  ++opt;
	  break;
	}
      } else {
	--cnt;
      }

      if (!next_option_safe(&opt, (unsigned char *)node->pdu->hdr 
			    + node->pdu->max_size)) {
	debug("drop\n");
	goto error;
      }
    }

    debug("set data to %p (pdu ends at %p)\n", (unsigned char *)opt, (unsigned char *)node->pdu->hdr + node->pdu->max_size);
    node->pdu->data = (unsigned char *)opt;
  }

  /* and add new node to receive queue */
  coap_transaction_id(&node->remote, node->pdu, &node->id);
  coap_insert_node(&ctx->recvqueue, node, _order_timestamp);

#ifndef NDEBUG
  if (LOG_DEBUG <= coap_get_log_level()) {
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 40
#endif
    unsigned char addr[INET6_ADDRSTRLEN+8];

    if (coap_print_addr(&src, addr, INET6_ADDRSTRLEN+8))
      debug("** received %d bytes from %s:\n", (int)bytes_read, addr);

    coap_show_pdu( node->pdu );
  }
#endif

  return 0;
 error:
  coap_delete_node(node);
  return -1;
}

int
coap_remove_from_queue(coap_queue_t **queue, coap_tid_t id, coap_queue_t **node) {
  coap_queue_t *p, *q;

  if ( !queue || !*queue)
    return 0;

  /* replace queue head if PDU's time is less than head's time */

  if ( id == (*queue)->id ) { /* found transaction */
    *node = *queue;
    *queue = (*queue)->next;
    (*node)->next = NULL;
    /* coap_delete_node( q ); */
    debug("*** removed transaction %u\n", id);
    return 1;
  }

  /* search transaction to remove (only first occurrence will be removed) */
  q = *queue;
  do {
    p = q;
    q = q->next;
  } while ( q && id != q->id );

  if ( q ) {			/* found transaction */
    p->next = q->next;
    q->next = NULL;
    *node = q;
    /* coap_delete_node( q ); */
    debug("*** removed transaction %u\n", id);
    return 1;
  }

  return 0;

}

coap_queue_t *
coap_find_transaction(coap_queue_t *queue, coap_tid_t id) {
  while (queue && queue->id != id)
    queue = queue->next;

  return queue;
}

coap_pdu_t *
coap_new_error_response(coap_pdu_t *request, unsigned char code, 
			coap_opt_filter_t opts) {
  coap_opt_iterator_t opt_iter;
  coap_pdu_t *response;
  size_t size = sizeof(coap_hdr_t) + 4; /* some bytes for fence-post options */
  unsigned char buf[2];
  int type; 

#if COAP_ERROR_PHRASE_LENGTH > 0
  char *phrase = coap_response_phrase(code);

  /* Need some more space for the error phrase and the Content-Type option */
  if (phrase)
    size += strlen(phrase) + 2;
#endif

  assert(request);

  /* cannot send ACK if original request was not confirmable */
  type = request->hdr->type == COAP_MESSAGE_CON 
    ? COAP_MESSAGE_ACK
    : COAP_MESSAGE_NON;

  /* Estimate how much space we need for options to copy from
   * request. We always need the Token, for 4.02 the unknown critical
   * options must be included as well. */
  coap_option_clrb(opts, COAP_OPTION_CONTENT_TYPE); /* we do not want this */
  coap_option_setb(opts, COAP_OPTION_TOKEN);

  coap_option_iterator_init(request, &opt_iter, opts);

  while(coap_option_next(&opt_iter))
    size += COAP_OPT_SIZE(opt_iter.option);

  /* Now create the response and fill with options and payload data. */
  response = coap_pdu_init(type, code, request->hdr->id, size);
  if (response) {
#if COAP_ERROR_PHRASE_LENGTH > 0
    if (phrase)
      coap_add_option(response, COAP_OPTION_CONTENT_TYPE, 
		      coap_encode_var_bytes(buf, COAP_MEDIATYPE_TEXT_PLAIN), buf);
#endif

    /* copy all options */
    coap_option_iterator_init(request, &opt_iter, opts);
    while(coap_option_next(&opt_iter))
      coap_add_option(response, opt_iter.type, 
		      COAP_OPT_LENGTH(opt_iter.option),
		      COAP_OPT_VALUE(opt_iter.option));

#if COAP_ERROR_PHRASE_LENGTH > 0
    if (phrase)
      coap_add_data(response, strlen(phrase), (unsigned char *)phrase);
#endif
  }

  return response;
}

coap_pdu_t *
wellknown_response(coap_context_t *context, coap_pdu_t *request) {
  coap_pdu_t *resp;
  coap_opt_iterator_t opt_iter;
  coap_opt_t *token;
  size_t len;
  unsigned char buf[2];

  resp = coap_pdu_init(request->hdr->type == COAP_MESSAGE_CON 
		       ? COAP_MESSAGE_ACK 
		       : COAP_MESSAGE_NON,
		       COAP_RESPONSE_CODE(205),
		       request->hdr->id, COAP_MAX_PDU_SIZE);
  if (!resp)
    return NULL;

  /* add Content-Type */
  coap_add_option(resp, COAP_OPTION_CONTENT_TYPE,
     coap_encode_var_bytes(buf, COAP_MEDIATYPE_APPLICATION_LINK_FORMAT), buf);
  
  token = coap_check_option(request, COAP_OPTION_TOKEN, &opt_iter);
  if (token)
    coap_add_option(resp, COAP_OPTION_TOKEN, 
		    COAP_OPT_LENGTH(token), COAP_OPT_VALUE(token));
  
  /* set payload of response */
  len = resp->max_size - resp->length;
  
  if (!print_wellknown(context, resp->data, &len,
	       coap_check_option(request, COAP_OPTION_URI_QUERY, &opt_iter))) {
    debug("print_wellknown failed\n");
    coap_delete_pdu(resp);
    return NULL;
  } 
  
  resp->length += len;
  return resp;
}

#define WANT_WKC(Pdu,Key)					\
  (((Pdu)->hdr->code == COAP_REQUEST_GET) && is_wkc(Key))

void
handle_request(coap_context_t *context, coap_queue_t *node) {

	LOGI("Incoming REQUEST id%d mid%d", node->id, node->pdu->hdr->id);

  coap_method_handler_t h = NULL;
  coap_pdu_t *response = NULL;
  coap_opt_filter_t opt_filter;
  coap_resource_t *resource;
  coap_key_t key;

  coap_option_filter_clear(opt_filter);
  coap_option_setb(opt_filter, COAP_OPTION_TOKEN); /* we always need the token */
  
  /* try to find the resource from the request URI */
  coap_hash_request_uri(node->pdu, key);
  resource = coap_get_resource_from_key(context, key);
  
  if (!resource) {
		/* The resource was not found. Check if the request URI happens to
		 * be the well-known URI. In that case, we generate a default
		 * response, otherwise, we return 4.04 */

		switch(node->pdu->hdr->code) {

			case COAP_REQUEST_GET:
			  if (is_wkc(key)) {	/* GET request for .well-known/core */
				info("create default response for %s\n", COAP_DEFAULT_URI_WELLKNOWN);
				response = wellknown_response(context, node->pdu);

			  } else { /* GET request for any another resource, return 4.04 */

				debug("GET for unknown resource 0x%02x%02x%02x%02x, return 4.04\n",
					  key[0], key[1], key[2], key[3]);
				response =
				  coap_new_error_response(node->pdu, COAP_RESPONSE_CODE(404),
							  opt_filter);
			  }
			  break;

			default: 			/* any other request type */

			  debug("unhandled request for unknown resource 0x%02x%02x%02x%02x\r\n",
				key[0], key[1], key[2], key[3]);
			  if (!coap_is_mcast(&node->local))
			response = coap_new_error_response(node->pdu, COAP_RESPONSE_CODE(405),
							   opt_filter);
		}

		if (response && coap_send(context, &node->remote, response) == COAP_INVALID_TID) {
		  warn("cannot send response for transaction %u\n", node->id);
		}
		coap_delete_pdu(response);

		return;
  }
  
  /* the resource was found, check if there is a registered handler */
  /* h is the registered handler in this case */
  if ((size_t)node->pdu->hdr->code - 1 <
      sizeof(resource->handler)/sizeof(coap_method_handler_t))
    h = resource->handler[node->pdu->hdr->code - 1];
  
  /*
   * I THINK THE LOGIC OF THIS REQUEST HANDLER IS DEFINITELY
   * NOT OPTIMAL. TWEAK IT TO MAKE IT WORK BUT CONSIDER A
   * RADICAL REWORK IN THE FUTURE
   */


  /* there is a registered handler */
  if (h) {
		debug("call custom handler for resource 0x%02x%02x%02x%02x\n",
		  key[0], key[1], key[2], key[3]);

		response = coap_pdu_init(node->pdu->hdr->type == COAP_MESSAGE_CON
					 ? COAP_MESSAGE_ACK
					 : COAP_MESSAGE_NON,
					 0, node->pdu->hdr->id, COAP_MAX_PDU_SIZE);
		if (response) {
			  coap_opt_iterator_t opt_iter;
			  str token = { 0, NULL };

			  if (coap_check_option(node->pdu, COAP_OPTION_TOKEN, &opt_iter)) {
				token.length = COAP_OPT_LENGTH(opt_iter.option);
				token.s = COAP_OPT_VALUE(opt_iter.option);
			  }

			  h(context, resource, &node->remote, node->pdu, &token, response);

			  /* TODO would be convenient to add to the alive mids
			   * before actually processing the request in its handler h(.)
			   * and later, when you know if that handler caused an RST or ACK
			   * updating the record with this info.
			   */

			  /* Not strictly necessary but helpful. */
		      coap_clean_expired_mids(context->alive_mids);

			  coap_alive_mid_t *newmid = coap_mid_alive_init();
			  newmid->expiry = get_ntp() + EXCHANGE_LIFETIME*1000000000LL;
			  newmid->peer = node->remote;
			  newmid->mid = node->pdu->hdr->id;
			  newmid->type = -1; //request was NON, ACK/RST to this request undefined
			  // if response is NON it means that the request was NON
			  // so if response is NON, don't touch it anymore
			  // if response is ACK or RST, complete the record with this info
			  if (response->hdr->type == COAP_MESSAGE_ACK)
				  newmid->type = COAP_MESSAGE_ACK;
			  else if (response->hdr->type == COAP_MESSAGE_RST)
				  newmid->type = COAP_MESSAGE_RST;
			  else if (response->hdr->type == COAP_MESSAGE_CON)
				  LOGW("undefined situation, answering a CON message with CON response"
						  "and not with ACK");
			  // in any case register the mid in the list
			  LL_APPEND(context->alive_mids, newmid);

			  if ( (response->hdr->type != COAP_MESSAGE_NON ||
			  (response->hdr->code >= 64  && !coap_is_mcast(&node->local)) ) /*&& response != NULL*/) {

				if (coap_send(context, &node->remote, response) == COAP_INVALID_TID) {
				  debug("cannot send response for message %d\n", node->pdu->hdr->id);
				}
			  }
			  //if (response != NULL)
				  coap_delete_pdu(response);
		} else {
		  warn("cannot generate response\r\n");
		}
  } else {
		if (WANT_WKC(node->pdu, key)) {
		  debug("create default response for %s\n", COAP_DEFAULT_URI_WELLKNOWN);
		  response = wellknown_response(context, node->pdu);
		} else
		  response = coap_new_error_response(node->pdu, COAP_RESPONSE_CODE(405),
						 opt_filter);

		if (!response || (coap_send(context, &node->remote, response)
				  == COAP_INVALID_TID)) {
		  debug("cannot send response for transaction %u\n", node->id);
		}
		coap_delete_pdu(response);
  }  

}

static inline void
handle_response(coap_context_t *context, 
		coap_queue_t *sent, coap_queue_t *rcvd) {

	LOGI("Incoming RESPONSE");
  
  /* Call application-specific reponse handler when available.  If
   * not, we must acknowledge confirmable messages. */
  if (context->response_handler) {
    context->response_handler(context, 
			      &rcvd->remote, sent ? sent->pdu : NULL, 
			      rcvd->pdu, rcvd->id);
  } else {
    /* send ACK if rcvd is confirmable (i.e. a separate response) */
    coap_send_ack(context, &rcvd->remote, rcvd->pdu);
  }
}

static inline int
#ifdef __GNUC__
handle_locally(coap_context_t *context __attribute__ ((unused)), 
	       coap_queue_t *node __attribute__ ((unused))) {
#else /* not a GCC */
handle_locally(coap_context_t *context, coap_queue_t *node) {
#endif /* GCC */
  /* this function can be used to check if node->pdu is really for us */
  return 1;
}

coap_alive_mid_t *
coap_mid_alive_init() {
	LOGW("initializing alive mid record");
	coap_alive_mid_t *temp = malloc(sizeof(coap_alive_mid_t));
	if (temp == NULL) return NULL;
	memset(temp, 0, sizeof(coap_alive_mid_t));
	temp->next = NULL;
	return temp;
}

void
coap_clean_expired_mids(coap_alive_mid_t *list) {
	int64_t now = get_ntp();
	coap_alive_mid_t *c, *p;
	LL_FOREACH_SAFE(list, c, p) {
		if(c->expiry < now) {
			LOGW("mid:%d expired, deleting", ntohs(c->mid));
			LL_DELETE(list, c);
			free(c);
		}
	}
}

/* Compares the source address, too, as per CoAP standard. */
coap_alive_mid_t *
mid_is_alive(coap_context_t *context, coap_queue_t *rcvd) {
	LOGW("checking if the just received mid is alive");
	coap_alive_mid_t *t;
	LL_FOREACH(context->alive_mids, t) {
		if ( rcvd->pdu->hdr->id == t->mid &&
				coap_address_equals(&rcvd->remote, &t->peer) ) {
			LOGW("mid:%d was found alive against:%d", ntohs(rcvd->pdu->hdr->id), ntohs(t->mid));
			return t;
		}
	}
	return NULL;
}


void
coap_dispatch( coap_context_t *context ) {
  coap_queue_t *rcvd = NULL, *sent = NULL;
  coap_pdu_t *response;
  coap_opt_filter_t opt_filter;
  coap_key_t key;
  coap_resource_t *res;
  unsigned short mid;
  coap_queue_t *temp, *btemp;
  coap_registration_handler_t h = NULL;
  coap_address_t dest;
  int queuefound = 0;
  coap_alive_mid_t *t;

  //int gotrst = 0;

  if (!context)
    return;

  memset(opt_filter, 0, sizeof(coap_opt_filter_t));

  while ( context->recvqueue ) {
    rcvd = context->recvqueue;

    /* remove node from recvqueue */
    context->recvqueue = context->recvqueue->next;
    rcvd->next = NULL;

    if ( rcvd->pdu->hdr->version != COAP_DEFAULT_VERSION ) {
      debug("dropped packet with unknown version %u\n", rcvd->pdu->hdr->version);
      goto cleanup;
    }
    
    LOGI("--- Received packet ---------");
    printpdu(rcvd->pdu);
	LOGI("-----------------------------");


	/*
	 * good place to insert here..
	 */
	UDP_IN_counter++;
	UDP_IN_octects += rcvd->pdu->length; //correctly set in coap_read() to recvfrom()'s return value

	if (rcvd->pdu->hdr->type == COAP_MESSAGE_NON) {
		IN_NON_counter++;
		IN_NON_octects += rcvd->pdu->length;
	}
	else if (rcvd->pdu->hdr->type == COAP_MESSAGE_CON) {
		IN_CON_counter++;
		IN_CON_octects += rcvd->pdu->length;
	}
	else if (rcvd->pdu->hdr->type == COAP_MESSAGE_ACK) {
		IN_ACK_counter++;
		IN_ACK_octects += rcvd->pdu->length;
	}
	else if (rcvd->pdu->hdr->type == COAP_MESSAGE_RST) {
		IN_RST_counter++;
		IN_RST_octects += rcvd->pdu->length;
	}


    switch ( rcvd->pdu->hdr->type ) {
    case COAP_MESSAGE_ACK:

      LOGI("Incoming ACK mid%d", rcvd->pdu->hdr->id);

      /* find transaction in sendqueue to stop retransmission */
      /* Careful that sent could be != NULL even if no element has
       * been found. */
      queuefound = coap_remove_from_queue(&context->sendqueue, rcvd->id, &sent);

      if (queuefound && sent != NULL && sent->reg != NULL) { //Yes, C uses short-circuit evaluation
		  /* We now have in sent the entry of the queue that has just been
		   * detached from the linked list. Though if we sent a normal response
		   * and not a notification, the reg pointer will be NULL; check that,
		   * then we can safely zero-out the registration's fail count
		   * and release the pointer to the registration that was contained in sent.
		   * This pointer was checked-out when the queue entry
		   * had been created (specifically coap_send_confirmed()
		   * and coap_notify() functions).
		   */
    	  LOGI("Found observe-related transaction id%d mid%d in sendqueue facing ACK id%d mid%d",
    			  sent->id, sent->pdu->hdr->id, rcvd->id, rcvd->pdu->hdr->id);
    	  /* Have to protect to ACK that arrive late, when the failcount
    	   * has already topped and the registration is still in memory and
    	   * in the process of being destroyed. Cannot touch it anymore when is
    	   * being destroyed!
    	   * Still, it is acked and since we destroy the transaction
    	   * we have to release the pointer.
    	   */
    	  if (sent->reg->fail_cnt <= COAP_OBS_MAX_FAIL)
    		  sent->reg->fail_cnt = 0;
    	  res = coap_get_resource_from_key(context, sent->reg->reskey);
    	  if (res != NULL)
    		  coap_registration_release(res, sent->reg);
      }
      else if (queuefound) LOGI("Found oneshot-related transaction in sendqueue facing ACK id%d mid%d",
    		  rcvd->id, rcvd->pdu->hdr->id);
      else LOGI("Not found any transaction in sendqueue facing ACK id%d mid%d",
    		  rcvd->id, rcvd->pdu->hdr->id);

      if (rcvd->pdu->hdr->code == 0)
	goto cleanup;

      /* FIXME: if sent code was >= 64 the message might have been a 
       * notification. Then, we must flag the observer to be alive
       * by setting obs->fail_cnt = 0. */
      break;

    case COAP_MESSAGE_RST :

    	LOGI("Incoming RST mid%d", rcvd->pdu->hdr->id);
    	//gotrst = 1;

      /* We have sent something the receiver disliked, so we remove
       * not only the transaction but also the subscriptions we might
       * have. */

#ifndef WITH_CONTIKI
      coap_log(LOG_ALERT, "got RST for message %u\n", ntohs(rcvd->pdu->hdr->id));
#else /* WITH_CONTIKI */
      coap_log(LOG_ALERT, "got RST for message %u\n", uip_ntohs(rcvd->pdu->hdr->id));
#endif /* WITH_CONTIKI */



      /* @todo remove observer for this resource, if any 
       * get token from sent and try to find a matching resource. Uh!
       * no, no, only resource+destination here as well, there's only one.
       */

      /* Find transaction in sendqueue to stop retransmission */
      queuefound = coap_remove_from_queue(&context->sendqueue, rcvd->id, &sent);

      if (queuefound && sent != NULL && sent->reg != NULL) { //Yes, C uses short-circuit evaluation
    	  /* A transaction for this message ID has been found;
    	   * It's a RST and therefore we should not only delete this
    	   * transaction, but also trigger a stop of the stream. This work
    	   * is performed by the on_unregister function associated
    	   * with the resource and therefore we must first
    	   * identify the resource using sent->reg.
    	   */
    	  LOGI("Found observe-related transaction id%d mid%d in sendqueue facing RST id%d mid%d",
    			  sent->id, sent->pdu->hdr->id, rcvd->id, rcvd->pdu->hdr->id);
    	  res = coap_get_resource_from_key(context, sent->reg->reskey);
    	  if (res != NULL ) {

    		  if ((coap_registration_handler_t*)res->on_unregister != NULL
    				  && !(sent->reg->invalid))
    			  res->on_unregister(context, sent->reg);

			  /* Release because the pointer that was in the sendqueue's
			   * transaction is now gone!
			   */
			  coap_registration_release(res, sent->reg);
    	  }
          else if (queuefound) LOGI("Found oneshot-related transaction in sendqueue facing RST id%d mid%d",
        		  rcvd->id, rcvd->pdu->hdr->id);
          else LOGI("Not found any transaction in sendqueue facing RST id%d mid%d",
        		  rcvd->id, rcvd->pdu->hdr->id);
      }

      break;

    case COAP_MESSAGE_NON :	/* check for unknown critical options */
      if (coap_option_check_critical(context, rcvd->pdu, opt_filter) == 0)
    	  goto cleanup;

      coap_clean_expired_mids(context->alive_mids);
      t = mid_is_alive(context, rcvd);
      if ( t != NULL ) { //duplicate
    	  Duplicate_Count++;
    	  /* Request already processed and no need to send ACK/RST:
    	   * goto cleanup, skipping the request handlers. */
    	  goto cleanup;
      }

      break;

    case COAP_MESSAGE_CON :	/* check for unknown critical options */
      if (coap_option_check_critical(context, rcvd->pdu, opt_filter) == 0) {
		/* FIXME: send response only if we have received a request. Otherwise,
		 * send RST. */
		response =
		  coap_new_error_response(rcvd->pdu, COAP_RESPONSE_CODE(402), opt_filter);
		if (!response)
		  warn("coap_dispatch: cannot create error reponse\n");
		else {
		  if (coap_send(context, &rcvd->remote, response)
			  == COAP_INVALID_TID)
			warn("coap_dispatch: error sending reponse\n");
		  coap_delete_pdu(response);
		}
		goto cleanup;
      }

      coap_clean_expired_mids(context->alive_mids);
      t = mid_is_alive(context, rcvd);
      if ( t != NULL ) { // treat as duplicate
    	  Duplicate_Count++;
    	  /* Request already processed but we need to send ACK/RST:
    	   * if the first time it arrived it was a CON as well, we'll know
    	   * how we answered the first time, whether ACK or RST, and replay
    	   * it now. But the first time might have been a NON, in which case
    	   * in the record there won't be any indication of our reaction
    	   * because there isn't any reaction to a NON. We decide to
    	   * reply RST in this case, because anyway this time it's a CON
    	   * and we have to reply something. If we ignore this the sender will retry
    	   * transmission. */
    	  if (t->type == COAP_MESSAGE_ACK) {
    		  LOGW("Replaying ACK to a duplicate request");
    		  coap_send_ack(context, &rcvd->remote, rcvd->pdu);
    	  }
    	  else {
    		  LOGW("Replaying RST to a duplicate request");
    		  coap_send_rst(context, &rcvd->remote, rcvd->pdu);
    	  }

    	  goto cleanup;
      }


      break;
    }

    //if (gotrst == 0) { //what's that?
    /* Pass message to upper layer if a specific handler was
     * registered for a request that should be handled locally. */
    if (handle_locally(context, rcvd)) {
      if (COAP_MESSAGE_IS_REQUEST(rcvd->pdu->hdr))
	handle_request(context, rcvd);
      else if (COAP_MESSAGE_IS_RESPONSE(rcvd->pdu->hdr))
	handle_response(context, sent, rcvd);
      else {
	debug("dropped message with invalid code\n");
	LOGI("dropped message with invalid code");
	coap_send_message_type(context, &rcvd->remote, rcvd->pdu, 
				 COAP_MESSAGE_RST);
      }
    }
    //}
    
  cleanup:
    coap_delete_node(sent);
    coap_delete_node(rcvd);
  }
}

int
coap_can_exit( coap_context_t *context ) {
  return !context || (context->recvqueue == NULL && context->sendqueue == NULL);
}

#ifdef WITH_CONTIKI

/*---------------------------------------------------------------------------*/
/* CoAP message retransmission */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(coap_retransmit_process, ev, data)
{
  coap_tick_t now;
  coap_queue_t *nextpdu;

  PROCESS_BEGIN();

  debug("Started retransmit process\r\n");

  while(1) {
    PROCESS_YIELD();
    if (ev == PROCESS_EVENT_TIMER) {
      if (etimer_expired(&the_coap_context.retransmit_timer)) {
	
	nextpdu = coap_peek_next(&the_coap_context);
	
	coap_ticks(&now);
	while (nextpdu && nextpdu->t <= now) {
	  coap_retransmit(&the_coap_context, coap_pop_next(&the_coap_context));
	  nextpdu = coap_peek_next(&the_coap_context);
	}

	/* need to set timer to some value even if no nextpdu is available */
	etimer_set(&the_coap_context.retransmit_timer, 
		   nextpdu ? nextpdu->t - now : 0xFFFF);
      } 
#ifndef WITHOUT_OBSERVE
      if (etimer_expired(&the_coap_context.notify_timer)) {
	coap_check_notify(&the_coap_context);
	etimer_reset(&the_coap_context.notify_timer);
      }
#endif /* WITHOUT_OBSERVE */
    }
  }
  
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

#endif /* WITH_CONTIKI */

unsigned int
printit( const unsigned char *data, unsigned int len,
		unsigned char *result, unsigned int buflen, int encode_always ) {
  const unsigned char hex[] = "0123456789ABCDEF";
  unsigned int cnt = 0;

  if (len == 0) {
    *result++ = '\\';
    *result++ = 'x';
    *result++ = hex[0];
    cnt += 3;
    goto finish;
  }

  while ( len && (cnt < buflen-1) ) {
    if ( !encode_always && isprint( *data ) ) {
      *result++ = *data;
      ++cnt;
    } else {
      if ( cnt+4 < buflen-1 ) {
	*result++ = '\\';
	*result++ = 'x';
	*result++ = hex[(*data & 0xf0) >> 4];
	*result++ = hex[*data & 0x0f ];
	cnt += 4;
      } else
	break;
    }

    ++data; --len;
  }

 finish:

  *result = '\0';
  return cnt;
}


printpdu(const coap_pdu_t *pdu) {
  unsigned char buf[COAP_MAX_PDU_SIZE]; /* need some space for output creation */
  int encode = 0;

  LOGI("v:%d t:%d oc:%d c:%d id:%u",
	  pdu->hdr->version, pdu->hdr->type,
	  pdu->hdr->optcnt, pdu->hdr->code, ntohs(pdu->hdr->id));

  /* show options, if any */
  if (pdu->hdr->optcnt) {
    coap_opt_iterator_t opt_iter;
    coap_option_iterator_init((coap_pdu_t *)pdu, &opt_iter, COAP_OPT_ALL);

    while (coap_option_next(&opt_iter)) {


      if (opt_iter.type == COAP_OPTION_URI_PATH ||
	  opt_iter.type == COAP_OPTION_PROXY_URI ||
	  opt_iter.type == COAP_OPTION_URI_HOST ||
	  opt_iter.type == COAP_OPTION_LOCATION_PATH ||
	  opt_iter.type == COAP_OPTION_LOCATION_QUERY ||
	  opt_iter.type == COAP_OPTION_URI_PATH ||
	  opt_iter.type == COAP_OPTION_URI_QUERY) {
	encode = 0;
      } else {
	encode = 1;
      }

      if (printit(COAP_OPT_VALUE(opt_iter.option),
			 COAP_OPT_LENGTH(opt_iter.option),
			 buf, sizeof(buf), encode ))
	LOGI("opt%d: '%s',", opt_iter.type, buf);
    }

  }

  if (pdu->data < (unsigned char *)pdu->hdr + pdu->length) {
    printit(pdu->data,
		   (unsigned char *)pdu->hdr + pdu->length - pdu->data,
		   buf, sizeof(buf), 0 );
    LOGI("data: %s", buf);
  }

}


