/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2018, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

#include "curl_setup.h"

#include "urldata.h"
#include "urlapi-int.h"
#include "strcase.h"
#include "dotdot.h"
#include "url.h"
#include "escape.h"

/* The last 3 #include files should be in this order */
#include "curl_printf.h"
#include "curl_memory.h"
#include "memdebug.h"

/* Internal representation of CURLURL. Point to URL-encoded strings. */
struct Curl_URL {
  char *scheme;
  char *user;
  char *password;
  char *options; /* IMAP only? */
  char *host;
  char *port;
  char *path;
  char *query;
  char *fragment;

  char *scratch; /* temporary scratch area */
  long portnum; /* the numerical version */
};

#define DEFAULT_SCHEME "https"

/* free all members */
static void free_urlhandle(struct Curl_URL *u)
{
  free(u->scheme);
  free(u->user);
  free(u->password);
  free(u->options);
  free(u->host);
  free(u->port);
  free(u->path);
  free(u->query);
  free(u->fragment);
  free(u->scratch);
}

/* copy the full contents of one handle onto another */
static void copy_urlhandle(struct Curl_URL *to,
                           struct Curl_URL *from)
{
  *to = *from;
}

/*
 * Find the separator at the end of the host name, or the '?' in cases like
 * http://www.url.com?id=2380
 */
static const char *find_host_sep(const char *url)
{
  const char *sep;
  const char *query;

  /* Find the start of the hostname */
  sep = strstr(url, "//");
  if(!sep)
    sep = url;
  else
    sep += 2;

  query = strchr(sep, '?');
  sep = strchr(sep, '/');

  if(!sep)
    sep = url + strlen(url);

  if(!query)
    query = url + strlen(url);

  return sep < query ? sep : query;
}

/*
 * Decide in an encoding-independent manner whether a character in an
 * URL must be escaped. The same criterion must be used in strlen_url()
 * and strcpy_url().
 */
static bool urlchar_needs_escaping(int c)
{
    return !(ISCNTRL(c) || ISSPACE(c) || ISGRAPH(c));
}

/*
 * strlen_url() returns the length of the given URL if the spaces within the
 * URL were properly URL encoded.
 * URL encoding should be skipped for host names, otherwise IDN resolution
 * will fail.
 */
size_t Curl_strlen_url(const char *url, bool relative)
{
  const unsigned char *ptr;
  size_t newlen = 0;
  bool left = TRUE; /* left side of the ? */
  const unsigned char *host_sep = (const unsigned char *) url;

  if(!relative)
    host_sep = (const unsigned char *) find_host_sep(url);

  for(ptr = (unsigned char *)url; *ptr; ptr++) {

    if(ptr < host_sep) {
      ++newlen;
      continue;
    }

    switch(*ptr) {
    case '?':
      left = FALSE;
      /* fall through */
    default:
      if(urlchar_needs_escaping(*ptr))
        newlen += 2;
      newlen++;
      break;
    case ' ':
      if(left)
        newlen += 3;
      else
        newlen++;
      break;
    }
  }
  return newlen;
}

/* strcpy_url() copies a url to a output buffer and URL-encodes the spaces in
 * the source URL accordingly.
 * URL encoding should be skipped for host names, otherwise IDN resolution
 * will fail.
 */
void Curl_strcpy_url(char *output, const char *url, bool relative)
{
  /* we must add this with whitespace-replacing */
  bool left = TRUE;
  const unsigned char *iptr;
  char *optr = output;
  const unsigned char *host_sep = (const unsigned char *) url;

  if(!relative)
    host_sep = (const unsigned char *) find_host_sep(url);

  for(iptr = (unsigned char *)url;    /* read from here */
      *iptr;         /* until zero byte */
      iptr++) {

    if(iptr < host_sep) {
      *optr++ = *iptr;
      continue;
    }

    switch(*iptr) {
    case '?':
      left = FALSE;
      /* fall through */
    default:
      if(urlchar_needs_escaping(*iptr)) {
        snprintf(optr, 4, "%%%02x", *iptr);
        optr += 3;
      }
      else
        *optr++=*iptr;
      break;
    case ' ':
      if(left) {
        *optr++='%'; /* add a '%' */
        *optr++='2'; /* add a '2' */
        *optr++='0'; /* add a '0' */
      }
      else
        *optr++='+'; /* add a '+' here */
      break;
    }
  }
  *optr = 0; /* zero terminate output buffer */

}

/*
 * Returns true if the given URL is absolute (as opposed to relative)
 */
bool Curl_is_absolute_url(const char *url)
{
  char prot[16]; /* URL protocol string storage */
  char letter;   /* used for a silly sscanf */

  return (2 == sscanf(url, "%15[^?&/:]://%c", prot, &letter)) ? TRUE : FALSE;
}

/*
 * Concatenate a relative URL to a base URL making it absolute.
 * URL-encodes any spaces.
 * The returned pointer must be freed by the caller unless NULL
 * (returns NULL on out of memory).
 */
char *Curl_concat_url(const char *base, const char *relurl)
{
  /***
   TRY to append this new path to the old URL
   to the right of the host part. Oh crap, this is doomed to cause
   problems in the future...
  */
  char *newest;
  char *protsep;
  char *pathsep;
  size_t newlen;
  bool host_changed = FALSE;

  const char *useurl = relurl;
  size_t urllen;

  /* we must make our own copy of the URL to play with, as it may
     point to read-only data */
  char *url_clone = strdup(base);

  if(!url_clone)
    return NULL; /* skip out of this NOW */

  /* protsep points to the start of the host name */
  protsep = strstr(url_clone, "//");
  if(!protsep)
    protsep = url_clone;
  else
    protsep += 2; /* pass the slashes */

  if('/' != relurl[0]) {
    int level = 0;

    /* First we need to find out if there's a ?-letter in the URL,
       and cut it and the right-side of that off */
    pathsep = strchr(protsep, '?');
    if(pathsep)
      *pathsep = 0;

    /* we have a relative path to append to the last slash if there's one
       available, or if the new URL is just a query string (starts with a
       '?')  we append the new one at the end of the entire currently worked
       out URL */
    if(useurl[0] != '?') {
      pathsep = strrchr(protsep, '/');
      if(pathsep)
        *pathsep = 0;
    }

    /* Check if there's any slash after the host name, and if so, remember
       that position instead */
    pathsep = strchr(protsep, '/');
    if(pathsep)
      protsep = pathsep + 1;
    else
      protsep = NULL;

    /* now deal with one "./" or any amount of "../" in the newurl
       and act accordingly */

    if((useurl[0] == '.') && (useurl[1] == '/'))
      useurl += 2; /* just skip the "./" */

    while((useurl[0] == '.') &&
          (useurl[1] == '.') &&
          (useurl[2] == '/')) {
      level++;
      useurl += 3; /* pass the "../" */
    }

    if(protsep) {
      while(level--) {
        /* cut off one more level from the right of the original URL */
        pathsep = strrchr(protsep, '/');
        if(pathsep)
          *pathsep = 0;
        else {
          *protsep = 0;
          break;
        }
      }
    }
  }
  else {
    /* We got a new absolute path for this server */

    if((relurl[0] == '/') && (relurl[1] == '/')) {
      /* the new URL starts with //, just keep the protocol part from the
         original one */
      *protsep = 0;
      useurl = &relurl[2]; /* we keep the slashes from the original, so we
                              skip the new ones */
      host_changed = TRUE;
    }
    else {
      /* cut off the original URL from the first slash, or deal with URLs
         without slash */
      pathsep = strchr(protsep, '/');
      if(pathsep) {
        /* When people use badly formatted URLs, such as
           "http://www.url.com?dir=/home/daniel" we must not use the first
           slash, if there's a ?-letter before it! */
        char *sep = strchr(protsep, '?');
        if(sep && (sep < pathsep))
          pathsep = sep;
        *pathsep = 0;
      }
      else {
        /* There was no slash. Now, since we might be operating on a badly
           formatted URL, such as "http://www.url.com?id=2380" which doesn't
           use a slash separator as it is supposed to, we need to check for a
           ?-letter as well! */
        pathsep = strchr(protsep, '?');
        if(pathsep)
          *pathsep = 0;
      }
    }
  }

  /* If the new part contains a space, this is a mighty stupid redirect
     but we still make an effort to do "right". To the left of a '?'
     letter we replace each space with %20 while it is replaced with '+'
     on the right side of the '?' letter.
  */
  newlen = Curl_strlen_url(useurl, !host_changed);

  urllen = strlen(url_clone);

  newest = malloc(urllen + 1 + /* possible slash */
                  newlen + 1 /* zero byte */);

  if(!newest) {
    free(url_clone); /* don't leak this */
    return NULL;
  }

  /* copy over the root url part */
  memcpy(newest, url_clone, urllen);

  /* check if we need to append a slash */
  if(('/' == useurl[0]) || (protsep && !*protsep) || ('?' == useurl[0]))
    ;
  else
    newest[urllen++]='/';

  /* then append the new piece on the right side */
  Curl_strcpy_url(&newest[urllen], useurl, !host_changed);

  free(url_clone);

  return newest;
}

/*
 * parse_hostname_login()
 *
 * Parse the login details (user name, password and options) from the URL and
 * strip them out of the host name
 *
 */
static CURLUcode parse_hostname_login(struct Curl_URL *u, char **hostname,
                                      unsigned int flags)
{
  CURLUcode result = CURLURLE_OK;
  CURLcode ccode;
  char *userp = NULL;
  char *passwdp = NULL;
  char *optionsp = NULL;

  /* At this point, we're hoping all the other special cases have
   * been taken care of, so conn->host.name is at most
   *    [user[:password][;options]]@]hostname
   *
   * We need somewhere to put the embedded details, so do that first.
   */

  char *ptr = strchr(*hostname, '@');
  char *login = *hostname;

  if(!ptr)
    goto out;

  /* We will now try to extract the
   * possible login information in a string like:
   * ftp://user:password@ftp.my.site:8021/README */
  *hostname = ++ptr;

  /* We could use the login information in the URL so extract it. Only parse
     options if the handler says we should. */
  ccode = Curl_parse_login_details(login, ptr - login - 1,
                                   &userp, &passwdp, &optionsp);
  if(ccode) {
    result = CURLURLE_MALFORMED_INPUT;
    goto out;
  }

  if(userp) {
    if(flags & CURLURL_DISALLOW_USER) {
      /* Option DISALLOW_USER is set and url contains username. */
      result = CURLURLE_USER_NOT_ALLOWED;
      goto out;
    }

    u->user = userp;
  }

  if(passwdp)
    u->password = passwdp;

  if(optionsp)
    u->options = optionsp;

  return CURLURLE_OK;
  out:

  free(userp);
  free(passwdp);
  free(optionsp);

  return result;
}

static CURLUcode parse_port(struct Curl_URL *u, char *hostname)
{
  char *portptr;
  char endbracket;
  int len;

  if((1 == sscanf(hostname, "[%*45[0123456789abcdefABCDEF:.]%c%n",
                  &endbracket, &len)) &&
     (']' == endbracket)) {
    /* this is a RFC2732-style specified IP-address */
    portptr = &hostname[len];
    if (*portptr != ':')
      return CURLURLE_MALFORMED_INPUT;
  }
  else
    portptr = strchr(hostname, ':');

  if(portptr) {
    char *rest;
    long port;
    char portbuf[7];

    port = strtol(portptr + 1, &rest, 10);  /* Port number must be decimal */

    if((port <= 0) || (port > 0xffff))
      /* Single unix standard says port numbers are 16 bits long, but we don't
         treat port zero as OK. */
      return CURLURLE_BAD_PORT_NUMBER;

    if(rest[0])
      return CURLURLE_BAD_PORT_NUMBER;

    if(rest != &portptr[1]) {
      *portptr++ = '\0'; /* cut off the name there */
      *rest = 0;
      /* generate a new to get rid of leading zeroes etc */
      snprintf(portbuf, sizeof(portbuf), "%ld", port);
      u->portnum = port;
      u->port = strdup(portbuf);
      if(!u->port)
        return CURLURLE_OUT_OF_MEMORY;
    }
    else {
      /* Browser behavior adaptation. If there's a colon with no digits after,
         just cut off the name there which makes us ignore the colon and just
         use the default port. Firefox and Chrome both do that. */
      *portptr = '\0';
    }
  }

  return CURLURLE_OK;
}

static CURLUcode hostname_check(char *hostname, unsigned int flags)
{
  const char *l; /* accepted characters */
  size_t len;
  size_t hlen = strlen(hostname);
  (void)flags;

  if(hostname[0] == '[') {
    hostname++;
    l = "0123456789abcdefABCDEF::.";
    hlen -= 2;
  }
  else
    l = "0123456789abcdefghijklimnopqrstuvwxyz-.ABCDEFGHIJKLIMNOPQRSTUVWXYZ";

  len = strspn(hostname, l);
  if(hlen != len)
    /* hostname with bad content */
    return CURLURLE_MALFORMED_INPUT;

  return CURLURLE_OK;
}

/*
 * Parse the URL and setup the relevant members of the Curl_URL struct.
 */
static CURLUcode parseurl(char *url, struct Curl_URL *u, unsigned int flags)
{
  char *path;
  bool path_alloced = FALSE;
  char *hostname;
  char *query = NULL;
  char *fragment = NULL;
  int rc;
  CURLUcode result;
  bool url_has_scheme = FALSE;
  size_t urllen = strlen(url);

  /*************************************************************
   * Parse the URL.
   *
   * We need to parse the url even when using the proxy, because we will need
   * the hostname and port in case we are trying to SSL connect through the
   * proxy -- and we don't know if we will need to use SSL until we parse the
   * url ...
   ************************************************************/
  if(url[0] == ':')
    return CURLURLE_MALFORMED_INPUT;

  /* allocate scratch area */
  path = u->scratch = malloc(urllen * 2 + 2);
  if(!path)
    return CURLURLE_OUT_OF_MEMORY;

  hostname = &path[urllen + 1];

  /* MSDOS/Windows style drive prefix, eg c: in c:foo */
#define STARTS_WITH_DRIVE_PREFIX(str) \
  ((('a' <= str[0] && str[0] <= 'z') || \
    ('A' <= str[0] && str[0] <= 'Z')) && \
   (str[1] == ':'))

  /* MSDOS/Windows style drive prefix, optionally with
   * a '|' instead of ':', followed by a slash or NUL */
#define STARTS_WITH_URL_DRIVE_PREFIX(str) \
  ((('a' <= (str)[0] && (str)[0] <= 'z') || \
    ('A' <= (str)[0] && (str)[0] <= 'Z')) && \
   ((str)[1] == ':' || (str)[1] == '|') && \
   ((str)[2] == '/' || (str)[2] == '\\' || (str)[2] == 0))

  { /* check for a scheme */
    int i;
    for(i = 0; i < 16 && url[i]; ++i) {
      if(url[i] == '/')
        break;
      if(url[i] == ':') {
        url_has_scheme = TRUE;
        break;
      }
    }
  }

  /* handle the file: scheme */
  if(url_has_scheme && strncasecompare(url, "file:", 5)) {
    rc = sscanf(url, "%*15[^\n/:]:%[^\n]", path);
    if(rc != 1)
      return CURLURLE_MALFORMED_INPUT;

    /* Extra handling URLs with an authority component (i.e. that start with
     * "file://")
     *
     * We allow omitted hostname (e.g. file:/<path>) -- valid according to
     * RFC 8089, but not the (current) WHAT-WG URL spec.
     */
    if(path[0] == '/' && path[1] == '/') {
      /* swallow the two slashes */
      char *ptr = &path[2];

      /*
       * According to RFC 8089, a file: URL can be reliably dereferenced if:
       *
       *  o it has no/blank hostname, or
       *
       *  o the hostname matches "localhost" (case-insensitively), or
       *
       *  o the hostname is a FQDN that resolves to this machine.
       *
       * For brevity, we only consider URLs with empty, "localhost", or
       * "127.0.0.1" hostnames as local.
       *
       * Additionally, there is an exception for URLs with a Windows drive
       * letter in the authority (which was accidentally omitted from RFC 8089
       * Appendix E, but believe me, it was meant to be there. --MK)
       */
      if(ptr[0] != '/' && !STARTS_WITH_URL_DRIVE_PREFIX(ptr)) {
        /* the URL includes a host name, it must match "localhost" or
           "127.0.0.1" to be valid */
        if(!checkprefix("localhost/", ptr) &&
           !checkprefix("127.0.0.1/", ptr)) {
          /* Invalid file://hostname/, expected localhost or 127.0.0.1 or
             none */
          return CURLURLE_MALFORMED_INPUT;
        }
        ptr += 9; /* now points to the slash after the host */
      }

      /* This cannot be done with strcpy, as the memory chunks overlap! */
      memmove(path, ptr, strlen(ptr) + 1);
    }

#if !defined(MSDOS) && !defined(WIN32) && !defined(__CYGWIN__)
    /* Don't allow Windows drive letters when not in Windows.
     * This catches both "file:/c:" and "file:c:" */
    if(('/' == path[0] && STARTS_WITH_URL_DRIVE_PREFIX(&path[1])) ||
       STARTS_WITH_URL_DRIVE_PREFIX(path)) {
      /* File drive letters are only accepted in MSDOS/Windows */
      return CURLURLE_MALFORMED_INPUT;
    }
#else
    /* If the path starts with a slash and a drive letter, ditch the slash */
    if('/' == path[0] && STARTS_WITH_URL_DRIVE_PREFIX(&path[1])) {
      /* This cannot be done with strcpy, as the memory chunks overlap! */
      memmove(path, &path[1], strlen(&path[1]) + 1);
    }
#endif

  }
  else {
    /* clear path */
    char slashbuf[4];
    char schemebuf[16];
    char *schemep = &schemebuf[0];
    path[0] = 0;

    rc = sscanf(url,
                "%15[^\n/:]:%3[/]%[^\n/?#]%[^\n]",
                schemebuf, slashbuf, hostname, path);
    if(2 == rc) {
      return CURLURLE_MALFORMED_INPUT;
    }
    if(3 > rc) {

      if(!(flags & CURLURL_DEFAULT_SCHEME))
        return CURLURLE_MALFORMED_INPUT;

      /*
       * The URL was badly formatted, let's try without scheme specified.
       */
      rc = sscanf(url, "%[^\n/?#]%[^\n]", hostname, path);
      if(1 > rc) {
        /*
         * We couldn't even get this format. djgpp 2.04 has a sscanf() bug
         * where 'hostname' is assigned, but the return value is EOF!
         */
#if defined(__DJGPP__) && (DJGPP_MINOR == 4)
        if(!(rc == -1 && *hostname))
#endif
        {
          return CURLURLE_MALFORMED_INPUT;
        }
      }

      schemep = (char *) DEFAULT_SCHEME;
    }

    if(!Curl_builtin_scheme(schemep) &&
       !(flags & CURLURL_NON_SUPPORT_SCHEME))
      return CURLURLE_UNSUPPORTED_SCHEME;

    u->scheme = strdup(schemep);
    if(!u->scheme)
      return CURLURLE_OUT_OF_MEMORY;
  }

  query = strchr(path, '?');
  if(query)
    *query++ = 0;

  fragment = strchr(query?query:path, '#');
  if(fragment)
    *fragment++ = 0;

  if(!path[0])
    /* if there's no path set, use a single slash */
    path = (char *)"/";

  /* If the URL is malformatted (missing a '/' after hostname before path) we
   * insert a slash here. The only letters except '/' that can start a path is
   * '?' and '#' - as controlled by the two sscanf() patterns above.
   */
  if(path[0] != '/') {
    /* We need this function to deal with overlapping memory areas. We know
       that the memory area 'path' points to is 'urllen' bytes big and that
       is bigger than the path. Use +1 to move the zero byte too. */
    memmove(&path[1], path, strlen(path) + 1);
    path[0] = '/';
  }
  else if(!(flags & CURLURL_PATH_AS_IS)) {
    /* sanitise paths and remove ../ and ./ sequences according to RFC3986 */
    char *newp = Curl_dedotdotify(path);
    if(!newp)
      return CURLURLE_OUT_OF_MEMORY;

    if(strcmp(newp, path)) {
      /* if we got a new version */
      path = newp;
      path_alloced = TRUE;
    }
    else
      free(newp);
  }

  u->path = path_alloced?path:strdup(path);
  if(!u->path)
    return CURLURLE_OUT_OF_MEMORY;

  /*
   * Parse the login details and strip them out of the host name.
   */
  result = parse_hostname_login(u, &hostname, flags);
  if(result)
    return result;

  result = parse_port(u, hostname);
  if(result)
    return result;

  result = hostname_check(hostname, flags);
  if(result)
    return result;

  u->host = strdup(hostname);
  if(!u->host)
    return CURLURLE_OUT_OF_MEMORY;

  if(query && query[0]) {
    u->query = strdup(query);
    if(!u->query)
      return CURLURLE_OUT_OF_MEMORY;
  }
  if(fragment && fragment[0]) {
    u->fragment = strdup(fragment);
    if(!u->fragment)
      return CURLURLE_OUT_OF_MEMORY;
  }

  free(u->scratch);
  u->scratch = NULL;

  return CURLURLE_OK;
}

/*
 */
CURLUcode curl_url(char *URL, CURLURL **urlhandle, unsigned int flags)
{
  struct Curl_URL *u;
  CURLUcode result = CURLURLE_OK;
  *urlhandle = NULL;
  u = calloc(sizeof(struct Curl_URL), 1);
  if(!u)
    return CURLURLE_OUT_OF_MEMORY;
  result = parseurl(URL, u, flags);
  if(result || (flags & CURLURL_VERIFY_ONLY))
    curl_url_cleanup(u);
  else
    *urlhandle = u;
  return result;
}

void curl_url_cleanup(CURLURL *u)
{
  if(u) {
    free_urlhandle(u);
    free(u);
  }
}

#define DUP(dest, src, name)         \
  if(src->name) {                    \
    dest->name = strdup(src->name);  \
    if(!dest->name)                  \
      goto fail;                     \
  }

CURLURL *curl_url_dup(CURLURL *in)
{
  struct Curl_URL *u = calloc(sizeof(struct Curl_URL), 1);
  if(u) {
    DUP(u, in, scheme);
    DUP(u, in, user);
    DUP(u, in, password);
    DUP(u, in, options);
    DUP(u, in, host);
    DUP(u, in, port);
    DUP(u, in, path);
    DUP(u, in, query);
    DUP(u, in, fragment);
    u->portnum = in->portnum;
  }
  return u;
  fail:
  curl_url_cleanup(u);
  return NULL;
}

CURLUcode curl_url_get(CURLURL *u, CURLUPart what,
                       char **part, unsigned int flags)
{
  char *ptr;
  CURLUcode ifmissing;
  char portbuf[7];
  (void)flags;
  if(!u)
    return CURLURLE_BAD_HANDLE;
  if(!part)
    return CURLURLE_BAD_PARTPOINTER;
  *part = NULL;

  switch(what) {
  case CURLUPART_SCHEME:
    ptr = u->scheme;
    ifmissing = CURLURLE_NO_SCHEME;
    break;
  case CURLUPART_USER:
    ptr = u->user;
    ifmissing = CURLURLE_NO_USER;
    break;
  case CURLUPART_PASSWORD:
    ptr = u->password;
    ifmissing = CURLURLE_NO_PASSWORD;
    break;
  case CURLUPART_OPTIONS:
    ptr = u->options;
    ifmissing = CURLURLE_NO_OPTIONS;
    break;
  case CURLUPART_HOST:
    ptr = u->host;
    ifmissing = CURLURLE_NO_HOST;
    break;
  case CURLUPART_PORT:
    ptr = u->port;
    ifmissing = CURLURLE_NO_PORT;
    if(!ptr && (flags & CURLURL_DEFAULT_PORT) && u->scheme) {
      /* there's no stored port number, but asked to deliver
         a default one for the scheme */
      const struct Curl_handler *h =
        Curl_builtin_scheme(u->scheme);
      if(h) {
        snprintf(portbuf, sizeof(portbuf), "%ld", h->defport);
        ptr = portbuf;
      }
    }
    else if(ptr && u->scheme) {
      /* there is a stored port number, but ask to inhibit if
         it matches the default one for the scheme */
      const struct Curl_handler *h =
        Curl_builtin_scheme(u->scheme);
      if(h && (h->defport == u->portnum) &&
         (flags & CURLURL_NO_DEFAULT_PORT))
        ptr = NULL;
    }
    break;
  case CURLUPART_PATH:
    ptr = u->path;
    ifmissing = CURLURLE_NO_PATH;
    break;
  case CURLUPART_QUERY:
    ptr = u->query;
    ifmissing = CURLURLE_NO_QUERY;
    break;
  case CURLUPART_FRAGMENT:
    ptr = u->fragment;
    ifmissing = CURLURLE_NO_FRAGMENT;
    break;
  case CURLUPART_URL:
    if(u->host) {
      char *url;
      char *scheme;
      char *port = u->port;
      if(u->scheme)
        scheme = u->scheme;
      else if(flags & CURLURL_DEFAULT_SCHEME)
        scheme = (char *) DEFAULT_SCHEME;
      else
        return CURLURLE_NO_SCHEME;

      if(scheme) {
        if(!port && (flags & CURLURL_DEFAULT_PORT)) {
          /* there's no stored port number, but asked to deliver
             a default one for the scheme */
          const struct Curl_handler *h = Curl_builtin_scheme(scheme);
          if(h) {
            snprintf(portbuf, sizeof(portbuf), "%ld", h->defport);
            port = portbuf;
          }
        }
        else if(port) {
          /* there is a stored port number, but asked to inhibit if it matches
             the default one for the scheme */
          const struct Curl_handler *h = Curl_builtin_scheme(scheme);
          if(h && (h->defport == u->portnum) &&
             (flags & CURLURL_NO_DEFAULT_PORT))
            port = NULL;
        }
      }
      url = aprintf("%s://%s%s%s%s%s%s%s%s%s%s%s%s",
                    scheme,
                    u->user ? u->user : "",
                    u->password ? ":": "",
                    u->password ? u->password : "",
                    (u->user || u->password) ? "@": "",
                    u->host,
                    port ? ":": "",
                    port ? port : "",
                    u->path,
                    u->query? "?": "",
                    u->query? u->query : "",
                    u->fragment? "#": "",
                    u->fragment? u->fragment : ""
        );
      if(!url)
        return CURLURLE_OUT_OF_MEMORY;
      *part = url;
      return CURLURLE_OK;
    }
    else {
      /* can still do file: */
      return CURLURLE_NO_HOST;
    }
    break;
  default:
    ifmissing = CURLURLE_UNKNOWN_PART;
    ptr = NULL;
  }
  if(ptr) {
    *part = strdup(ptr);
    if(!*part)
      return CURLURLE_OUT_OF_MEMORY;
    return CURLURLE_OK;
  }
  else
    return ifmissing;
}

CURLUcode curl_url_set(CURLURL *u, CURLUPart what,
                       char *part, unsigned int flags)
{
  char **storep;
  long port = 0;
  if(!u)
    return CURLURLE_BAD_HANDLE;
  if(!part)
    return CURLURLE_BAD_PARTPOINTER;

  switch(what) {
  case CURLUPART_SCHEME:
    if(!(flags & CURLURL_NON_SUPPORT_SCHEME) &&
       /* verify that it is a fine scheme */
       !Curl_builtin_scheme(part))
      return CURLURLE_UNSUPPORTED_SCHEME;
    storep = &u->scheme;
    break;
  case CURLUPART_USER:
    storep = &u->user;
    break;
  case CURLUPART_PASSWORD:
    storep = &u->password;
    break;
  case CURLUPART_OPTIONS:
    storep = &u->options;
    break;
  case CURLUPART_HOST:
    storep = &u->host;
    break;
  case CURLUPART_PORT:
    port = strtol(part, NULL, 10);  /* Port number must be decimal */
    if((port <= 0) || (port > 0xffff))
      return CURLURLE_BAD_PORT_NUMBER;
    storep = &u->port;
    break;
  case CURLUPART_PATH:
    storep = &u->path;
    break;
  case CURLUPART_QUERY:
    storep = &u->query;
    break;
  case CURLUPART_FRAGMENT:
    storep = &u->fragment;
    break;
  case CURLUPART_URL: {
    /*
     * Allow a new absolute URL to replace the existing contents.
     *
     * If the existing contents is enough for a URL, allow a relative URL to
     * replace it.
     */
    CURLUcode result;
    char *oldurl;
    char *redired_url;
    CURLURL *handle2;
    if(Curl_is_absolute_url(part)) {
      result = curl_url(part, &handle2, flags);
      if(!result) {
        free_urlhandle(u);
        copy_urlhandle(u, handle2);
        free(handle2);
      }
      return result;
    }
    /* extract the full "old" URL to do the redirect on */
    result = curl_url_get(u, CURLUPART_URL, &oldurl, flags);
    if(result)
      return result;

    /* apply the relative part to create a new URL */
    redired_url = Curl_concat_url(oldurl, part);
    free(oldurl);
    if(!redired_url)
      return CURLURLE_OUT_OF_MEMORY;

    /* now parse the new URL */
    result = curl_url(redired_url, &handle2, flags);
    free(redired_url);
    if(!result) {
      free_urlhandle(u);
      copy_urlhandle(u, handle2);
      free(handle2);
    }
    return result;
  }
  default:
    return CURLURLE_UNKNOWN_PART;
  }
  if(storep) {
    char *newp = strdup(part);
    if(!*newp)
      return CURLURLE_OUT_OF_MEMORY;
    free(*storep);
    *storep = newp;
  }
  /* set after the string, to make it not assigned if the allocation above
     fails */
  if(port)
    u->portnum = port;
  return CURLURLE_OK;
}
