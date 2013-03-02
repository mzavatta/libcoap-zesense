/*
 * libcoap (ZeSense extension)
 * -- New types and utilities to support
 *    draft-ietf-core-observe
 *
 * Marco Zavatta
 * <marco.zavatta@telecom-bretagne.eu>
 * <marco.zavatta@mail.polimi.it>
 */

#ifndef ZE_ASYNCHRNONOUS_H
#define ZE_ASYNCHRNONOUS_H

#include "config.h"
#include "address.h"
#include "hashkey.h"
#include "pdu.h"
#include "str.h"

typedef struct coap_registration_t {
	struct coap_registration_t *next; /**< next element in linked list */
  	coap_address_t subscriber;	    /**< address and port of subscriber */

  	unsigned int non;		/**< send non-confirmable notifies if @c 1  */
  	unsigned int non_cnt;	/**< up to 15 non-confirmable notifies allowed */
  	unsigned int fail_cnt;	    /**< up to 3 confirmable notifies can fail */

  	size_t token_length;		/**< actual length of token */
  	unsigned char token[8];	/**< token used for subscription */

  	/* Resource that we're observing */
  	coap_key_t reskey;

  	/* Reference count */
  	int refcnt;

  	/* Invalid flag, to be set when the registration
  	 * is in the process to be deleted. The deletion
  	 * trigger can either be an explicit client request TODO
  	 * or a topped fail count.
  	 */
  	int invalid;

  	/* @todo CON/NON flag, block size */

  	/* requested content format */
  	/* TODO: might be more than one! */
  	//unsigned char content_format[2];

  	/* TODO: some strange stuff on Entity Tags */

  	/* data sessio parameters
  	 * observe option value sequence number,
  	 * as per draft-loreto-core-coap-streaming-00 */
  	short notcnt;

  	/* sender report, stream control parameters */
  	int srready;
  	long ntptwin;
  	int rtptwin;
  	int octcount;
  	int packcount;

  	int last_sr_octcount;
  	int last_sr_packcount;

} coap_registration_t;


coap_registration_t *
coap_registration_init(coap_key_t reskey, coap_address_t sub, str *token);

#endif
