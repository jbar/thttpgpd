/* libhttpd.c - HTTP protocol library
**
** Copyright � 1995,1998,1999,2000,2001 by Jef Poskanzer <jef@mail.acme.com>.
** Copyright � 2012-2014 by Jean-Jacques Brucker <open-udc@googlegroups.com>.
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**	notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**	notice, this list of conditions and the following disclaimer in the
**	documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
** OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
** HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
** SUCH DAMAGE.
*/


#include "config.h"
#include "version.h"

#ifdef HAVE_DEFINES_H
#include "defines.h"
#endif

#ifdef SHOW_SERVER_VERSION
#define EXPOSED_SERVER_SOFTWARE SOFTWARE_NAME"/"SOFTWARE_VERSION
#else /* SHOW_SERVER_VERSION */
#define EXPOSED_SERVER_SOFTWARE SOFTWARE_NAME
#endif /* SHOW_SERVER_VERSION */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif /* HAVE_MEMORY_H */
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> /* for uintptr_t */
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>
#include <gpgme.h>

#ifdef HAVE_DIRENT_H
# include <dirent.h>
# define NAMLEN(dirent) strlen((dirent)->d_name)
#else
# define dirent direct
# define NAMLEN(dirent) (dirent)->d_namlen
# ifdef HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# ifdef HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# ifdef HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 2048
#endif

#ifndef BUFSIZE
#define BUFSIZE 32768
#endif

/* extern golbal variable */
extern char* argv0;
extern gpgme_ctx_t main_gpgctx;

#ifdef AUTH_FILE
extern char* crypt( const char* key, const char* setting );
#endif /* AUTH_FILE */

#include "libhttpd.h"
#include "mmc.h"
#include "timers.h"
#include "match.h"
#include "tdate_parse.h"
#include "hkp.h"
#ifdef OPENUDC
#include "udc.h"
#endif /* OPENUDC */

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifndef HAVE_INT64T
typedef long long int64_t;
#endif

#ifndef HAVE_SOCKLENT
typedef int socklen_t;
#endif

#ifdef __CYGWIN__
#define timezone  _timezone
#endif

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

/* a struct passed to callbacks for gpgme data buffers */
struct fp2fd_gpg_data_handle {
	FILE * fpin;
	int fdout;
}; 

/* Forwards. */
static void free_httpd_server( httpd_server* hs );
static int init_listen_sockets(const char * hostname, unsigned short port, int * listen_fds,  size_t size);
static void add_response( httpd_conn* hc, char* str );
static void send_response_tail( httpd_conn* hc );
static void defang(const char* str, char* dfstr, int dfsize );
#ifdef AUTH_FILE
static void send_authenticate( httpd_conn* hc, char* realm );
static int b64_decode( const char* str, unsigned char* space, int size );
static int auth_check( httpd_conn* hc ); /* WARNING: NOT thread safe ! (and quiet heavy) */
#endif /* AUTH_FILE */
static void send_dirredirect( httpd_conn* hc );
static int hexit( char c );
#ifdef GENERATE_INDEXES
static void strencode( char* to, int tosize, char* from );
#endif /* GENERATE_INDEXES */
//static char* expand_symlinks( char* path, char** restP, int no_symlink_check );
static char* bufgets( httpd_conn* hc );
static void de_dotdot( char* file );
static void init_mime( void );
static void figure_mime( httpd_conn* hc );
#ifdef CGI_TIMELIMIT
static void cgi_kill2( ClientData client_data, struct timeval* nowP );
static void cgi_kill( ClientData client_data, struct timeval* nowP );
#endif /* CGI_TIMELIMIT */
/* drop_child() is called by the parent process when a child will handle the request */
static void drop_child(const char * type,pid_t pid,httpd_conn* hc);
/* child_r_start() is called early(first) in the child process which will handle the request */
static void child_r_start(httpd_conn* hc);
static int launch_process(void (*funct) (httpd_conn* ), httpd_conn* hc, int methods, char * fname);
#ifdef GENERATE_INDEXES
static void ls( httpd_conn* hc );
#endif /* GENERATE_INDEXES */
static char* build_env( char* fmt, char* arg );
static char** make_envp( httpd_conn* hc );
static char** make_argp( httpd_conn* hc );
static void cgi_interpose_input(interpose_args_t * args);
static ssize_t fp2fd_gpg_data_rd_cb(struct fp2fd_gpg_data_handle * handle, void *buffer, size_t size);
static void gpg_data_release_cb(void *handle);
static void cgi_child( httpd_conn* hc );
static void make_log_entry(const httpd_conn* hc, time_t now, int status);
static inline int sockaddr_check( const struct sockaddr * sa );
static inline size_t sockaddr_len( const struct sockaddr * sa );

static void
free_httpd_server( httpd_server* hs )
	{
	if ( hs->binding_hostname != (char*) 0 )
		free( (void*) hs->binding_hostname );
	if ( hs->cwd != (char*) 0 )
		free( (void*) hs->cwd );
	if ( hs->cgi_pattern != (char*) 0 )
		free( (void*) hs->cgi_pattern );
	if ( hs->sig_pattern != (char*) 0 )
		free( (void*) hs->sig_pattern );
	free( (void*) hs );
	}


httpd_server* httpd_initialize( char* hostname, unsigned short port,
	char* cgi_pattern, char * fastcgi_pass, char* sig_pattern,
	int cgi_limit, char* cwd, int bfield, FILE* logfp ) {

	httpd_server* hs;
	static char ghnbuf[256];
	char* cp;

	hs = NEW( httpd_server, 1 );
	if ( hs == (httpd_server*) 0 )
		{
		syslog( LOG_CRIT, "out of memory allocating an httpd_server" );
		return (httpd_server*) 0;
		}

	if ( hostname != (char*) 0 )
		{
		hs->binding_hostname = strdup( hostname );
		if ( hs->binding_hostname == (char*) 0 )
			{
			syslog( LOG_CRIT, "out of memory copying hostname" );
			return (httpd_server*) 0;
			}
		hs->server_hostname = hs->binding_hostname;
		}
	else
		{
		hs->binding_hostname = (char*) 0;
		hs->server_hostname = (char*) 0;
		if ( gethostname( ghnbuf, sizeof(ghnbuf) ) == 0 )
			hs->server_hostname = ghnbuf;
		}

	hs->port = port;
	if ( cgi_pattern == (char*) 0 )
		hs->cgi_pattern = (char*) 0;
	else
		{
		/* Nuke any leading slashes. */
		if ( cgi_pattern[0] == '/' )
			++cgi_pattern;
		hs->cgi_pattern = strdup( cgi_pattern );
		if ( hs->cgi_pattern == (char*) 0 )
			{
			syslog( LOG_CRIT, "out of memory copying cgi_pattern" );
			return (httpd_server*) 0;
			}
		/* Nuke any leading slashes in the cgi pattern. */
		while ( ( cp = strstr( hs->cgi_pattern, "|/" ) ) != (char*) 0 )
			(void) strcpy( cp + 1, cp + 2 );
		}

	if ( fastcgi_pass == (char*) 0 )
		hs->fastcgi_saddr = (struct sockaddr *) 0;
	/* else ... */
		
	if ( sig_pattern == (char*) 0 )
		hs->sig_pattern = (char*) 0;
	else
		{
		/* Nuke any leading slashes. */
		if ( sig_pattern[0] == '/' )
			++sig_pattern;
		hs->sig_pattern = strdup( sig_pattern );
		if ( hs->sig_pattern == (char*) 0 )
			{
			syslog( LOG_CRIT, "out of memory copying sig_pattern" );
			return (httpd_server*) 0;
			}
		/* Nuke any leading slashes in the sig pattern. */
		while ( ( cp = strstr( hs->sig_pattern, "|/" ) ) != (char*) 0 )
			(void) strcpy( cp + 1, cp + 2 );
		}
	hs->cgi_limit = cgi_limit;
	hs->cgi_count = 0;
	hs->cwd = strdup( cwd );
	if ( hs->cwd == (char*) 0 )
		{
		syslog( LOG_CRIT, "out of memory copying cwd" );
		return (httpd_server*) 0;
		}
	hs->bfield = bfield;
	hs->logfp = logfp;

	/* Initialize listen sockets. */
	if ( init_listen_sockets(hostname, port, hs->listen_fds, SIZEOFARRAY(hs->listen_fds))  < 1 ) {
		free_httpd_server( hs );
		return (httpd_server*) 0;
	}

	init_mime();

	/* Done initializing. */
	if ( hs->binding_hostname == (char*) 0 )
		syslog(
			LOG_NOTICE, "%.80s starting on port %d", EXPOSED_SERVER_SOFTWARE,
			(int) hs->port );
	else
		syslog(
			LOG_NOTICE, "%.80s starting on %.80s, port %d", EXPOSED_SERVER_SOFTWARE,
			hostname,
			(int) hs->port );
	return hs;
}

/*
 * \return The number of listening socket (1 to nmemb) if success, -1 on error.
 */
static int init_listen_sockets(const char * hostname, unsigned short port, int * listen_fds,  size_t nmemb) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int s, i;
	char service[10];

	if (snprintf(service, sizeof(service), "%d", port)>=sizeof(service))
		return -1;

	for (i=0;i<nmemb;i++)
		listen_fds[i]=-1;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;    	/* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM;	/* Sequenced, reliable, connection-based */
	hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
	hints.ai_protocol = 0;          /* Any protocol */
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

	s = getaddrinfo(hostname, service, &hints, &result);
	if (s != 0) {
		syslog( LOG_CRIT, "getaddrinfo: %s\n", gai_strerror(s));
		return -1;
	}

	for (i=0, rp = result;i<(nmemb-1) && rp ; rp = rp->ai_next) {
		listen_fds[i] = socket(rp->ai_family, rp->ai_socktype,
		rp->ai_protocol);
		if (listen_fds[i] == -1)
			continue;

		/* Allow reuse of local addresses. */
		s = 1;
		if ( setsockopt(listen_fds[i], SOL_SOCKET, SO_REUSEADDR, (char*) &s, sizeof(s) ) < 0 ) {
			char * str=get_ip_str(rp->ai_addr);
			syslog( LOG_WARNING, "setsockopt SO_REUSEADDR [%.80s]:%.80s - %m", str, service);
			free(str);
		}

		/* Try to restrict PF_INET6 socket to IPv6 communications only. */
		if (rp->ai_addr->sa_family == AF_INET6) {
			s=1;
			if ( setsockopt(listen_fds[i], IPPROTO_IPV6, IPV6_V6ONLY, (char*) &s, sizeof(s) ) < 0 ) {
				char * str=get_ip_str(rp->ai_addr);
				syslog( LOG_WARNING, "setsockopt IPV6_V6ONLY [%.80s]:%.80s - %m", str, service);
				free(str);
			}
		}

		/* Set non-blocking mode (CRITICAL) */
		if ( httpd_set_ndelay( listen_fds[i] ) < 0 ) { 
			char * str=get_ip_str(rp->ai_addr);
			syslog( LOG_CRIT, "httpd_set_ndelay [%.80s]:%.80s - %m", str, service );
			free(str);
			close( listen_fds[i] );
			listen_fds[i] = -1;
			freeaddrinfo(result);
			return -1;
		}

		if (bind(listen_fds[i], rp->ai_addr, rp->ai_addrlen)) {
			/* bind fail */
			char * str=get_ip_str(rp->ai_addr);
			syslog(LOG_WARNING, "bind [%.80s]:%.80s - %m", str, service );
			free(str);
			close(listen_fds[i]);
			listen_fds[i] = -1;
		}
		else if ( listen( listen_fds[i], LISTEN_BACKLOG ) ) {
			/* listen fail */
			char * str=get_ip_str(rp->ai_addr);
			syslog(LOG_WARNING, "listen [%.80s]:%.80s - %m", str, service );
			free(str);
			close(listen_fds[i]);
			listen_fds[i] = -1;
		}
		else {
			/* Success */
			i++;
			/* Use accept filtering, if available. */
#ifdef SO_ACCEPTFILTER
#if ( __FreeBSD_version >= 411000 )
#define ACCEPT_FILTER_NAME "httpready"
#else
#define ACCEPT_FILTER_NAME "dataready"
#endif
			struct accept_filter_arg af;
			(void) bzero( &af, sizeof(af) );
			(void) strcpy( af.af_name, ACCEPT_FILTER_NAME );
			(void) setsockopt(
				listen_fds[i], SOL_SOCKET, SO_ACCEPTFILTER, (char*) &af, sizeof(af) );
#endif /* SO_ACCEPTFILTER */
		}
	}

	freeaddrinfo(result);           /* No longer needed */

	if (listen_fds[0] < 0) {
		/* No address succeeded */
		syslog(LOG_CRIT, "init_listen_sockets(%.80s,%d,,%d) : No address succeeded", hostname, port, nmemb );
		return -1;
	}

	return i;
}

void
httpd_terminate( httpd_server* hs )
	{
	httpd_unlisten( hs );
	if ( hs->logfp != (FILE*) 0 )
		(void) fclose( hs->logfp );
	free_httpd_server( hs );
	}

/* Call to unlisten/close socket(s) listening for new connections. */
void httpd_unlisten( httpd_server* hs ) {
	int i;

	for (i=0;hs->listen_fds[i]>=0;i++) {
		(void) close( hs->listen_fds[i] );
		hs->listen_fds[i]=-1;
	}
}


/* Conditional macro to allow two alternate forms for use in the built-in
** error pages.  If EXPLICIT_ERROR_PAGES is defined, the second and more
** explicit error form is used; otherwise, the first and more generic
** form is used.
*/
#ifdef EXPLICIT_ERROR_PAGES
#define ERROR_FORM(a,b) b
#else /* EXPLICIT_ERROR_PAGES */
#define ERROR_FORM(a,b) a
#endif /* EXPLICIT_ERROR_PAGES */

char* ok200title = "OK";
char* ok206title = "Partial Content";

char* err302title = "Found";
char* err302form = "The actual URL is '%.80s'.\n";

char* err304title = "Not Modified";

char* httpd_err400title = "Bad Request";
char* httpd_err400form =
	"Your request has bad syntax or is inherently impossible to satisfy.\n";

#ifdef AUTH_FILE
char* err401title = "Unauthorized";
char* err401form =
	"Authorization required for the URL '%.80s'.\n";
#endif /* AUTH_FILE */

char* err403title = "Forbidden";
#ifndef EXPLICIT_ERROR_PAGES
char* err403form =
	"You do not have permission to get URL '%.80s' from this server.\n";
#endif /* !EXPLICIT_ERROR_PAGES */

char* err404title = "Not Found";
char* err404form =
	"The requested URL '%.80s' was not found on this server.\n";

char* httpd_err408title = "Request Timeout";
char* httpd_err408form =
	"No request appeared within a reasonable time period.\n";

char * err411title = "Length Required";
char * err413title = "Request Entity Too Large";
char * err415title = "Unsupported Media Type";

char* err500title = "Internal Error";
char* err500form =
	"There was an unusual problem serving the requested URL (%.80s).\n";

char* err501title = "Not Implemented";
char* err501form =
	"The requested feature '%.80s' is not implemented for this url.\n";

char* httpd_err503title = "Service Temporarily Overloaded";
char* httpd_err503form =
	"The requested URL '%.80s' is temporarily overloaded.  Please try again later.\n";


/* Append a string to the buffer waiting to be sent as response. */
static void
add_response( httpd_conn* hc, char* str )
	{
	size_t len;

	len = strlen( str );
	httpd_realloc_str( &hc->response, &hc->maxresponse, hc->responselen + len );
	(void) memmove( &(hc->response[hc->responselen]), str, len );
	hc->responselen += len;
	}

/* Send the buffered response. */
void httpd_write_response( httpd_conn* hc ) {
	/* Send the response, if necessary. */
	if ( hc->responselen > 0 ) {
		(void) httpd_write_fully( hc->conn_fd, hc->response, hc->responselen );
		hc->responselen = 0;
	}
}

/* Set non-blocking (previously a.k.a. O_NDELAY) mode on a socket, pipe...
 * \return like fcntl, -1 on error (cf errno).
 */
int httpd_set_ndelay( int fd ) {
	int flags, newflags;

	flags = fcntl( fd, F_GETFL, 0 );
	if ( flags != -1 ) {
		newflags = flags | (int) O_NONBLOCK;
		if ( newflags != flags )
			flags =  fcntl( fd, F_SETFL, newflags );
	}
	return flags;
}

/* Clear O_NONBLOCK / set blocking mode on a socket, pipe...
 * \return like fcntl, -1 on error (cf errno).
 */
int httpd_clear_ndelay( int fd ) {
	int flags, newflags;

	flags = fcntl( fd, F_GETFL, 0 );
	if ( flags != -1 ) {
		newflags = flags & ~ (int) O_NONBLOCK;
		if ( newflags != flags )
			flags = fcntl( fd, F_SETFL, newflags );
	}
	return flags;
}


void
send_mime( httpd_conn* hc, int status, char* title, char* encodings, char* extraheads, char* type, off_t length, time_t mod )
	{
	time_t now;
	const char* rfc1123fmt = "%a, %d %b %Y %T GMT";
	char nowbuf[100];
	char modbuf[100];
	char fixed_type[500];
	char buf[1000];

	hc->status = status;
	hc->bytes_to_send = length;
	if ( hc->http_version > 9 )
		{
		now = time( (time_t*) 0 );

		if ( mod == (time_t) 0 )
			mod = now;
		(void) strftime( nowbuf, sizeof(nowbuf), rfc1123fmt, gmtime( &now ) );
		(void) strftime( modbuf, sizeof(modbuf), rfc1123fmt, gmtime( &mod ) );
		(void) snprintf(
			fixed_type, sizeof(fixed_type), type, DEFAULT_CHARSET );
		(void) snprintf( buf, sizeof(buf),
			"%.20s %d %s\015\012Server: %s\015\012Content-Type: %s\015\012Date: %s\015\012Last-Modified: %s\015\012Accept-Ranges: bytes\015\012Connection: close\015\012",
			hc->protocol, status, title, EXPOSED_SERVER_SOFTWARE, fixed_type,
			nowbuf, modbuf );
		add_response( hc, buf );
		if ( status < 200 || status >= 400 )
			{
			(void) snprintf( buf, sizeof(buf),
				"Cache-Control: no-cache,no-store\015\012" );
			add_response( hc, buf );
			}
		if ( encodings[0] != '\0' )
			{
			(void) snprintf( buf, sizeof(buf),
				"Content-Encoding: %s\015\012", encodings );
			add_response( hc, buf );
			}
		if ( status == 206 )
			{
			(void) snprintf( buf, sizeof(buf),
				"Content-Range: bytes %lld-%lld/%lld\015\012%s %lld\015\012",
				(int64_t) hc->first_byte_index, (int64_t) hc->last_byte_index,
				(int64_t) length, "Content-Length:",
				(int64_t) ( hc->last_byte_index - hc->first_byte_index + 1 ) );
			add_response( hc, buf );
			}
		else if ( length >= 0 )
			{
			(void) snprintf( buf, sizeof(buf),
				"%s %lld\015\012","Content-Length:", (int64_t) length );
			add_response( hc, buf );
			}
		if ( extraheads[0] != '\0' )
			add_response( hc, extraheads );
		add_response( hc, "\015\012" );
		}

	make_log_entry( hc, now, status);
	hc->bfield |= HC_LOG_DONE;
	}


static int str_alloc_count = 0;
static size_t str_alloc_size = 0;

/* Note: this function kills its calling process (exit) if realloc fail */
void
httpd_realloc_str( char** strP, size_t* maxsizeP, size_t size )
	{
	if ( *maxsizeP == 0 )
		{
		*maxsizeP = MAX( 200, size + 100 );
		*strP = NEW( char, *maxsizeP + 1 );
		++str_alloc_count;
		str_alloc_size += *maxsizeP;
		}
	else if ( size > *maxsizeP )
		{
		str_alloc_size -= *maxsizeP;
		*maxsizeP = MAX( *maxsizeP * 2, size * 5 / 4 );
		*strP = RENEW( *strP, char, *maxsizeP + 1 );
		str_alloc_size += *maxsizeP;
		}
	else
		return;
	if ( *strP == (char*) 0 )
		{
		syslog(
			LOG_ERR, "out of memory reallocating a string to %d bytes",
			*maxsizeP );
		exit( 1 );
		}
	}

static void
send_response_tail( httpd_conn* hc )
	{
	char buf[1000];

	(void) snprintf( buf, sizeof(buf), "\
<HR>\n\
<ADDRESS><A HREF=\"%s\">%s</A></ADDRESS>\n\
</BODY>\n\
</HTML>\n",
		SOFTWARE_ADDRESS, EXPOSED_SERVER_SOFTWARE );
	add_response( hc, buf );
	}


static void
defang( const char* str, char* dfstr, int dfsize )
	{
	int i;
	char* cp2;

	for ( i = 0, cp2 = dfstr;
		  str[i] != '\0' && cp2 - dfstr < dfsize - 5;
		  ++i, ++cp2 )
		{
		switch ( str[i] )
			{
			case '<':
			*cp2++ = '&';
			*cp2++ = 'l';
			*cp2++ = 't';
			*cp2 = ';';
			break;
			case '>':
			*cp2++ = '&';
			*cp2++ = 'g';
			*cp2++ = 't';
			*cp2 = ';';
			break;
			default:
			*cp2 = str[i];
			break;
			}
		}
	*cp2 = '\0';
	}

void
httpd_send_err( httpd_conn* hc, int status, char* title, char* extraheads, const char* form, const char* arg )
	{
	char defanged_arg[1000], buf[2000];

	/* log server error */
	if (status>=500)
		syslog( LOG_ERR, "HTTP %d (%.80s) - %m \"%.80s\"",status,arg,hc->encodedurl );

	send_mime(
		hc, status, title, "", extraheads, "text/html; charset=%s", (off_t) -1,
		(time_t) 0 );
	if ( hc->method == METHOD_HEAD ) {
		httpd_write_response( hc );
		return;
	}

	(void) snprintf( buf, sizeof(buf), "\
<HTML>\n\
<HEAD><TITLE>%d %s</TITLE></HEAD>\n\
<BODY BGCOLOR=\"#cc9999\" TEXT=\"#000000\" LINK=\"#2020ff\" VLINK=\"#4040cc\">\n\
<H2>%d %s</H2>\n",
		status, title, status, title );
	add_response( hc, buf );
	defang( arg, defanged_arg, sizeof(defanged_arg) );
	(void) snprintf( buf, sizeof(buf), form, defanged_arg );
	add_response( hc, buf );
	/* if ( match( "**MSIE**", hc->useragent ) )
	// Fuck off old (~#!) MSIE !!
		{
		int n;
		add_response( hc, "<!--\n" );
		for ( n = 0; n < 6; ++n )
			add_response( hc, "Padding so that MSIE deigns to show this error instead of its own canned one.\n");
		add_response( hc, "-->\n" );
		} */
	send_response_tail( hc );

	httpd_write_response( hc );
	}

/* Only used by httpd_parse_resp() which control data and syslog errors itself.
 */
static void httpd_send_err2(int fd, int status, char* title, const char* form)
	{
	httpd_dprintf(fd,"HTTP/1.1 %d %s\015\012Server: %s\015\012Content-Type: text/html\015\012Accept-Ranges: bytes\015\012Connection: close\015\012\015\012", status, title, EXPOSED_SERVER_SOFTWARE);
	httpd_dprintf(fd,"\
<HTML>\n\
<HEAD><TITLE>%d %s</TITLE></HEAD>\n\
<BODY BGCOLOR=\"#cc9999\" TEXT=\"#000000\" LINK=\"#2020ff\" VLINK=\"#4040cc\">\n\
<H2>%d %s</H2>\n",status, title, status, title );
	httpd_dprintf(fd,form,"");
	httpd_dprintf(fd, "\
<HR>\n\
<ADDRESS><A HREF=\"%s\">%s</A></ADDRESS>\n\
</BODY>\n\
</HTML>\n",
		SOFTWARE_ADDRESS, EXPOSED_SERVER_SOFTWARE );
}

#ifdef AUTH_FILE
static void
send_authenticate( httpd_conn* hc, char* realm )
	{
	static char* header;
	static size_t maxheader = 0;
	static char headstr[] = "WWW-Authenticate: Basic realm=\"";

	httpd_realloc_str(
		&header, &maxheader, sizeof(headstr) + strlen( realm ) + 3 );
	(void) snprintf( header, maxheader, "%s%s\"\015\012", headstr, realm );
	httpd_send_err( hc, 401, err401title, header, err401form, hc->encodedurl );
	/* If the request was a POST then there might still be data to be read,
	** so we need to do a lingering close.
	*/
	if ( hc->method == METHOD_POST )
		hc->bfield |= HC_SHOULD_LINGER;
	}


/* Base-64 decoding.  This represents binary data as printable ASCII
** characters.  Three 8-bit binary bytes are turned into four 6-bit
** values, like so:
**
**   [11111111]  [22222222]  [33333333]
**
**   [111111] [112222] [222233] [333333]
**
** Then the 6-bit values are represented using the characters "A-Za-z0-9+/".
*/

static int b64_decode_table[256] = {
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 00-0F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 10-1F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,  /* 20-2F */
	52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,  /* 30-3F */
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,  /* 40-4F */
	15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,  /* 50-5F */
	-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,  /* 60-6F */
	41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,  /* 70-7F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 80-8F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 90-9F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* A0-AF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* B0-BF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* C0-CF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* D0-DF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* E0-EF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1   /* F0-FF */
	};

/* Do base-64 decoding on a string.  Ignore any non-base64 bytes.
** Return the actual number of bytes generated.  The decoded size will
** be at most 3/4 the size of the encoded, and may be smaller if there
** are padding characters (blanks, newlines).
*/
static int
b64_decode( const char* str, unsigned char* space, int size )
	{
	const char* cp;
	int space_idx, phase;
	int d, prev_d = 0;
	unsigned char c;

	space_idx = 0;
	phase = 0;
	for ( cp = str; *cp != '\0'; ++cp )
		{
		d = b64_decode_table[(int) *cp];
		if ( d != -1 )
			{
			switch ( phase )
				{
				case 0:
				++phase;
				break;
				case 1:
				c = ( ( prev_d << 2 ) | ( ( d & 0x30 ) >> 4 ) );
				if ( space_idx < size )
					space[space_idx++] = c;
				++phase;
				break;
				case 2:
				c = ( ( ( prev_d & 0xf ) << 4 ) | ( ( d & 0x3c ) >> 2 ) );
				if ( space_idx < size )
					space[space_idx++] = c;
				++phase;
				break;
				case 3:
				c = ( ( ( prev_d & 0x03 ) << 6 ) | d );
				if ( space_idx < size )
					space[space_idx++] = c;
				phase = 0;
				break;
				}
			prev_d = d;
			}
		}
	return space_idx;
	}

/* Returns -1 == unauthorized, 0 == no auth file, 1 = authorized. */
static int
auth_check( httpd_conn* hc)
	{
	static char* authpath;
	static size_t maxauthpath = 0;
	char * dirname, *cp;
	struct stat sb;
	char authinfo[550];
	char* authpass;
	char* colon;
	int l;
	FILE* fp;
	char line[500];
	char* cryp;
	static char* prevauthpath;
	static size_t maxprevauthpath = 0;
	static time_t prevmtime;
	static char* prevuser;
	static size_t maxprevuser = 0;
	static char* prevcryp;
	static size_t maxprevcryp = 0;


	if ( S_ISDIR(hc->sb.st_mode) )
		dirname=hc->realfilename;
	else {
		httpd_realloc_str( &hc->tmpbuff, &hc->maxtmpbuff, strlen(hc->realfilename) );
		dirname=hc->tmpbuff;
		(void) strcpy( dirname, hc->realfilename );
		cp = strrchr( dirname, '/' );
		if ( cp == (char*) 0 )
			(void) strcpy( dirname, "." );
		else
			*cp = '\0';
	}


	/* Construct auth filename. */
	httpd_realloc_str(
		&authpath, &maxauthpath, strlen( dirname ) + 1 + sizeof(AUTH_FILE) );
	(void) snprintf( authpath, maxauthpath, "%s/%s", dirname, AUTH_FILE );

			
	/* Does this directory have an auth file? */
	if ( stat( authpath, &sb ) < 0 )
		/* Nope, let the request go through. */
		return 0;

	/* Does this request contain basic authorization info? */
	if ( hc->authorization[0] == '\0' ||
		 strncmp( hc->authorization, "Basic ", 6 ) != 0 )
		{
		/* Nope, return a 401 Unauthorized. */
		send_authenticate( hc, dirname );
		return -1;
		}

	/* Is it the authorization file ? */
	if ( !S_ISDIR(hc->sb.st_mode) && 
			(!strcmp( hc->realfilename, AUTH_FILE ) /* for the root directory */
			|| !strcmp(hc->realfilename, authpath) ) )
		{
		syslog(
			LOG_NOTICE,
			"%.80s URL \"%.80s\" tried to retrieve an auth file",
			hc->client_addr, hc->encodedurl );
		httpd_send_err(
			hc, 403, err403title, "",
			ERROR_FORM( err403form, "The requested URL '%.80s' is an authorization file, retrieving it is not permitted.\n" ), hc->encodedurl );
		return -1;
		}
	/* Note : Jef's code for previous test was not bad : 
	if ( expnlen == sizeof(AUTH_FILE)-1 &&  strcmp( hc->expnfilename, AUTH_FILE ) == 0 )
	else if ( expnlen >= sizeof(AUTH_FILE) &&
			  strcmp( &(hc->expnfilename[expnlen - sizeof(AUTH_FILE) + 1]), AUTH_FILE ) == 0 &&
			  hc->expnfilename[expnlen - sizeof(AUTH_FILE)] == '/' ) */
	

	/* Decode it. */
	l = b64_decode(
		&(hc->authorization[6]), (unsigned char*) authinfo,
		sizeof(authinfo) - 1 );
	authinfo[l] = '\0';
	/* Split into user and password. */
	authpass = strchr( authinfo, ':' );
	if ( authpass == (char*) 0 )
		{
		/* No colon?  Bogus auth info. */
		send_authenticate( hc, dirname );
		return -1;
		}
	*authpass++ = '\0';
	/* If there are more fields, cut them off. */
	colon = strchr( authpass, ':' );
	if ( colon != (char*) 0 )
		*colon = '\0';

	/* See if we have a cached entry and can use it. */
	if ( maxprevauthpath != 0 &&
		 strcmp( authpath, prevauthpath ) == 0 &&
		 sb.st_mtime == prevmtime &&
		 strcmp( authinfo, prevuser ) == 0 )
		{
		/* Yes.  Check against the cached encrypted password. */
		if ( strcmp( crypt( authpass, prevcryp ), prevcryp ) == 0 )
			{
			/* Ok! */
			httpd_realloc_str(
				&hc->remoteuser, &hc->maxremoteuser, strlen( authinfo ) );
			(void) strcpy( hc->remoteuser, authinfo );
			return 1;
			}
		else
			{
			/* No. */
			send_authenticate( hc, dirname );
			return -1;
			}
		}

	/* Open the password file. */
	fp = fopen( authpath, "r" );
	if ( fp == (FILE*) 0 )
		{
		/* The file exists but we can't open it?  Disallow access. */
		syslog(
			LOG_ERR, "%.80s auth file %.80s could not be opened - %m",
			hc->client_addr, authpath );
		httpd_send_err(
			hc, 403, err403title, "",
			ERROR_FORM( err403form, "The requested URL '%.80s' is protected by an authentication file, but the authentication file cannot be opened.\n" ),
			hc->encodedurl );
		return -1;
		}

	/* Read it. */
	while ( fgets( line, sizeof(line), fp ) != (char*) 0 )
		{
		/* Nuke newline. */
		l = strlen( line );
		if ( line[l - 1] == '\n' )
			line[l - 1] = '\0';
		/* Split into user and encrypted password. */
		cryp = strchr( line, ':' );
		if ( cryp == (char*) 0 )
			continue;
		*cryp++ = '\0';
		/* Is this the right user? */
		if ( strcmp( line, authinfo ) == 0 )
			{
			/* Yes. */
			(void) fclose( fp );
			/* So is the password right? */
			if ( strcmp( crypt( authpass, cryp ), cryp ) == 0 )
				{
				/* Ok! */
				httpd_realloc_str(
					&hc->remoteuser, &hc->maxremoteuser, strlen( line ) );
				(void) strcpy( hc->remoteuser, line );
				/* And cache this user's info for next time. */
				httpd_realloc_str(
					&prevauthpath, &maxprevauthpath, strlen( authpath ) );
				(void) strcpy( prevauthpath, authpath );
				prevmtime = sb.st_mtime;
				httpd_realloc_str(
					&prevuser, &maxprevuser, strlen( authinfo ) );
				(void) strcpy( prevuser, authinfo );
				httpd_realloc_str( &prevcryp, &maxprevcryp, strlen( cryp ) );
				(void) strcpy( prevcryp, cryp );
				return 1;
				}
			else
				{
				/* No. */
				send_authenticate( hc, dirname );
				return -1;
				}
			}
		}

	/* Didn't find that user.  Access denied. */
	(void) fclose( fp );
	send_authenticate( hc, dirname );
	return -1;
	}

#endif /* AUTH_FILE */


static void
send_dirredirect( httpd_conn* hc )
	{
	static char* location;
	static char* header;
	static size_t maxlocation = 0, maxheader = 0;
	static char headstr[] = "Location: ";

	if ( hc->query[0] != '\0')
		{
		char* cp = strchr( hc->encodedurl, '?' );
		if ( cp != (char*) 0 )		/* should always find it */
			*cp = '\0';
		httpd_realloc_str(
			&location, &maxlocation,
			strlen( hc->encodedurl ) + 2 + strlen( hc->query ) );
		(void) snprintf( location, maxlocation,
			"%s/?%s", hc->encodedurl, hc->query );
		}
	else
		{
		httpd_realloc_str(
			&location, &maxlocation, strlen( hc->encodedurl ) + 1 );
		(void) snprintf( location, maxlocation,
			"%s/", hc->encodedurl );
		}
	httpd_realloc_str(
		&header, &maxheader, sizeof(headstr) + strlen( location ) );
	(void) snprintf( header, maxheader,
		"%s%s\015\012", headstr, location );
	httpd_send_err( hc, 302, err302title, header, err302form, location );
	}


char*
httpd_method_str( int method )
	{
	switch ( method )
		{
		case METHOD_GET: return "GET";
		case METHOD_HEAD: return "HEAD";
		case METHOD_POST: return "POST";
		default: return "UNKNOWN";
		}
	}

static int hexit( char c ) {
	if ( c >= '0' && c <= '9' )
		return c - '0';
	if ( c >= 'a' && c <= 'f' )
		return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' )
		return c - 'A' + 10;
	return -1;	
}

/* Copies and decodes a string.  It's ok for 'from' and 'to' to be the
** same string. Return the lenght of decoded string.
*/
int strdecode( char* to, char* from ) {
	int a,b,r;
	for ( r=0 ; *from != '\0'; to++, from++, r++ ) {
		if ( from[0] == '%' && (a=hexit(from[1])) >= 0 && (b=hexit(from[2])) >= 0 ) {
		    *to = a* 16 + b;
			from += 2;
		} else
			*to = *from;
	}
	*to = '\0';
	return r;
}

/* Copies and decodes a query string.  It's ok for 'from' and 'to' to be the
** same string. Return the lenght of decoded string.
*/
int strdecodequery( char* to, char* from ) {
	int a,b,r;
	for ( r=0 ; *from != '\0'; to++, from++, r++ ) {
		if ( from[0] == '%' && (a=hexit(from[1])) >= 0 && (b=hexit(from[2])) >= 0 ) {
			*to = a* 16 + b;
			from += 2;
		} else if ( from[0] == '+' )
			*to = ' ';
		else
			*to = *from;
	}
	*to = '\0';
	return r;
}

#ifdef GENERATE_INDEXES
/* Copies and encodes a string. */
static void
strencode( char* to, int tosize, char* from )
	{
	int tolen;

	for ( tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from )
		{
		if ( isalnum(*from) || strchr( "/_.-~", *from ) != (char*) 0 )
			{
			*to = *from;
			++to;
			++tolen;
			}
		else
			{
			(void) sprintf( to, "%%%02x", (int) *from & 0xff );
			to += 3;
			tolen += 3;
			}
		}
	*to = '\0';
	}
#endif /* GENERATE_INDEXES */

#if 0
/* Expands all symlinks in the given filename, eliding ..'s and leading /'s.
** Returns the expanded path (pointer to static string), or (char*) 0 on
** errors.  Also returns, in the string pointed to by restP, any trailing
** parts of the path that don't exist.
**
** This is a fairly nice little routine.  It handles any size filenames
** without excessive mallocs.
*/
static char*
expand_symlinks( char* path, char** restP, int no_symlink_check )
	{
	static char* checked;
	static char* rest;
	char link[5000];
	static size_t maxchecked = 0, maxrest = 0;
	size_t checkedlen, restlen, linklen, prevcheckedlen, prevrestlen;
	int nlinks, i;
	char* r;
	char* cp1;
	char* cp2;

	if ( no_symlink_check )
		{
		/* If we are chrooted, we can actually skip the symlink-expansion,
		** since it's impossible to get out of the tree.  However, we still
		** need to do the pathinfo check, and the existing symlink expansion
		** code is a pretty reasonable way to do this.  So, what we do is
		** a single stat() of the whole filename - if it exists, then we
		** return it as is with nothing in restP.  If it doesn't exist, we
		** fall through to the existing code.
		**
		** One side-effect of this is that users can't symlink to central
		** approved CGIs any more.  The workaround is to use the central
		** URL for the CGI instead of a local symlinked one.
		*/
		struct stat sb;
		if ( stat( path, &sb ) != -1 )
			{
			checkedlen = strlen( path );
			httpd_realloc_str( &checked, &maxchecked, checkedlen );
			(void) strcpy( checked, path );
			/* Trim trailing slashes. */
			while ( checked[checkedlen - 1] == '/' )
				{
				checked[checkedlen - 1] = '\0';
				--checkedlen;
				}
			httpd_realloc_str( &rest, &maxrest, 0 );
			rest[0] = '\0';
			*restP = rest;
			return checked;
			}
		}

	/* Start out with nothing in checked and the whole filename in rest. */
	httpd_realloc_str( &checked, &maxchecked, 1 );
	checked[0] = '\0';
	checkedlen = 0;
	restlen = strlen( path );
	httpd_realloc_str( &rest, &maxrest, restlen );
	(void) strcpy( rest, path );
	/* Remove any leading slashes. */
	while ( rest[0] == '/' )
		{
		(void) strcpy( rest, &(rest[1]) );
		--restlen;
		}
	r = rest;
	nlinks = 0;

	/* While there are still components to check... */
	while ( restlen > 0 )
		{
		/* Save current checkedlen in case we get a symlink.  Save current
		** restlen in case we get a non-existant component.
		*/
		prevcheckedlen = checkedlen;
		prevrestlen = restlen;

		/* Grab one component from r and transfer it to checked. */
		cp1 = strchr( r, '/' );
		if ( cp1 != (char*) 0 )
			{
			i = cp1 - r;
			if ( i == 0 )
				{
				/* Special case for absolute paths. */
				httpd_realloc_str( &checked, &maxchecked, checkedlen + 1 );
				(void) strncpy( &checked[checkedlen], r, 1 );
				checkedlen += 1;
				}
			else if ( strncmp( r, "..", MAX( i, 2 ) ) == 0 )
				{
				/* Ignore ..'s that go above the start of the path. */
				if ( checkedlen != 0 )
					{
					cp2 = strrchr( checked, '/' );
					if ( cp2 == (char*) 0 )
						checkedlen = 0;
					else if ( cp2 == checked )
						checkedlen = 1;
					else
						checkedlen = cp2 - checked;
					}
				}
			else
				{
				httpd_realloc_str( &checked, &maxchecked, checkedlen + 1 + i );
				if ( checkedlen > 0 && checked[checkedlen-1] != '/' )
					checked[checkedlen++] = '/';
				(void) strncpy( &checked[checkedlen], r, i );
				checkedlen += i;
				}
			checked[checkedlen] = '\0';
			r += i + 1;
			restlen -= i + 1;
			}
		else
			{
			/* No slashes remaining, r is all one component. */
			if ( strcmp( r, ".." ) == 0 )
				{
				/* Ignore ..'s that go above the start of the path. */
				if ( checkedlen != 0 )
					{
					cp2 = strrchr( checked, '/' );
					if ( cp2 == (char*) 0 )
						checkedlen = 0;
					else if ( cp2 == checked )
						checkedlen = 1;
					else
						checkedlen = cp2 - checked;
					checked[checkedlen] = '\0';
					}
				}
			else
				{
				httpd_realloc_str(
					&checked, &maxchecked, checkedlen + 1 + restlen );
				if ( checkedlen > 0 && checked[checkedlen-1] != '/' )
					checked[checkedlen++] = '/';
				(void) strcpy( &checked[checkedlen], r );
				checkedlen += restlen;
				}
			r += restlen;
			restlen = 0;
			}

		/* Try reading the current filename as a symlink */
		if ( checked[0] == '\0' )
			continue;
		linklen = readlink( checked, link, sizeof(link) - 1 );
		if ( linklen == -1 )
			{
			if ( errno == EINVAL )
				continue;			   /* not a symlink */
			if ( errno == EACCES || errno == ENOENT || errno == ENOTDIR )
				{
				/* That last component was bogus.  Restore and return. */
				*restP = r - ( prevrestlen - restlen );
				if ( prevcheckedlen == 0 )
					(void) strcpy( checked, "." );
				else
					checked[prevcheckedlen] = '\0';
				return checked;
				}
			syslog( LOG_ERR, "readlink %.80s - %m", checked );
			return (char*) 0;
			}
		++nlinks;
		if ( nlinks > MAX_LINKS )
			{
			syslog( LOG_ERR, "too many symlinks in %.80s", path );
			return (char*) 0;
			}
		link[linklen] = '\0';
		if ( link[linklen - 1] == '/' )
			link[--linklen] = '\0';	 /* trim trailing slash */

		/* Insert the link contents in front of the rest of the filename. */
		if ( restlen != 0 )
			{
			(void) strcpy( rest, r );
			httpd_realloc_str( &rest, &maxrest, restlen + linklen + 1 );
			for ( i = restlen; i >= 0; --i )
				rest[i + linklen + 1] = rest[i];
			(void) strcpy( rest, link );
			rest[linklen] = '/';
			restlen += linklen + 1;
			r = rest;
			}
		else
			{
			/* There's nothing left in the filename, so the link contents
			** becomes the rest.
			*/
			httpd_realloc_str( &rest, &maxrest, linklen );
			(void) strcpy( rest, link );
			restlen = linklen;
			r = rest;
			}

		if ( rest[0] == '/' )
			{
			/* There must have been an absolute symlink - zero out checked. */
			checked[0] = '\0';
			checkedlen = 0;
			}
		else
			{
			/* Re-check this component. */
			checkedlen = prevcheckedlen;
			checked[checkedlen] = '\0';
			}
		}

	/* Ok. */
	*restP = r;
	if ( checked[0] == '\0' )
		(void) strcpy( checked, "." );
	return checked;
	}
#endif

int
httpd_get_conn( httpd_server* hs, int listen_fd, httpd_conn* hc )
	{
	struct sockaddr sa;
	socklen_t sz;

	if ( ! hc->initialized )
		{
		hc->read_size = 0;
		httpd_realloc_str( &hc->read_buf, &hc->read_size, 500 );
		hc->maxdecodedurl =
			hc->maxorigfilename = hc->maxencodings =
			hc->maxtmpbuff = hc->maxquery = hc->maxaccept =
			hc->maxaccepte = hc->maxreqhost = hc->maxhostdir =
			hc->maxremoteuser = hc->maxresponse = 0;
		httpd_realloc_str( &hc->decodedurl, &hc->maxdecodedurl, 1 );
		httpd_realloc_str( &hc->origfilename, &hc->maxorigfilename, 1 );
		httpd_realloc_str( &hc->encodings, &hc->maxencodings, 0 );
		httpd_realloc_str( &hc->query, &hc->maxquery, 0 );
		httpd_realloc_str( &hc->accept, &hc->maxaccept, 0 );
		httpd_realloc_str( &hc->accepte, &hc->maxaccepte, 0 );
		httpd_realloc_str( &hc->reqhost, &hc->maxreqhost, 0 );
		httpd_realloc_str( &hc->hostdir, &hc->maxhostdir, 0 );
		httpd_realloc_str( &hc->remoteuser, &hc->maxremoteuser, 0 );
		httpd_realloc_str( &hc->response, &hc->maxresponse, 0 );
		hc->initialized = 1;
		}

	/* Accept the new connection. */
	sz = sizeof(sa);
	hc->conn_fd = accept( listen_fd, &sa, &sz );
	if ( hc->conn_fd < 0 )
		{
		if ( errno == EWOULDBLOCK )
			return GC_NO_MORE;
		syslog( LOG_ERR, "accept - %m" );
		return GC_FAIL;
		}
	if ( ! sockaddr_check( &sa ) )
		{
		syslog( LOG_ERR, "unknown sockaddr family" );
		close( hc->conn_fd );
		hc->conn_fd = -1;
		return GC_FAIL;
		}
	(void) fcntl( hc->conn_fd, F_SETFD, 1 );
	hc->hs = hs;
	hc->client_addr=get_ip_str(&sa);
	hc->read_idx = 0;
	hc->checked_idx = 0;
	hc->checked_state = CHST_FIRSTWORD;
	hc->method = METHOD_UNKNOWN;
	hc->status = 0;
	hc->bytes_to_send = -1;
	hc->bytes_sent = 0;
	hc->encodedurl = "";
	hc->decodedurl[0] = '\0';
	hc->protocol = "UNKNOWN";
	hc->origfilename[0] = '\0';
	hc->encodings[0] = '\0';
	hc->query[0] = '\0';
	hc->referer = "";
	hc->useragent = "";
	hc->accept[0] = '\0';
	hc->accepte[0] = '\0';
	hc->acceptl = "";
	hc->cookie = "";
	hc->contenttype = "";
	hc->reqhost[0] = '\0';
	hc->hdrhost = "";
	hc->hostdir[0] = '\0';
	hc->authorization = "";
	hc->forwardedfor = "";
	hc->remoteuser[0] = '\0';
	hc->response[0] = '\0';
	hc->responselen = 0;
	hc->bytesranges = "";
	hc->if_modified_since = (time_t) -1;
	hc->range_if = (time_t) -1;
	hc->contentlength = -1;
	hc->type = "";
	hc->http_version=10;
	hc->first_byte_index = 0;
	hc->last_byte_index = -1;
	hc->bfield=0;
	hc->file_address = (char*) 0;
	hc->boundary[0] = '\0';
	return GC_OK;
	}


/* Checks hc->read_buf to see whether a complete request has been read so far;
** either the first line has two words (an HTTP/0.9 request), or the first
** line has three words and there's a blank line present.
**
** hc->read_idx is how much has been read in; hc->checked_idx is how much we
** have checked so far; and hc->checked_state is the current state of the
** finite state machine.
*/
int
httpd_got_request( httpd_conn* hc )
	{
	char c;

	for ( ; hc->checked_idx < hc->read_idx; ++hc->checked_idx )
		{
		c = hc->read_buf[hc->checked_idx];
		switch ( hc->checked_state )
			{
			case CHST_FIRSTWORD:
			switch ( c )
				{
				case ' ': case '\t':
				hc->checked_state = CHST_FIRSTWS;
				break;
				case '\012': case '\015':
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;
				}
			break;
			case CHST_FIRSTWS:
			switch ( c )
				{
				case ' ': case '\t':
				break;
				case '\012': case '\015':
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;
				default:
				hc->checked_state = CHST_SECONDWORD;
				break;
				}
			break;
			case CHST_SECONDWORD:
			switch ( c )
				{
				case ' ': case '\t':
				hc->checked_state = CHST_SECONDWS;
				break;
				case '\012': case '\015':
				/* The first line has only two words - an HTTP/0.9 request. */
				return GR_GOT_REQUEST;
				}
			break;
			case CHST_SECONDWS:
			switch ( c )
				{
				case ' ': case '\t':
				break;
				case '\012': case '\015':
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;
				default:
				hc->checked_state = CHST_THIRDWORD;
				break;
				}
			break;
			case CHST_THIRDWORD:
			switch ( c )
				{
				case ' ': case '\t':
				hc->checked_state = CHST_THIRDWS;
				break;
				case '\012':
				hc->checked_state = CHST_LF;
				break;
				case '\015':
				hc->checked_state = CHST_CR;
				break;
				}
			break;
			case CHST_THIRDWS:
			switch ( c )
				{
				case ' ': case '\t':
				break;
				case '\012':
				hc->checked_state = CHST_LF;
				break;
				case '\015':
				hc->checked_state = CHST_CR;
				break;
				default:
				hc->checked_state = CHST_BOGUS;
				return GR_BAD_REQUEST;
				}
			break;
			case CHST_LINE:
			switch ( c )
				{
				case '\012':
				hc->checked_state = CHST_LF;
				break;
				case '\015':
				hc->checked_state = CHST_CR;
				break;
				}
			break;
			case CHST_LF:
			switch ( c )
				{
				case '\012':
				/* Two newlines in a row - a blank line - end of request. */
				return GR_GOT_REQUEST;
				case '\015':
				hc->checked_state = CHST_CR;
				break;
				default:
				hc->checked_state = CHST_LINE;
				break;
				}
			break;
			case CHST_CR:
			switch ( c )
				{
				case '\012':
				hc->checked_state = CHST_CRLF;
				break;
				case '\015':
				/* Two returns in a row - end of request. */
				return GR_GOT_REQUEST;
				default:
				hc->checked_state = CHST_LINE;
				break;
				}
			break;
			case CHST_CRLF:
			switch ( c )
				{
				case '\012':
				/* Two newlines in a row - end of request. */
				return GR_GOT_REQUEST;
				case '\015':
				hc->checked_state = CHST_CRLFCR;
				break;
				default:
				hc->checked_state = CHST_LINE;
				break;
				}
			break;
			case CHST_CRLFCR:
			switch ( c )
				{
				case '\012': case '\015':
				/* Two CRLFs or two CRs in a row - end of request. */
				return GR_GOT_REQUEST;
				default:
				hc->checked_state = CHST_LINE;
				break;
				}
			break;
			case CHST_BOGUS:
			return GR_BAD_REQUEST;
			}
		}
	return GR_NO_REQUEST;
	}


int
httpd_parse_request( httpd_conn* hc )
	{
	char* buf;
	char* method_str;
	char* url;
	char* protocol;
	char* reqhost;
	char* eol;
	char* cp;

	hc->checked_idx = 0;		/* reset */
	method_str = bufgets( hc );
	url = strpbrk( method_str, " \t\012\015" );
	if ( url == (char*) 0 )
		{
		httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
		return -1;
		}
	*url++ = '\0';
	url += strspn( url, " \t\012\015" );
	protocol = strpbrk( url, " \t\012\015" );
	if ( protocol == (char*) 0 )
		{
		protocol = "HTTP/0.9";
		hc->http_version = 9;
		}
	else
		{
		*protocol++ = '\0';
		protocol += strspn( protocol, " \t\012\015" );
		if ( *protocol != '\0' )
			{
			eol = strpbrk( protocol, " \t\012\015" );
			if ( eol != (char*) 0 )
				*eol = '\0';
			if ( strcasecmp( protocol, "HTTP/1.0" ) != 0 )
				hc->http_version = 11;
			}
		}
	hc->protocol = protocol;

	/* Check for HTTP/1.1 absolute URL. */
	if ( strncasecmp( url, "http://", 7 ) == 0 )
		{
		if ( hc->http_version < 11 )
			{
			httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
			return -1;
			}
		reqhost = url + 7;
		url = strchr( reqhost, '/' );
		if ( url == (char*) 0 )
			{
			httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
			return -1;
			}
		*url = '\0';
		if ( /*strchr( reqhost, '/' ) != (char*) 0 || *(useless)*/ reqhost[0] == '.' )
			{
			httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
			return -1;
			}
		httpd_realloc_str( &hc->reqhost, &hc->maxreqhost, strlen( reqhost ) );
		(void) strcpy( hc->reqhost, reqhost );
		*url = '/';
		}

	if ( *url != '/' )
		{
		httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
		return -1;
		}

	if ( strcasecmp( method_str, httpd_method_str( METHOD_GET ) ) == 0 )
		hc->method = METHOD_GET;
	else if ( strcasecmp( method_str, httpd_method_str( METHOD_HEAD ) ) == 0 )
		hc->method = METHOD_HEAD;
	else if ( strcasecmp( method_str, httpd_method_str( METHOD_POST ) ) == 0 )
		hc->method = METHOD_POST;
	else
		{
		httpd_send_err( hc, 501, err501title, "", err501form, method_str );
		return -1;
		}

	hc->encodedurl = url;
	httpd_realloc_str(
		&hc->decodedurl, &hc->maxdecodedurl, strlen( hc->encodedurl ) );
	strdecode( hc->decodedurl, hc->encodedurl );

	httpd_realloc_str(
		&hc->origfilename, &hc->maxorigfilename, strlen( hc->decodedurl ) );
	(void) strcpy( hc->origfilename, &hc->decodedurl[1] );
	/* Special case for top-level URL. */
	if ( hc->origfilename[0] == '\0' )
		(void) strcpy( hc->origfilename, "." );

	/* Extract query string from encoded URL. */
	cp = strchr( hc->encodedurl, '?' );
	if ( cp != (char*) 0 )
		{
		++cp;
		httpd_realloc_str( &hc->query, &hc->maxquery, strlen( cp ) );
		(void) strcpy( hc->query, cp );
		/* Remove query from (decoded) origfilename. */
		cp = strchr( hc->origfilename, '?' );
		if ( cp != (char*) 0 )
			*cp = '\0';
		}

	de_dotdot( hc->origfilename );
	if ( hc->origfilename[0] == '/' ||
		 ( hc->origfilename[0] == '.' && hc->origfilename[1] == '.' &&
		   ( hc->origfilename[2] == '\0' || hc->origfilename[2] == '/' ) ) )
		{
		httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
		return -1;
		}

	if ( hc->http_version > 9 )
		{
		/* Read the MIME headers. */
		while ( ( buf = bufgets( hc ) ) != (char*) 0 )
			{
			if ( buf[0] == '\0' )
				break;
			if ( strncasecmp( buf, "Referer:", 8 ) == 0 )
				{
				cp = &buf[8];
				cp += strspn( cp, " \t" );
				hc->referer = cp;
				}
			else if ( strncasecmp( buf, "User-Agent:", 11 ) == 0 )
				{
				cp = &buf[11];
				cp += strspn( cp, " \t" );
				hc->useragent = cp;
				}
			else if ( strncasecmp( buf, "Host:", 5 ) == 0 )
				{
				cp = &buf[5];
				cp += strspn( cp, " \t" );
				hc->hdrhost = cp;
				if ( strchr( hc->hdrhost, '/' ) != (char*) 0 || hc->hdrhost[0] == '.' )
					{
					httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
					return -1;
					}
				}
			else if ( strncasecmp( buf, "Accept:", 7 ) == 0 )
				{
				cp = &buf[7];
				cp += strspn( cp, " \t" );
				if ( hc->accept[0] != '\0' )
					{
					if ( strlen( hc->accept ) > 5000 )
						{
						syslog(
							LOG_ERR, "%.80s way too much Accept: data",
							hc->client_addr );
						continue;
						}
					httpd_realloc_str(
						&hc->accept, &hc->maxaccept,
						strlen( hc->accept ) + 2 + strlen( cp ) );
					(void) strcat( hc->accept, ", " );
					}
				else
					httpd_realloc_str(
						&hc->accept, &hc->maxaccept, strlen( cp ) );
				(void) strcat( hc->accept, cp );
				}
			else if ( strncasecmp( buf, "Accept-Encoding:", 16 ) == 0 )
				{
				cp = &buf[16];
				cp += strspn( cp, " \t" );
				if ( hc->accepte[0] != '\0' )
					{
					if ( strlen( hc->accepte ) > 5000 )
						{
						syslog(
							LOG_ERR, "%.80s way too much Accept-Encoding: data",
							hc->client_addr );
						continue;
						}
					httpd_realloc_str(
						&hc->accepte, &hc->maxaccepte,
						strlen( hc->accepte ) + 2 + strlen( cp ) );
					(void) strcat( hc->accepte, ", " );
					}
				else
					httpd_realloc_str(
						&hc->accepte, &hc->maxaccepte, strlen( cp ) );
				(void) strcpy( hc->accepte, cp );
				}
			else if ( strncasecmp( buf, "Accept-Language:", 16 ) == 0 )
				{
				cp = &buf[16];
				cp += strspn( cp, " \t" );
				hc->acceptl = cp;
				}
			else if ( strncasecmp( buf, "If-Modified-Since:", 18 ) == 0 )
				{
				cp = &buf[18];
				hc->if_modified_since = tdate_parse( cp );
				if ( hc->if_modified_since == (time_t) -1 )
					syslog( LOG_DEBUG, "unparsable time: %.80s", cp );
				}
			else if ( strncasecmp( buf, "Cookie:", 7 ) == 0 )
				{
				cp = &buf[7];
				cp += strspn( cp, " \t" );
				hc->cookie = cp;
				}
			else if ( strncasecmp( buf, "Range:", 6 ) == 0 )
				{
				/* Only support "%d-", "%d-%d" and "-%d" using fdwatch and mmap.
				 * TODO: support multirange ("%d-%d,%d-%d,%d-") using a fork() */
				cp = &buf[6];
				cp += strspn( cp, " \t" );

				/* http://www.w3.org/Protocols/rfc2616/rfc2616-sec3.html#sec3.12 */
				if ( strncasecmp( cp, "bytes", 5 ) == 0
						&& (cp+=5) && (*cp == '=' || *cp == ' ' || *cp == '\t')
						&& (cp = strpbrk( cp, "=" )) )
					{
					cp++;
					if ( strchr( cp, ',' ) == (char*) 0 )
						{
						char* cp_dash;
						cp_dash = strrchr( cp, '-' );
						if ( cp_dash != (char*) 0 )
							{
							cp_dash++;
							hc->bfield |= HC_GOT_RANGE;
							hc->first_byte_index = atoll( cp );
							/* (hc->first_byte_index<0) means we are in a "-%d" case */
							if ( hc->first_byte_index >= 0 && isdigit( (int) *cp_dash ) )
								hc->last_byte_index = atoll( cp_dash );
							}
						}
					else
						hc->bytesranges = cp;
					}
					
				}
			else if ( strncasecmp( buf, "If-Range:", 9 ) == 0 )
				{
				cp = &buf[9];
				hc->range_if = tdate_parse( cp );
				if ( hc->range_if == (time_t) -1 )
					syslog( LOG_DEBUG, "unparsable time: %.80s", cp );
				}
			else if ( strncasecmp( buf, "Content-Type:", 13 ) == 0 )
				{
				cp = &buf[13];
				cp += strspn( cp, " \t" );
				hc->contenttype = cp;
				}
			else if ( strncasecmp( buf, "Content-Length:", 15 ) == 0 )
				{
				cp = &buf[15];
				hc->contentlength = atol( cp );
				}
			else if ( strncasecmp( buf, "Authorization:", 14 ) == 0 )
				{
				cp = &buf[14];
				cp += strspn( cp, " \t" );
				hc->authorization = cp;
				}
			else if ( strncasecmp( buf, "Connection:", 11 ) == 0 )
				{
				cp = &buf[11];
				cp += strspn( cp, " \t" );
				if ( strcasecmp( cp, "keep-alive" ) == 0 )
					hc->bfield |= HC_KEEP_ALIVE;
				}
			else if ( strncasecmp( buf, "X-Forwarded-For:", 16 ) == 0 )
				{
				cp = &buf[16];
				cp += strspn( cp, " \t" );
				hc->forwardedfor=cp;
				}
#ifdef LOG_UNKNOWN_HEADERS
			else if ( strncasecmp( buf, "Accept-Charset:", 15 ) == 0 ||
					  strncasecmp( buf, "Accept-Language:", 16 ) == 0 ||
					  strncasecmp( buf, "Agent:", 6 ) == 0 ||
					  strncasecmp( buf, "Cache-Control:", 14 ) == 0 ||
					  strncasecmp( buf, "Cache-Info:", 11 ) == 0 ||
					  strncasecmp( buf, "Charge-To:", 10 ) == 0 ||
					  strncasecmp( buf, "Client-IP:", 10 ) == 0 ||
					  strncasecmp( buf, "Date:", 5 ) == 0 ||
					  strncasecmp( buf, "Extension:", 10 ) == 0 ||
					  strncasecmp( buf, "Forwarded:", 10 ) == 0 ||
					  strncasecmp( buf, "From:", 5 ) == 0 ||
					  strncasecmp( buf, "HTTP-Version:", 13 ) == 0 ||
					  strncasecmp( buf, "Max-Forwards:", 13 ) == 0 ||
					  strncasecmp( buf, "Message-Id:", 11 ) == 0 ||
					  strncasecmp( buf, "MIME-Version:", 13 ) == 0 ||
					  strncasecmp( buf, "Negotiate:", 10 ) == 0 ||
					  strncasecmp( buf, "Pragma:", 7 ) == 0 ||
					  strncasecmp( buf, "Proxy-Agent:", 12 ) == 0 ||
					  strncasecmp( buf, "Proxy-Connection:", 17 ) == 0 ||
					  strncasecmp( buf, "Security-Scheme:", 16 ) == 0 ||
					  strncasecmp( buf, "Session-Id:", 11 ) == 0 ||
					  strncasecmp( buf, "UA-Color:", 9 ) == 0 ||
					  strncasecmp( buf, "UA-CPU:", 7 ) == 0 ||
					  strncasecmp( buf, "UA-Disp:", 8 ) == 0 ||
					  strncasecmp( buf, "UA-OS:", 6 ) == 0 ||
					  strncasecmp( buf, "UA-Pixels:", 10 ) == 0 ||
					  strncasecmp( buf, "User:", 5 ) == 0 ||
					  strncasecmp( buf, "Via:", 4 ) == 0 ||
					  strncasecmp( buf, "X-", 2 ) == 0 )
				; /* ignore */
			else
				syslog( LOG_DEBUG, "unknown request header: %.80s", buf );
#endif /* LOG_UNKNOWN_HEADERS */
			}
		}

	if ( hc->http_version > 10 )
		{
		/* Check that HTTP/1.1 requests specify a host, as required. */
		if ( hc->reqhost[0] == '\0' && hc->hdrhost[0] == '\0' )
			{
			httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
			return -1;
			}

		/* If the client wants to do keep-alives, it might also be doing
		** pipelining.  There's no way for us to tell.  Since we don't
		** implement keep-alives yet, if we close such a connection there
		** might be unread pipelined requests waiting.  So, we have to
		** do a lingering close.
		*/
		if ( hc->bfield & HC_KEEP_ALIVE )
			hc->bfield |= HC_SHOULD_LINGER;
		}

	/* Detach sign asked, response inspired from rfc3156 (which is for emails) */
	if ( hc->hs->sig_pattern != (char*) 0
#ifdef HAVE_STRCASESTR
		&& strcasestr(hc->accept,"multipart/msigned")
#else
		&& strstr(hc->accept,"multipart/msigned") /* won't work if client use funny upper cases :'-( :-p */
#endif
		&& !match( hc->hs->sig_pattern, hc->origfilename ) )
			hc->bfield |= HC_DETACH_SIGN;

	/* Ok, the request has been parsed.  Now we resolve stuff that
	** may require the entire request.
	*/

	/* Tilde mapping. */
	/*if ( hc->expnfilename[0] == '~' )
		{}*/

	//syslog( LOG_DEBUG, "hc->realfilename: %s", hc->realfilename ); // hc->realfilename should be null
#ifdef VHOSTING
	/* Virtual host mapping ("el cheapo"). */
	/* We differ a little bit from specification (http://www.w3.org/Protocols/rfc2616/rfc2616-sec5.html#sec5.2):
	 * if the given host has no specific subdirectory, we ignore it and serve ressource
	 * from the main directory ( while RFC 2616 tells to return a HTTP error 400)
	 */
	if ( hc->hs->bfield & HS_VIRTUAL_HOST ) {
		char * hostdir, * toexpand;
		struct stat sb;
		int len, lenh;

		hostdir= hc->reqhost[0] != '\0' ? hc->reqhost : hc->hdrhost ;
		toexpand=hc->origfilename;

		if ( hostdir[0] != '\0' ) {
			/* Remove port number if given (Note: we assume that given host isn't an IPv6 address) */
			cp = strchr( hostdir, ':' );
			if ( cp != (char*) 0 )
				*cp = '\0';

			if ( stat( hostdir, &sb ) == 0 ) {
				lenh=strlen(hostdir);

				/* copy hostdir to hc->hostdir (used by make_log_entry) */
				httpd_realloc_str( &hc->hostdir, &hc->maxhostdir, lenh );
				strcpy( hc->hostdir, hostdir );
				/* If http log analysers dislike missing '/' in the begining of an url,
				 * use following code instead.
				httpd_realloc_str( &hc->hostdir, &hc->maxhostdir, lenh + 1 );
				hc->hostdir[0]='/';
				strcpy( &hc->hostdir[1], hostdir ); */

				/* Prepend hostdir to the filename. */
				len=strlen(hc->origfilename);
				httpd_realloc_str( &hc->tmpbuff, &hc->maxtmpbuff, lenh + 1 + len );
				(void) strcpy( hc->tmpbuff, hostdir );
				hc->tmpbuff[lenh]='/';
				(void) strcpy( &hc->tmpbuff[lenh+1], hc->origfilename );
				toexpand=hc->tmpbuff;

			} else if ( errno != ENOENT )
				syslog( LOG_DEBUG, "vhost stat %.80s - %m", hostdir );

			/* Re-display port number if given */
			if ( cp != (char*) 0 )
				*cp = ':';
		}
		hc->realfilename=realpath(toexpand,NULL);
	}
	else
#endif /* VHOSTING */
	/* Expand all symbolic links in the filename. Since Posix-2008 realpath is (thread) safe on almost all platform */
		hc->realfilename=realpath(hc->origfilename,NULL);

	/* If the expanded filename is not null, check that it's still
	** within the current directory or the alternate directory.
	*/
	if ( hc->realfilename ) {
		size_t cwdlen=strlen( hc->hs->cwd );
		if ( strncmp(hc->realfilename, hc->hs->cwd, cwdlen -1) == 0 
				&& ( hc->realfilename[cwdlen-1]=='\0' || hc->realfilename[cwdlen-1]=='/' ) )
			{
				if (hc->realfilename[cwdlen-1]=='\0') /* if realpath() behave differently on differeny platforms: || ( hc->realfilename[cwdlen-1]=='/' && hc->realfilename[cwdlen]=='\0' ) */
					strcpy(hc->realfilename,".");
				else
					/* Elide the current directory. */
					strcpy(hc->realfilename, &hc->realfilename[cwdlen] );
			}
		else
			{
			syslog(
				LOG_NOTICE, "%.80s URL \"%.80s\" goes outside the web tree",
				hc->client_addr, hc->encodedurl );
			httpd_send_err(
				hc, 403, err403title, "",
				ERROR_FORM( err403form, "The requested URL '%.80s' resolves to a file outside the permitted web server directory tree.\n" ),
				hc->encodedurl );
			return -1;
			}
	} else if (errno != ENOENT)
		syslog( LOG_ERR, "realpath %.80s - %m", hc->origfilename);

	return 0;
	}


static char*
bufgets( httpd_conn* hc )
	{
	int i;
	char c;

	for ( i = hc->checked_idx; hc->checked_idx < hc->read_idx; ++hc->checked_idx )
		{
		c = hc->read_buf[hc->checked_idx];
		if ( c == '\012' || c == '\015' )
			{
			hc->read_buf[hc->checked_idx] = '\0';
			++hc->checked_idx;
			if ( c == '\015' && hc->checked_idx < hc->read_idx &&
				 hc->read_buf[hc->checked_idx] == '\012' )
				{
				hc->read_buf[hc->checked_idx] = '\0';
				++hc->checked_idx;
				}
			return &(hc->read_buf[i]);
			}
		}
	return (char*) 0;
	}

/*! de_dotdot delete (clean) all useless '/' and '.' in a filename */
static void
de_dotdot( char* file )
	{
	char* cp;
	char* cp2;
	int l;

	/* Collapse any multiple / sequences. */
	while ( ( cp = strstr( file, "//") ) != (char*) 0 )
		{
		for ( cp2 = cp + 2; *cp2 == '/'; ++cp2 )
			continue;
		(void) strcpy( cp + 1, cp2 );
		}

	/* Remove leading ./ and any /./ sequences. */
	while ( strncmp( file, "./", 2 ) == 0 )
		(void) strcpy( file, file + 2 );
	while ( ( cp = strstr( file, "/./") ) != (char*) 0 )
		(void) strcpy( cp, cp + 2 );

	/* Alternate between removing leading ../ and removing xxx/../ */
	for (;;)
		{
		while ( strncmp( file, "../", 3 ) == 0 )
			(void) strcpy( file, file + 3 );
		cp = strstr( file, "/../" );
		if ( cp == (char*) 0 )
			break;
		for ( cp2 = cp - 1; cp2 >= file && *cp2 != '/'; --cp2 )
			continue;
		(void) strcpy( cp2 + 1, cp + 4 );
		}

	/* Also elide any xxx/.. at the end. */
	while ( ( l = strlen( file ) ) > 3 &&
			strcmp( ( cp = file + l - 3 ), "/.." ) == 0 )
		{
		for ( cp2 = cp - 1; cp2 >= file && *cp2 != '/'; --cp2 )
			continue;
		if ( cp2 < file )
			break;
		*cp2 = '\0';
		}
	}


void
httpd_close_conn( httpd_conn* hc, struct timeval* nowP )
	{

	if ( hc->file_address != (char*) 0 )
		{
		mmc_unmap( hc->file_address, &(hc->sb), nowP );
		hc->file_address = (char*) 0;
		}
	if ( hc->conn_fd >= 0 )
		{
		(void) close( hc->conn_fd );
		hc->conn_fd = -1;
		}
	free( (void*) hc->realfilename );
	hc->realfilename=NULL;
	}

void
httpd_destroy_conn( httpd_conn* hc )
	{
	if ( hc->initialized )
		{
		free( (void*) hc->client_addr );
		free( (void*) hc->read_buf );
		free( (void*) hc->decodedurl );
		free( (void*) hc->origfilename );
		free( (void*) hc->encodings );
		free( (void*) hc->query );
		free( (void*) hc->accept );
		free( (void*) hc->accepte );
		free( (void*) hc->reqhost );
		free( (void*) hc->hostdir );
		free( (void*) hc->remoteuser );
		free( (void*) hc->response );
		hc->initialized = 0;
		}
	}


struct mime_entry {
	char* ext;
	size_t ext_len;
	char* val;
	size_t val_len;
	};
static struct mime_entry enc_tab[] = {
#include "mime_encodings.h"
	};
static struct mime_entry typ_tab[] = {
#include "mime_types.h"
	};

/* qsort comparison routine. */
static int ext_compare(struct mime_entry* a,struct mime_entry* b ) {
	return strcmp( a->ext, b->ext );
}


static void
init_mime( void )
	{
	int i;

	/* Sort the tables so we can do binary search. */
	qsort( enc_tab, SIZEOFARRAY(enc_tab), sizeof(*enc_tab),(int(*)(const void *, const void *)) ext_compare );
	qsort( typ_tab, SIZEOFARRAY(typ_tab), sizeof(*typ_tab),(int(*)(const void *, const void *)) ext_compare );

	/* Fill in the lengths. */
	for ( i = 0; i < SIZEOFARRAY(enc_tab); ++i )
		{
		enc_tab[i].ext_len = strlen( enc_tab[i].ext );
		enc_tab[i].val_len = strlen( enc_tab[i].val );
		}
	for ( i = 0; i < SIZEOFARRAY(typ_tab); ++i )
		{
		typ_tab[i].ext_len = strlen( typ_tab[i].ext );
		typ_tab[i].val_len = strlen( typ_tab[i].val );
		}

	}


/* Figure out MIME encodings and type based on the filename.  Multiple
** encodings are separated by commas, and are listed in the order in
** which they were applied to the file.
*/
static void
figure_mime( httpd_conn* hc )
	{
	char* prev_dot;
	char* dot;
	char* ext;
	int me_indexes[100], n_me_indexes;
	size_t ext_len, encodings_len;
	int i, top, bot, mid;
	int r;
	char* default_type = "text/html; charset=%s";

	/* Peel off encoding extensions until there aren't any more. */
	n_me_indexes = 0;
	for ( prev_dot = &hc->realfilename[strlen(hc->realfilename)]; ; prev_dot = dot )
		{
		for ( dot = prev_dot - 1; dot >= hc->realfilename && *dot != '.'; --dot )
			;
		if ( dot < hc->realfilename )
			{
			/* No dot found.  No more encoding extensions, and no type
			** extension either.
			*/
			hc->type = default_type;
			goto done;
			}
		ext = dot + 1;
		ext_len = prev_dot - ext;
		/* Search the encodings table.  Linear search is fine here, there
		** are only a few entries.
		*/
		for ( i = 0; i < SIZEOFARRAY(enc_tab); ++i )
			{
			if ( ext_len == enc_tab[i].ext_len && strncasecmp( ext, enc_tab[i].ext, ext_len ) == 0 )
				{
				if ( n_me_indexes < SIZEOFARRAY(me_indexes) )
					{
					me_indexes[n_me_indexes] = i;
					++n_me_indexes;
					}
				goto next;
				}
			}
		/* No encoding extension found.  Break and look for a type extension. */
		break;

		next: ;
		}

	/* Binary search for a matching type extension. */
	top = SIZEOFARRAY(typ_tab) - 1;
	bot = 0;
	while ( top >= bot )
		{
		mid = ( top + bot ) / 2;
		r = strncasecmp( ext, typ_tab[mid].ext, ext_len );
		if ( r < 0 )
			top = mid - 1;
		else if ( r > 0 )
			bot = mid + 1;
		else
			if ( ext_len < typ_tab[mid].ext_len )
				top = mid - 1;
			else if ( ext_len > typ_tab[mid].ext_len )
				bot = mid + 1;
			else
				{
				hc->type = typ_tab[mid].val;
				goto done;
				}
		}
	hc->type = default_type;

	done:

	/* The last thing we do is actually generate the mime-encoding header. */
	hc->encodings[0] = '\0';
	encodings_len = 0;
	for ( i = n_me_indexes - 1; i >= 0; --i )
		{
		httpd_realloc_str(
			&hc->encodings, &hc->maxencodings,
			encodings_len + enc_tab[me_indexes[i]].val_len + 1 );
		if ( hc->encodings[0] != '\0' )
			{
			(void) strcpy( &hc->encodings[encodings_len], "," );
			++encodings_len;
			}
		(void) strcpy( &hc->encodings[encodings_len], enc_tab[me_indexes[i]].val );
		encodings_len += enc_tab[me_indexes[i]].val_len;
		}

	}


#ifdef CGI_TIMELIMIT
static void
cgi_kill2( ClientData client_data, struct timeval* nowP )
	{
	pid_t pid=client_data.pid;

	if ( kill( -pid, SIGKILL ) == 0 )
		syslog( LOG_ERR, "hard-killed child process group %d", pid );
	}

static void
cgi_kill( ClientData client_data, struct timeval* nowP )
	{
	pid_t pid = client_data.pid;

	if ( kill( pid, SIGINT ) == 0 )
		syslog( LOG_ERR, "killed child process %d", pid );
	/* In case this isn't enough, schedule an uncatchable kill. */
	if ( tmr_create( nowP, cgi_kill2, client_data, 5 * 1000L, 0 ) == (Timer*) 0 )
		{
		syslog( LOG_CRIT, "tmr_create(cgi_kill2) failed" );
		exit( 1 );
		}
	}
#endif /* CGI_TIMELIMIT */

/*! drop_child should by call by the parent when a child will handle the request */
static void drop_child(const char * type,pid_t pid,httpd_conn* hc) {
	ClientData client_data;
	httpd_conn** tmphcs;

	++hc->hs->cgi_count;
	syslog( LOG_DEBUG, "%s spawned %s process %d for '%.200s'", hc->client_addr, type, pid, hc->origfilename);

	/* set the process group id to a new one for hard killing of all the process group (cgi_kill2,...)) */
	if (setpgid(pid,0)) {
		syslog( LOG_ERR, "hard-kill %d because %s fail - %m", pid,"setpgid");
		kill( pid, SIGKILL );
	}

	/* Memorise pid and it's hc */
	if (pid<hctab.pidmin) {
		tmphcs=realloc(hctab.hcs, (hctab.pidmax-pid)*sizeof(httpd_conn *));
		if (tmphcs) {
			memmove(&tmphcs[hctab.pidmin-pid], tmphcs, (hctab.pidmax-hctab.pidmin)*sizeof(httpd_conn *));
			memset(tmphcs, 0, (hctab.pidmin-pid)*sizeof(httpd_conn *));
			hctab.hcs=tmphcs;
			hctab.pidmin=pid;
			hctab.hcs[0]=hc;
		} else {
			syslog( LOG_ERR, "hard-kill %d because %s fail - %m", pid,"realloc(hctab.hcs,...)");
			kill( -pid, SIGKILL );
		}
	} else if (pid>=hctab.pidmax) {
		tmphcs=realloc(hctab.hcs, (pid+128-hctab.pidmin)*sizeof(httpd_conn *));
		if (tmphcs) {
			memset(&tmphcs[hctab.pidmax-hctab.pidmin], 0, (pid+128-hctab.pidmax)*sizeof(httpd_conn *));
			hctab.hcs=tmphcs;
			hctab.pidmax=pid+128;
			hctab.hcs[pid-hctab.pidmin]=hc;
		} else {
			syslog( LOG_ERR, "hard-kill %d because %s fail - %m", pid,"realloc(hctab.hcs,...)");
			kill( -pid, SIGKILL );
		}
	} else
		hctab.hcs[pid-hctab.pidmin]=hc;

#ifdef CGI_TIMELIMIT
	/* Schedule a kill for the child process, in case it runs too long */
	client_data.pid = pid;
	if ( tmr_create( (struct timeval*) 0, cgi_kill, client_data, CGI_TIMELIMIT * 1000L, 0 ) == (Timer*) 0 )
		{
		syslog( LOG_ERR, "hard-kill %d because %s fail - %m", pid,"tmr_create(cgi_kill...)");
		kill( -pid, SIGKILL );
		//syslog( LOG_CRIT, "tmr_create(cgi_kill %d) failed (%s)",pid,type);
		//exit(EXIT_FAILURE);
		}
#endif /* CGI_TIMELIMIT */


	hc->status = 200;
	hc->bytes_sent = CGI_BYTECOUNT;
	hc->bfield &= ~HC_SHOULD_LINGER;
	/* The child should hold the log */
	hc->bfield |= HC_LOG_DONE;
}

/*! child_r_start should be call early by the child handling the request */
static void child_r_start(httpd_conn* hc) {
	int s=1;

	httpd_unlisten( hc->hs ); 

	/* set signals to default behavior. */
#ifdef HAVE_SIGSET
	(void) sigset( SIGTERM, SIG_DFL );
	(void) sigset( SIGINT, SIG_DFL );
	(void) sigset( SIGCHLD, SIG_DFL );
	(void) sigset( SIGPIPE, SIG_DFL );
	(void) sigset( SIGHUP, SIG_DFL );
	(void) sigset( SIGUSR1, SIG_DFL );
	(void) sigset( SIGUSR2, SIG_DFL );
	(void) sigset( SIGALRM, SIG_DFL );
	(void) sigset( SIGBUS, SIG_DFL );
#else /* HAVE_SIGSET */
	(void) signal( SIGTERM, SIG_DFL );
	(void) signal( SIGINT, SIG_DFL );
	(void) signal( SIGCHLD, SIG_DFL );
	(void) signal( SIGPIPE, SIG_DFL );
	(void) signal( SIGHUP, SIG_DFL );
	(void) signal( SIGUSR1, SIG_DFL );
	(void) signal( SIGUSR2, SIG_DFL );
	(void) signal( SIGALRM, SIG_DFL );
	(void) signal( SIGBUS, SIG_DFL );
#endif /* HAVE_SIGSET */

#ifdef CGI_NICE
	/* Set priority. */
	(void) nice( CGI_NICE );
#endif /* CGI_NICE */

	/* activate TCP_NODELAY for CGI and spawned process (cf. man 7 tcp) */
	if ( setsockopt(hc->conn_fd, IPPROTO_TCP, TCP_NODELAY, (char*) &s, sizeof(s) ) < 0 )
		syslog( LOG_WARNING, "setsockopt TCP_NODELAY - %m");
}

/*
 * \param methods: accepted HTTP methods (bitwise-or of METHOD_GET or METHOD_POST).
 * \return a negative number to finish the connection, or 0 if it have fork.
 */
static int launch_process(void (*funct) (httpd_conn* ), httpd_conn* hc, int methods, char * fname) {
	int r;

	if ( ! (hc->method & methods) ) {
		httpd_send_err( hc, 501, err501title, "", err501form, httpd_method_str( hc->method ) );
		return(-1);
	}

	/* To much forks already running */
	if ( hc->hs->cgi_limit > 0 && hc->hs->cgi_count >= hc->hs->cgi_limit ) {
		httpd_send_err(hc, 503, httpd_err503title, "", httpd_err503form, hc->encodedurl );
		return(-1);
	}
	r = fork( );
	if ( r < 0 ) {
		httpd_send_err(hc, 500, err500title, "", err500form, "f" );
		return(-1);
	}
	if ( r > 0 ) {
		/* Parent process. */
		drop_child(fname,r,hc);
		return(0);
	}

	/* Child process. */
	child_r_start(hc);
	funct(hc);
	/* If the child process forget to exit... : */
	exit(-2);
}

#ifdef GENERATE_INDEXES

/* qsort comparison routine */
static int name_compare(char ** a, char ** b) {
	return strcmp( *a, *b );
}

static void ls(httpd_conn* hc) {
	DIR* dirp;
	struct dirent* de;
	int namlen;
	static int maxnames = 0;
	int nnames;
	static char* names;
	static char** nameptrs;
	static char* name;
	static size_t maxname = 0;
	static char* rname;
	static size_t maxrname = 0;
	static char* encrname;
	static size_t maxencrname = 0;
	FILE* fp;
	int i;
	struct stat sb;
	struct stat lsb;
	char modestr[20];
	char* linkprefix;
	char link[MAXPATHLEN+1];
	int linklen;
	char* fileclass;
	time_t now;
	char* timestr;

	dirp = opendir( hc->realfilename );
	if ( dirp == (DIR*) 0 ) {
		syslog( LOG_ERR, "opendir %.80s - %m", hc->realfilename );
		httpd_send_err( hc, 404, err404title, "", err404form, hc->encodedurl );
		exit(1);
	}

	send_mime(
		hc, 200, ok200title, "", "", "text/html; charset=%s",
		(off_t) -1, hc->sb.st_mtime );
	httpd_write_response( hc );

	if ( hc->method == METHOD_HEAD )
		exit(0);

	/* Open a stdio stream so that we can use fprintf, which is more
	** efficient than a bunch of separate write()s.  We don't have
	** to worry about double closes or file descriptor leaks cause
	** we're in a subprocess.
	*/
	httpd_clear_ndelay( hc->conn_fd );
	fp = fdopen( hc->conn_fd, "w" );
	if ( fp == (FILE*) 0 )
		{
		syslog( LOG_ERR, "fdopen - %m" );
		httpd_send_err(
			hc, 500, err500title, "", err500form, hc->encodedurl );
		closedir( dirp );
		exit( 1 );
		}

	(void) fprintf( fp, "\
<HTML>\n\
<HEAD><TITLE>Index of %.80s</TITLE></HEAD>\n\
<BODY BGCOLOR=\"#99cc99\" TEXT=\"#000000\" LINK=\"#2020ff\" VLINK=\"#4040cc\">\n\
<H2>Index of %.80s</H2>\n\
<PRE>\n\
mode  links  bytes  last-changed  name\n\
<HR>",
		hc->encodedurl, hc->encodedurl );

	/* Read in names. */
	nnames = 0;
	while ( ( de = readdir( dirp ) ) != 0 )	 /* dirent or direct */
		{
		if ( nnames >= maxnames )
			{
			if ( maxnames == 0 )
				{
				maxnames = 100;
				names = NEW( char, maxnames * ( MAXPATHLEN + 1 ) );
				nameptrs = NEW( char*, maxnames );
				}
			else
				{
				maxnames *= 2;
				names = RENEW( names, char, maxnames * ( MAXPATHLEN + 1 ) );
				nameptrs = RENEW( nameptrs, char*, maxnames );
				}
			if ( names == (char*) 0 || nameptrs == (char**) 0 )
				{
				syslog( LOG_ERR, "out of memory reallocating directory names" );
				exit( 1 );
				}
			for ( i = 0; i < maxnames; ++i )
				nameptrs[i] = &names[i * ( MAXPATHLEN + 1 )];
			}
		namlen = NAMLEN(de);
		(void) strncpy( nameptrs[nnames], de->d_name, namlen );
		nameptrs[nnames][namlen] = '\0';
		++nnames;
		}
	closedir( dirp );

	/* Sort the names. */
	qsort( nameptrs, nnames, sizeof(*nameptrs),(int(*)(const void *, const void *)) name_compare );

	/* Generate output. */
	for ( i = 0; i < nnames; ++i )
		{
		httpd_realloc_str(
			&name, &maxname,
			strlen( hc->realfilename ) + 1 + strlen( nameptrs[i] ) );
		httpd_realloc_str(
			&rname, &maxrname,
			strlen( hc->origfilename ) + 1 + strlen( nameptrs[i] ) );
		if ( hc->realfilename[0] == '\0' ||
			 strcmp( hc->realfilename, "." ) == 0 )
			{
			(void) strcpy( name, nameptrs[i] );
			(void) strcpy( rname, nameptrs[i] );
			}
		else
			{
			(void) snprintf( name, maxname,
				"%s/%s", hc->realfilename, nameptrs[i] );
			if ( strcmp( hc->origfilename, "." ) == 0 )
				(void) snprintf( rname, maxrname,
					"%s", nameptrs[i] );
			else
				(void) snprintf( rname, maxrname,
					"%s%s", hc->origfilename, nameptrs[i] );
			}
		httpd_realloc_str(
			&encrname, &maxencrname, 3 * strlen( rname ) + 1 );
		strencode( encrname, maxencrname, rname );

		if ( stat( name, &sb ) < 0 || lstat( name, &lsb ) < 0 )
			continue;

		linkprefix = "";
		link[0] = '\0';
		/* Break down mode word.  First the file type. */
		switch ( lsb.st_mode & S_IFMT )
			{
			case S_IFIFO:  modestr[0] = 'p'; break;
			case S_IFCHR:  modestr[0] = 'c'; break;
			case S_IFDIR:  modestr[0] = 'd'; break;
			case S_IFBLK:  modestr[0] = 'b'; break;
			case S_IFREG:  modestr[0] = '-'; break;
			case S_IFSOCK: modestr[0] = 's'; break;
			case S_IFLNK:  modestr[0] = 'l';
			linklen = readlink( name, link, sizeof(link) - 1 );
			if ( linklen != -1 )
				{
				link[linklen] = '\0';
				linkprefix = " -&gt; ";
				}
			break;
			default:	   modestr[0] = '?'; break;
			}
		/* Now the world permissions.  Owner and group permissions
		** are not of interest to web clients.
		*/
		modestr[1] = ( lsb.st_mode & S_IROTH ) ? 'r' : '-';
		modestr[2] = ( lsb.st_mode & S_IWOTH ) ? 'w' : '-';
		modestr[3] = ( lsb.st_mode & S_IXOTH ) ? 'x' : '-';
		modestr[4] = '\0';

		/* We also leave out the owner and group name, they are
		** also not of interest to web clients.  Plus if we're
		** running under chroot(), they would require a copy
		** of /etc/passwd and /etc/group, which we want to avoid.
		*/

		/* Get time string. */
		now = time( (time_t*) 0 );
		timestr = ctime( &lsb.st_mtime );
		timestr[ 0] = timestr[ 4];
		timestr[ 1] = timestr[ 5];
		timestr[ 2] = timestr[ 6];
		timestr[ 3] = ' ';
		timestr[ 4] = timestr[ 8];
		timestr[ 5] = timestr[ 9];
		timestr[ 6] = ' ';
		if ( now - lsb.st_mtime > 60*60*24*182 )		/* 1/2 year */
			{
			timestr[ 7] = ' ';
			timestr[ 8] = timestr[20];
			timestr[ 9] = timestr[21];
			timestr[10] = timestr[22];
			timestr[11] = timestr[23];
			}
		else
			{
			timestr[ 7] = timestr[11];
			timestr[ 8] = timestr[12];
			timestr[ 9] = ':';
			timestr[10] = timestr[14];
			timestr[11] = timestr[15];
			}
		timestr[12] = '\0';

		/* The ls -F file class. */
		switch ( sb.st_mode & S_IFMT )
			{
			case S_IFDIR:  fileclass = "/"; break;
			case S_IFSOCK: fileclass = "="; break;
			case S_IFLNK:  fileclass = "@"; break;
			default:
			fileclass = ( sb.st_mode & S_IXOTH ) ? "*" : "";
			break;
			}

		/* And print. */
		(void)  fprintf( fp,
		   "%s %3ld  %10lld  %s  <A HREF=\"/%.500s%s\">%.80s</A>%s%s%s\n",
			modestr, (long) lsb.st_nlink, (int64_t) lsb.st_size,
			timestr, encrname, S_ISDIR(sb.st_mode) ? "/" : "",
			nameptrs[i], linkprefix, link, fileclass );
		}

	(void) fprintf( fp, "</PRE></BODY>\n</HTML>\n" );
	(void) fclose( fp );
	exit( 0 );
}

#endif /* GENERATE_INDEXES */


static char*
build_env( char* fmt, char* arg )
	{
	char* cp;
	size_t size;
	static char* buf;
	static size_t maxbuf = 0;

	size = strlen( fmt ) + strlen( arg );
	if ( size > maxbuf )
		httpd_realloc_str( &buf, &maxbuf, size );
	(void) snprintf( buf, maxbuf, fmt, arg );
	cp = strdup( buf );
	if ( cp == (char*) 0 )
		{
		syslog( LOG_ERR, "out of memory copying environment variable" );
		exit( 1 );
		}
	return cp;
	}

/* Set up environment variables. Be real careful here to avoid
** letting malicious clients overrun a buffer.  We don't have
** to worry about freeing stuff since we're a sub-process.
*/
static char**
make_envp( httpd_conn* hc )
	{
	static char* envp[50];
	int envn;
	char* cp;
	char buf[256];

	envn = 0;
	envp[envn++] = build_env( "PATH=%s", CGI_PATH );
#ifdef CGI_LD_LIBRARY_PATH
	envp[envn++] = build_env( "LD_LIBRARY_PATH=%s", CGI_LD_LIBRARY_PATH );
#endif /* CGI_LD_LIBRARY_PATH */
	envp[envn++] = build_env( "SERVER_SOFTWARE=%s", EXPOSED_SERVER_SOFTWARE );
	/* server-name */
	cp = hc->hs->server_hostname;
	if ( cp != (char*) 0 )
		envp[envn++] = build_env( "SERVER_NAME=%s", cp );
	envp[envn++] = "GATEWAY_INTERFACE=CGI/1.1";
	envp[envn++] = build_env("SERVER_PROTOCOL=%s", hc->protocol);
	(void) snprintf( buf, sizeof(buf), "%d", (int) hc->hs->port );
	envp[envn++] = build_env( "SERVER_PORT=%s", buf );
	envp[envn++] = build_env(
		"REQUEST_METHOD=%s", httpd_method_str( hc->method ) );
	envp[envn++] = build_env(
		"SCRIPT_NAME=/%s", strcmp( hc->origfilename, "." ) == 0 ?
		"" : hc->origfilename );
	if ( hc->query[0] != '\0')
		envp[envn++] = build_env( "QUERY_STRING=%s", hc->query );
	envp[envn++] = build_env(
		"REMOTE_ADDR=%s", hc->client_addr );
	if ( hc->referer[0] != '\0' )
		envp[envn++] = build_env( "HTTP_REFERER=%s", hc->referer );
	if ( hc->useragent[0] != '\0' )
		envp[envn++] = build_env( "HTTP_USER_AGENT=%s", hc->useragent );
	if ( hc->accept[0] != '\0' )
		envp[envn++] = build_env( "HTTP_ACCEPT=%s", hc->accept );
	if ( hc->accepte[0] != '\0' )
		envp[envn++] = build_env( "HTTP_ACCEPT_ENCODING=%s", hc->accepte );
	if ( hc->acceptl[0] != '\0' )
		envp[envn++] = build_env( "HTTP_ACCEPT_LANGUAGE=%s", hc->acceptl );
	if ( hc->cookie[0] != '\0' )
		envp[envn++] = build_env( "HTTP_COOKIE=%s", hc->cookie );
	if ( hc->contenttype[0] != '\0' )
		envp[envn++] = build_env( "CONTENT_TYPE=%s", hc->contenttype );
	if ( hc->hdrhost[0] != '\0' )
		envp[envn++] = build_env( "HTTP_HOST=%s", hc->hdrhost );
	if ( hc->contentlength != -1 )
		{
		(void) snprintf(
			buf, sizeof(buf), "%lu", (unsigned long) hc->contentlength );
		envp[envn++] = build_env( "CONTENT_LENGTH=%s", buf );
		}
	if ( hc->remoteuser[0] != '\0' )
		envp[envn++] = build_env( "REMOTE_USER=%s", hc->remoteuser );
	if ( hc->authorization[0] != '\0' )
		envp[envn++] = build_env( "AUTH_TYPE=%s", "Basic" );
		/* We only support Basic auth at the moment. */
	if ( hc->forwardedfor[0] != '\0' )
		envp[envn++] = build_env( "HTTP_X_FORWARDED_FOR=%s", hc->forwardedfor );
	if ( getenv( "TZ" ) != (char*) 0 )
		envp[envn++] = build_env( "TZ=%s", getenv( "TZ" ) );
	envp[envn++] = build_env( "CGI_PATTERN=%s", hc->hs->cgi_pattern );
	envp[envn++] = build_env( "SIG_EXCLUDE_PATTERN=%s", hc->hs->sig_pattern );

	envp[envn] = (char*) 0;
	return envp;
	}


/* Set up argument vector.  Again, we don't have to worry about freeing stuff
** since we're a sub-process.  This gets done after make_envp() because we
** scribble on hc->query.
*/
static char**
make_argp( httpd_conn* hc )
	{
	char** argp;
	int argn;
	char* cp1;
	char* cp2;

	/* By allocating an arg slot for every character in the query, plus
	** one for the filename and one for the NULL, we are guaranteed to
	** have enough.  We could actually use strlen/2.
	*/
	argp = NEW( char*, strlen( hc->query ) + 2 );
	if ( argp == (char**) 0 )
		return (char**) 0;

	argp[0] = strrchr( hc->realfilename, '/' );
	if ( argp[0] != (char*) 0 )
		++argp[0];
	else
		argp[0] = hc->realfilename;

	argn = 1;
	/* According to the CGI spec at http://hoohoo.ncsa.uiuc.edu/cgi/cl.html,
	** "The server should search the query information for a non-encoded =
	** character to determine if the command line is to be used, if it finds
	** one, the command line is not to be used."
	*/
	if ( strchr( hc->query, '=' ) == (char*) 0 )
		{
		for ( cp1 = cp2 = hc->query; *cp2 != '\0'; ++cp2 )
			{
			if ( *cp2 == '+' )
				{
				*cp2 = '\0';
				strdecode( cp1, cp1 );
				argp[argn++] = cp1;
				cp1 = cp2 + 1;
				}
			}
        /* for the last str without '+' */
		if ( cp2 != cp1 ) /* if client forge a request ending with some '+' don't care of it */
			{
			strdecode( cp1, cp1 );
			argp[argn++] = cp1;
			}
		}

	argp[argn] = (char*) 0;
	return argp;
	}


/* This routine is used only for POST requests.  It reads the data
** from the request and sends it to the child process.  The only reason
** we need to do it this way instead of just letting the child read
** directly is that we have already read part of the data into our
** buffer.
*/
static void
cgi_interpose_input(interpose_args_t * args)
	{
	const httpd_conn * hc=args->hc;
	size_t c;
	ssize_t r;
	char buf[BUFSIZE];

	c = hc->read_idx - hc->checked_idx;
	if ( c > 0 )
		{
		if ( httpd_write_fully( args->wfd, &(hc->read_buf[hc->checked_idx]), c ) != c )
			return;
		}
	while ( c < hc->contentlength )
		{
		r = read( args->rfd, buf, MIN( sizeof(buf), hc->contentlength - c ) );
		if ( r < 0 && ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) )
			{
			struct timespec tim={0, 300000000}; /* 300 ms */
			nanosleep(&tim, NULL);
			continue;
			}
		if ( r <= 0 )
			return;
		if ( httpd_write_fully( args->wfd, buf, r ) != r )
			return;
		c += r;
		}

		/* Special hack to deal with broken browsers that send a LF or CRLF
		** after POST data, causing TCP resets - we just read and discard up
		** to 2 bytes.  Unfortunately this doesn't fix the problem for CGIs
		** which avoid the interposer process due to their POST data being
		** short.  Creating an interposer process for all POST CGIs is
		** unacceptably expensive.  The eventual fix will come when interposing
		** gets integrated into the main loop as a tasklet instead of a process.
		*/
		{
			char buf[2];

		/* we are in a sub-process, re-turn on no-delay mode that we
		** previously cleared.
		*/
		httpd_set_ndelay( args->rfd );
		/* And read up to 2 bytes. */
		(void) read( args->rfd, buf, sizeof(buf) );
		}
	}

/* callback to get data from a FILE stream and duplicate it to and fd output */
static ssize_t fp2fd_gpg_data_rd_cb(struct fp2fd_gpg_data_handle * handle, void *buffer, size_t size)
{
	size_t result;

	/* should not happen */
	/*if ( !size || size>SSIZE_MAX )
		return(-1); */

	result = fread(buffer,1,size,handle->fpin);
	if (!result) {
		if (feof(handle->fpin))
			return(0);
		else
			return(-1);
	}
	if (httpd_write_fully( handle->fdout, buffer, result ) != result)
		return(-1);

	return(result);
}

/* dummy function for callback based gpgme data objects */
static void gpg_data_release_cb(void *handle)
{
	    /* must just be present... bug or feature?!? */
}

/*! Check the dirname of a file path, and create missing directories if needed
 * \return like mkdir: 0 on succes, -1 on error (cf. errno) */
static int mk_path(const char * path) {
	char * cp , * dir=strdup(path);
	struct stat std;

	if (!dir)
		return(-1);

	cp=dir;

	while (*cp == '/')
		cp++;

	cp=strchr(cp,'/');
	while (cp) {
		*cp='\0';
		if ( stat(dir,&std) < 0 && mkdir(dir,0755) < 0) {
		/* If the path does not exist and we fail to create it */
			free(dir);
			return -1;
		}
		*cp='/';
		while (*cp == '/') /* in case of mutiple ///// ... */
			cp++;
		cp=strchr(cp,'/');
	}
	free(dir);
	return 0;
}

/*! Chdir into the dirname of a file path.
 * \return the basename of the file path, or NULL on error (cf. errno)
 * \Note: if input is not a file path (eg finishing with a '/') it will return an empty string (""). */
static const char * chdir_path(const char * path) {
	char * directory, * binary;

	directory = strdup(path);
	if (! directory)
		return NULL;

	binary = strrchr( directory, '/' );
	if ( ! binary ) {
		free(directory);
		return path;
	} else {
		*binary = '\0';
		if ( chdir(directory) == 0 ) {
			free(directory);
			return &path[(binary-directory+1)];
		} else {
			free(directory);
			return(NULL);
		}
	}
}

/*! parse an HTTP response (CGI or not) from rfd, sign it if status in it is 2XX, and write it to the hc->conn_fd.
 * \param fd: the file descriptor to read the response from
 * \param cgi: If set, the function won't sign if Content-type is already "multipart/signed" or if HC_DETACH_SIGN is unset, and won't cache the signature. If unset, the function will always sign (regardless of HC_DETACH_SIGN) and will use cached signature if HC_GOT_RANGE is unset.
 * \Note: it closes the rfd and the hc->conn_fd before to return.
 *
 */
void httpd_parse_resp(interpose_args_t * args) {
	const httpd_conn * hc=args->hc;
	int optcgi=args->option;
#define SIG_CACHE_DIR "../"SIG_CACHEDIR
#define HTTP_MAX_CONTENTHEADERS 9
#define HTTP_MAX_HEADERS 40

#define HTTPD_PARSE_RESP_RETURN(code) { \
	make_log_entry( hc, 0, (code)>0?code:status); \
	(fp?fclose(fp):close(args->rfd)); \
	close(args->wfd); \
	free(buf); \
	for (i=0;i<n_c_headers;i++) \
		free(c_headers[i]); \
	for (i=0;i<n_o_headers;i++) \
		free(o_headers[i]); \
	return ; \
	}

	FILE * fp;
	char * c_headers[HTTP_MAX_CONTENTHEADERS]={(char *)0};
	char * o_headers[HTTP_MAX_HEADERS]={(char *)0};
	int  n_c_headers=0,n_o_headers=0,do_sign,use_cache;
	ssize_t r;
	size_t buflen=BUFSIZE;
	char * buf=malloc(buflen);
	int status=-1,i;
	char * title, * cp;
	char fcache[MAXPATHLEN];
	struct stat sts;


	do_sign=(optcgi?0:1);
	use_cache=0; /* will be set to 1 (use cache) or 2 (do the cache) if ( !optcgi and SIG_CACHE_DIR exist) later */

	/* use a file descriptor for getline (which is POSIX since 2008, glibc >= 2.10 )*/
	if ( !(fp=fdopen(args->rfd,"r")) ) {
		syslog( LOG_ERR, "fdopen - %m");
		httpd_send_err2(args->wfd, 500, err500title, err500form);
		HTTPD_PARSE_RESP_RETURN(500);
	}

	/* Figure out the status.  Look for a Status: or Location: header;
	** else if there's an HTTP header line, get it from there; else
	** default to 200.
	*/
	for (;;) {
		r=getline(&buf,&buflen,fp);
		if ( r < 0 ) {
		   	if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) { /* EAGAIN should no more happen as blocking mode */
				struct timespec tim={0, 100000000}; /* 100 ms */
				nanosleep(&tim, NULL);
				continue;
			} else if ( feof(fp) ) {
				break;
			} else {	/* unmanaged error (mem,... ) */
				syslog( LOG_ERR, "getline - %m");
				httpd_send_err2(args->wfd, 500, err500title, err500form);
				HTTPD_PARSE_RESP_RETURN(500);
			}
		} else if (r<=2) /* end of headers reached */
			break;

		if (status<0) {
			if ( !strncmp(buf, "HTTP/", 5) ) {
				cp = buf+5;
				cp += strcspn( cp, " \t" );
				status = atoi( cp );
			} else if ( !strncasecmp(buf, "Status:", 7) ) {
				cp = buf+7;
				cp += strspn( cp, " \t" );
				status = atoi( cp );
			} else if ( !strncasecmp(buf, "Location:", 9) )
				status = 302;
		}

		if ( !do_sign && (hc->bfield & HC_DETACH_SIGN) &&!strncasecmp(buf, "Content-Type:", 13) ) {
			cp = buf+13;
			cp += strspn( cp, " \t" );
			if ( strncmp(cp,"multipart/msigned",sizeof("multipart/msigned")-1) )
				/* if output is not signed while it was asked, we will do it */
				do_sign=1;
		}
		
		if ( !strncasecmp( buf, "Content-",8 ) ) {
			if (! (c_headers[n_c_headers]=strdup(buf)) ) {
				syslog( LOG_ERR, "strdup - %m");
				httpd_send_err2(args->wfd, 500, err500title, err500form);
				HTTPD_PARSE_RESP_RETURN(500);
			}
			n_c_headers=MIN(HTTP_MAX_CONTENTHEADERS-1,n_c_headers+1);
		} else {
			if (! (o_headers[n_o_headers]=strdup(buf)) ) {
				syslog( LOG_ERR, "strdup - %m");
				httpd_send_err2(args->wfd, 500, err500title, err500form);
				HTTPD_PARSE_RESP_RETURN(500);
			}
			n_o_headers=MIN(HTTP_MAX_HEADERS-2,n_o_headers+1); /* Keep 1 slot empty to eventually insert "HTTP/..." */
		}
	}
	
	/* If there were no "Content-*:" headers, bail. */
	if ( !c_headers[0] ) {
		syslog( LOG_ERR, "no header (%d)",optcgi);
		httpd_send_err2(args->wfd, 500, err500title, err500form);
		HTTPD_PARSE_RESP_RETURN(500);
	}

	if (optcgi) {
		/* set a default status (because cgi may return none) */
		if (status<0)
			status=200;

		/* Check if first header start with "HTTP/ ... ", (still for cgi) */
		if ( !o_headers[0] || strncmp(o_headers[0], "HTTP/", 5 ) ) {

			for (i=n_o_headers;i>0;i--)
				o_headers[i]=o_headers[i-1];
			n_o_headers++;

			if ( ! (o_headers[0]=malloc(100)) ) {
				syslog( LOG_ERR, "malloc - %m");
				httpd_send_err2(args->wfd, 500, err500title, err500form);
				HTTPD_PARSE_RESP_RETURN(500);
			}

			/* Insert the status line. */
			switch ( status )
				{
				case 200: title = ok200title; break;
				case 302: title = err302title; break;
				case 304: title = err304title; break;
				case 400: title = httpd_err400title; break;
#ifdef AUTH_FILE
				case 401: title = err401title; break;
#endif /* AUTH_FILE */
				case 403: title = err403title; break;
				case 404: title = err404title; break;
				case 408: title = httpd_err408title; break;
				case 411: title = err411title; break;
				case 413: title = err413title; break;
				case 415: title = err415title; break;
				case 500: title = err500title; break;
				case 501: title = err501title; break;
				case 503: title = httpd_err503title; break;
				default: title = "Something"; break;
				}
			snprintf(o_headers[0],100, "HTTP/1.0 %d %s\015\012", status, title );
		}
	} else {
#ifdef SIG_CACHEDIR
		if ( stat(SIG_CACHE_DIR,&sts) < 0 || !S_ISDIR(sts.st_mode) ) {
			syslog( LOG_ERR,"invalid cache dir %s - %m",SIG_CACHE_DIR);
		} else if ( snprintf(fcache,MAXPATHLEN,"%s/%s",SIG_CACHE_DIR,hc->realfilename) >= MAXPATHLEN ) {
			syslog( LOG_ERR,"too big cache path - %s",hc->realfilename);
		} else if (!(hc->bfield & HC_GOT_RANGE)){
			use_cache=2; /* by default: do the cache */
			if ( stat(fcache,&sts) == 0 ) { 
				if ( ! S_ISREG(sts.st_mode) ) {
					/* May happen ... */
					remove(fcache);
					syslog( LOG_WARNING,"remove %s - %m",fcache);
				} else if ( sts.st_mtime > hc->sb.st_mtime )
				/* The cached signature seems newer than file */
					use_cache=1; /* just use it */
			}
		}
#else /* SIG_CACHEDIR */
		use_cache=0;
#endif /* SIG_CACHEDIR */
	}

	if (do_sign && status>=200 && status<300) {

#define HTTPD_PARSE_SIGN_CLEAN() { \
	gpgme_data_release(gpgdata); \
	gpgme_data_release(gpgsig); \
	}
		char * bound=random_boundary((char *)hc->boundary,BOUNDARYLEN);
		gpgme_error_t gpgerr;
		gpgme_data_t gpgdata,gpgsig;
		struct gpgme_data_cbs gpgcbs = {
			(gpgme_data_read_cb_t) fp2fd_gpg_data_rd_cb,	/* read method */
			NULL,									/* write method */
			NULL,									/* seek method */
			gpg_data_release_cb						/* release method */
		};
		struct fp2fd_gpg_data_handle cb_handle = {
			fp,				/* fp in */
			args->wfd     		/* fd out */
		};

		if (!bound) {
			syslog(LOG_ERR, "malloc - %m");
			httpd_send_err2(args->wfd, 500, err500title, err500form);
			HTTPD_PARSE_RESP_RETURN(500);
		}

		gpgerr = gpgme_data_new_from_cbs(&gpgdata, &gpgcbs,&cb_handle);
		if (gpgerr == GPG_ERR_NO_ERROR) 
			gpgerr = gpgme_data_new(&gpgsig);

		if ( gpgerr != GPG_ERR_NO_ERROR) {
			syslog(LOG_ERR, gpgme_strerror(gpgerr));
			httpd_send_err2(args->wfd, 500, err500title, err500form);
			HTTPD_PARSE_SIGN_CLEAN();
			HTTPD_PARSE_RESP_RETURN(500);
		}

		/* Write the headers. */
		for (i=0;i<n_o_headers;i++) {
			r=strlen(o_headers[i]);
			if (httpd_write_fully(args->wfd,o_headers[i],r) !=r ) {
				HTTPD_PARSE_SIGN_CLEAN();
				HTTPD_PARSE_RESP_RETURN(-1);
			}
		}

		r=snprintf(buf,buflen, "%s %s; %s=%s\015\012\015\012--%s\015\012","Content-Type:","multipart/msigned","boundary",bound,bound);
		r=MIN(r,buflen);
		if (httpd_write_fully(args->wfd,buf,r) !=r ) {
			HTTPD_PARSE_SIGN_CLEAN();
			HTTPD_PARSE_RESP_RETURN(-1);
		}
		/* Write the "Content-*" headers. */
		for (i=0;i<n_c_headers;i++) {
			r=strlen(c_headers[i]);
			if (httpd_write_fully(args->wfd,c_headers[i],r) !=r ) {
				HTTPD_PARSE_SIGN_CLEAN();
				HTTPD_PARSE_RESP_RETURN(-1);
			}
		}
		httpd_write_fully(args->wfd,"\015\012",2);

		/* contrary to RFC 3156, no headers are signed, only the content */
		if (use_cache==1) {
			for (;;) {
				r = fread(buf,sizeof(char), buflen-1,fp );
				if ( r <= 0 ) {
					if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) { /* should not happen (in blocking mode) */
						struct timespec tim={0, 100000000}; /* 100 ms */
						nanosleep(&tim, NULL);
						continue;
					} else if ( feof(fp) ) {
						break;
					} else {	/* unmanaged error (mem,... ) */
						syslog( LOG_ERR, "fread - %m");
						HTTPD_PARSE_SIGN_CLEAN();
						HTTPD_PARSE_RESP_RETURN(-1);
					}
				}
				if ( httpd_write_fully(args->wfd, buf, r ) != r ) {
					HTTPD_PARSE_SIGN_CLEAN();
					HTTPD_PARSE_RESP_RETURN(-1);
				}
			}
			gpgerr=GPG_ERR_NO_ERROR;
		} else
			gpgerr = gpgme_op_sign (main_gpgctx, gpgdata,gpgsig,GPGME_SIG_MODE_DETACH);

		if ( gpgerr == GPG_ERR_NO_ERROR) {
			off_t siglen;
			FILE * sigfile;
			if (use_cache==1) 
				siglen=sts.st_size;
			else {
				siglen=gpgme_data_seek(gpgsig, 0, SEEK_END);
				gpgme_data_seek(gpgsig, 0, SEEK_SET);
			}	
			r=snprintf(buf,buflen, "\015\012--%s\015\012%s %s\015\012%s %d\015\012\015\012",bound,"Content-Type:","application/pgp-signature","Content-Length:",(int) siglen);
			r=MIN(r,buflen);
			if (httpd_write_fully(args->wfd,buf,r) !=r ) {
				HTTPD_PARSE_SIGN_CLEAN();
				HTTPD_PARSE_RESP_RETURN(-1);
			}

			if (use_cache==2) {
			/* (Try to) Cache the signature */
				if ( mk_path(fcache) == 0 && (sigfile=fopen(fcache,"w")) ) {
			   		while ( (r=gpgme_data_read(gpgsig, buf, buflen)) > 0 )
						if (fwrite(buf,sizeof(char),r,sigfile) != r) {
						   unlink(fcache);
					   	   break;
						}	   
					fclose(sigfile);
					gpgme_data_seek(gpgsig, 0, SEEK_SET);	
				}
			}

			if (use_cache==1) {
			/* output cached signature */
				sigfile=fopen(fcache,"r");
				if (sigfile) {
					while ( (r=fread(buf,sizeof(char), buflen-1, sigfile)) )
						if ( httpd_write_fully(args->wfd, buf, r ) != r ) {
							HTTPD_PARSE_SIGN_CLEAN();
							HTTPD_PARSE_RESP_RETURN(-1);
						}
					fclose(sigfile);
				}
			} else {
				while ( (r=gpgme_data_read(gpgsig, buf, buflen)) > 0 )
					if (httpd_write_fully(args->wfd,buf,r) !=r ) {
						HTTPD_PARSE_SIGN_CLEAN();
						HTTPD_PARSE_RESP_RETURN(-1);
					}
			}
		} else {
			r=snprintf( buf,buflen, "\015\012--%s\015\012\015\012gpgme_op_sign -> %d : %s \015\012", bound, gpgerr,gpgme_strerror(gpgerr));
			r=MIN(r,buflen);
			if (httpd_write_fully(args->wfd,buf,r) !=r ) {
				HTTPD_PARSE_SIGN_CLEAN();
				HTTPD_PARSE_RESP_RETURN(-1);
			}
		}
		r=snprintf(buf,buflen, "\015\012--%s--\015\012",bound);
		httpd_write_fully(args->wfd, buf,MIN(r,buflen));
		HTTPD_PARSE_SIGN_CLEAN();
		HTTPD_PARSE_RESP_RETURN(status);
	} else {
		/* Write the headers. */
		for (i=0;i<n_o_headers;i++) {
			r=strlen(o_headers[i]);
			if (httpd_write_fully(args->wfd,o_headers[i],r) !=r ) {
				HTTPD_PARSE_RESP_RETURN(-1);
			}
		}
		/* Write the "Content-*" headers. */
		for (i=0;i<n_c_headers;i++) {
			r=strlen(c_headers[i]);
			if (httpd_write_fully(args->wfd,c_headers[i],r) !=r ) {
				HTTPD_PARSE_RESP_RETURN(-1);
			}
		}
		httpd_write_fully(args->wfd,"\015\012",2);
	
		/* Echo the rest of the output. */
		for (;;) {
			r = fread(buf,sizeof(char), buflen-1,fp );
			if ( r <= 0 ) {
				if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) { /* should not happen (in blocking mode) */
					struct timespec tim={0, 100000000}; /* 100 ms */
					nanosleep(&tim, NULL);
					continue;
				} else if ( feof(fp) ) {
					break;
				} else {	/* unmanaged error (mem,... ) */
					syslog( LOG_ERR, "fread - %m");
					HTTPD_PARSE_RESP_RETURN(-1);
				}
			}
			if ( httpd_write_fully(args->wfd, buf, r ) != r )
				HTTPD_PARSE_RESP_RETURN(-1);
		}
		HTTPD_PARSE_RESP_RETURN(status);
	}
}

/* CGI child process. */
static void
cgi_child( httpd_conn* hc ) {
	char** argp;
	char** envp;
	int interpose_input,interpose_output;

	/* Unset close-on-exec flag for this socket.  This actually shouldn't
	** be necessary, according to POSIX a dup()'d file descriptor does
	** *not* inherit the close-on-exec flag, its flag is always clear.
	** However, Linux messes this up and does copy the flag to the
	** dup()'d descriptor, so we have to clear it.
	* TODO: This could be ifdeffed for Linux only.
	*/
	(void) fcntl( hc->conn_fd, F_SETFD, 0 );

	/* Make the environment vector. */
	envp = make_envp( hc );
	/* Make the argument vector. */
	argp = make_argp( hc );

	interpose_input=(hc->method == METHOD_POST && hc->read_idx > hc->checked_idx );
	interpose_output=( strncmp(argp[0], "nph-", 4) && hc->http_version > 9 );

	//syslog( LOG_ERR, "in:%d out:%d read_idx:%d checked_idx:%d",interpose_input,interpose_output,hc->read_idx,hc->checked_idx);

	/* First duplicate the socket to stdin/stdout/stderr, if there
	** was already something on it,we clobber it, but that doesn't matter
	** since at this point the only fd of interest is the connection.
	** All others will be closed on exec.
	** Note: if syslog use stdin,stdout or stderr, it will be closed. But it should use a fd > 2 since it was open before to close them in thttpd.c.
	*/
	
	if ( dup2(hc->conn_fd,STDIN_FILENO) < 0
			|| dup2(hc->conn_fd,STDOUT_FILENO) < 0
			|| dup2(hc->conn_fd,STDERR_FILENO) < 0 ) {
		httpd_send_err( hc, 500, err500title, "", err500form, "d" );
		exit(EXIT_FAILURE);
	} else if (!interpose_output)
		/* Log now as there is no output interposer to do it */
		make_log_entry(hc, 0, 200);

	if ( interpose_input || interpose_output ) {
		/* we will need to create an interposer process */
		pid_t ipid;
		int pin[2],pou[2];

		/* hc->conn_fd should be stdin,stdout and stderr. So save it first */
		hc->conn_fd=dup(hc->conn_fd);
		if ( hc->conn_fd < 0 ) {
			httpd_send_err( hc, 500, err500title, "", err500form, "d" );
			exit(EXIT_FAILURE);
		}

		/* Then create needed pipe(s) : */
		if ( interpose_input ) {
			if ( pipe( pin ) < 0 ) {
				httpd_send_err( hc, 500, err500title, "", err500form, "p" );
				exit(EXIT_FAILURE);
			}
		}
		if ( interpose_output ) {
			if ( pipe( pou ) < 0 ) {
				httpd_send_err( hc, 500, err500title, "", err500form, "p" );
				exit( 1 );
			}
		}

		/* Now we fork */
		ipid = fork( );
		if ( ipid < 0 ) {
			httpd_send_err( hc, 500, err500title, "", err500form, "f" );
			exit(EXIT_FAILURE);
		}
		if ( ipid > 0 ) {
			/* Parent Interposer process. */
			interpose_args_t agin,agou;
			pthread_t tin,tou;
			int s;

			if ( interpose_input ) {
				close(pin[0]);
				/* Create a thread for input */
				agin.rfd=hc->conn_fd;
				agin.wfd=pin[1];
				agin.hc=hc;
				s=pthread_create(&tin, NULL,(void * (*)(void *)) &cgi_interpose_input, &agin);
				if ( s !=0 ) {
					errno=s;
					httpd_send_err( hc, 500, err500title, "", err500form, "thc" );
					exit(EXIT_FAILURE);
				}
			}

			if ( interpose_output ) {
				close(pou[1]);
				/* And parse output */
				agou.rfd=pou[0];
				agou.wfd=hc->conn_fd;
				agou.hc=hc;
				agou.option=1;
			    s=pthread_create(&tou, NULL,(void * (*)(void *)) &httpd_parse_resp, &agou);
				if ( s !=0 ) {
					errno=s;
					httpd_send_err( hc, 500, err500title, "", err500form, "thc" );
					exit(EXIT_FAILURE);
				}
			}

            if (wait(&s) == -1) {
					httpd_send_err( hc, 500, err500title, "", err500form, "wait" );
					exit(EXIT_FAILURE);
            }

			if ( interpose_output ) {
                pthread_join(tou, NULL);
            }
            shutdown( hc->conn_fd, SHUT_RDWR );
			exit(EXIT_SUCCESS);
		}
		/* child process */
		if ( interpose_input ) {
			dup2( pin[0], STDIN_FILENO );
#ifndef HAVE_CLOSEFROM
            close(pin[1]);
#endif
        }
		if ( interpose_output ) {
			dup2( pou[1], STDOUT_FILENO );
			dup2( pou[1], STDERR_FILENO );
#ifndef HAVE_CLOSEFROM
		    close(pou[0]);
#endif
		}
	}

#ifdef HAVE_CLOSEFROM
	closefrom(STDERR_FILENO+1);
#else
	{
		int i;
	/* Note: we arbitrarily choose a limit, because it should not exist fd after. But if it's false we have to use sysconf(_SC_OPEN_MAX) instead */
		for (i=STDERR_FILENO+1;i<10;i++)
			close(i);
	}
#endif

	/* cgi don't have to manage EINTR or EAGAIN, turn off no-delay mode. */
	httpd_clear_ndelay( STDIN_FILENO );
	httpd_clear_ndelay( STDOUT_FILENO );
	httpd_clear_ndelay( STDERR_FILENO ); //should be useless ass STDOUT_FILENO and STDERR_FILENO describe the same file

	/* Split the program into directory and binary, so we can chdir()
	** to the program's own directory.  This isn't in the CGI 1.1
	** spec, but it's what other HTTP servers do,
	** and run it. */
	execve(chdir_path(hc->realfilename), argp, envp );

	/* Something went wrong. */
	openlog( argv0, LOG_NDELAY|LOG_PID, LOG_FACILITY );
	syslog( LOG_ERR, "execve %.80s - %m", hc->realfilename );
	httpd_send_err( hc, 500, err500title, "", err500form, hc->encodedurl );
	shutdown( hc->conn_fd, SHUT_WR );
	exit(EXIT_FAILURE);
}

/*
 * \return a negative number to finish the connection, or 0 if success.
 */
int
httpd_start_request( httpd_conn* hc, struct timeval* nowP ) {
	static const char* index_names[] = { INDEX_NAMES };
	int i;
	size_t expnlen, indxlen;

	if ( hc->method != METHOD_GET && hc->method != METHOD_HEAD &&
		 hc->method != METHOD_POST )
		{
		httpd_send_err(
			hc, 501, err501title, "", err501form, httpd_method_str( hc->method ) );
		return -1;
		}

	/* Embedded action(s) on specific url */
	if ( !strncmp(hc->origfilename,"pks/",4) ) {
		if ( !strcmp(hc->origfilename+4,"lookup") )
			return launch_process(hkp_lookup, hc, METHOD_GET, "hkp");
		if ( !strcmp(hc->origfilename+4,"add") )
			return launch_process(hkp_add, hc, METHOD_POST, "hkp");
	}
#ifdef OPENUDC
	if ( !strncmp(hc->origfilename,"udc/",4) ) {
		if ( !strcmp(hc->origfilename+4,"create") )
			return launch_process(udc_create, hc, METHOD_POST, "udc");
		if ( !strcmp(hc->origfilename+4,"validate") )
			return launch_process(udc_validate, hc, METHOD_POST, "udc");
	}
#endif

	/* If there's no realfilename, it's should be a non-existent file. */
	if ( ! hc->realfilename ) {
		httpd_send_err( hc, 404, err404title, "", err404form, hc->encodedurl );
		return -1;
	}

	expnlen = strlen( hc->realfilename );

	/* Stat the file. */
	if ( stat( hc->realfilename, &hc->sb ) < 0 )
		{
		httpd_send_err( hc, 500, err500title, "", err500form, hc->encodedurl );
		return -1;
		}

	/* Is it world-readable or world-executable?  We check explicitly instead
	** of just trying to open it, so that no one ever gets surprised by
	** a file that's not set world-readable and yet somehow is
	** readable by the HTTP server and therefore the *whole* world.
	*/
	if ( ! ( hc->sb.st_mode & ( S_IROTH | S_IXOTH ) ) )
		{
		syslog(
			LOG_DEBUG,
			"%.80s URL \"%.80s\" resolves to a non world-readable file",
			hc->client_addr, hc->encodedurl );
		httpd_send_err(
			hc, 403, err403title, "",
			ERROR_FORM( err403form, "The requested URL '%.80s' resolves to a file that is not world-readable.\n" ),
			hc->encodedurl );
		return -1;
		}

#ifdef FORBID_HIDDEN_RESSOURCE
	{
	/* Is it hidden ?  ( basename or a parent dir beginning with a '.' ) */
	/* Note: we have stat realfilename wich is, if request was on a symlink, the symlink destination */
		char * cp=hc->realfilename;
		do {
			if ( cp[0] == '.' && cp[1] != '\0' ) {
				httpd_send_err(
					hc, 403, err403title, "",
					ERROR_FORM( err403form, "The requested URL '%.80s' resolves to something hidden (an element of its real path began with '.').\n" ),
					hc->encodedurl );
				return -1;
			}
		} while ( (cp=strchr(cp, '/')) && cp++ );
	}
#endif

#ifdef AUTH_FILE
		/* Check authorization for this directory. */
		if ( auth_check( hc ) == -1 )
			return -1;
#endif /* AUTH_FILE */

	/* Is it a directory? */
	if ( S_ISDIR(hc->sb.st_mode) )
		{
		/* Special handling for directory URLs that don't end in a slash.
		** We send back an explicit redirect with the slash, because
		** otherwise many clients can't build relative URLs properly.
		*/
		if ( strcmp( hc->origfilename, "" ) != 0 &&
			 strcmp( hc->origfilename, "." ) != 0 &&
			 hc->origfilename[strlen( hc->origfilename ) - 1] != '/' )
			{
			send_dirredirect( hc );
			return -1;
			}

		/* Check for an index file. */
		for ( i = 0; i < SIZEOFARRAY(index_names); ++i )
			{
			httpd_realloc_str(
				&hc->tmpbuff, &hc->maxtmpbuff,
				expnlen + 1 + strlen( index_names[i] ) );
			(void) strcpy( hc->tmpbuff, hc->realfilename );
			indxlen = strlen( hc->tmpbuff );
			if ( indxlen == 0 || hc->tmpbuff[indxlen - 1] != '/' )
				(void) strcat( hc->tmpbuff, "/" );
			if ( strcmp( hc->tmpbuff, "./" ) == 0 )
				hc->tmpbuff[0] = '\0';
			(void) strcat( hc->tmpbuff, index_names[i] );
			if ( stat( hc->tmpbuff, &hc->sb ) >= 0 )
				goto got_one;
			}

		/* Nope, no index file, so it's an actual directory request. */
#ifdef GENERATE_INDEXES
		/* Directories must be readable for indexing. */
		if ( ! ( hc->sb.st_mode & S_IROTH ) )
			{
			syslog(
				LOG_DEBUG,
				"%.80s URL \"%.80s\" tried to index a directory with indexing disabled",
				hc->client_addr, hc->encodedurl );
			httpd_send_err(
				hc, 403, err403title, "",
				ERROR_FORM( err403form, "The requested URL '%.80s' resolves to a directory that has indexing disabled.\n" ),
				hc->encodedurl );
			return -1;
			}

		/* Ok, generate an index. */
		return launch_process(ls, hc, METHOD_HEAD | METHOD_GET, "indexing");
#else /* GENERATE_INDEXES */
		syslog(
			LOG_DEBUG, "%.80s URL \"%.80s\" tried to index a directory",
			hc->client_addr, hc->encodedurl );
		httpd_send_err(
			hc, 403, err403title, "",
			ERROR_FORM( err403form, "The requested URL '%.80s' is a directory, and directory indexing is disabled on this server.\n" ),
			hc->encodedurl );
		return -1;
#endif /* GENERATE_INDEXES */

		got_one: ;
		/* Got an index file.  Expand symlinks again.
		*/
		free(hc->realfilename);
		hc->realfilename=realpath(hc->tmpbuff,NULL);

		/* If the expanded filename is not null, check that it's still
		** within the current directory or the alternate directory.
		*/
		if ( hc->realfilename ) {
			if ( strncmp(
					 hc->realfilename, hc->hs->cwd, strlen( hc->hs->cwd ) ) == 0 )
				{
				/* Elide the current directory. */
				(void) strcpy(
					hc->realfilename, &hc->realfilename[strlen( hc->hs->cwd )] );
				}
			else
				{
				syslog(
					LOG_NOTICE, "%.80s URL \"%.80s\" goes outside the web tree",
					hc->client_addr, hc->encodedurl );
				httpd_send_err(
					hc, 403, err403title, "",
					ERROR_FORM( err403form, "The requested URL '%.80s' resolves to a file outside the permitted web server directory tree.\n" ),
					hc->encodedurl );
				return -1;
				}
		} else {
			httpd_send_err( hc, 500, err500title, "", err500form, hc->encodedurl );
			return -1;
		}

		/* Now, is the index version world-readable or world-executable? */
		if ( ! ( hc->sb.st_mode & ( S_IROTH | S_IXOTH ) ) )
			{
			syslog(
				LOG_DEBUG,
				"%.80s URL \"%.80s\" resolves to a non-world-readable index file",
				hc->client_addr, hc->encodedurl );
			httpd_send_err(
				hc, 403, err403title, "",
				ERROR_FORM( err403form, "The requested URL '%.80s' resolves to an index file that is not world-readable.\n" ),
				hc->encodedurl );
			return -1;
			}
		}
	/* If it is not a regular file or a dir, forbid acces.  */
	else if ( ! S_ISREG(hc->sb.st_mode) )
		{
		syslog(
			LOG_DEBUG,
			"%.80s URL \"%.80s\" doesn't resolves to a regular file or a directory.",
			hc->client_addr, hc->encodedurl );
		httpd_send_err(
			hc, 403, err403title, "",
			ERROR_FORM( err403form, "The requested URL '%.80s' doesn't resolves to a regular file or a directory.\n" ),
			hc->encodedurl );
		return -1;
		}

	/* If it's world executable and not in the CGI area, or if there's 
	** pathinfo, someone's trying to either serve or run a non-CGI
	** file as CGI.  Either case is prohibited.
	*/
	if ( hc->sb.st_mode & S_IXOTH )
		{	
		if ( hc->hs->cgi_pattern != (char*) 0 
		&& match( hc->hs->cgi_pattern, hc->realfilename ) )
		    return launch_process(cgi_child, hc, METHOD_HEAD | METHOD_GET | METHOD_POST, "CGI");
		else
			{
			syslog(
				LOG_NOTICE, "%.80s URL \"%.80s\" is executable but isn't CGI",
				hc->client_addr, hc->encodedurl );
			httpd_send_err(
				hc, 403, err403title, "",
				ERROR_FORM( err403form, "The requested URL '%.80s' resolves to a file which is marked executable but is not a CGI file; retrieving it is forbidden.\n" ),
				hc->encodedurl );
			return -1;
			}
		}
	/* Fill in last_byte_index and first_byte_index,, if necessary. */
	if (hc->bfield & HC_GOT_RANGE) {
		if ( hc->first_byte_index < 0 ) {
			hc->first_byte_index = MAX(0,hc->sb.st_size + hc->first_byte_index);
			hc->last_byte_index = hc->sb.st_size - 1;
		} else if ( hc->last_byte_index == -1 || hc->last_byte_index >= hc->sb.st_size )
			hc->last_byte_index = hc->sb.st_size - 1;
	}

	figure_mime( hc );

	if ( hc->method == METHOD_HEAD ) {
		if ( (hc->bfield & HC_GOT_RANGE) &&
			 ( hc->last_byte_index >= hc->first_byte_index ) &&
			 ( ( hc->last_byte_index != hc->sb.st_size - 1 ) ||
			   ( hc->first_byte_index > 0 ) ) &&
			 ( hc->range_if == (time_t) -1 ||
			   hc->range_if == hc->sb.st_mtime ) )
		{
			send_mime(hc, 206, ok206title, hc->encodings, "", hc->type, hc->sb.st_size,hc->sb.st_mtime );
		}
		else {
			send_mime(hc, 200, ok200title, hc->encodings, "", hc->type, hc->sb.st_size,hc->sb.st_mtime );
		}
	}
	else if ( hc->if_modified_since != (time_t) -1 &&
		 hc->if_modified_since >= hc->sb.st_mtime )
		{
		send_mime(
			hc, 304, err304title, hc->encodings, "", hc->type, (off_t) -1,
			hc->sb.st_mtime );
		}
	else {
		hc->file_address = mmc_map( hc->realfilename, &(hc->sb), nowP );
		if ( hc->file_address == (char*) 0 ) {
			httpd_send_err( hc, 500, err500title, "", err500form, hc->encodedurl );
			return -1;
		}
		/* (Won't sign If To much forks are already running )*/
		if (hc->bfield & HC_DETACH_SIGN && ( hc->hs->cgi_limit <= 0 || hc->hs->cgi_count < hc->hs->cgi_limit ) ) {
			int ipid,p[2];

			if ( pipe( p ) < 0 ) {
				httpd_send_err( hc, 500, err500title, "", err500form, hc->encodedurl );
				return(-1);
			}
			ipid = fork( );
			if ( ipid < 0 ) {
				httpd_send_err( hc, 500, err500title, "", err500form, hc->encodedurl );
				return(-1);
			}
			if ( ipid == 0 ) {
				/* Child Interposer process. */
				interpose_args_t args = { p[0], hc->conn_fd, hc , 0 };
				child_r_start(hc);
				close(p[1]);
				httpd_parse_resp(&args);
				exit( 0 );
			}
			/* Parent process. */
			close(p[0]);
			drop_child("parse_resp",ipid,hc);
			/* overwrite hc->conn_fd by the pipe output */
			if ( dup2(p[1],hc->conn_fd) < 0 ) {
				httpd_send_err( hc, 500, err500title, "", err500form, "d" );
				close(p[1]); /* To end child */
				return(-1);
			}
			close(p[1]); /* it have been dupped on hc->conn_fd */
			/* Set the pipe write end to no-delay mode. */
			httpd_set_ndelay(hc->conn_fd);
		}

		if ( (hc->bfield & HC_GOT_RANGE) &&
			 ( hc->last_byte_index >= hc->first_byte_index ) &&
			 ( ( hc->last_byte_index != hc->sb.st_size - 1 ) ||
			   ( hc->first_byte_index > 0 ) ) &&
			 ( hc->range_if == (time_t) -1 ||
			   hc->range_if == hc->sb.st_mtime ) )
		{
			send_mime(hc, 206, ok206title, hc->encodings, "", hc->type, hc->sb.st_size,hc->sb.st_mtime );
		}
		else {
			send_mime(hc, 200, ok200title, hc->encodings, "", hc->type, hc->sb.st_size,hc->sb.st_mtime );
			hc->bfield &= ~HC_GOT_RANGE;
		}
	}
	return 0;
}

static void make_log_entry(const httpd_conn* hc, time_t now, int status) {
	char* ru;
	char* rfc1413;

	char url[305];
	char bytes[40];

	if ( hc->hs->bfield & HS_NO_LOG || hc->bfield & HC_LOG_DONE )
		return;

	/* This is straight CERN Combined Log Format - the only tweak
	** being that if we're using syslog() we leave out the date, because
	** syslogd puts it in.  The included syslogtocern script turns the
	** results into true CERN format.
	*/

	/* Format remote user. */
	if ( hc->remoteuser[0] != '\0' )
		ru = hc->remoteuser;
	else
		ru = "-";

	/* Format user-identifier,
	 * in fact we use the unused second field (rfc1413) to display
	 * first entry in the X-Forwarded-For header given by the client.
	 * Thus permit to log both real-IP client and the one given unsecurly by this header.*/
	if ( hc->forwardedfor[0] != '\0' ) {
		rfc1413 = hc->forwardedfor;
		rfc1413 += strcspn( hc->forwardedfor, " \t," );
		/* here we cut eventual intermediate proxy given by this header fields,
		 * but WARNING: we don't fix it after displaying log
		 * (because we are suppose to make_log_entry() at the end of request handle) */
		*rfc1413='\0';
		rfc1413 = hc->forwardedfor;
	}
	if ( hc->forwardedfor[0] == '\0' )
		/* retest in case client forge an "X-Forwarded-For: ," header */
		rfc1413 = "-";

	/* Format the url. */
	(void) snprintf( url, sizeof(url),"%.200s", hc->encodedurl );
	/* Format the bytes. */
	if ( hc->bytes_to_send >= 0 )
		(void) snprintf( bytes, sizeof(bytes), "%lld", (int64_t) hc->bytes_to_send );
	else
		(void) strcpy( bytes, "-" );

	/* Logfile or syslog? */
	if ( hc->hs->logfp != (FILE*) 0 ) {
		struct tm* t;
		const char* cernfmt_nozone = "%d/%b/%Y:%H:%M:%S";
		char date_nozone[100];
		int zone;
		char sign;
		char date[100];

		/* Get the current time, if necessary. */
		if ( now == (time_t) 0 )
			now = time( (time_t*) 0 );
		/* Format the time, forcing a numeric timezone (some log analyzers
		** are stoooopid about this).
		*/
		t = localtime( &now );
		(void) strftime( date_nozone, sizeof(date_nozone), cernfmt_nozone, t );
#ifdef HAVE_TM_GMTOFF
		zone = t->tm_gmtoff / 60L;
#else
		zone = -timezone / 60L;
		/* Probably have to add something about daylight time here. */
#endif
		if ( zone >= 0 )
			sign = '+';
		else {
			sign = '-';
			zone = -zone;
		}
		zone = ( zone / 60 ) * 100 + zone % 60;
		(void) snprintf( date, sizeof(date),
			"%s %c%04d", date_nozone, sign, zone );
		/* And write the log entry. */
		(void) fprintf( hc->hs->logfp,
			"%.80s %.80s %.80s [%s] \"%.80s %.80s%.300s %.80s\" %d %s \"%.200s\" \"%.200s\"\n",
			hc->client_addr, rfc1413, ru, date, httpd_method_str( hc->method ),
			hc->hostdir, url, hc->protocol,
			status, bytes, hc->referer, hc->useragent );
#ifdef FLUSH_LOG_EVERY_TIME
		(void) fflush( hc->hs->logfp );
#endif
	} else
		syslog( LOG_INFO,
			"%.80s %.80s %.80s \"%.80s %.80s%.200s %.80s\" %d %s \"%.200s\" \"%.200s\"",
			hc->client_addr, rfc1413, ru, httpd_method_str( hc->method ),
			hc->hostdir, url, hc->protocol,
			status, bytes, hc->referer, hc->useragent );

}

char *get_ip_str(const struct sockaddr * sa) {
	char str[MAX(INET6_ADDRSTRLEN,INET_ADDRSTRLEN)+1];

#if 0
// getnameinfo vs inet_ntop = ?? vs perfomance ?? */
	if (getnameinfo( sa, sockaddr_len( sa ), str, sizeof(str), 0, 0, NI_NUMERICHOST ))
		strncpy(str, "fail", sizeof(str));
#endif
	switch(sa->sa_family) {
		case AF_INET:
			inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),str, sizeof(str));
		break;

		case AF_INET6:
			inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),str, sizeof(str));
			// Elide IPv6ish prefix for IPv4 addresses.
			/*if ( IN6_IS_ADDR_V4MAPPED( &(((struct sockaddr_in6 *)sa)->sin6_addr ) && strncmp( str, "::ffff:", 7 ) == 0 )
				return strdup(&str[7]); */
		break;

		default:
			strncpy(str, "Unknown AF", sizeof(str));
	}
	return strdup(str);
}

static inline int sockaddr_check( const struct sockaddr * sa ) {
	switch ( sa->sa_family ) {
		case AF_INET: return 1;
		case AF_INET6: return 1;
		default: return 0;
	}
}

static inline size_t sockaddr_len( const struct sockaddr * sa ) {
	switch ( sa->sa_family ) {
		case AF_UNIX: return sizeof(struct sockaddr_un);
		case AF_INET: return sizeof(struct sockaddr_in);
		case AF_INET6: return sizeof(struct sockaddr_in6);
		default: return 0;		/* shouldn't happen */
	}
}

/* like dprintf, but manage EAGAIN or EINTR.
*/
int httpd_dprintf( int fd, const char* format, ... ) {
	va_list ap;
	int r;
	char * buf=malloc(MAXPATHLEN);
	
	if (!buf)
		return(-1);

	va_start( ap, format );
	r=vsnprintf(buf,MAXPATHLEN,format,ap);
	va_end( ap );
	if (r>=MAXPATHLEN) {
		char * buf2;
		if ((buf2=realloc(buf,r+1)) != buf ) {
				free(buf);
				buf=buf2;
		}
		if (!buf)
			return(-1);
		va_start( ap, format );
		r=vsnprintf(buf,r,format,ap);
		va_end( ap );
	}

	if (r>0) {
		r=httpd_write_fully(fd,buf,r);
		free(buf);
		return r;
	}
	free(buf);
	return r;
}

/*! httpd_read_fully read the requested buffer completely, accounting for interruptions.
 * \return the effective number of bytes read.
 */
ssize_t
httpd_read_fully( int fd, void* buf, size_t nbytes )
	{
	ssize_t nread=0;

	/*if (nbytes>SSIZE_MAX)
		nbytes=SSIZE_MAX;*/

	while ( nread < nbytes )
		{
		ssize_t r;

		r = read( fd, (char*) buf + nread, nbytes - nread );
		if ( r < 0 && ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) ) /* should only happen when O_NONBLOCK is set */
			{
			struct timespec tim={0, 300000000}; /* 300 ms */
			nanosleep(&tim, NULL);
			continue;
			}
		if ( r < 0 ) 
			{
			syslog( LOG_ERR, "httpd_read_fully - %d - %m", r );
			return r;
			}
		if ( r == 0 )
			break;
		nread += r;
		}

	return nread;
	}


/*! httpd_write_fully write the requested buffer completely, accounting for interruptions.
 * \return the effective number of bytes written.
 */
ssize_t httpd_write_fully( int fd, const void* buf, size_t nbytes ) {
	ssize_t nwritten=0;

	while ( nwritten < nbytes ) {
		ssize_t r;

		r = write( fd, (char*) buf + nwritten, nbytes - nwritten );
		if ( r < 0 && ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) ) { /* should only happen when O_NONBLOCK is set */ 
			struct timespec tim={0, 50000000}; /* 50 ms */
			nanosleep(&tim, NULL);
			continue;
		}
		if ( r < 0 ) {
			syslog( LOG_ERR, "httpd_write_fully (%d) - %m", fd );
			return r;
		}
		if ( r == 0 )
			break;
		nwritten += r;
	}

	return nwritten;
}

/* Generate debugging statistics syslog message. */
void
httpd_logstats( long secs )
	{
	if ( str_alloc_count > 0 )
		syslog( LOG_INFO,
			"  libhttpd - %d strings allocated, %lu bytes (%g bytes/str)",
			str_alloc_count, (unsigned long) str_alloc_size,
			(float) str_alloc_size / str_alloc_count );
	}

/* Generate a random string of size len from charset [G-Vg-v]
 * If buff is NULL a new string is allocated,
 * else len + 1 terminating null byte ('\0') will be wrinting into buff
 *\return a boundary string, or NULL if malloc failed.
 */
char *random_boundary(char * buff, unsigned short len) {

    char *q;
	static int srand_called=0;

	if (!buff && !(buff=malloc(len+1)) )
			return NULL;

	buff[len]='\0';

	/* call srand exactly one time to save bit cpu */
	if(!srand_called) {
		srandom(time((time_t *)0));
		srand_called=1;
	}
	for(q=buff; len; len--, q++) {
		//*q=(char) (rand()%2?rand()%16+103:rand()%16+71) ;
		*q=(char) (rand()%2?(rand()+(uintptr_t)buff)%16+103:rand()%16+71) ;
	}
	return buff;
}

