/* config.h - configuration defines for ludd and libhttpd
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

#ifndef _CONFIG_H_
#define _CONFIG_H_

/* CONFIGURE: OpenUDC support ( http://openudc.org ) */
//#define OPENUDC

#include "version.h"

/* The following configuration settings are sorted in order of decreasing
** likelihood that you'd want to change them - most likely first, least
** likely last.
**
** In case you're not familiar with the convention, "#ifdef notdef"
** is a Berkeleyism used to indicate temporarily disabled code.
** The idea here is that you re-enable it by just moving it outside
** of the ifdef.
*/

#ifdef OPENUDC

/* the 2 next cflags SHOULD be set by the cmake/configure OpenUDC options, but if not let's define them here */
#ifndef FORBID_HIDDEN_RESSOURCE
#define FORBID_HIDDEN_RESSOURCE
#endif /* FORBID_HIDDEN_RESSOURCE */
#ifndef CHECK_UDID2
#define CHECK_UDID2
#endif /* CHECK_UDID2 */
#ifndef CURRENCY_CODE
#define CURRENCY_CODE OPENUDC
#endif /* CURRENCY_CODE */

#else /* OPENUDC */

#ifndef VHOSTING
#define VHOSTING
#endif /* VHOSTING */

#endif /* OPENUDC */

/* CONFIGURE: CGI programs must match this pattern to get executed.  It's
** a simple shell-style wildcard pattern, with * meaning any string not
** containing a slash, ** meaning any string at all, and ? meaning any
** single character; or multiple such patterns separated by |.  The
** patterns get checked against the filename part of the incoming URL.
**
** Restricting CGI programs to a single directory lets the site administrator
** review them for security holes, and is strongly recommended.  If there
** are individual users that you trust, you can enable their directories too.
**
** You can also specify a CGI pattern on the command line, with the -c flag.
** Such a pattern overrides this compiled-in default.
**
** If no CGI pattern is specified, neither here nor on the command line,
** then CGI programs cannot be run at all.  If you want to disable CGI
** as a security measure that's how you do it, just don't define any
** pattern here and -c flag or "cgipat=..." will be ignored.
*/
#ifdef notdef
/* Some sample patterns.  Allow programs only in one central directory: */
#define CGI_PATTERN "/cgi-bin/*"
/* Allow programs in a central directory, or anywhere in a trusted
** user's tree: */
#define CGI_PATTERN "/cgi-bin/*|/jef/**"
/* Allow any program ending with a .cgi: */
#define CGI_PATTERN "*.cgi"
/* When virtual hosting, enable the central directory on every host: */
#define CGI_PATTERN "/*/cgi-bin/*"
/* no cgi at all (but user may define some with -c flag or "cgipat=..." */
#define CGI_PATTERN ""
#endif

#define CGI_PATTERN ""

/* CONFIGURE: Requested file or CGI must NOT match this pattern to get signed.
** It's a simple shell-style wildcard pattern, with * meaning any string
** not containing a slash, ** meaning any string at all, and ? meaning any
** single character; or multiple such patterns separated by |.  The
** patterns get checked against the filename part of the incoming URL.
**
** Signed response is asked by the client by specifying the "multipart/msigned"
** type in its "Accept:" request header.
** Here you may restrict the range of ressources which should answer to such
** request, which may save a lot of CPU if not caching the signature, or disk
** space if your server serve a bunch of little files.
**
** You can also specify this exclusion pattern on the command line, with the
** -s flag. Such a pattern overrides this compiled-in default.
**
** If no exclusion SIG pattern is specified, neither here nor on the command line,
** then everything may be signed by the server ("Content-Type: multpart/msigned"
** may be responded). If you want to disable completely signed responses, just
** don't define any pattern here (and "sigpat=..." or -s flag won't be read),
** or use the pattern "**".
**
** Note: Unlike CGI checking which follow first symlink destination,
** SIG checking is done with the asked filename in the URL, thus permit to
** restrict also signing done by embedded actions (like "pks/lookup").
*/
#define SIG_EXCLUDE_PATTERN ""

/* CONFIGURE: sigcache directory (inside the application home directory)
 * which contain all the cached signatures. If a "multipart/msigned" is asked
 * through the "Accept:" request header, and requested ressource match SIG_PATTERN ;
 * then it check if the expanded file is older than the cached signature.
 *
 * If the cached signature does not exist or is older than the file, then the
 * server will generate it into this dir (writing in must be enabled), else it
 * will use the cached signature to sign the requested file.
 *
 * You may undefine this to disable signatures caching, but that's not recommanded !
 */
#define SIG_CACHEDIR "sigcache"

/* CONFIGURE: Maximum number of simultaneous connexion per client (ip). 
 * This use external tool iptables (which have to be in your $PATH and
 * need the root privileges).
 * If this is defined to zero or less, no system("iptables...") calls
 * are done and it should be more sensitive to DOS attacks.
 * This can also be set in the runtime config file, the option name is
 * "connlimit".
*/
#ifndef DEFAULT_CONNLIMIT
#define DEFAULT_CONNLIMIT 0
#endif /* DEFAULT_CONNLIMIT */

/* CONFIGURE: How many seconds to allow CGI programs to run before killing
** them.  This is in case someone writes a CGI program that goes into an
** infinite loop, or does a massive database lookup that would take hours,
** or whatever.  If you don't want any limit, comment this out, but that's
** probably a really bad idea.
*/
#ifndef CGI_TIMELIMIT
#define CGI_TIMELIMIT 300
#endif /* CGI_TIMELIMIT */

/* CONFIGURE: Maximum number of simultaneous CGI programs allowed.
** If this many are already running, then attempts to run more will
** return an HTTP 503 error.  If this is defined to zero, there is
** no limit (and you'd better have a lot of memory).  This can also be
** set in the runtime config file, the option name is "cgilimit".
*/
#ifndef CGI_LIMIT
#define CGI_LIMIT 10000
#endif

/* CONFIGURE: How many seconds to allow for reading the initial request
** on a new connection.
*/
#define IDLE_READ_TIMELIMIT 15

/* CONFIGURE: How many seconds before an idle connection gets closed.
*/
#define IDLE_SEND_TIMELIMIT 300

/* CONFIGURE: The syslog facility to use.  Using this you can set up your
** syslog.conf so that all ludd messages go into a separate file.  Note
** that even if you use the -l command line flag to send logging to a
** file, errors still get sent via syslog.
*/
#define LOG_FACILITY LOG_DAEMON

/* CONFIGURE: The file to use for authentication.  If this is defined then
** ludd checks for this file in the local directory before every fetch.
** If the file exists then authentication is done, otherwise the fetch
** proceeds as usual.
**
** If you undefine this then ludd will not implement authentication
** at all and will not check for auth files, which saves a bit of CPU time.
*/
//#define AUTH_FILE ".htpasswd"

/* NOT WORKING: The file to use for OpenPGP POST authentication.  If this is defined
 * then ludd checks for this file in the local directory before every POST to a cgi.
 * If the file exists then authentication is done, otherwise the fetch
 * proceeds as usual.
 *
 * If authentication succeed, ie if the POST message is a "multipart/msigned" and
 * the key used to sign is in the file with a level greater or equal than 2 and the
 * signature is good. Then POST is transmited to cgi without "multipart/msigned"
 * encapsulation and such string "pgpuid=...&pgpfpr=...&pgpdate=" are added to the query string.
 *
 * If you undefine this then ludd will not implement OpenPGP POST authentication
 * at all and will not check for such files, which saves a bit of CPU time.
 * NOTE: That feature is not priority for external cgi. But we use a similar mechanism 
 * for udc/create or udc/validate.
*/
//#define PGPAUTH_FILE ".htPOSTpgp"

/* CONFIGURE: This is required for OpenUDC compatibility : it checks that your bot
 * certificate contain a valid udid2. 
 * (ie: "udid2;c;[A-Z]\{1,20\};[A-Z-]\{1,20\};[0-9-]\{10\};[0-9.e+-]\{14\};[0-9]\+" )
 * Note: If -nk is passed, which means new keys are accepted through pks/add, it will
 * also check that they contain a valid udid2 or ubot1.
 */
//#define CHECK_UDID2

/* CONFIGURE: It implies to log keys sended to pks/add (still via syslog).
 */
#define PKS_ADD_LOG

/* CONFIGURE: The default character set name to use with text MIME types.
** This gets substituted into the MIME types where they have a "%s".
**
** You can override this in the config file with the "charset" setting,
** or on the command like with the -T flag.
*/
#define DEFAULT_CHARSET "utf-8"

/* Most people won't want to change anything below here. */

/* CONFIGURE: Undefine this if you want ludd/thttpgpd to hide its specific
 * version when returning into to browsers.  Instead it'll just say "ludd"
 * or "thttpgpd" with no version.
*/
#define SHOW_SERVER_VERSION

/* CONFIGURE: When started as root, the default username to switch to after
** initializing.  If this user (or the one specified by the -u flag) does
** not exist, the program will refuse to run.
*/
#ifndef DEFAULT_USER
#define DEFAULT_USER "_"SOFTWARE_NAME
#endif

/* CONFIGURE: Default configuration file to search in the running directory.
 */
#define DEFAULT_CFILE SOFTWARE_NAME".conf"

/* CONFIGURE: data directory (inside the application home directory)
 * which contain all public data. It should also be defined in Makefiles
 * because there may be things (like cgi programs) to install in it.
 */
#define WEB_DIR "pub"

/* CONFIGURE: Forbid acces to hidden ressource
 * (those with an element of its path begining with a ".")
 */
//#define FORBID_HIDDEN_RESSOURCE

/* CONFIGURE: required (at least) version of gpgme
 */
#define GPGME_VERSION_MIN "1.2.0"

/* CONFIGURE: If this is defined, some of the built-in error pages will
** have more explicit information about exactly what the problem is.
** Some sysadmins don't like this, for security reasons.
*/
#define EXPLICIT_ERROR_PAGES

/* CONFIGURE: nice(2) value to use for CGI programs.  If this is undefined,
** CGI programs run at normal priority.
*/
//#define CGI_NICE 10

/* CONFIGURE: $PATH to use for CGI programs.
*/
#define CGI_PATH "/usr/local/bin:/bin:/usr/bin:/usr/lib/cgi-bin"

/* CONFIGURE: If defined, $LD_LIBRARY_PATH to use for CGI programs.
*/
#ifdef notdef
#define CGI_LD_LIBRARY_PATH "/usr/local/lib:/usr/lib"
#endif

/* CONFIGURE: How often to run the occasional cleanup job.
*/
#define OCCASIONAL_TIME 120

/* CONFIGURE: Seconds between stats syslogs.  If this is undefined then
** no stats are accumulated and no stats syslogs are done.
*/
#define STATS_TIME 36000

/* CONFIGURE: The mmap cache tries to keep the total number of mapped
** files below this number, so you don't run out of kernel file descriptors.
** If you have reconfigured your kernel to have more descriptors, you can
** raise this and ludd will keep more maps cached.  However it's not
** a hard limit, ludd will go over it if you really are accessing
** a whole lot of files.
*/
#define DESIRED_MAX_MAPPED_FILES 1000

/* CONFIGURE: The mmap cache also tries to keep the total mapped bytes
** below this number, so you don't run out of address space.  Again
** it's not a hard limit, ludd will go over it if you really are
** accessing a bunch of large files.
*/
#define DESIRED_MAX_MAPPED_BYTES 1000000000

/* You almost certainly don't want to change anything below here. */

/* CONFIGURE: When throttling CGI programs, we don't know how many bytes
** they send back to the client because it would be inefficient to
** interpose a counter.  CGI programs are much more expensive than
** regular files to serve, so we set an arbitrary and high byte count
** that gets applied to all CGI programs for throttling purposes.
*/
#define CGI_BYTECOUNT 25000

/* CONFIGURE: The default port to listen on.
 * 80 is the standard HTTP port, 11371 the standard HKP one.
*/
#ifndef DEFAULT_PORT
#define DEFAULT_PORT 11371
#endif /* DEFAULT_PORT */

/* CONFIGURE: A list of index filenames to check.  The files are searched
** for in this order.
*/
#define INDEX_NAMES "index.html", "index.htm", "index.xhtml", "index.xht", "Default.htm", "index.cgi"

/* CONFIGURE: If this is defined then ludd will automatically generate
** index pages for directories that don't have an explicit index file.
** If you want to disable this behavior site-wide, perhaps for security
** reasons, just undefine this.  Note that you can disable indexing of
** individual directories by merely doing a "chmod 711" on them - the
** standard Unix file permission to allow file access but disable "ls".
*/
#define GENERATE_INDEXES

/* CONFIGURE: Whether to log unknown request headers.  Most sites will not
** want to log them, which will save them a bit of CPU time.
*/
#ifdef notdef
#define LOG_UNKNOWN_HEADERS
#endif

/* CONFIGURE: Whether to fflush() the log file after each request.  If
** this is turned off there's a slight savings in CPU cycles.
*/
#define FLUSH_LOG_EVERY_TIME

/* CONFIGURE: Time between updates of the throttle table's rolling averages. */
#define THROTTLE_TIME 2

/* CONFIGURE: The listen() backlog queue length.  The 1024 doesn't actually
** get used, the kernel uses its maximum allowed value.  This is a config
** parameter only in case there's some OS where asking for too high a queue
** length causes an error.  Note that on many systems the maximum length is
** way too small - see http://www.acme.com/software/thttpd/notes.html
*/
#define LISTEN_BACKLOG 1024

/* CONFIGURE: Maximum number of throttle patterns that any single URL can
** be included in.  This has nothing to do with the number of throttle
** patterns that you can define, which is unlimited.
*/
#define MAXTHROTTLENUMS 10

/* CONFIGURE: Number of file descriptors to reserve for uses other than
** connections.  Currently this is 15, representing one for the listen fd,
** one for dup()ing at connection startup time, one for reading the file,
** one for syslog, and possibly one for the regular log file, which is
** five, plus some for who knows what (CGIs, parse_response...).
*/
#define SPARE_FDS 15

/* CONFIGURE: How many milliseconds to leave a connection open while doing a
** lingering close.
*/
#define LINGER_TIME 500

/* CONFIGURE: Maximum number of symbolic links to follow before
** assuming there's a loop.
*/
#define MAX_LINKS 16

/* CONFIGURE: You don't even want to know.
*/
#define MIN_WOULDBLOCK_DELAY 100L

#endif /* _CONFIG_H_ */
