/* Copyright (c) 2008-2009, UNINETT AS
 * Copyright (c) 2010, NORDUnet A/S */
/* See LICENSE for licensing information. */

#define _GNU_SOURCE
#ifdef SYS_SOLARIS9
#include <sys/inttypes.h>
#else
#include <stdint.h>
#endif
#include "list.h"
#include "tlv11.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>

struct tlv *maketlv(uint8_t t, uint8_t l, void *v) {
    struct tlv *tlv;

    tlv = malloc(sizeof(struct tlv));
    if (!tlv)
	return NULL;
    tlv->t = t;
    tlv->l = l;
    if (l && v) {
	tlv->v = malloc(l);
	if (!tlv->v) {
	    free(tlv);
	    return NULL;
	}
	memcpy(tlv->v, v, l);
    } else
	tlv->v = NULL;
    return tlv;
}

struct tlv *copytlv(struct tlv *in) {
    return in ? maketlv(in->t, in->l, in->v) : NULL;
}

void freetlv(struct tlv *tlv) {
    if (tlv) {
	free(tlv->v);
	free(tlv);
    }
}

int eqtlv(struct tlv *t1, struct tlv *t2) {
    if (!t1 || !t2)
	return t1 == t2;
    if (t1->t != t2->t || t1->l != t2->l)
	return 0;
    return memcmp(t1->v, t2->v, t1->l) == 0;
}

struct list *copytlvlist(struct list *tlvs) {
    struct list *out;
    struct list_node *node;

    if (!tlvs)
	return NULL;
    out = list_create();
    if (!out)
	return NULL;
    for (node = list_first(tlvs); node; node = list_next(node)) {
	if (!list_push(out, copytlv((struct tlv *)node->data))) {
	    freetlvlist(out);
	    return NULL;
	}
    }
    return out;
}

void freetlvlist(struct list *tlvs) {
    struct tlv *tlv;
    while ((tlv = (struct tlv *)list_shift(tlvs)))
	freetlv(tlv);
    list_destroy(tlvs);
}

void rmtlv(struct list *tlvs, uint8_t t) {
    struct list_node *n, *p;
    struct tlv *tlv;

    p = NULL;
    n = list_first(tlvs);
    while (n) {
	tlv = (struct tlv *)n->data;
	if (tlv->t == t) {
	    list_removedata(tlvs, tlv);
	    freetlv(tlv);
	    n = p ? list_next(p) : list_first(tlvs);
	} else {
	    p = n;
	    n = list_next(n);
	}
    }
}

uint8_t *tlv2str(struct tlv *tlv) {
    if(!tlv)
        return '\0';
    uint8_t *s = malloc(tlv->l + 1);
    if (s) {
	memcpy(s, tlv->v, tlv->l);
	s[tlv->l] = '\0';
    }
    return s;
}

struct tlv *resizetlv(struct tlv *tlv, uint8_t newlen) {
    uint8_t *newv;
    if (newlen != tlv->l) {
        newv = realloc(tlv->v, newlen);
        if (newlen && !newv)
            return NULL;
        tlv->v = newv;
        tlv->l = newlen;
    }
    return tlv;
}

uint32_t tlv2longint(struct tlv *tlv) {
    if(!tlv) return 0;
    uint32_t n = 0;
    n += tlv->v[3];
    n += tlv->v[2] << 8;
    n += tlv->v[1] << 16;
    n += tlv->v[0] << 24;
    return n;
}

char* tlv2ipv4addr(struct tlv *tlv) {
    if(!tlv) return 0;
    char *rval = "undef";
    if(tlv->v) {
        uint8_t *v = tlv2str(tlv);
	char *str;
        if(asprintf(&str, "%d.%d.%d.%d", v[0], v[1], v[2], v[3]) >=0) {
            rval = str;
        }
        free(v);
    }
    return rval;
}

/* Local Variables: */
/* c-file-style: "stroustrup" */
/* End: */
