/*
 * libcoap (ZeSense extension)
 * -- New types and utilities to support
 *    draft-ietf-core-observe
 *
 * Marco Zavatta
 * <marco.zavatta@telecom-bretagne.eu>
 * <marco.zavatta@mail.polimi.it>
 */

#include "config.h"
#include "address.h"

struct coap_registration_t;

typedef struct coap_registration_t {
	struct coap_registration_t *next; /**< next element in linked list */
  	coap_address_t subscriber;	    /**< address and port of subscriber */

  	unsigned int non:1;		/**< send non-confirmable notifies if @c 1  */
  	unsigned int non_cnt:4;	/**< up to 15 non-confirmable notifies allowed */
  	unsigned int fail_cnt:2;	/**< up to 3 confirmable notifies can fail */

  	size_t token_length;		/**< actual length of token */
  	unsigned char token[8];	/**< token used for subscription */

  	/* Resource that we're observing */
  	coap_key_t reskey;

  	/* Reference count */
  	int refcnt;

  	/* Delete flag, to be set when the client requests
  	 * a delete of the observer relationship
  	 * (or a deletion of the resource)
  	 * but there are other references around.
  	 */
  	int delflag;

  	/* @todo CON/NON flag, block size */

  	/* requested content format */
  	/* TODO: might be more than one! */
  	//unsigned char content_format[2];

  	/* TODO: some strange stuff on Entity Tags */

  	/* observe option value sequence number,
  	 * actually there is a global one in coap_context_t */
  	//unsigned char seq_num[2];

} coap_registration_t;

typedef coap_registration_t* coap_ticket_t;

coap_registration_t *
coap_registration_init(coap_key_t reskey, coap_address_t sub, str *token);

void
coap_registration_release(coap_registration_t* r);

coap_registration_t *
coap_registration_checkout(coap_registration_t *r);

coap_registration_t *
coap_add_registration(coap_resource_t *resource,
		coap_address_t *peer, str *token);

void
coap_delete_registration(coap_resource_t *resource,
		coap_address_t *peer, str *token);
