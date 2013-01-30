#include "asynchronous.h"


coap_registration_t *
coap_registration_init(coap_key_t reskey, coap_address_t sub, str *token) {

	coap_registration_t *s;
	s = (coap_registration_t *)coap_malloc(sizeof(coap_registration_t));
	if (!s)
		return NULL;
	memset(s, 0, sizeof(coap_registration_t));

	s->next = NULL;
	s->refcnt = 0;
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
coap_registration_release(coap_context_t *context, coap_registration_t *r) {
	/*
	 * FIXME
	 * We make it aware that the object is within a list
	 * so when the refcnt is 0, before freeing, we strip it
	 * from the list and reconnect the siblings
	 */
	coap_resource_t *res;
	coap_registration_t *p;

	assert(r);
	r->refcnt--;
	if (r->refcnt == 0) {
		/* Unfortunately as it is a single-linked list
		 * for the deletion we must restart from the head
		 * deletion O(n)..
		 */
		res = coap_get_resource_from_key(context, r->reskey);
		if (res != NULL) {
			p = res->subscribers;
			while (p != NULL && p->next != r) p = p->next;
			if (p != NULL) p->next = r->next;
		}
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

	coap_registration_t *s = NULL, *found = NULL;
	assert(peer);

	found = coap_find_registration(resource, peer);
	/* Replace, remember that we must not move it because
	 * the value of this pointer identifies the observer at
	 * the streaming manager level.
	 */
	if (found) {
		found->token_length = token->length;
		memset(found->token, 0, 8);
		memcpy(found->token, token->s, min(s->token_length, 8));
		//no need to copy subscriber, it's the same one

		//s = coap_registration_checkout(found);
		s = found;
	}
	/* Add, prepending. */
	else {
		s = coap_registration_init(resource->key, *peer, token);
		s->next = resource->subscribers;
		/* Generate a new ticket,
		 * a new observation has been created.
		 * Nope, we've decided to do it outside, the ticket gets
		 * generated when it is actually passed to the Streaming
		 * Manager
		 */
		//resource->subscribers = coap_registration_checkout(s);
		resource->subscribers = s;
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

coap_registration_t *
coap_find_registration(coap_resource_t *resource,
		coap_address_t *peer) {

	coap_registration_t *s = NULL;

	s = resource->subscribers;
	while (s != NULL) {
		if (coap_address_equals(&(s->subscriber), peer) == 1)
			break;
		s = s->next;
	}

	return s;
}
