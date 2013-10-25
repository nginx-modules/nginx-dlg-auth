#include <time.h>
#include <stddef.h>
#include <stdio.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_crypt.h>
#include <hawkc.h>
#include <ciron.h>
#include "ticket.h"

#include "nginx_dlg_auth.h"
#include "nginx_dlg_auth_var.h"


/*
 * ciron user-provided buffer sizes. The size has been determined by using
 * some usual tickets, observing the required sizes and then adding a fair amount of
 * space. E.g. Having seen 350 bytes required I choose 1024 for the buffer.
 *
 * We do size checking before using the buffer and report an error if the buffer
 * sizes below are exceeded. More requirement for space rather indicates an attack,
 * than normal use.
 */
#define ENCRYPTION_BUFFER_SIZE 1024
#define OUTPUT_BUFFER_SIZE 512

/*
 * We differentiate tickets that grant access to only-safe and safe and
 * unsafe HTTP methods.
 * This macro is used to test what kind of method we have.
 */
#define IS_UNSAFE_METHOD(m) (!( \
		((m) == NGX_HTTP_GET) || \
		((m) == NGX_HTTP_HEAD) || \
		((m) == NGX_HTTP_OPTIONS) || \
		((m) == NGX_HTTP_PROPFIND) \
		))

#define MAX_PWD_TAB_ENTRIES 100

/*
 * Module per-location configuration.
 */
typedef struct {
	/* Authentication realm a given ticket must grant access to */
    ngx_str_t realm;

    /* iron password to unseal received access tickets. */
    ngx_str_t iron_password;

    /* iron password table for password rotation */
    struct CironPwdTableEntry pwd_table_entries[MAX_PWD_TAB_ENTRIES];
    struct CironPwdTable pwd_table;

    /* Allowed skew when comparing request timestamp with our own clock */
    ngx_uint_t allowed_clock_skew;

    /* Host to use for signature validation instead of request host */
    ngx_str_t  host;

    /* Port to use for signature validation instead of request port */
    ngx_str_t  port;

} ngx_http_dlg_auth_loc_conf_t;


/*
 * Functions for configuration handling
 */
static char * ngx_http_dlg_auth_iron_passwd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_dlg_auth_init(ngx_conf_t *cf);
static void *ngx_http_dlg_auth_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_dlg_auth_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static int is_digits_only(ngx_str_t *str);
/*
 * Functions for request processing
 */
static ngx_int_t ngx_dlg_auth_handler(ngx_http_request_t *r);
static ngx_int_t ngx_dlg_auth_authenticate(ngx_http_request_t *r,ngx_http_dlg_auth_loc_conf_t *conf, ngx_http_dlg_auth_ctx_t *ctx);
static void ngx_dlg_auth_rename_authorization_header(ngx_http_request_t *r);
static ngx_int_t ngx_dlg_auth_send_simple_401(ngx_http_request_t *r, ngx_str_t *realm);
static ngx_int_t ngx_dlg_auth_send_401(ngx_http_request_t *r, HawkcContext hawkc_ctx);
static void determine_host_and_port(ngx_http_dlg_auth_loc_conf_t *conf, ngx_http_request_t *r,ngx_str_t *host, ngx_str_t *port);
static void get_host_and_port(ngx_str_t host_header, ngx_str_t *host, ngx_str_t *port);

/*
 * Functions for variable setting.
 */
ngx_int_t store_client(ngx_http_request_t *r, ngx_http_dlg_auth_ctx_t *ctx,Ticket ticket);
ngx_int_t store_expires(ngx_http_request_t *r, ngx_http_dlg_auth_ctx_t *ctx,Ticket ticket);
ngx_int_t store_clockskew(ngx_http_request_t *r, ngx_http_dlg_auth_ctx_t *ctx, time_t clockskew);

/*
 * The configuration directives
 */
static ngx_command_t ngx_dlg_auth_commands[] = {

	{ ngx_string("dlg_auth"),
	  NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF
	                       |NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
	  NGX_HTTP_LOC_CONF_OFFSET,
	  offsetof(ngx_http_dlg_auth_loc_conf_t, realm),
	  NULL },

	{ ngx_string("dlg_auth_iron_pwd"),
	  NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF
	                       |NGX_CONF_TAKE12,
	  ngx_http_dlg_auth_iron_passwd,
	  NGX_HTTP_LOC_CONF_OFFSET,
	  0,
	  NULL },

	  { ngx_string("dlg_auth_allowed_clock_skew"),
	        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
	        ngx_conf_set_num_slot,
	        NGX_HTTP_LOC_CONF_OFFSET,
	        offsetof(ngx_http_dlg_auth_loc_conf_t, allowed_clock_skew),
	        NULL },


    { ngx_string("dlg_auth_host"),
    	  NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF
    	                       |NGX_CONF_TAKE1,
    	  ngx_conf_set_str_slot,
    	  NGX_HTTP_LOC_CONF_OFFSET,
    	  offsetof(ngx_http_dlg_auth_loc_conf_t, host),
    	  NULL },

    { ngx_string("dlg_auth_port"),
    	  NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF
    	                       |NGX_CONF_TAKE1,
    	  ngx_conf_set_str_slot,
    	  NGX_HTTP_LOC_CONF_OFFSET,
    	  offsetof(ngx_http_dlg_auth_loc_conf_t, port),
    	  NULL },

    ngx_null_command /* command termination */
};

/*
 * The static (configuration) module context.
 */
static ngx_http_module_t  nginx_dlg_auth_module_ctx = {
	ngx_http_auth_dlg_add_variables,     /* preconfiguration */
    ngx_http_dlg_auth_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_dlg_auth_create_loc_conf,   /* create location configuration */
    ngx_http_dlg_auth_merge_loc_conf     /* merge location configuration */
};

/*
 * The module definition itself.
 */
ngx_module_t  nginx_dlg_auth_module = {
    NGX_MODULE_V1,
    &nginx_dlg_auth_module_ctx,       /* module context */
    ngx_dlg_auth_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/*
 * This function handles the dlg_auth_iron_pwd directive. If a single value
 * is supplied, it is interpreted as the single password used for sealing,
 * unsealing.
 *
 * If two values are provided, the directive is interpreted as pair of
 * password ID and password and it is then stored in the password table.
 */
static char * ngx_http_dlg_auth_iron_passwd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	ngx_http_dlg_auth_loc_conf_t  *lcf;
    ngx_str_t *value;

	lcf = conf;
    value = cf->args->elts;

    /*
     * Single password case.
     */
    if(cf->args->nelts == 2) {
    	if(lcf->iron_password.len != 0) {
    		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "dlg_auth_iron_pwd directive must not be used more than once for setting single password");
    		return NGX_CONF_ERROR;
    	}
    	if(lcf->pwd_table.nentries != 0) {
    		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "dlg_auth_iron_pwd directive does not allow mixed use of password table and single password");
    		return NGX_CONF_ERROR;
    	}
    	lcf->iron_password.data =  ngx_pstrdup(cf->pool, &(value[1]));
    	lcf->iron_password.len = value[1].len;
    /*
     * Password table entry case.
     */
    } else if(cf->args->nelts == 3) {
    	int i;
    	if(lcf->iron_password.len != 0) {
    		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "dlg_auth_iron_pwd directive does not allow mixed use of password table and single password");
    		return NGX_CONF_ERROR;
    	}
    	if(lcf->pwd_table.nentries == MAX_PWD_TAB_ENTRIES) {
    		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Too many dlg_auth_iron_pwd directives, please use less id/password pairs");
    		return NGX_CONF_ERROR;
    	}
    	i = lcf->pwd_table.nentries;
    	/* value[1] is password ID, value[2] is password */
    	lcf->pwd_table.entries[i].password_id_len = value[1].len;
    	lcf->pwd_table.entries[i].password_id = value[1].data;
    	lcf->pwd_table.entries[i].password_len = value[2].len;
    	lcf->pwd_table.entries[i].password = value[2].data;
    	lcf->pwd_table.nentries++;
    } else {
    	/* Should never be here because nginx enforces NGX_CONF_TAKE12 */
   		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "dlg_auth_iron_pwd directive takes only one or two arguments");
   		return NGX_CONF_ERROR;
    }
    return NGX_CONF_OK;
}

/*
 * Initialization function to register handler to
 * nginx access phase.
 */
static ngx_int_t ngx_http_dlg_auth_init(ngx_conf_t *cf) {
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    if( (h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers)) == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_dlg_auth_handler;

    return NGX_OK;
}

/*
 * Allocate new per-location config
 */
static void *ngx_http_dlg_auth_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_dlg_auth_loc_conf_t  *conf;
    if( (conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_dlg_auth_loc_conf_t))) == NULL) {
        return NULL;
    }
    /* Initialize realm */
    conf->realm.len = 0;
    conf->realm.data = NULL;

    /* Initialize password */
    conf->iron_password.len = 0;
    conf->iron_password.data = NULL;

    /* Initialize password table */
    conf->pwd_table.entries = conf->pwd_table_entries;
    conf->pwd_table.nentries = 0;

    /* Initialize clock skew */
    conf->allowed_clock_skew = NGX_CONF_UNSET_UINT;

    /* Initialize explicit signature verification host and port */
    conf->host.len = 0;
    conf->host.data = NULL;
    conf->port.len = 0;
    conf->port.data = NULL;

    return conf;
}

/*
 * Inherit per-location configuration if it has not been set
 * specifically.
 */
static char * ngx_http_dlg_auth_merge_loc_conf(ngx_conf_t *cf, void *vparent, void *vchild) {
    ngx_http_dlg_auth_loc_conf_t  *parent = (ngx_http_dlg_auth_loc_conf_t*)vparent;
    ngx_http_dlg_auth_loc_conf_t  *child = (ngx_http_dlg_auth_loc_conf_t*)vchild;

    /* Merge realm */
    if (child->realm.len == 0) {
        child->realm.len = parent->realm.len;
        child->realm.data = parent->realm.data;
    }
    /* Merge single password, if any */
    if (child->iron_password.len == 0) {
        child->iron_password.len = parent->iron_password.len;
        child->iron_password.data = parent->iron_password.data;
    }

    /* Merge password table if any */
    if(child->pwd_table.nentries == 0) {
    	int i;
    	for(i=0;i<parent->pwd_table.nentries;i++) {
    		child->pwd_table.entries[i].password_id_len = parent->pwd_table.entries[i].password_id_len;
    		child->pwd_table.entries[i].password_id = parent->pwd_table.entries[i].password_id;
    		child->pwd_table.entries[i].password_len = parent->pwd_table.entries[i].password_len;
    		child->pwd_table.entries[i].password = parent->pwd_table.entries[i].password;
    	}
    	child->pwd_table.nentries = parent->pwd_table.nentries;
    }




    /*
     * Inherit or set default allowed clock skew of 1s.
     */
    ngx_conf_merge_uint_value(child->allowed_clock_skew, parent->allowed_clock_skew, 1);

    /*
     * Inherit explicit request signature host and port setting.
     */
    if (child->host.len == 0) {
        child->host.len = parent->host.len;
        child->host.data = parent->host.data;
    }
    if (child->port.len == 0) {
        child->port.len = parent->port.len;
        child->port.data = parent->port.data;
    }

    /*
     * If dlg_auth module applies to this location, perform some config sanity checks.
     */

    if(child->realm.len != 0) {
        /* We need iron password or password table */
        if(child->iron_password.len == 0 && child->pwd_table.nentries == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Neither iron password nor iron password table configured");
       	    return NGX_CONF_ERROR;
        }
        /* Check that explicit port is a number value
         * (nginx int value support is not used for easier use of Hawk libs)
         */
        if(child->port.len > 0) {
            if( ! is_digits_only(&(child->port))) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%V is not a valid port number" , &(child->port));
       	        return NGX_CONF_ERROR;
       	    }
        }
    }
    return NGX_CONF_OK;
}



/*
 * The actual handler - this is called during access phase.
 *
 * What we do here is to parse the authorization header,
 * validate the Hawk signature and then check access
 * grant using the sealed ticket provided as the Hawk ID.
 *
 * If authentication and authorization succeeds, we strip the
 * authorization header from the request to enable caching
 *
 * NGINX does not support request header removal, so instead
 * we just rename the header.
 * (Removal is next to impossible because headers are stored
 * in arrays and removing a header would invalidate pointers
 * to it, held by various other portions of the processed
 * request)
 */
static ngx_int_t ngx_dlg_auth_handler(ngx_http_request_t *r) {
    ngx_http_dlg_auth_loc_conf_t  *conf;
    ngx_int_t rc;
    ngx_http_dlg_auth_ctx_t *ctx;

    /*
     * Allocate and store our per request context (used to
     * store the data to be made accessible as variable values).
     */
    if( (ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_dlg_auth_ctx_t))) == NULL) {
    	return NGX_ERROR;
    }
    ngx_http_set_ctx(r, ctx, nginx_dlg_auth_module);

    /*
     * First, get the configuration and check whether we apply to
     * the current location.
     */
    conf = ngx_http_get_module_loc_conf(r, nginx_dlg_auth_module);
    if (conf->realm.data == NULL) {
        return NGX_DECLINED;
    }

    /*
     * User can disable ourselves by setting the realm to 'off'. This includes
     * terminating inheritance.
     * But see https://github.com/algermissen/nginx-dlg-auth/issues/14
     */
    if (conf->realm.len == 3 && ngx_strncmp(conf->realm.data, "off", 3) == 0) {
        return NGX_DECLINED;
    }

    /*
     * Authorization header presence is required, of course.
     */

    if (r->headers_in.authorization == NULL) {
    	return ngx_dlg_auth_send_simple_401(r,&(conf->realm));
    }

    /*
     * Authenticate and authorize and 'remove' (rename) authorization header if ok.
     */
    if( (rc =  ngx_dlg_auth_authenticate(r,conf,ctx)) != NGX_OK) {
    	return rc;
    }
    ngx_dlg_auth_rename_authorization_header(r);

    return NGX_OK;
}

/*
 * This is the heart of the module, where authentication and authorization
 * takes place.
 *
 */
static ngx_int_t ngx_dlg_auth_authenticate(ngx_http_request_t *r, ngx_http_dlg_auth_loc_conf_t *conf,ngx_http_dlg_auth_ctx_t *ctx) {

	/*
	 * Variables necessary for Hawk.
	 */
	HawkcError he;
	struct HawkcContext hawkc_ctx;
	int hmac_is_valid;
	ngx_str_t host;
	ngx_str_t port;

	/*
	 * Variables necessary for ciron.
	 */
    struct CironContext ciron_ctx;
	CironError ce;
	CironOptions encryption_options = CIRON_DEFAULT_ENCRYPTION_OPTIONS;
    CironOptions integrity_options = CIRON_DEFAULT_INTEGRITY_OPTIONS;
	unsigned char encryption_buffer[ENCRYPTION_BUFFER_SIZE];
	unsigned char output_buffer[OUTPUT_BUFFER_SIZE];
	int check_len;
	int output_len;

	/*
	 * Ticket processing and authorization checking.
	 */
	TicketError te;
	struct Ticket ticket;
	time_t now;
	time_t clock_skew;

    /*
     * Determine the host and port values to be used for signature validation.
     */
	determine_host_and_port(conf,r,&host,&port);

	/*
	 * Initialize Hawkc context with original request data
	 */
	hawkc_context_init(&hawkc_ctx);
	hawkc_context_set_method(&hawkc_ctx,r->method_name.data, r->method_name.len);
	hawkc_context_set_path(&hawkc_ctx,r->unparsed_uri.data, r->unparsed_uri.len);
	hawkc_context_set_host(&hawkc_ctx,host.data,host.len);
	hawkc_context_set_port(&hawkc_ctx,port.data,port.len);

	/*
	 * Parse Hawk Authorization header.
	 */
	if( (he = hawkc_parse_authorization_header(&hawkc_ctx,r->headers_in.authorization->value.data, r->headers_in.authorization->value.len)) != HAWKC_OK) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to parse Authorization header %V, reason: %s" ,&(r->headers_in.authorization->value), hawkc_get_error(&hawkc_ctx));
		if(he == HAWKC_BAD_SCHEME_ERROR) {
			return ngx_dlg_auth_send_simple_401(r,&(conf->realm));
		}
		if(he == HAWKC_PARSE_ERROR) {
			return NGX_HTTP_BAD_REQUEST;
		}
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	/*
	 * ciron requires the caller to provide buffers for the decryption process
	 * and the unsealed result. We are providing static buffers, but still need
	 * to check the size. If the static buffers are not enough, we have
	 * received an invalid ticket anyway.
	 *
	 * Using static buffers makes sense here, because we know the aprox. token length
	 * in advance - we assume a fixed max. number of scopes. See definiton
	 * of ENCRYPTION_BUFFER_SIZE and OUTPUT_BUFFER_SIZE for how the size
	 * is estimated.
	 */

	if( (check_len = (int)ciron_calculate_encryption_buffer_length(encryption_options, hawkc_ctx.header_in.id.len)) > (int)sizeof(encryption_buffer)) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Required encryption buffer length %d too big. This might indicate an attack",
				check_len);
		return NGX_HTTP_BAD_REQUEST;
	}
    /* FIXME The last 0 is an issue with ciron: We won't know the password_id before unsealsing. but we need buffer size before.
     * Suggested FIX: ignore the passwordID on unsealing - hen this buffer length will always be passwordId.len too long.
     * That is not a problem! Hence we pass 0.
     * See https://github.com/algermissen/ciron/issues/15
     */
	if( (check_len = (int)ciron_calculate_unseal_buffer_length(encryption_options, integrity_options,hawkc_ctx.header_in.id.len,0)) > (int)sizeof(output_buffer)) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Required output buffer length %d too big. This might indicate an attack",
					check_len);
			return NGX_HTTP_BAD_REQUEST;
	}

	/*
	 * The sealed ticket is the Hawk id parameter. We unseal it, parse the ticket JSON
	 * and extract password and algorithm to validate the Hawk signature.
	 */
	if( (ce =ciron_unseal(&ciron_ctx,hawkc_ctx.header_in.id.data, hawkc_ctx.header_in.id.len, &(conf->pwd_table),conf->iron_password.data, conf->iron_password.len,
			encryption_options, integrity_options, encryption_buffer, output_buffer, &output_len)) != CIRON_OK) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to unseal ticket: %s" , ciron_get_error(&ciron_ctx));
			return NGX_HTTP_BAD_REQUEST;
	}
	if( (te = ticket_from_string(&ticket , (char*)output_buffer,output_len)) != OK) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to parse ticket JSON, %s" , ticket_strerror(te));
		return NGX_HTTP_BAD_REQUEST;
	}

	if(store_client(r,ctx,&ticket) != NGX_OK ) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to store client variable, storage function returned error");
		// We can still serve the request despite this error, so no error return
	}

	if(store_expires(r,ctx,&ticket) != NGX_OK ) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to store expires variable, storage function returned error");
		// We can still serve the request despite this error, so no error return
	}

	/*
	 * Now we can take password and algorithm from ticket and store them in Hawkc context.
	 */

	hawkc_context_set_password(&hawkc_ctx,ticket.pwd.data,ticket.pwd.len);
	hawkc_context_set_algorithm(&hawkc_ctx,ticket.hawkAlgorithm);

	/*
	 * Validate the HMAC signature of the request.
	 */

	if( (he = hawkc_validate_hmac(&hawkc_ctx, &hmac_is_valid)) != HAWKC_OK) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to validate request signature: %s" , hawkc_get_error(&hawkc_ctx));
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	if(!hmac_is_valid) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Invalid signature in %V" ,&(r->headers_in.authorization->value) );
		return ngx_dlg_auth_send_simple_401(r,&(conf->realm));
	}

	time(&now);
	clock_skew = now - hawkc_ctx.header_in.ts;
	if(store_clockskew(r,ctx,clock_skew) != NGX_OK) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to store clock_skew variable, storage function returned error");
		// We can still serve the request, so no error return
	}

	/*
	 * Check request timestamp, allowing for some skew.
	 * If the client's clock differs to much from the server's clock, we send the client a 401
	 * and our current time so it understands the offset and can send the request again.
	 */
	if(abs(clock_skew) > (time_t)(conf->allowed_clock_skew)) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Clock skew too large mine: %d, got %d ,skew is %d" , now , hawkc_ctx.header_in.ts,
				clock_skew);
		hawkc_www_authenticate_header_set_ts(&hawkc_ctx,now);
		return ngx_dlg_auth_send_401(r, &hawkc_ctx);
	}

	/* FIXME Check nonce, see https://github.com/algermissen/nginx-dlg-auth/issues/1 */

	/*
	 * Now the request has been authenticated by way of Hawk and we use the ticket
	 * itself to check access rights.
	 */

	/*
	 * Tickets contain a parameter rw which has to be set to true to grant
	 * access using unsafe HTTP methods.
	 */
	if(IS_UNSAFE_METHOD(r->method)) {
		if(ticket.rw == 0) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Ticket does not represent grant for unsafe methods");
			return NGX_HTTP_FORBIDDEN;
		}
	}

	/*
	 * Check whether ticket has expired.
	 */
	if(ticket.exp < now) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Ticket has expired");
		/* FIXME: probably set defined error code in auth header. This is a todo for the overall auth delegation (e.g. Oz) */
		return ngx_dlg_auth_send_simple_401(r,&(conf->realm));

	}

	/*
	 * Now we check whether the ticket applies to the necessary scope.
	 */
	if(!ticket_has_scope(&ticket,host.data, host.len,conf->realm.data,conf->realm.len)) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Ticket does not represent grant for access to scope %V" ,&(conf->realm) );
		return ngx_dlg_auth_send_simple_401(r,&(conf->realm));
	}

	return NGX_OK;
}

/*
 * Removing request headers is next to impossible in NGINX because
 * they come as an array. Removing would invalidate various pointers
 * held by other parts of the request struct. This is way too error
 * prone, so renaming the headers seems like the better solution to
 * make the upstream response cacheable.
 * We rename be changing the first two characters to 'x-', thus
 * Authorization will be passed as X-thorization.
 */
static void ngx_dlg_auth_rename_authorization_header(ngx_http_request_t *r) {
	ngx_uint_t nelts;
	size_t size;
	unsigned int i;

	/*
	 * Headers come as a list which we have to iterate over to find
	 * the appropriate bucket.
	 * FIXME: I think we can achieve the same by simply using the authorization pointer
	 * and then setting it to NULL.
	 */

	nelts = r->headers_in.headers.part.nelts;
	size = r->headers_in.headers.size;

  	for(i=0; i < nelts; i++) {
   		void *elt;
   		ngx_table_elt_t *data;
   		elt = ((char*)r->headers_in.headers.part.elts) + (i * size); /* FIXME warning void* in arithm. */
   		data = (ngx_table_elt_t*)elt;

   		if(data->key.len == 13 && memcmp(data->lowcase_key,"authorization",13) == 0) {
   			memcpy(data->key.data,"X-",2);
   			memcpy(data->lowcase_key,"x-",2);
   			r->headers_in.authorization = NULL;
   			break;
   		}
   	}
}

/*
 * Send a simple Hawk 401 response.
 * This simply adds a WWW-Authenticate: Hawk <realm> header and responds with 401.
 */
static ngx_int_t ngx_dlg_auth_send_simple_401(ngx_http_request_t *r, ngx_str_t *realm) {
    	ngx_str_t challenge;
    	unsigned char *p;
    	/*
    	 * Add new header.
    	 */
    	if( (r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers)) == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->headers_out.www_authenticate->hash = 1;
        ngx_str_set(&r->headers_out.www_authenticate->key, "WWW-Authenticate");

        challenge.len = 13 + realm->len;
        if( (challenge.data = ngx_pnalloc(r->pool, challenge.len)) == NULL) {
          return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        p = ngx_cpymem(challenge.data, (unsigned char*)"Hawk realm=\"",12);
        p = ngx_cpymem(p, realm->data, realm->len);
        p = ngx_cpymem(p, (unsigned char*)"\"", 1);

        r->headers_out.www_authenticate->value = challenge;
        return NGX_HTTP_UNAUTHORIZED;
}

/*
 * This implements returning a 401 response using the supplied HawkcContext to construct
 * the WWW-Authenticate header.
 */
static ngx_int_t ngx_dlg_auth_send_401(ngx_http_request_t *r, HawkcContext hawkc_ctx) {
		HawkcError e;
    	ngx_str_t challenge;
    	size_t n,check_n;

 		if( (e = hawkc_calculate_www_authenticate_header_length(hawkc_ctx,&n)) != HAWKC_OK) {
    		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Error when calculating authentication header length, %s" ,
    				hawkc_get_error(hawkc_ctx));
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
 		}

    	/*
    	 * Add new header.
    	 */
    	if( (r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers)) == NULL) {
    		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to add new header, ngx_list_push returned NULL");

            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->headers_out.www_authenticate->hash = 1;
        ngx_str_set(&r->headers_out.www_authenticate->key, "WWW-Authenticate");

        challenge.len = n;
        if( (challenge.data = ngx_pnalloc(r->pool, challenge.len)) == NULL) {
       	  ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to allocate space for new header");
          return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

 		if( (e = hawkc_create_www_authenticate_header(hawkc_ctx, challenge.data,&check_n)) != HAWKC_OK) {
   			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to create WWW-Authenticate header with timestamp, %s" ,
   					hawkc_get_error(hawkc_ctx));
 			return NGX_HTTP_INTERNAL_SERVER_ERROR;
 		}
 		/*
        p = ngx_cpymem(challenge.data, (unsigned char*)"Hawk realm=\"",12);
        p = ngx_cpymem(p, realm->data, realm->len);
        p = ngx_cpymem(p, (unsigned char*)"\"", 1);
        */
 		if(check_n != n) {
       	  ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "check_n != n");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
 		}

        r->headers_out.www_authenticate->value = challenge;

        return NGX_HTTP_UNAUTHORIZED;
}


/*
 * Determine the host and port to use for request signature validation.
 */
static void determine_host_and_port(ngx_http_dlg_auth_loc_conf_t *conf,
                            ngx_http_request_t *r,ngx_str_t *host, ngx_str_t *port) {
    ngx_str_t request_host;
    ngx_str_t request_port;
    /*
     * Initialize, so that we can safely check xxx->len for 0.
     */
    host->len = 0;
    host->data = NULL;
    port->len = 0;
    port->data = NULL;

    /*
     * Handle any explicitly set values for host or port.
     */
    if(conf->host.len != 0) {
        host->data = conf->host.data;
        host->len = conf->host.len;
    }
    if(conf->port.len != 0) {
        host->data = conf->port.data;
        host->len = conf->port.len;
    }


    /*
     * If host and port have both been explicitly set in configuration, we are done at
     * this point.
     */
    if(host->len != 0 && port->len != 0) {
        return;
    }

    /*
     * Maybe add support for X-Forwarded-Host & freinds.
     * See https://github.com/algermissen/nginx-dlg-auth/issues/12
     */

    /*
     * Extract host and port from request.
     */
    get_host_and_port(r->headers_in.host->value,&request_host,&request_port);

    /*
     * If host or port has not been explicitly set in configuration file or has been
     * determined otherwise (e.g. X-Forwarded-Host & friends) finally use the
     * host and/or port from the request.
     */
    if(host->len != 0) {
        host->data = request_host.data;
        host->len = request_host.len;
    }

    if(port->len != 0) {
        port->data = request_port.data;
        port->len = request_port.len;
    }



}


/*
 * Obtain original host and port from HTTP Host header value.
 *
 * Callers need to check the port len themselves and if it is 0
 * they are responsible for setting the default port.
 */
static void get_host_and_port(ngx_str_t host_header, ngx_str_t *host, ngx_str_t *port) {
    u_char *p;
    unsigned int i;
    port->len = 0;
    p = host_header.data;
    /* Extract host */
    host->data = p;
    i=0;
    while(i < host_header.len && *p != ':') {
        p++;
        i++;
    }
	host->len = i;
	/* If we found delimiter and still have stuff to read, process port. */
	if(*p == ':' && i+1<host_header.len) {
		p++;
		i++;
		port->data = p;
		while(i < host_header.len) {
			p++;
			i++;
			port->len++;
		}
	}

    /*
     * If there is no request port given in the Host header, use the URI scheme specific
     * default port.
     */
     if(port->len == 0) {
#if (NGX_HTTP_SSL)
        if(r->connection->ssl) {
            port->data = (u_char *)"443";
            port->len = 3;
        } else {
#endif
                port->data = (u_char *)"80";
                port->len = 2;
#if (NGX_HTTP_SSL)
        }
#endif
    }

}


/*
 * Store the requesting client name in a variable.
 */
ngx_int_t store_client(ngx_http_request_t *r, ngx_http_dlg_auth_ctx_t *ctx,Ticket ticket) {

	 if( (ctx->client.data = ngx_pcalloc(r->pool, ticket->client.len)) == NULL) {
		 return NGX_ERROR;
	 }
	 memcpy(ctx->client.data,ticket->client.data,ticket->client.len);
	 ctx->client.len = ticket->client.len;
	 return NGX_OK;
}

/*
 * Store the expiry seconds of the ticket in a variable.
 */
ngx_int_t store_expires(ngx_http_request_t *r, ngx_http_dlg_auth_ctx_t *ctx,Ticket ticket) {

	 /* 20 bytes is plenty for time_t value */
	 if( (ctx->expires.data = ngx_pcalloc(r->pool, 20)) == NULL) {
		 return NGX_ERROR;
	 }
	 ctx->expires.len = hawkc_ttoa(ctx->expires.data,ticket->exp);
	 return NGX_OK;
}

/*
 * Store the clock skew in a variable.
 */
ngx_int_t store_clockskew(ngx_http_request_t *r, ngx_http_dlg_auth_ctx_t *ctx, time_t clockskew) {

	 /* 20 bytes is plenty for time_t value */
	 if( (ctx->clockskew.data = ngx_pcalloc(r->pool, 20)) == NULL) {
		 return NGX_ERROR;
	 }
	 ctx->clockskew.len = hawkc_ttoa(ctx->clockskew.data,clockskew);
	 return NGX_OK;
}

/*
 * Check whether a string represents a number greater or equal to 0.
 * Returns 1 if string is number greater or equal to 0, 0 otherwise.
 */
static int is_digits_only(ngx_str_t *str) {
    int i=0;
    while(i < str->len ) {
        char c = str->data[i];
        if( c < '0' || c > '9') {
            return 0;
        }
        i++;
    }
    return 1;
}


