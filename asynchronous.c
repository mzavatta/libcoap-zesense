#include "asynchronous.h"
#include "resource.h"
#include "mem.h"


#define min(a,b) ((a) < (b) ? (a) : (b))

coap_registration_t *
coap_registration_init(coap_key_t reskey, coap_address_t sub, str *token) {

	coap_registration_t *s;
	s = (coap_registration_t *)coap_malloc(sizeof(coap_registration_t));
	if (!s)
		return NULL;
	memset(s, 0, sizeof(coap_registration_t));

	s->next = NULL;
	s->refcnt = 0;
	s->invalid = 0;

	s->notcnt = (short)rand(); //randomized RTP-like

	s->srready = 0;
	s->ntptwin = 0;
	s->rtptwin = 0;
	s->octcount = 0;
	s->packcount = 0;
	s->last_sr_octcount = 0;
	s->last_sr_packcount = 0;

	memcpy(s->reskey, reskey, 4);
	memcpy(&(s->subscriber), &sub, sizeof(coap_address_t));

	//we've declared the token as an array here
	if (token && token->length) {
		s->token_length = token->length;
		memcpy(s->token, token->s, min(s->token_length, 8));
	}

	return s;
}


