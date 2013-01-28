#include "asynchronous.h"


coap_registration_t *
coap_registration_init(coap_key_t reskey, coap_address_t sub, str *token) {

	coap_registration_t *s;
	s = (coap_registration_t *)coap_malloc(sizeof(coap_registration_t));
	if (!s)
		return NULL;
	memset(s, 0, sizeof(coap_registration_t));

	s->next = NULL;
	s->refcnt = 1;
	s->reskey = reskey;
	memcpy(&s->subscriber, &sub, sizeof(coap_address_t));

	//we've declared the token as an array here
	if (token && token->length) {
		s->token_length = token->length;
		memcpy(s->token, token->s, min(s->token_length, 8));
	}

	return s;
}

void
coap_registration_release(coap_registration_t *r) {
	assert(r);
	r->refcnt--;
	if (r->refcnt == 0) {
		/* FIXME
		 * since it's stored into a linked list we cannot
		 * just free it blindly, we need to connect the
		 * siblings first
		 */
		free(r);
	}
}

coap_registration_t *
coap_registration_checkout(coap_registration_t *r) {
	assert(r);
	r->refcnt++;
	return r;
}


coap_registration_t *
coap_add_registration(coap_resource_t *resource,
		coap_address_t *peer, str *token) {

	coap_registration_t *s = NULL, *found = NULL; //*bfound = NULL;
	assert(peer);

	s = coap_registration_init(resource->key, *peer, token);

	/* Check if there is already a subscription for this peer. */
	/*
	LL_FOREACH(resource->subscribers, found) {
		if (coap_address_equals(&found->subscriber, peer))
			break;
		bfound = found;
	}*/
	/* replace *//*
	if (found) {
		s->next = found->next;
		bfound->next = s;
		coap_registration_release(found);
	}*/

	found = coap_find_registration(resource, peer);
	/* replace
	 * !! we must not move it because the value of this pointer
	 * identifies the observer at the streaming manager
	 * level. if we don't delete the stream first, we must
	 * keep the same reference !!
	 */
	if (found) {
		found->token_length = token->length;
		memcpy(found->token, 0, 8);
		memcpy(found->token, token->s, min(s->token_length, 8));
		//no need to copy subscriber, it's the same one
		s = found;
		s = coap_registration_checkout(s);
	}
	/* add */
	else {
		LL_PREPEND(resource->subscribers, s);
	}
	return s;
}


void
coap_delete_registration(coap_resource_t *resource,
		coap_address_t *peer, str *token) {

	coap_registration_t *s;

	//s = coap_find_observer(resource, observer, token);
	s = coap_find_registration(resource, peer);

	if (s) {
    LL_DELETE(resource->subscribers, s);
    /* FIXME: notify observer that its subscription has been removed */
    coap_registration_release(s);
	}
}
