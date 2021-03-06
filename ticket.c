#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <ctype.h>
#include "ticket.h"
#include "jsmn.h"


/*
 * This max number of tokens must be large enough to handle usual tickets.
 * The number of tokens varies with the number of realms in the scope.
 * MAX_TOKENS has been calculated like this:
 * 1 Token for the overall object
 * 16 Tokens for the 8 fields (2 tokens each)
 * MAX_SCOPE (see ticket.h) tokens for the scope
 * => 27 tokens.
 *
 */
#define MAX_TOKENS 27


/*
 * Structure to hold state while parsing.
 */
typedef struct Builder {
	jsmn_parser parser;
	jsmntok_t tokens[MAX_TOKENS];
	size_t i;
	Ticket ticket;
	size_t ntokens;
	char *input;
} *Builder;

static void ticket_init(Ticket t);
static TicketError do_algo(Builder builder);
static TicketError do_string(Builder builder, HawkcString *s);
static TicketError do_rw(Builder builder, int *v);
static TicketError do_time(Builder builder, time_t *tp);
static TicketError do_scope(Builder builder);

/*
 * digittoint was missing in some compile environments, so we supply
 * our own.
 */
static int my_digittoint(char ch) {
  int d = ch - '0';
  if ((unsigned) d < 10) {
    return d;
  }
  d = ch - 'a';
  if ((unsigned) d < 6) {
    return d + 10;
  }
  d = ch - 'A';
  if ((unsigned) d < 6) {
    return d + 10;
  }
  return -1;
}



/** Error strings used by ticket_strerror
 *
 */
static char *error_strings[] = {
		"Success", /* OK */
		"Ticket JSON corrupted", /* ERROR_JSON_INVAL */
		"Too many JSON tokens in ticket", /* ERROR_JSON_NTOKENS */
		"Ticket JSON misses a part", /* ERROR_JSON_PART */
		"Not enough tokens in ticket JSON to parse expected token", /* ERROR_MISSING_EXPECTED_TOKEN */
		"Unexpected token type", /* ERROR_UNEXPECTED_TOKEN_TYPE */
		"Unexpected token name", /* ERROR_UNEXPECTED_TOKEN_NAME */
		"Unable to parse time value", /* ERROR_PARSE_TIME_VALUE */
		"Too many realms in ticket", /* ERROR_NREALMS */
		"Unknown Hawk algorithm", /* ERROR_UNKNOWN_HAWK_ALGORITHM */
		"Error" , /* ERROR */
		NULL
};

char* ticket_strerror(TicketError e) {
	/* assert(e >= OK && e <= ERROR); */
	return error_strings[e];
}

TicketError ticket_from_string(Ticket ticket, char *json_string,size_t len) {
	TicketError e;
	jsmnerr_t jsmn_error;
	struct Builder builder;

	/* Initialize builder */
	memset(&builder,0,sizeof(builder));

	/* Initialize parser */
	jsmn_init(&(builder.parser));
	builder.ticket = ticket;
	builder.input = json_string;
	/* Initialize ticket */
	ticket_init(builder.ticket);

	if( (jsmn_error = jsmn_parse(&(builder.parser), builder.input, len, builder.tokens, MAX_TOKENS)) != JSMN_SUCCESS) {
		switch(jsmn_error) {
		case JSMN_ERROR_INVAL:
			return ERROR_JSON_INVAL;
		case JSMN_ERROR_NOMEM:
			return ERROR_JSON_NTOKENS;
		case JSMN_ERROR_PART:
			return ERROR_JSON_PART;
		default:
			/* Should never be reached */
			return OK;
		}
		/* Should never be reached */
		return ERROR;
	}

	/* Make number of tokens accessible more conveniently */
	builder.ntokens = builder.parser.toknext;

	/* Build ticket from parsed JSON tokens */
	for (builder.i = 0; builder.i< (size_t)builder.parser.toknext;builder.i++) {
        jsmntok_t *t = &(builder.tokens[builder.i]);
        unsigned int length = t->end - t->start;
        char *s = builder.input + t->start;

        if (t->type == JSMN_STRING) {
        	if(length == 6 && strncmp(s,"client",length) == 0) {
        		if( (e = do_string(&builder,&(ticket->client))) != OK) {
        			return e;
        		}
        	} else if(length == 3 && strncmp(s,"pwd",length) == 0) {
        		if( (e = do_string(&builder,&(ticket->pwd))) != OK) {
        			return e;
        		}
        	} else if(length == 13 && strncmp(s,"hawkAlgorithm",length) == 0) {
        		if( (e = do_algo(&builder)) != OK) {
        			return e;
        		}
        	} else if(length == 5 && strncmp(s,"owner",length) == 0) {
        		if( (e = do_string(&builder,&(ticket->owner))) != OK) {
        			return e;
        		}
        	} else if(length == 5 && strncmp(s,"scope",length) == 0) {
        		if( (e = do_scope(&builder)) != OK) {
        			return e;
        		}
        	} else if(length == 6 && strncmp(s,"scopes",length) == 0) {
        		if( (e = do_scope(&builder)) != OK) {
        			return e;
        		}
        	} else if(length == 4 && strncmp(s,"user",length) == 0) {
        		if( (e = do_string(&builder,&(ticket->user))) != OK) {
        			return e;
        		}
        	} else if(length == 3 && strncmp(s,"exp",length) == 0) {
        		if( (e = do_time(&builder,&(ticket->exp))) != OK) {
        			return e;
        		}
           	} else if(length == 2 && strncmp(s,"rw",length) == 0) {
           		if( (e = do_rw(&builder,&(ticket->rw))) != OK) {
           			return e;
           		}
        	} else {
        		return ERROR_UNEXPECTED_TOKEN_NAME;
        	}
        } else if (t->type == JSMN_PRIMITIVE) {
        	/* Primitives are handled in dedicated functions */
        	return ERROR_UNEXPECTED_TOKEN_TYPE;
        } else if (t->type == JSMN_ARRAY) {
        	/* Scopes array is handled by dedicated function. Should never come here */
        	return ERROR_UNEXPECTED_TOKEN_TYPE;
        } else if (t->type == JSMN_OBJECT) {
        	/* Only object we should encounter is the ticket itself, which is at i=0 */
        	if(builder.i != 0) {
        		return ERROR_UNEXPECTED_TOKEN_TYPE;
        	}
        } else {
       		return ERROR_UNEXPECTED_TOKEN_TYPE;
        }
    }
	return OK;
}

TicketError do_string(Builder builder, HawkcString *s) {
	jsmntok_t *t;
	builder->i++;
	if(builder->i >= builder->ntokens) {
		return ERROR_MISSING_EXPECTED_TOKEN;
	}
	t = &(builder->tokens[builder->i]);
	if(t->type != JSMN_STRING) {
		return ERROR_UNEXPECTED_TOKEN_TYPE;
	}
	/* FIXME Cast implies data is only ascii - maybe I need to rewrite jsmn at some point */
   s->data = (unsigned char*)builder->input+t->start;
   s->len = t->end - t->start;
   return OK;
}

TicketError do_time(Builder builder, time_t *tp) {
	time_t x = 0;
	char *p;
	int i;
	jsmntok_t *t;
	builder->i++;
	if(builder->i >= builder->ntokens) {
		return ERROR_MISSING_EXPECTED_TOKEN;
	}
	t = &(builder->tokens[builder->i]);
	if(t->type != JSMN_PRIMITIVE) {
		return ERROR_UNEXPECTED_TOKEN_TYPE;
	}
	p = builder->input+t->start;
	i = t->start;
	while(i < t->end) {
		if(!isdigit(*p)) {
			return ERROR_PARSE_TIME_VALUE;
		}
		x = (x * 10) + my_digittoint(*p);
		p++;
		i++;
	}
	*tp = x;
	return OK;
}

TicketError do_rw(Builder builder, int *v) {
	jsmntok_t *t;
	*v = 0; /* Use rw=false as a safe default */
	builder->i++;
	if(builder->i >= builder->ntokens) {
		return ERROR_MISSING_EXPECTED_TOKEN;
	}
	t = &(builder->tokens[builder->i]);
	/* check for 'true' only, false is safe default for rw */
	if( (t->type == JSMN_PRIMITIVE) && (t->end - t->start == 4) && (strncmp(builder->input+t->start,"true",4) == 0)) {
		*v = 1;
	}
	return OK;
}

TicketError do_scope(Builder builder) {
	jsmntok_t *t;
	int i;
	builder->i++;
	if(builder->i >= builder->ntokens) {
		return ERROR_MISSING_EXPECTED_TOKEN;
	}
	t = &(builder->tokens[builder->i]);
	if(t->type != JSMN_ARRAY) {
		return ERROR_UNEXPECTED_TOKEN_TYPE;
	}
	if(t->size > MAX_REALMS) {
		return ERROR_NREALMS;
	}
	for(i=0;i<t->size;i++) {
		do_string(builder,&(builder->ticket->realms[i]));
	}
	builder->ticket->nrealms = t->size;
	return OK;
}

TicketError do_algo(Builder builder) {
	HawkcString algo;
	HawkcAlgorithm a;
	TicketError e;
	if( (e = do_string(builder,&algo)) != OK) {
		return e;
	}
	/* FIXME Cast implies data is only ascii - maybe I need to rewrite jsmn at some point */
	if( (a = hawkc_algorithm_by_name((char*)algo.data, algo.len)) == NULL) {
		return ERROR_UNKNOWN_HAWK_ALGORITHM;
	}
	builder->ticket->hawkAlgorithm = a;
	return OK;
}

int ticket_has_realm(Ticket ticket, unsigned char *realm, size_t realm_len) {
	size_t i;
	for(i=0;i<ticket->nrealms;i++) {
		if(ticket->realms[i].len == realm_len) {
			if(memcmp(ticket->realms[i].data,realm,realm_len) == 0) {
				return 1;
			}
		}
	}
	return 0;
}

void ticket_init(Ticket t) {
	memset(t,0,sizeof(struct Ticket));
	t->rw = 0; /* false is default */
}

