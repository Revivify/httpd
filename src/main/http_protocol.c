/* ====================================================================
 * Copyright (c) 1995-1997 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission.
 *
 * 5. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 */
  
/*
 * http_protocol.c --- routines which directly communicate with the client.
 *
 * Code originally by Rob McCool; much redone by Robert S. Thau
 * and the Apache Group.
 */

#define CORE_PRIVATE
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_main.h"
#include "http_log.h"		/* For errors detected in basic auth
				 * common support code...
				 */
#include "util_date.h"          /* For parseHTTPdate and BAD_DATE */
#include <stdarg.h>

#define SET_BYTES_SENT(r) \
  do { if (r->sent_bodyct) \
	  bgetopt (r->connection->client, BO_BYTECT, &r->bytes_sent); \
  } while (0)


static int parse_byterange (char *range, long clength, long *start, long *end)
{
    char *dash = strchr(range, '-');

    if (!dash)
	return 0;

    if ((dash == range)) {
	/* In the form "-5" */
	*start = clength - atol(dash + 1);
	*end = clength - 1;
    }
    else {
	*dash = '\0';
	dash++;
	*start = atol(range);
	if (*dash)
	    *end = atol(dash);
	else	/* "5-" */
	    *end = clength -1;
    }

    if (*start > *end)
	return 0;

    if (*end >= clength)
	*end = clength - 1;

    return 1;
}

static int internal_byterange(int, long*, request_rec*, char**, long*, long*);

int set_byterange (request_rec *r)
{
    char *range = table_get (r->headers_in, "Range");
    char *if_range = table_get (r->headers_in, "If-Range");
    char ts[MAX_STRING_LEN], *match;
    long range_start, range_end;

    /* Also check, for backwards-compatibility with second-draft
     * Luotonen/Franks byte-ranges (e.g. Netscape Navigator 2-3)
     *
     * We support this form, with Request-Range, and (farther down) we
     * send multipart/x-byteranges instead of multipart/byteranges for
     * Request-Range based requests to work around a bug in Netscape
     * Navigator 2 and 3.
     */

    if (!range) range = table_get (r->headers_in, "Request-Range");

    /* Reasons we won't do ranges... */

    if (!r->clength || r->assbackwards) return 0;
    if (!range || strncmp(range, "bytes=", 6)) {
	table_set (r->headers_out, "Accept-Ranges", "bytes");
	return 0;
    }

    /* Check the If-Range header. Golly, this is a long if statement */

    if (if_range
	&& !((if_range[0] == '"') /* an entity tag */
	     && (match = table_get(r->headers_out, "Etag"))
	     && (match[0] == '"') && !strcasecmp(if_range, match))
	&& !((if_range[0] != '"') /* a date */
	     && (match = table_get(r->headers_out, "Last-Modified"))
	     && (!strcasecmp(if_range, match))))
	return 0;
    
    if (!strchr(range, ',')) {
	/* A single range */
	if (!parse_byterange(pstrdup(r->pool, range + 6), r->clength,
			     &range_start, &range_end))
	    return 0;

	r->byterange = 1;

	ap_snprintf(ts, sizeof(ts), "bytes %ld-%ld/%ld", range_start, range_end,
		r->clength);
	table_set(r->headers_out, "Content-Range",
		  pstrdup(r->pool, ts));
	ap_snprintf(ts, sizeof(ts), "%ld", range_end - range_start + 1);
	table_set(r->headers_out, "Content-Length", ts);
    }
    else {
	/* a multiple range */
	char boundary[33];	/* Long enough */
	char *r_range = pstrdup(r->pool, range + 6);
	long tlength = 0;
	
	r->byterange = 2;
	ap_snprintf(boundary, sizeof(boundary), "%lx%lx", r->request_time, (long)getpid());
	r->boundary = pstrdup(r->pool, boundary);
	while (internal_byterange(0, &tlength, r, &r_range, NULL, NULL));
	ap_snprintf(ts, sizeof(ts), "%ld", tlength);
	table_set(r->headers_out, "Content-Length", ts);
    }
    
    r->status = PARTIAL_CONTENT;
    r->range = range + 6;

    return 1;
}

int each_byterange (request_rec *r, long *offset, long *length) {
    return internal_byterange(1, NULL, r, &r->range, offset, length);
}

/* If this function is called with realreq=1, it will spit out
 * the correct headers for a byterange chunk, and set offset and
 * length to the positions they should be.
 *
 * If it is called with realreq=0, it will add to tlength the length
 * it *would* have used with realreq=1.
 *
 * Either case will return 1 if it should be called again, and 0
 * when done.
 *
 */

static int internal_byterange(int realreq, long *tlength, request_rec *r,
			      char **r_range, long *offset, long *length) {
    long range_start, range_end;
    char *range;

    if (!**r_range) {
	if (r->byterange > 1) {
	    if (realreq)
		rvputs(r, "\015\012--", r->boundary, "--\015\012", NULL);
	    else
		*tlength += 4 + strlen(r->boundary) + 4;
	}
	return 0;
    }

    range = getword_nc(r->pool, r_range, ',');
    if (!parse_byterange(range, r->clength, &range_start, &range_end))
	/* Skip this one */
	return internal_byterange(realreq, tlength, r, r_range, offset,
				  length);

    if (r->byterange > 1) {
	char *ct = r->content_type ? r->content_type : default_type(r);
	char ts[MAX_STRING_LEN];
	
	ap_snprintf(ts, sizeof(ts), "%ld-%ld/%ld", range_start, range_end, r->clength);
	if (realreq)
	    rvputs(r, "\015\012--", r->boundary, "\015\012Content-type: ",
	       ct, "\015\012Content-range: bytes ", ts, "\015\012\015\012",
	       NULL);
	else
	    *tlength += 4 + strlen(r->boundary) + 16 + strlen(ct) + 23 +
		strlen(ts) + 4;
    }

    if (realreq) {
	*offset = range_start;
	*length = range_end - range_start + 1;
    }
    else {
	*tlength += range_end - range_start + 1;
    }
    return 1;
}

int set_content_length (request_rec *r, long clength)
{
    char ts[MAX_STRING_LEN];

    r->clength = clength;

    ap_snprintf (ts, sizeof(ts), "%ld", clength);
    table_set (r->headers_out, "Content-Length", pstrdup (r->pool, ts));

    return 0;
}

int set_keepalive(request_rec *r)
{
    char *conn   = table_get(r->headers_in,  "Connection");
    char *length = table_get(r->headers_out, "Content-Length");
    char *tenc   = table_get(r->headers_out, "Transfer-Encoding");
    int ka_sent;

    if (r->connection->keepalive == -1)  /* Did we get bad input? */
        r->connection->keepalive = 0;
    else if (r->server->keep_alive && (!r->server->keep_alive_max ||
	(r->server->keep_alive_max > r->connection->keepalives)) &&
	(r->server->keep_alive_timeout > 0) &&
	(r->status == HTTP_NOT_MODIFIED || r->status == HTTP_NO_CONTENT
	 || r->header_only || length || tenc ||
	 ((r->proto_num >= 1001) && (r->chunked = 1))) &&
	(!find_token(r->pool, conn, "close")) &&
	((ka_sent = find_token(r->pool, conn, "keep-alive")) ||
	 r->proto_num >= 1001)) {
	/*
	 * The (r->chunked = 1) in the above expression is a side-effect
	 * that sets the output to chunked encoding if it is not already
	 * length-delimited.  It is not a bug, though it is annoying.
	 */
	char header[256];
	int left = r->server->keep_alive_max - r->connection->keepalives;
	
	r->connection->keepalive = 1;
	r->connection->keepalives++;
	
	/* If they sent a Keep-Alive token, send one back */
	if (ka_sent) {
	    if (r->server->keep_alive_max)
		ap_snprintf(header, sizeof(header), "timeout=%d, max=%d",
			    r->server->keep_alive_timeout, left);
	    else
		ap_snprintf(header, sizeof(header), "timeout=%d",
			    r->server->keep_alive_timeout);
	    rputs("Connection: Keep-Alive\015\012", r);
	    rvputs(r, "Keep-Alive: ", header, "\015\012", NULL);
	}

	return 1;
    }

    /* We only really need to send this to HTTP/1.1 clients, but we
     * always send it anyway, because a broken proxy may identify itself
     * as HTTP/1.0, but pass our request along with our HTTP/1.1 tag
     * to a HTTP/1.1 client. Better safe than sorry.
     */
    rputs("Connection: close\015\012", r);

    return 0;
}

int set_last_modified(request_rec *r, time_t mtime)
{
    char *etag, weak_etag[MAX_STRING_LEN];
    char *if_modified_since = table_get(r->headers_in, "If-Modified-Since");
    char *if_unmodified     = table_get(r->headers_in, "If-Unmodified-Since");
    char *if_nonematch      = table_get(r->headers_in, "If-None-Match");
    char *if_match          = table_get(r->headers_in, "If-Match");
    time_t now = time(NULL);

    if (now < 0)
        now = r->request_time;

    table_set(r->headers_out, "Last-Modified",
              gm_timestr_822(r->pool, (mtime > now) ? now : mtime));

    /* Make an ETag header out of various pieces of information. We use
     * the last-modified date and, if we have a real file, the
     * length and inode number - note that this doesn't have to match
     * the content-length (i.e. includes), it just has to be unique
     * for the file.
     *
     * If the request was made within a second of the last-modified date,
     * we send a weak tag instead of a strong one, since it could
     * be modified again later in the second, and the validation
     * would be incorrect.
     */

    if (r->finfo.st_mode != 0)
        ap_snprintf(weak_etag, sizeof(weak_etag), "W/\"%lx-%lx-%lx\"", 
		(unsigned long)r->finfo.st_ino,
		(unsigned long)r->finfo.st_size, (unsigned long)mtime);
    else
        ap_snprintf(weak_etag, sizeof(weak_etag), "W/\"%lx\"",
		(unsigned long)mtime);

    etag = weak_etag + ((r->request_time - mtime > 1) ? 2 : 0);
    table_set (r->headers_out, "ETag", etag);

    /* Check for conditional requests --- note that we only want to do
     * this if we are successful so far and we are not processing a
     * subrequest or an ErrorDocument.
     *
     * The order of the checks is important, since etag checks are supposed
     * to be more accurate than checks relative to the modification time.
     */
    
    if (!is_HTTP_SUCCESS(r->status) || r->no_local_copy)
        return OK;

    /* If an If-Match request-header field was given and
     * if our ETag does not match any of the entity tags in that field
     * and the field value is not "*" (meaning match anything), then
     *    respond with a status of 412 (Precondition Failed).
     */

    if (if_match) {
        if ((if_match[0] != '*') && !find_token(r->pool, if_match, etag))
            return HTTP_PRECONDITION_FAILED;
    }

    /* Else if a valid If-Unmodified-Since request-header field was given
     * and the requested resource has been modified since the time
     * specified in this field, then the server MUST
     *    respond with a status of 412 (Precondition Failed).
     */

    else if (if_unmodified) {
        time_t ius = parseHTTPdate(if_unmodified);

        if ((ius != BAD_DATE) && (mtime > ius))
            return HTTP_PRECONDITION_FAILED;
    }

    /* If an If-None-Match request-header field was given and
     * if our ETag matches any of the entity tags in that field or
     * if the field value is "*" (meaning match anything), then
     *    if the request method was GET or HEAD, the server SHOULD
     *       respond with a 304 (Not Modified) response.
     *    For all other request methods, the server MUST
     *       respond with a status of 412 (Precondition Failed).
     */

    if (if_nonematch) {
        if ((if_nonematch[0] == '*') || find_token(r->pool,if_nonematch,etag))
            return (r->method_number == M_GET) ? HTTP_NOT_MODIFIED
                                               : HTTP_PRECONDITION_FAILED;
    }

    /* Else if a valid If-Modified-Since request-header field was given
     * and it is a GET or HEAD request
     * and the requested resource has not been modified since the time
     * specified in this field, then the server MUST
     *    respond with a status of 304 (Not Modified).
     * A date later than the server's current request time is invalid.
     */

    else if (if_modified_since && (r->method_number == M_GET)) {
        time_t ims = parseHTTPdate(if_modified_since);

        if ((ims >= mtime) && (ims <= r->request_time))
            return HTTP_NOT_MODIFIED;
    }

    return OK;
}

/* Get a line of protocol input, including any continuation lines
 * caused by MIME folding (or broken clients) if fold != 0, and place it
 * in the buffer s, of size n bytes, without the ending newline.
 *
 * Returns -1 on error, or the length of s.
 *
 * Note: Because bgets uses 1 char for newline and 1 char for NUL,
 *       the most we can get is (n - 2) actual characters if it
 *       was ended by a newline, or (n - 1) characters if the line
 *       length exceeded (n - 1).  So, if the result == (n - 1),
 *       then the actual input line exceeded the buffer length,
 *       and it would be a good idea for the caller to puke 400 or 414.
 */
static int getline(char *s, int n, BUFF *in, int fold)
{
    char *pos, next;
    int retval;
    int total = 0;

    pos = s;

    do {
        retval = bgets(pos, n, in);    /* retval == -1 if error, 0 if EOF */

        if (retval <= 0)
            return ((retval < 0) && (total == 0)) ? -1 : total;

        /* retval is the number of characters read, not including NUL     */

        n     -= retval;      /* Keep track of how much of s is full      */
        pos   += (retval-1);  /*               and where s ends           */
        total += retval;      /*               and how long s has become  */

        if (*pos == '\n') {   /* Did we get a full line of input?         */
            *pos = '\0';
            --total; ++n;
        }
        else return total;    /* if not, input line exceeded buffer size  */

    /* Continue appending if line folding is desired and
     * the last line was not empty and we have room in the buffer and
     * the next line begins with a continuation character.
     */
    } while (fold && (retval != 1) && (n > 1) &&
             (blookc(&next, in) == 1) &&
             ((next == ' ') || (next == '\t')));

    return total;
}

void parse_uri (request_rec *r, const char *uri)
{
    const char *s;

#ifdef __EMX__
    /* Variable for OS/2 fix below. */
    int loop;
#endif

/* A proxy request contains a ':' early on, but not as first character */
    for (s=uri; s != '\0'; s++)
	if (!isalnum(*s) && *s != '+' && *s != '-' && *s != '.') break;

    if (*s == ':' && s != uri)
    {
	r->proxyreq = 1;
	r->uri = pstrdup(r->pool, uri);
	r->args = NULL;
    }
    else if (r->method && !strcmp(r->method, "TRACE")) {
	r->proxyreq = 0;
	r->uri = pstrdup(r->pool, uri);
	r->args = NULL;
    }
    else {
	r->proxyreq = 0;
	r->uri = getword (r->pool, &uri, '?');

#ifdef __EMX__
    /* Handle path translations for OS/2 and plug security hole. */
    /* This will prevent "http://www.wherever.com/..\..\/" from
       returning a directory for the root drive. */
    for (loop = 0; loop <= strlen(r->uri); ++loop) {
        if (r->uri[loop] == '\\')
            r->uri[loop] = '/';
    };
    
    /* Fix OS/2 HPFS filename case problem. */
    r->uri = strlwr(r->uri);
#endif

	if (*uri) r->args= pstrdup(r->pool, uri);
	else r->args = NULL;
    }
}

const char *check_fulluri (request_rec *r, const char *uri) {
  char *name, *host;
  int i, port;

  /* This routine parses full URLs, if they match the server */
  if (strncmp(uri, "http://", 7)) return uri;
  name = pstrdup(r->pool, uri + 7);
  
  /* Find the hostname, assuming a valid request */
  i = ind(name, '/');
  name[i] = '\0';

  /* Find the port */
  host = getword_nc(r->pool, &name, ':');
  if (*name) port = atoi(name);
  else port = 80;

  /* Make sure ports patch */
  if (port != r->server->port) return uri;

  /* Save it for later use */
  r->hostname = pstrdup(r->pool, host);
  r->hostlen = 7 + i;

  /* The easy cases first */
  if (!strcasecmp(host, r->server->server_hostname)) {
    return (uri + r->hostlen);
  }
  else if (!strcmp(host, inet_ntoa(r->connection->local_addr.sin_addr))) {
    return (uri + r->hostlen);
  }

  /* Now things get a bit trickier - check the IP address(es) of the host */
  /* they gave, see if it matches ours.                                   */
  else {
    struct hostent *hp;
    int n;

    if ((hp = gethostbyname(host))) {
      for (n = 0; hp->h_addr_list[n] != NULL; n++) {
	if (r->connection->local_addr.sin_addr.s_addr ==
	    (((struct in_addr *)(hp->h_addr_list[n]))->s_addr)) {
	  return (uri + r->hostlen);
	}
      }
    }
  }
  
  return uri;
}

int read_request_line (request_rec *r)
{
    char l[HUGE_STRING_LEN];
    const char *ll = l, *uri;
    conn_rec *conn = r->connection;
    int major = 1, minor = 0;	/* Assume HTTP/1.0 if non-"HTTP" protocol*/
    int len;
    
    /* Read past empty lines until we get a real request line,
     * a read error, the connection closes (EOF), or we timeout.
     *
     * We skip empty lines because browsers have to tack a CRLF on to the end
     * of POSTs to support old CERN webservers.  But note that we may not
     * have flushed any previous response completely to the client yet.
     * We delay the flush as long as possible so that we can improve
     * performance for clients that are pipelining requests.  If a request
     * is pipelined then we won't block during the (implicit) read() below.
     * If the requests aren't pipelined, then the client is still waiting
     * for the final buffer flush from us, and we will block in the implicit
     * read().  B_SAFEREAD ensures that the BUFF layer flushes if it will
     * have to block during a read.
     */
    bsetflag( conn->client, B_SAFEREAD, 1 );
    while ((len = getline(l, HUGE_STRING_LEN, conn->client, 0)) <= 0) {
        if ((len < 0) || bgetflag(conn->client, B_EOF)) {
	    bsetflag( conn->client, B_SAFEREAD, 0 );
            return 0;
	}
    }
    bsetflag( conn->client, B_SAFEREAD, 0 );
    if (len == (HUGE_STRING_LEN - 1))
        return 0;               /* Should be a 414 error status instead */

    r->request_time = time(NULL);
    r->the_request = pstrdup (r->pool, l);
    r->method = getword_white(r->pool, &ll);
    uri = getword_white(r->pool, &ll);
    uri = check_fulluri(r, uri);
    parse_uri (r, uri);
    
    r->assbackwards = (ll[0] == '\0');
    r->protocol = pstrdup (r->pool, ll[0] ? ll : "HTTP/0.9");
    sscanf(r->protocol, "HTTP/%d.%d", &major, &minor);
    r->proto_num = 1000*major + minor;

    return 1;
}

void get_mime_headers (request_rec *r)
{
    conn_rec *c = r->connection;
    int len;
    char *value;
    char field[MAX_STRING_LEN];

    /* Read header lines until we get the empty separator line,
     * a read error, the connection closes (EOF), or we timeout.
     * Should we also check for overflow (len == MAX_STRING_LEN-1)?
     */
    while ((len = getline(field, MAX_STRING_LEN, c->client, 1)) > 0) {

        if (!(value = strchr(field,':')))     /* Find the colon separator */
            continue;                         /*  or should puke 400 here */

        *value = '\0';
        ++value;
        while (isspace(*value)) ++value;      /* Skip to start of value   */

        table_merge(r->headers_in, field, value);
    }
}

static void check_hostalias (request_rec *r) {
  const char *hostname=r->hostname;
  char *host = getword(r->pool, &hostname, ':');	/* Get rid of port */
  int port = (*hostname) ? atoi(hostname) : 80;
  server_rec *s;
  int l;

  if (port && (port != r->server->port))
    return;

  l = strlen(host)-1;
  if ((host[l]) == '.') {
    host[l] = '\0';
  }

  r->hostname = host;

  for (s = r->server->next; s; s = s->next) {
    const char *names;
    server_addr_rec *sar;

    if ((!strcasecmp(host, s->server_hostname)) && (port == s->port)) {
      r->server = r->connection->server = s;
      if (r->hostlen && !strncmp(r->uri, "http://", 7)) {
	r->uri += r->hostlen;
	parse_uri(r, r->uri);
      }
    }

    /* search all the names from <VirtualHost> directive */
    for( sar = s->addrs; sar; sar = sar->next ) {
      if( !strcasecmp( sar->virthost, host ) ) {
	r->server = r->connection->server = s;
	if( r->hostlen && !strncmp( r->uri, "http://", 7) ) {
	  r->uri += r->hostlen;
	  r->proxyreq = 0;
	}
      }
    }

    /* search all the aliases from ServerAlias directive */
    names = s->names;
    if( names ) {
      while (*names) {
	char *name = getword_conf (r->pool, &names);

	if ((is_matchexp(name) && !strcasecmp_match(host, name)) ||
	    (!strcasecmp(host, name))) {
	  r->server = r->connection->server = s;
	  if (r->hostlen && !strncmp(r->uri, "http://", 7)) {
	    r->uri += r->hostlen;
	    r->proxyreq = 0;
	  }
	}
      }
    }
  }
}

void check_serverpath (request_rec *r) {
  server_rec *s;

  /* This is in conjunction with the ServerPath code in
   * http_core, so we get the right host attached to a non-
   * Host-sending request.
   */

  for (s = r->server->next; s; s = s->next) {
    if (s->path && !strncmp(r->uri, s->path, s->pathlen) &&
	(s->path[s->pathlen - 1] == '/' ||
	 r->uri[s->pathlen] == '/' ||
	 r->uri[s->pathlen] == '\0'))
      r->server = r->connection->server = s;
  }
}

request_rec *read_request (conn_rec *conn)
{
    request_rec *r = (request_rec *)pcalloc (conn->pool, sizeof(request_rec));

    r->connection = conn;
    r->server = conn->server;
    r->pool = make_sub_pool(conn->pool);

    conn->keptalive = conn->keepalive;
    conn->keepalive = 0;

    conn->user = NULL;
    conn->auth_type = NULL;

    r->headers_in = make_table (r->pool, 50);
    r->subprocess_env = make_table (r->pool, 50);
    r->headers_out = make_table (r->pool, 5);
    r->err_headers_out = make_table (r->pool, 5);
    r->notes = make_table (r->pool, 5);

    r->request_config = create_request_config (r->pool);
    r->per_dir_config = r->server->lookup_defaults; /* For now. */

    r->sent_bodyct = 0; /* bytect isn't for body */

    r->read_length  = 0;
    r->read_body    = REQUEST_NO_BODY;
    
    r->status = HTTP_OK;	/* Until further notice.
				 * Only changed by die(), or (bletch!)
				 * scan_script_header...
				 */

    /* Get the request... */
    
    keepalive_timeout ("read", r);
    if (!read_request_line (r)) return NULL;
    if (!r->assbackwards) get_mime_headers (r);

/* handle Host header here, to get virtual server */

    if (r->hostname || (r->hostname = table_get(r->headers_in, "Host")))
      check_hostalias(r);
    else
      check_serverpath(r);

    kill_timeout (r);
    conn->keptalive = 0;   /* We now have a request - so no more short timeouts */
    
    if(!strcmp(r->method, "HEAD")) {
        r->header_only=1;
	r->method_number = M_GET;
    }
    else if(!strcmp(r->method, "GET")) 
	r->method_number = M_GET;
    else if(!strcmp(r->method,"POST")) 
        r->method_number = M_POST;
    else if(!strcmp(r->method,"PUT")) 
        r->method_number = M_PUT;
    else if(!strcmp(r->method,"DELETE")) 
        r->method_number = M_DELETE;
    else if(!strcmp(r->method,"CONNECT"))
        r->method_number = M_CONNECT;
    else if(!strcmp(r->method,"OPTIONS"))
        r->method_number = M_OPTIONS;
    else if(!strcmp(r->method,"TRACE"))
        r->method_number = M_TRACE;
    else 
        r->method_number = M_INVALID; /* Will eventually croak. */

    return r;
}

/*
 * A couple of other functions which initialize some of the fields of
 * a request structure, as appropriate for adjuncts of one kind or another
 * to a request in progress.  Best here, rather than elsewhere, since
 * *someone* has to set the protocol-specific fields...
 */

void set_sub_req_protocol (request_rec *rnew, const request_rec *r)
{
    rnew->assbackwards = 1;	/* Don't send headers from this. */
    rnew->no_local_copy = 1;	/* Don't try to send USE_LOCAL_COPY for a
				 * fragment.
				 */
    rnew->method = "GET"; rnew->method_number = M_GET;
    rnew->protocol = "INCLUDED";

    rnew->status = HTTP_OK;

    rnew->headers_in = r->headers_in;
    rnew->subprocess_env = copy_table (rnew->pool, r->subprocess_env);
    rnew->headers_out = make_table (rnew->pool, 5);
    rnew->err_headers_out = make_table (rnew->pool, 5);
    rnew->notes = make_table (rnew->pool, 5);
    
    rnew->read_length = r->read_length;
    rnew->read_body   = REQUEST_NO_BODY;
    
    rnew->main = (request_rec *)r;
}

void finalize_sub_req_protocol (request_rec *sub)
{
    SET_BYTES_SENT (sub->main);
} 

/* Support for the Basic authentication protocol, and a bit for Digest.
 */

void note_auth_failure(request_rec *r)
{
    if (!strcasecmp(auth_type(r), "Basic"))
      note_basic_auth_failure(r);
    else if(!strcasecmp(auth_type(r), "Digest"))
      note_digest_auth_failure(r);
}

void note_basic_auth_failure(request_rec *r)
{
    if (strcasecmp(auth_type(r), "Basic"))
      note_auth_failure(r);
    else
      table_set (r->err_headers_out, "WWW-Authenticate",
		 pstrcat(r->pool, "Basic realm=\"", auth_name(r), "\"", NULL));
}

void note_digest_auth_failure(request_rec *r)
{
    char nonce[256];

    ap_snprintf(nonce, sizeof(nonce), "%lu", r->request_time);
    table_set (r->err_headers_out, "WWW-Authenticate",
               pstrcat(r->pool, "Digest realm=\"", auth_name(r),
                       "\", nonce=\"", nonce, "\"", NULL));
}

int get_basic_auth_pw (request_rec *r, char **pw)
{
    const char *auth_line = table_get (r->headers_in, "Authorization");
    char *t;
    
    if(!(t = auth_type(r)) || strcasecmp(t, "Basic"))
        return DECLINED;

    if (!auth_name (r)) {
        log_reason ("need AuthName", r->uri, r);
	return SERVER_ERROR;
    }
    
    if(!auth_line) {
        note_basic_auth_failure (r);
	return AUTH_REQUIRED;
    }

    if (strcmp(getword (r->pool, &auth_line, ' '), "Basic")) {
        /* Client tried to authenticate using wrong auth scheme */
        log_reason ("client used wrong authentication scheme", r->uri, r);
        note_basic_auth_failure (r);
	return AUTH_REQUIRED;
    }

    t = uudecode (r->pool, auth_line);
    r->connection->user = getword_nulls_nc (r->pool, &t, ':');
    r->connection->auth_type = "Basic";

    *pw = t;

    return OK;
}

/* New Apache routine to map status codes into array indicies 
 *  e.g.  100 -> 0,  101 -> 1,  200 -> 2 ...                     
 * The number of status lines must equal the value of RESPONSE_CODES (httpd.h)
 * and must be listed in order.
 */

static char *status_lines[] = {
   "100 Continue",
   "101 Switching Protocols",
#define LEVEL_200  2
   "200 OK",
   "201 Created",
   "202 Accepted",
   "203 Non-Authoritative Information",
   "204 No Content",
   "205 Reset Content",
   "206 Partial Content",
#define LEVEL_300  9
   "300 Multiple Choices",
   "301 Moved Permanently",
   "302 Moved Temporarily",
   "303 See Other",
   "304 Not Modified",
   "305 Use Proxy",
#define LEVEL_400 15
   "400 Bad Request",
   "401 Authorization Required",
   "402 Payment Required",
   "403 Forbidden",
   "404 File Not Found",
   "405 Method Not Allowed",
   "406 Not Acceptable",
   "407 Proxy Authentication Required",
   "408 Request Time-out",
   "409 Conflict",
   "410 Gone",
   "411 Length Required",
   "412 Precondition Failed",
   "413 Request Entity Too Large",
   "414 Request-URI Too Large",
   "415 Unsupported Media Type",
#define LEVEL_500 31
   "500 Internal Server Error",
   "501 Method Not Implemented",
   "502 Bad Gateway",
   "503 Service Temporarily Unavailable",
   "504 Gateway Time-out",
   "505 HTTP Version Not Supported",
   "506 Variant Also Varies"
}; 

/* The index is found by its offset from the x00 code of each level.
 * Although this is fast, it will need to be replaced if some nutcase
 * decides to define a high-numbered code before the lower numbers.
 * If that sad event occurs, replace the code below with a linear search
 * from status_lines[shortcut[i]] to status_lines[shortcut[i+1]-1];
 */

int index_of_response(int status)
{
    static int shortcut[6] = { 0, LEVEL_200, LEVEL_300, LEVEL_400,
                               LEVEL_500, RESPONSE_CODES };
    int i, pos;

    if (status < 100)          /* Below 100 is illegal for HTTP status */
        return LEVEL_500;

    for (i = 0; i < 5; i++) {
        status -= 100;
        if (status < 100) {
            pos = (status + shortcut[i]);
            if (pos < shortcut[i+1])
                return pos;
            else
                return LEVEL_500;     /* status unknown (falls in gap) */
        }
    }
   return LEVEL_500;                  /* 600 or above is also illegal */
}


void basic_http_header (request_rec *r)
{
    BUFF *fd = r->connection->client;
    char *t;
    
    if (r->assbackwards) return;
    
    if (!r->status_line)
        r->status_line = status_lines[index_of_response(r->status)];
    
    if(table_get(r->subprocess_env,"force-response-1.0"))
	t="HTTP/1.0";
    else
	t=SERVER_PROTOCOL;
    bvputs(fd, t, " ", r->status_line, "\015\012", NULL);
    bvputs(fd,"Date: ",gm_timestr_822 (r->pool, r->request_time),
	   "\015\012", NULL);
    bvputs(fd,"Server: ", SERVER_VERSION, "\015\012", NULL);
}

char *nuke_mime_parms (pool *p, char *content_type)
{
    /* How marvelous.  Arena doesn't *accept* "text/html; level=3"
     * as a MIME type, so we have to strip off the parms.
     */

#ifndef ARENA_BUG_WORKAROUND
    return content_type;
#else

    char *cp = strchr(content_type, ';');

    if (cp) {
        content_type = pstrdup (p, content_type);
	cp = strchr (content_type, ';');
	
        while (cp > content_type && isspace (cp[-1]))
	    --cp;
	*cp = '\0';
    }

    return content_type;
#endif
}

static char *make_allow(request_rec *r)
{
    int allowed = r->allowed;

    if( allowed == 0 ) {
	/* RFC2068 #14.7, Allow must contain at least one method.  So rather
	 * than deal with the possibility of trying not to emit an Allow:
	 * header, i.e. #10.4.6 says 405 Method Not Allowed MUST include
	 * an Allow header, we'll just say TRACE is valid.
	 */
	return( "TRACE" );
    }

    return 2 + pstrcat(r->pool, (allowed & (1 << M_GET)) ? ", GET, HEAD" : "",
		       (allowed & (1 << M_POST)) ? ", POST" : "",
		       (allowed & (1 << M_PUT)) ? ", PUT" : "",
		       (allowed & (1 << M_DELETE)) ? ", DELETE" : "",
		       (allowed & (1 << M_OPTIONS)) ? ", OPTIONS" : "",
		       (allowed & (1 << M_TRACE)) ? ", TRACE" : "",
		       NULL);
    
}

int send_http_trace (request_rec *r)
{
    array_header *hdrs_arr = table_elts(r->headers_in);
    table_entry *hdrs = (table_entry *)hdrs_arr->elts;
    int i;

    /* Get the original request */
    while (r->prev) r = r->prev;

    soft_timeout ("send", r);

    r->content_type = "message/http";
    send_http_header(r);
    
    /* Now we recreate the request, and echo it back */

    rvputs( r, r->the_request, "\015\012", NULL );

    for (i = 0; i < hdrs_arr->nelts; ++i) {
      if (!hdrs[i].key) continue;
      rvputs(r, hdrs[i].key, ": ", hdrs[i].val, "\015\012", NULL);
    }

    kill_timeout(r);
    return OK;
}

int send_http_options(request_rec *r)
{
    BUFF *fd = r->connection->client;
    const long int zero=0L;

    if (r->assbackwards) return DECLINED;

    soft_timeout ("send", r);

    basic_http_header(r);
    bputs("Connection: close\015\012", fd);
    bvputs(fd, "Allow: ", make_allow(r), "\015\012", NULL);
    bputs("\015\012", fd);

    bsetopt(fd, BO_BYTECT, &zero);
    kill_timeout (r);

    return OK;
}

/*
 * Here we try to be compatible with clients that want multipart/x-byteranges
 * instead of multipart/byteranges (also see above), as per HTTP/1.1. We
 * look for the Request-Range header (e.g. Netscape 2 and 3) as an indication
 * that the browser supports an older protocol. We also check User-Agent
 * for Microsoft Internet Explorer 3, which needs this as well.
 */

static int use_range_x(request_rec *r) {
    char *ua;
    return (table_get(r->headers_in, "Request-Range") ||
	    ((ua = table_get(r->headers_in, "User-Agent"))
	     && strstr(ua, "MSIE 3")));
}

void send_http_header(request_rec *r)
{
    conn_rec *c = r->connection;
    BUFF *fd = c->client;
    const long int zero=0L;
    array_header *hdrs_arr;
    table_entry *hdrs;
    int i;
    
    core_dir_config *dir_conf =
      (core_dir_config *)get_module_config(r->per_dir_config, &core_module);
    char *default_type = dir_conf->default_type;
  
    if (r->assbackwards) {
        if(!r->main)
	    bsetopt(fd, BO_BYTECT, &zero);
	r->sent_bodyct = 1;
	return;
    }
    
    basic_http_header (r);

    if (!table_get(r->subprocess_env, "nokeepalive"))
        set_keepalive (r);

    if (r->chunked) {
	bputs("Transfer-Encoding: chunked\015\012", fd);
	/* RFC2068 #4.4: Messages MUST NOT include both a Content-Length
	 * header field and the "chunked" transfer coding. */
        table_unset(r->headers_out, "Content-Length");
    }

    if (r->byterange > 1)
        bvputs(fd, "Content-Type: multipart/",
	       use_range_x(r) ? "x-byteranges" : "byteranges",
	       "; boundary=", r->boundary, "\015\012", NULL);
    else if (r->content_type)
        bvputs(fd, "Content-Type: ", 
		 nuke_mime_parms (r->pool, r->content_type), "\015\012", NULL);
    else if (default_type)
        bvputs(fd, "Content-Type: ", default_type, "\015\012", NULL);
    
    if (r->content_encoding)
        bvputs(fd,"Content-Encoding: ", r->content_encoding, "\015\012", NULL);
    
    if (r->content_languages && r->content_languages->nelts) {
	int i;
	bputs("Content-Language: ", fd);
	for (i = 0; i < r->content_languages->nelts; ++i) {
	    char *lang = ((char**)(r->content_languages->elts))[i];
	    bvputs(fd, i ? ", " : "", lang, NULL);
	}
	bputs("\015\012", fd);
    }
    else if (r->content_language)
        bvputs(fd,"Content-Language: ", r->content_language, "\015\012", NULL);

    /* We now worry about this here */

    if (r->no_cache && (r->proto_num >= 1001))
        bputs ("Cache-Control: private\015\012", fd);
    else if (r->no_cache)
        bvputs(fd,"Expires: ", gm_timestr_822(r->pool, r->request_time),
	       "\015\012", NULL);

    hdrs_arr = table_elts(r->headers_out);
    hdrs = (table_entry *)hdrs_arr->elts;
    for (i = 0; i < hdrs_arr->nelts; ++i) {
        if (!hdrs[i].key) continue;
	if (r->no_cache && !strcasecmp(hdrs[i].key, "Expires")) continue;
	bvputs(fd, hdrs[i].key, ": ", hdrs[i].val, "\015\012", NULL);
    }

    hdrs_arr = table_elts(r->err_headers_out);
    hdrs = (table_entry *)hdrs_arr->elts;
    for (i = 0; i < hdrs_arr->nelts; ++i) {
        if (!hdrs[i].key) continue;
	if (r->no_cache && !strcasecmp(hdrs[i].key, "Expires")) continue;
	bvputs(fd, hdrs[i].key, ": ", hdrs[i].val, "\015\012", NULL);
    }

    bputs("\015\012",fd);

    bsetopt(fd, BO_BYTECT, &zero);
    r->sent_bodyct = 1;		/* Whatever follows is real body stuff... */

    /* Set buffer flags for the body */
    if (r->chunked) bsetflag(fd, B_CHUNK, 1);
}

void finalize_request_protocol (request_rec *r) {
    BUFF *fd = r->connection->client;

    /* Turn off chunked encoding */

    if (r->chunked) {
        bsetflag(fd, B_CHUNK, 0);
	bputs("0\015\012", fd);
	/* If we had footer "headers", we'd send them now */
	bputs("\015\012", fd);
    }

}

/* Here we deal with getting the request message body from the client.
 * Whether or not the request contains a body is signaled by the presence
 * of a non-zero Content-Length or by a Transfer-Encoding: chunked.
 *
 * Note that this is more complicated than it was in Apache 1.1 and prior
 * versions, because chunked support means that the module does less.
 *
 * The proper procedure is this:
 *
 * 1. Call setup_client_block() near the beginning of the request
 *    handler. This will set up all the necessary properties, and will
 *    return either OK, or an error code. If the latter, the module should
 *    return that error code. The second parameter selects the policy to
 *    apply if the request message indicates a body, and how a chunked
 *    transfer-coding should be interpreted. Choose one of
 *
 *    REQUEST_NO_BODY          Send 413 error if message has any body
 *    REQUEST_CHUNKED_ERROR    Send 411 error if body without Content-Length
 *    REQUEST_CHUNKED_DECHUNK  If chunked, remove the chunks for me.
 *    REQUEST_CHUNKED_PASS     Pass the chunks to me without removal.
 *
 *    In order to use the last two options, the caller MUST provide a buffer
 *    large enough to hold a chunk-size line, including any extensions.
 *
 * 2. When you are ready to read a body (if any), call should_client_block().
 *    This will tell the module whether or not to read input. If it is 0,
 *    the module should assume that there is no message body to read.
 *    This step also sends a 100 Continue response to HTTP/1.1 clients,
 *    so should not be called until the module is *definitely* ready to
 *    read content. (otherwise, the point of the 100 response is defeated).
 *    Never call this function more than once.
 *
 * 3. Finally, call get_client_block in a loop. Pass it a buffer and its size.
 *    It will put data into the buffer (not necessarily a full buffer), and
 *    return the length of the input block. When it is done reading, it will
 *    return 0 if EOF, or -1 if there was an error.
 *    If an error occurs on input, we force an end to keepalive.
 */

int setup_client_block (request_rec *r, int read_policy)
{
    char *tenc = table_get(r->headers_in, "Transfer-Encoding");
    char *lenp = table_get(r->headers_in, "Content-length");

    r->read_body    = read_policy;
    r->read_chunked = 0;
    r->remaining    = 0;

    if (tenc) {
        if (strcasecmp(tenc, "chunked")) {
            log_printf(r->server, "Unknown Transfer-Encoding %s", tenc);
            return HTTP_BAD_REQUEST;
        }
        if (r->read_body == REQUEST_CHUNKED_ERROR) {
            log_reason("chunked Transfer-Encoding forbidden", r->uri, r);
            return (lenp) ? HTTP_BAD_REQUEST : HTTP_LENGTH_REQUIRED;
        }

        r->read_chunked = 1;
    }
    else if (lenp) {
        char *pos = lenp;

        while (isdigit(*pos) || isspace(*pos)) ++pos;
        if (*pos != '\0') {
            log_printf(r->server, "Invalid Content-Length %s", lenp);
            return HTTP_BAD_REQUEST;
        }

        r->remaining = atol(lenp);
    }

    if ((r->read_body == REQUEST_NO_BODY) &&
        (r->read_chunked || (r->remaining > 0))) {
        log_printf(r->server, "%s with body is not allowed for %s",
                   r->method, r->uri);
        return HTTP_REQUEST_ENTITY_TOO_LARGE;
    }

    return OK;
}

int should_client_block (request_rec *r)
{
    if (!r->read_chunked && (r->remaining <= 0))
        return 0;

    if (r->proto_num >= 1001) {    /* sending 100 Continue interim response */
        bvputs(r->connection->client,
            SERVER_PROTOCOL, " ", status_lines[0], "\015\012\015\012", NULL);
        bflush(r->connection->client);
    }

    return 1;
}

static long get_chunk_size (char *b)
{
    long chunksize = 0;

    while (isxdigit(*b)) {
        int xvalue = 0;

        if (*b >= '0' && *b <= '9')      xvalue = *b - '0';
        else if (*b >= 'A' && *b <= 'F') xvalue = *b - 'A' + 0xa;
        else if (*b >= 'a' && *b <= 'f') xvalue = *b - 'a' + 0xa;

        chunksize = (chunksize << 4) | xvalue;
        ++b;
    }

    return chunksize;
}

/* get_client_block is called in a loop to get the request message body.
 * This is quite simple if the client includes a content-length
 * (the normal case), but gets messy if the body is chunked. Note that
 * r->remaining is used to maintain state across calls and that
 * r->read_length is the total number of bytes given to the caller
 * across all invocations.  It is messy because we have to be careful not
 * to read past the data provided by the client, since these reads block.
 * Returns 0 on End-of-body, -1 on error or premature chunk end.
 *
 * Reading the chunked encoding requires a buffer size large enough to
 * hold a chunk-size line, including any extensions. For now, we'll leave
 * that to the caller, at least until we can come up with a better solution.
 */
long get_client_block (request_rec *r, char *buffer, int bufsiz)
{
    int c;
    long len_read, len_to_read;
    long chunk_start = 0;

    if (!r->read_chunked) {                 /* Content-length read */
        len_to_read = (r->remaining > bufsiz) ? bufsiz : r->remaining;
        len_read = bread(r->connection->client, buffer, len_to_read);
        if (len_read <= 0) {
            if (len_read < 0) r->connection->keepalive = -1;
            return len_read;
        }
        r->read_length += len_read;
        r->remaining   -= len_read;
        return len_read;
    }

    /* Handle chunked reading
     * Note: we are careful to shorten the input bufsiz so that there
     * will always be enough space for us to add a CRLF (if necessary).
     */
    if (r->read_body == REQUEST_CHUNKED_PASS)
        bufsiz -= 2;
    if (bufsiz <= 0)
        return -1;             /* Cannot read chunked with a small buffer */

    if (r->remaining == 0) {         /* Start of new chunk */

        chunk_start = getline(buffer, bufsiz, r->connection->client, 0);
        if ((chunk_start <= 0) || (chunk_start >= (bufsiz - 1))
                               || !isxdigit(*buffer)) {
            r->connection->keepalive = -1;
            return -1;
        }

        len_to_read = get_chunk_size(buffer);

        if (len_to_read == 0) {      /* Last chunk indicated, get footers */
            if (r->read_body == REQUEST_CHUNKED_DECHUNK) {
                get_mime_headers(r);
                ap_snprintf(buffer, bufsiz, "%ld", r->read_length);
                table_unset(r->headers_in, "Transfer-Encoding");
                table_set(r->headers_in, "Content-Length", buffer);
                return 0;
            }
            r->remaining = -1;       /* Indicate footers in-progress */
        }
        else {
            r->remaining = len_to_read;
        }
        if (r->read_body == REQUEST_CHUNKED_PASS) {
            buffer[chunk_start++] = CR;  /* Restore chunk-size line end  */
            buffer[chunk_start++] = LF;
            buffer += chunk_start;       /* and pass line on to caller   */
            bufsiz -= chunk_start;
        }
    }
                                     /* When REQUEST_CHUNKED_PASS, we are */
    if (r->remaining == -1) {        /* reading footers until empty line  */
        len_read = chunk_start;

        while ((bufsiz > 1) && ((len_read =
                getline(buffer, bufsiz, r->connection->client, 1)) > 0)) {

            if (len_read != (bufsiz - 1)) {
                buffer[len_read++] = CR;  /* Restore footer line end  */
                buffer[len_read++] = LF;
            }
            chunk_start += len_read;
            buffer      += len_read;
            bufsiz      -= len_read;
        }
        if (len_read < 0) {
            r->connection->keepalive = -1;
            return -1;
        }

        if (len_read == 0) {         /* Indicates an empty line */
            buffer[0] = CR;
            buffer[1] = LF;
            chunk_start += 2;
            r->remaining = -2;
        }
        r->read_length += chunk_start;
        return chunk_start;
    }
                                     /* When REQUEST_CHUNKED_PASS, we     */
    if (r->remaining == -2) {        /* finished footers when last called */
        r->remaining = 0;            /*     so now we must signal EOF     */
        return 0;
    }

    /* Otherwise, we are in the midst of reading a chunk of data */

    len_to_read = (r->remaining > bufsiz) ? bufsiz : r->remaining;
    
    len_read = bread(r->connection->client, buffer, len_to_read);
    if (len_read <= 0) {
        r->connection->keepalive = -1;
        return -1;
    }

    r->remaining -= len_read;

    if (r->remaining == 0) {         /* End of chunk, get trailing CRLF */
        if ((c = bgetc(r->connection->client)) == CR) {
           c = bgetc(r->connection->client);
        }
        if (c != LF) {
            r->connection->keepalive = -1;
            return -1;
        }
        if (r->read_body == REQUEST_CHUNKED_PASS) {
            buffer[len_read++] = CR;
            buffer[len_read++] = LF;
        }
    }
    r->read_length += (chunk_start + len_read);

    return (chunk_start + len_read);
}

long send_fd(FILE *f, request_rec *r) { return send_fd_length(f, r, -1); }

long send_fd_length(FILE *f, request_rec *r, long length)
{
    char buf[IOBUFSIZE];
    long total_bytes_sent;
    register int n, w, o, len;
    conn_rec *c = r->connection;
    
    if (length == 0) return 0;

    total_bytes_sent = 0;
    while (!r->connection->aborted) {
	if ((length > 0) && (total_bytes_sent + IOBUFSIZE) > length)
	    len = length - total_bytes_sent;
	else len = IOBUFSIZE;

        while ((n= fread(buf, sizeof(char), len, f)) < 1
	       && ferror(f) && errno == EINTR)
	    continue;
	
	if (n < 1) {
            break;
        }
        o=0;
	total_bytes_sent += n;
	
        while(n && !r->connection->aborted) {
            w=bwrite(c->client, &buf[o], n);
	    if(w <= 0)
		break;
	    reset_timeout(r); /* reset timeout after successfule write */
            n-=w;
	    o+=w;
        }
    }
    
    SET_BYTES_SENT(r);
    return total_bytes_sent;
}

int rputc (int c, request_rec *r)
{
    if (r->connection->aborted) return EOF;
    bputc(c, r->connection->client);
    SET_BYTES_SENT(r);
    return c;
}

int
rputs(const char *str, request_rec *r)
{
    if (r->connection->aborted) return EOF;
    SET_BYTES_SENT(r);
    return bputs(str, r->connection->client);
}

int rwrite(const void *buf, int nbyte, request_rec *r)
{
    int n;
    if (r->connection->aborted) return EOF;
    n=bwrite(r->connection->client, buf, nbyte);
    SET_BYTES_SENT(r);
    return n;
}

int rprintf(request_rec *r,const char *fmt,...)
    {
    va_list vlist;
    int n;

    if(r->connection->aborted) return EOF;
    va_start(vlist,fmt);
    n=vbprintf(r->connection->client,fmt,vlist);
    va_end(vlist);
    SET_BYTES_SENT(r);
    return n;
    }

int
rvputs(request_rec *r, ...)
{
    va_list args;
    int i, j, k;
    const char *x;
    BUFF *fb=r->connection->client;
    
    if (r->connection->aborted) return EOF;
    
    va_start (args, r);
    for (k=0;;)
    {
	x = va_arg(args, const char *);
	if (x == NULL) break;
	j = strlen(x);
	i = bwrite(fb, x, j);
	if (i != j)
	{
	    va_end(args);
	    return -1;
	}
	k += i;
    }
    va_end(args);

    SET_BYTES_SENT(r);
    return k;
}

int rflush (request_rec *r) {
    return bflush(r->connection->client);
}

static void send_header(request_rec *r, char *hdr)
{
    char *val = table_get(r->headers_out, hdr);
    if (val) bvputs(r->connection->client, hdr, ": ", val, "\015\012", NULL);
}

void send_error_response (request_rec *r, int recursive_error)
{
    conn_rec *c = r->connection;
    char *custom_response;
    char *location = table_get (r->headers_out, "Location");
    int status = r->status;
    int idx = index_of_response (status);

    if (!r->assbackwards) {
	int i;
	table *err_hdrs_arr = r->err_headers_out;
	table_entry *err_hdrs = (table_entry *)err_hdrs_arr->elts;
	table *hdrs_arr = r->headers_out;
	table_entry *hdrs = (table_entry *)hdrs_arr->elts;
  
        basic_http_header (r);
	
	/* For non-error statuses (2xx and 3xx), send out all the normal
	 * headers unless it is a 304. Don't send a Location unless its
	 * a redirect status (3xx).
	 */

	if ((is_HTTP_SUCCESS(status) || is_HTTP_REDIRECT(status)) &&
	    status != HTTP_NOT_MODIFIED) {
	    for (i = 0; i < hdrs_arr->nelts; ++i) {
		if (!hdrs[i].key) continue;
		if (!strcasecmp(hdrs[i].key, "Location") &&
		    !is_HTTP_REDIRECT(status))
		    continue;
		bvputs(c->client, hdrs[i].key, ": ", hdrs[i].val,
		       "\015\012", NULL);
	    }
	}
	
	if (status == HTTP_NOT_MODIFIED) {
	    send_header(r, "ETag");
	    send_header(r, "Content-Location");
	    send_header(r, "Expires");
	    send_header(r, "Cache-Control");
	    send_header(r, "Vary");
	    send_header(r, "Warning");
	    send_header(r, "WWW-Authenticate");
	    set_keepalive(r);
	    bputs("\015\012", c->client);
	    return;
	}

	/* Someday, we'd like to have persistent connections here.
	 * They're especially useful for redirects, multiple choices
	 * and auth requests. But we need to rewrite the rest of thi
	 * section, so for now, we don't use it.
	 */
	bputs("Connection: close\015\012", c->client);

	if ((status == METHOD_NOT_ALLOWED) || (status == NOT_IMPLEMENTED))
	    bvputs(c->client, "Allow: ", make_allow(r), "\015\012", NULL);
	
	for (i = 0; i < err_hdrs_arr->nelts; ++i) {
	    if (!err_hdrs[i].key) continue;
	    bvputs(c->client, err_hdrs[i].key, ": ", err_hdrs[i].val,
		   "\015\012", NULL);
	}

	bputs("Content-type: text/html\015\012\015\012", c->client);
    }

    if (r->header_only) return;
    
    if ((custom_response = response_code_string (r, idx))) {
        /*
	 * We have a custom response output. This should only be
	 * a text-string to write back. But if the ErrorDocument
	 * was a local redirect and the requested resource failed
	 * for any reason, the custom_response will still hold the
	 * redirect URL. We don't really want to output this URL
	 * as a text message, so first check the custom response 
	 * string to ensure that it is a text-string (using the
	 * same test used in die(), i.e. does it start with a
	 * "). If it doesn't, we've got a recursive error, so find
	 * the original error and output that as well.
	 */
        if (custom_response[0] == '\"') { 
            bputs(custom_response+1, c->client);
	      return;
	}
	/* Redirect failed, so get back the original error
	 */
	while (r->prev && (r->prev->status != HTTP_OK))
          r = r->prev;
    }
    {
	char *title = status_lines[idx];
	/* folks decided they didn't want the error code in the H1 text */

	char *h1 = 4 + status_lines[idx];
	BUFF *fd = c->client;
	
        bvputs
	    (
		fd,
		"<HTML><HEAD>\n<TITLE>",
		title,
		"</TITLE>\n</HEAD><BODY>\n<H1>",
		h1,
	       "</H1>\n",
	       NULL
	    );
	
        switch (status) {
	case REDIRECT:
	case MOVED:
	    bvputs(fd, "The document has moved <A HREF=\"",
		    escape_html(r->pool, location), "\">here</A>.<P>\n", NULL);
	    break;
	case HTTP_SEE_OTHER:
	    bvputs(fd, "The answer to your request is located <A HREF=\"",
		    escape_html(r->pool, location), "\">here</A>.<P>\n", NULL);
	    break;
	case HTTP_USE_PROXY:
	    bvputs(fd, "This resource is only accessible through the proxy\n",
		   escape_html(r->pool, location), "<BR>\nYou will need to ",
                   "configure your client to use that proxy.<P>\n", NULL);
	    break;
	case AUTH_REQUIRED:
	    bputs("This server could not verify that you\n", fd);
	    bputs("are authorized to access the document you\n", fd);
	    bputs("requested.  Either you supplied the wrong\n", fd);
	    bputs("credentials (e.g., bad password), or your\n", fd);
	    bputs("browser doesn't understand how to supply\n", fd);
	    bputs("the credentials required.<P>\n", fd);
	    break;
	case BAD_REQUEST:
	    bputs("Your browser sent a request that\n", fd);
	    bputs("this server could not understand.<P>\n", fd);
	    break;
	case FORBIDDEN:
	    bvputs(fd, "You don't have permission to access ",
		     escape_html(r->pool, r->uri), "\non this server.<P>\n",
		   NULL);
	    break;
	case NOT_FOUND:
	    bvputs(fd, "The requested URL ", escape_html(r->pool, r->uri),
		    " was not found on this server.<P>\n", NULL);
	    break;
	case METHOD_NOT_ALLOWED:
	    bvputs(fd, "The requested method ", r->method, " is not allowed "
		   "for the URL ", escape_html(r->pool, r->uri),
		   ".<P>\n", NULL);
	    break;
	case NOT_ACCEPTABLE:
	    bvputs(fd,
		  "An appropriate representation of the requested resource ",
		   escape_html(r->pool, r->uri),
		   " could not be found on this server.<P>\n", NULL);
	    /* fall through */
	case MULTIPLE_CHOICES: 
	    {
		char *list;
		if ((list = table_get (r->notes, "variant-list")))
		    bputs(list, fd);
	    }
	    break;
	case LENGTH_REQUIRED:
	    bvputs(fd, "A request of the requested method ", r->method,
		   " requires a valid Content-length.<P>\n", NULL);
	    break;
	case PRECONDITION_FAILED:
	    bvputs(fd, "The precondition on the request for the URL ",
		   escape_html(r->pool, r->uri), " evaluated to false.<P>\n",
		   NULL);
	    break;
	case NOT_IMPLEMENTED:
	    bvputs(fd, escape_html(r->pool, r->method), " to ",
		   escape_html(r->pool, r->uri), " not supported.<P>\n", NULL);
	    break;
	case BAD_GATEWAY:
	    bputs("The proxy server received an invalid\015\012", fd);
	    bputs("response from an upstream server.<P>\015\012", fd);
	    break;
	case VARIANT_ALSO_VARIES:
	    bvputs(fd, "A variant for the requested entity  ",
		   escape_html(r->pool, r->uri), " is itself a ",
		   "transparently negotiable resource.<P>\n", NULL);
  	    break;
	case HTTP_REQUEST_TIME_OUT:
	    bputs("I'm tired of waiting for your request.\n", fd);
  	    break;
	case HTTP_GONE:
	    bvputs(fd, "The requested resource<BR>",
		   escape_html(r->pool, r->uri),
                   "<BR>\nis no longer available on this server ",
		   "and there is no forwarding address.\n",
	           "Please remove all references to this resource.\n", NULL);
  	    break;
	case HTTP_REQUEST_ENTITY_TOO_LARGE:
	    bvputs(fd, "The requested resource<BR>",
	           escape_html(r->pool, r->uri), "<BR>\n",
	           "does not allow request data with ", r->method,
	           " requests, or the amount of data provided in\n",
	           "the request exceeds the capacity limit.\n", NULL);
	    break;
	case HTTP_REQUEST_URI_TOO_LARGE:
	    bputs("The requested URL's length exceeds the capacity\n", fd);
	    bputs("limit for this server.\n", fd);
  	    break;
	case HTTP_UNSUPPORTED_MEDIA_TYPE:
	    bputs("The supplied request data is not in a format\n", fd);
	    bputs("acceptable for processing by this resource.\n", fd);
  	    break;
	case HTTP_SERVICE_UNAVAILABLE:
	    bputs("The server is temporarily unable to service your\n", fd);
	    bputs("request due to maintenance downtime or capacity\n", fd);
	    bputs("problems. Please try again later.\n", fd);
  	    break;
	case HTTP_GATEWAY_TIME_OUT:
	    bputs("The proxy server did not receive a timely response\n", fd);
	    bputs("from the upstream server.<P>\n", fd);
	    break;
	default:  /* HTTP_INTERNAL_SERVER_ERROR */
	    bputs("The server encountered an internal error or\n", fd);
	    bputs("misconfiguration and was unable to complete\n", fd);
	    bputs("your request.<P>\n", fd);
	    bputs("Please contact the server administrator,\n ", fd);
	    bputs(escape_html(r->pool, r->server->server_admin), fd);
	    bputs(" and inform them of the time the error occurred,\n", fd);
	    bputs("and anything you might have done that may have\n", fd);
	    bputs("caused the error.<P>\n", fd);
	    break;
	}

        if (recursive_error) {
	    char x[80];
	    ap_snprintf (x, sizeof(x), 
		"Additionally, an error of type %d was encountered\n",
		recursive_error);
	    bputs(x, fd);
	    bputs("while trying to use an ErrorDocument to\n", fd);
	    bputs("handle the request.\n", fd);
	}
	bputs("</BODY></HTML>\n", fd);
    }
        
}

/* Finally, this... it's here to support nph- scripts
 * Now what ever are we going to do about them when HTTP-NG packetization
 * comes along?
 */

void client_to_stdout (conn_rec *c)
{
    bflush(c->client);
    dup2(c->client->fd, STDOUT_FILENO);
}
