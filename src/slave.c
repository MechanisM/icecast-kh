/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org, 
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 */

/* -*- c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/* slave.c
 * by Ciaran Anscomb <ciaran.anscomb@6809.org.uk>
 *
 * Periodically requests a list of streams from a master server
 * and creates source threads for any it doesn't already have.
 * */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#else
#include <winsock2.h>
#endif
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif
#ifdef HAVE_GETRLIMIT
#include <sys/resource.h>
#endif

#include "compat.h"

#include "timing/timing.h"
#include "thread/thread.h"
#include "avl/avl.h"
#include "net/sock.h"
#include "httpp/httpp.h"

#include "cfgfile.h"
#include "global.h"
#include "util.h"
#include "connection.h"
#include "refbuf.h"
#include "client.h"
#include "stats.h"
#include "logging.h"
#include "source.h"
#include "format.h"
#include "event.h"
#include "yp.h"
#include "slave.h"

#define CATMODULE "slave"


static void _slave_thread(void);
static void redirector_add (const char *server, int port, int interval);
static redirect_host *find_slave_host (const char *server, int port);
static int  relay_startup (client_t *client);
static int  relay_initialise (client_t *client);
static int  relay_read (client_t *client);
static void relay_release (client_t *client);

int slave_running = 0;
int worker_count;
int relays_connecting;
int streamlister;

static volatile int update_settings = 0;
static volatile int update_all_mounts = 0;
static volatile int restart_connection_thread = 0;
static time_t streamlist_check = 0;
static rwlock_t slaves_lock;
static spin_t relay_start_lock;

redirect_host *redirectors;
worker_t *workers;
rwlock_t workers_lock;

struct _client_functions relay_client_ops =
{
    relay_read,
    relay_release
};

struct _client_functions relay_startup_ops =
{
    relay_startup,
    relay_release
};

struct _client_functions relay_init_ops =
{
    relay_initialise,
    relay_release
};


relay_server *relay_copy (relay_server *r)
{
    relay_server *copy = calloc (1, sizeof (relay_server));

    if (copy)
    {
        relay_server_master *from = r->masters, **insert = &copy->masters;

        while (from)
        {
            relay_server_master *to = calloc (1, sizeof (relay_server_master));
            to->ip = (char *)xmlCharStrdup (from->ip);
            to->mount = (char *)xmlCharStrdup (from->mount);
            if (from->bind)
                to->bind = (char *)xmlCharStrdup (from->bind);
            to->port = from->port;
            to->timeout = from->timeout;
            *insert = to;
            from = from->next;
            insert = &to->next;
        }

        copy->localmount = (char *)xmlStrdup (XMLSTR(r->localmount));
        if (r->username)
            copy->username = (char *)xmlStrdup (XMLSTR(r->username));
        if (r->password)
            copy->password = (char *)xmlStrdup (XMLSTR(r->password));
        copy->mp3metadata = r->mp3metadata;
        copy->on_demand = r->on_demand;
        copy->interval = r->interval;
        copy->running = 1;
        r->source = NULL;
        DEBUG2 ("copy relay %s at %p", copy->localmount, copy);
    }
    return copy;
}


/* force a recheck of the relays. This will recheck the master server if
 * this is a slave and rebuild all mountpoints in the stats tree
 */
void slave_update_all_mounts (void)
{
    update_all_mounts = 1;
    update_settings = 1;
}


/* called on reload, so drop all redirection and trigger a relay checkup and
 * rebuild all stat mountpoints
 */
void slave_restart (void)
{
    restart_connection_thread = 1;
    slave_update_all_mounts ();
    streamlist_check = 0;
}

/* Request slave thread to check the relay list for changes and to
 * update the stats for the current streams.
 */
void slave_rebuild_mounts (void)
{
    update_settings = 1;
}


void slave_initialize(void)
{
    if (slave_running)
        return;

    thread_rwlock_create (&slaves_lock);
    slave_running = 1;
    streamlister = 0;
    streamlist_check = 0;
    update_settings = 0;
    update_all_mounts = 0;
    restart_connection_thread = 0;
    redirectors = NULL;
    workers = NULL;
    worker_count = 0;
    relays_connecting = 0;
    thread_spin_create (&relay_start_lock);
    thread_rwlock_create (&workers_lock);
#ifndef HAVE_CURL
    ERROR0 ("streamlist request disabled, rebuild with libcurl if required");
#endif
    _slave_thread ();
    slave_running = 0;
    yp_stop ();
    workers_adjust(0);
}


void slave_shutdown(void)
{
    thread_rwlock_destroy (&slaves_lock);
    thread_rwlock_destroy (&workers_lock);
    thread_spin_destroy (&relay_start_lock);
    yp_shutdown();
}


int redirect_client (const char *mountpoint, client_t *client)
{
    int ret = 0, which;
    redirect_host *checking, **trail;

    thread_rwlock_rlock (&slaves_lock);
    /* select slave entry */
    if (global.redirect_count == 0)
    {
        thread_rwlock_unlock (&slaves_lock);
        return 0;
    }
    which=(int) (((float)global.redirect_count)*rand()/(RAND_MAX+1.0)) + 1;
    checking = redirectors;
    trail = &redirectors;

    DEBUG2 ("random selection %d (out of %d)", which, global.redirect_count);
    while (checking)
    {
        DEBUG2 ("...%s:%d", checking->server, checking->port);
        if (checking->next_update && checking->next_update+10 < time(NULL))
        {
            /* no streamist request, expire slave for now */
            *trail = checking->next;
            global.redirect_count--;
            /* free slave details */
            INFO2 ("dropping redirector for %s:%d", checking->server, checking->port);
            free (checking->server);
            free (checking);
            checking = *trail;
            if (which > 0)
                which--; /* we are 1 less now */
            continue;
        }
        if (--which == 0)
        {
            char *location;
            /* add enough for "http://" the port ':' and nul */
            int len = strlen (mountpoint) + strlen (checking->server) + 15;
            const char *user = client->username;
            const char *pass = client->password;
            const char *args = httpp_getvar (client->parser, HTTPP_VAR_QUERYARGS);
            const char *colon = ":", *at_sign = "@";

            if (args)
                len += strlen (args);
            else
                args = "";
            if (user && pass)
                len += strlen (user) + strlen (pass);
            else
                colon = at_sign = user = pass = "";
            INFO2 ("redirecting listener to slave server at %s:%d", checking->server, checking->port);
            location = alloca (len);
            snprintf (location, len, "http://%s%s%s%s%s:%d%s%s",
                    user, colon, pass, at_sign,
                    checking->server, checking->port, mountpoint, args);
            client_send_302 (client, location);
            ret = 1;
        }
        trail = &checking->next;
        checking = checking->next;
    }
    thread_rwlock_unlock (&slaves_lock);
    return ret;
}


static http_parser_t *get_relay_response (connection_t *con, const char *mount,
        const char *server, int ask_for_metadata, const char *auth_header)
{
    ice_config_t *config = config_get_config ();
    char *server_id = strdup (config->server_id);
    http_parser_t *parser = NULL;
    char response [4096];

    config_release_config ();

    /* At this point we may not know if we are relaying an mp3 or vorbis
     * stream, but only send the icy-metadata header if the relay details
     * state so (the typical case).  It's harmless in the vorbis case. If
     * we don't send in this header then relay will not have mp3 metadata.
     */
    sock_write (con->sock, "GET %s HTTP/1.0\r\n"
            "User-Agent: %s\r\n"
            "Host: %s\r\n"
            "%s"
            "%s"
            "\r\n",
            mount,
            server_id,
            server,
            ask_for_metadata ? "Icy-MetaData: 1\r\n" : "",
            auth_header ? auth_header : "");

    free (server_id);
    memset (response, 0, sizeof(response));
    if (util_read_header (con->sock, response, 4096, READ_ENTIRE_HEADER) == 0)
    {
        INFO0 ("Header read failure");
        return NULL;
    }
    parser = httpp_create_parser();
    httpp_initialize (parser, NULL);
    if (! httpp_parse_response (parser, response, strlen(response), mount))
    {
        INFO0 ("problem parsing response from relay");
        httpp_destroy (parser);
        return NULL;
    }
    return parser;
}


/* Actually open the connection and do some http parsing, handle any 302
 * responses within here.
 */
static int open_relay_connection (client_t *client, relay_server *relay, relay_server_master *master)
{
    int redirects = 0;
    http_parser_t *parser = NULL;
    connection_t *con = &client->connection;
    char *server = strdup (master->ip);
    char *mount = strdup (master->mount);
    int port = master->port, timeout = master->timeout, ask_for_metadata = relay->mp3metadata;
    char *auth_header = NULL;

    if (relay->username && relay->password)
    {
        char *esc_authorisation;
        unsigned len = strlen(relay->username) + strlen(relay->password) + 2;

        DEBUG2 ("using username %s for %s", relay->username, relay->localmount);
        auth_header = malloc (len);
        snprintf (auth_header, len, "%s:%s", relay->username, relay->password);
        esc_authorisation = util_base64_encode(auth_header);
        free(auth_header);
        len = strlen (esc_authorisation) + 24;
        auth_header = malloc (len);
        snprintf (auth_header, len,
                "Authorization: Basic %s\r\n", esc_authorisation);
        free(esc_authorisation);
    }

    while (redirects < 10)
    {
        sock_t streamsock;
        char *bind = NULL;

        /* policy decision, we assume a source bind even after redirect, possible option */
        if (master->bind)
            bind = strdup (master->bind);

        if (bind)
            INFO4 ("connecting to %s:%d for %s, bound to %s", server, port, relay->localmount, bind);
        else
            INFO3 ("connecting to %s:%d for %s", server, port, relay->localmount);

        con->con_time = time (NULL);
        relay->in_use = master;
        streamsock = sock_connect_wto_bind (server, port, bind, timeout);
        free (bind);
        if (connection_init (con, streamsock, server) < 0)
        {
            WARN2 ("Failed to connect to %s:%d", server, port);
            break;
        }

        parser = get_relay_response (con, mount, server, ask_for_metadata, auth_header);

        if (parser == NULL)
        {
            ERROR4 ("Problem trying to start relay on %s (%s:%d%s)", relay->localmount,
                    server, port, mount);
            break;
        }
        if (strcmp (httpp_getvar (parser, HTTPP_VAR_ERROR_CODE), "302") == 0)
        {
            /* better retry the connection again but with different details */
            const char *uri, *mountpoint;
            int len;

            uri = httpp_getvar (parser, "location");
            INFO1 ("redirect received %s", uri);
            if (strncmp (uri, "http://", 7) != 0)
                break;
            uri += 7;
            mountpoint = strchr (uri, '/');
            free (mount);
            if (mountpoint)
                mount = strdup (mountpoint);
            else
                mount = strdup ("/");

            len = strcspn (uri, ":/");
            port = 80;
            if (uri [len] == ':')
                port = atoi (uri+len+1);
            free (server);
            server = calloc (1, len+1);
            strncpy (server, uri, len);
            connection_close (con);
            httpp_destroy (parser);
            parser = NULL;
        }
        else
        {
            if (httpp_getvar (parser, HTTPP_VAR_ERROR_MESSAGE))
            {
                ERROR2("Error from relay request: %s (%s)", relay->localmount,
                        httpp_getvar(parser, HTTPP_VAR_ERROR_MESSAGE));
                client->parser = NULL;
                break;
            }
            sock_set_blocking (streamsock, 0);
            thread_mutex_lock (&relay->source->lock);
            client->parser = parser; // old parser will be free in the format clear
            thread_mutex_unlock (&relay->source->lock);
            client->connection.discon_time = 0;
            client->connection.con_time = time (NULL);
            client_set_queue (client, NULL);
            free (server);
            free (mount);
            free (auth_header);

            return 0;
        }
        redirects++;
    }
    /* failed, better clean up */
    free (server);
    free (mount);
    free (auth_header);
    if (parser)
        httpp_destroy (parser);
    connection_close (con);
    con->con_time = time (NULL);
    if (relay->in_use) relay->in_use->skip = 1;
    return -1;
}


/* This does the actual connection for a relay. A thread is
 * started off if a connection can be acquired
 */
int open_relay (relay_server *relay)
{
    source_t *src = relay->source;
    relay_server_master *master = relay->masters;
    client_t *client = src->client;
    do
    {
        int ret;

        if (master->skip)
        {
            INFO3 ("skipping %s:%d for %s", master->ip, master->port, relay->localmount);
            continue;
        }
        thread_mutex_unlock (&src->lock);
        ret = open_relay_connection (client, relay, master);
        thread_mutex_lock (&src->lock);

        if (ret < 0)
            continue;

        if (connection_complete_source (src) < 0)
        {
            WARN1 ("Failed to complete initialisation on %s", relay->localmount);
            continue;
        }
        return 1;
    } while ((master = master->next) && global.running == ICE_RUNNING);
    return -1;
}

static void *start_relay_stream (void *arg)
{
    client_t *client = arg;
    relay_server *relay;
    source_t *src;
    int failed = 1, sources;

    global_lock();
    sources = ++global.sources;
    stats_event_args (NULL, "sources", "%d", global.sources);
    global_unlock();
    /* set the start time, because we want to decrease the sources on all failures */
    client->connection.con_time = time (NULL);
    do
    {
        ice_config_t *config = config_get_config();

        relay = client->shared_data;
        src = relay->source;

        thread_mutex_lock (&src->lock);
        src->flags |= SOURCE_PAUSE_LISTENERS;
        if (sources > config->source_limit)
        {
            config_release_config();
            WARN1 ("starting relayed mountpoint \"%s\" requires a higher sources limit", relay->localmount);
            break;
        }
        config_release_config();
        INFO1("Starting relayed source at mountpoint \"%s\"", relay->localmount);

        if (open_relay (relay) < 0)
            break;
        stats_event_inc (NULL, "source_relay_connections");
        source_init (src);
        failed = 0;
    } while (0);

    client->ops = &relay_client_ops;
    client->schedule_ms = timing_get_time();

    if (failed)
    {
        /* failed to start any connection, better clean up and reset */
        if (relay->on_demand)
            src->flags &= ~SOURCE_ON_DEMAND;
        else
        {
            yp_remove (relay->localmount);
            src->yp_public = -1;
        }

        relay->in_use = NULL;
        INFO2 ("listener count remaining on %s is %d", src->mount, src->listeners);
        src->flags &= ~SOURCE_PAUSE_LISTENERS;
        thread_mutex_unlock (&src->lock);
    }

    thread_spin_lock (&relay_start_lock);
    relays_connecting--;
    thread_spin_unlock (&relay_start_lock);

    client->flags |= CLIENT_ACTIVE;
    worker_wakeup (client->worker);
    return NULL;
}


static int relay_install (relay_server *relay)
{
    client_t *client = calloc (1, sizeof (client_t));

    connection_init (&client->connection, SOCK_ERROR, NULL);
    global_lock();
    client_register (client);
    global_unlock();
    client->shared_data = relay;
    client->ops = &relay_init_ops;

    client->flags |= CLIENT_ACTIVE;
    DEBUG1 ("adding relay client for %s", relay->localmount);
    client_add_worker (client);

    return 0;
}


int relay_toggle (relay_server *relay)
{
    source_t *source = relay->source;
    client_t *client;
    int ret = 0;

    thread_mutex_lock (&source->lock);
    client = source->client;
    thread_mutex_unlock (&source->lock);
    if (relay->running == 0)
    {
        client->ops = &relay_init_ops;
        ret = 1;
    }
    relay->running = relay->running ? 0 : 1;
    client->schedule_ms = 0;
    worker_wakeup (client->worker);
    slave_update_all_mounts();
    return ret;
}


/* compare the 2 relays to see if there are any changes, return 1 if
 * the relay needs to be restarted, 0 otherwise
 */
static int relay_has_changed (relay_server *new, relay_server *old)
{
    do
    {
        relay_server_master *oldmaster = old->masters, *newmaster = new->masters;

        while (oldmaster && newmaster)
        {
            if (strcmp (newmaster->mount, oldmaster->mount) != 0)
                break;
            if (strcmp (newmaster->ip, oldmaster->ip) != 0)
                break;
            if (newmaster->port != oldmaster->port)
                break;
            oldmaster = oldmaster->next;
            newmaster = newmaster->next;
        }
        if (oldmaster || newmaster)
            break;
        if (new->mp3metadata != old->mp3metadata)
            break;
        if (new->on_demand != old->on_demand)
            old->on_demand = new->on_demand;
        return 0;
    } while (0);
    new->source = old->source;
    return 1;
}


/* go through updated looking for relays that are different configured. The
 * returned list contains relays that should be kept running, current contains
 * the list of relays to shutdown
 */
static relay_server *
update_relay_set (relay_server **current, relay_server *updated)
{
    relay_server *relay = updated;
    relay_server *existing_relay, **existing_p;
    relay_server *new_list = NULL;

    while (relay)
    {
        existing_relay = *current;
        existing_p = current;

        while (existing_relay)
        {
            /* break out if keeping relay */
            if (strcmp (relay->localmount, existing_relay->localmount) == 0)
            {
                relay_server *new = existing_relay;

                if (global.running == ICE_RUNNING && relay_has_changed (relay, existing_relay))
                {
                    source_t *source = existing_relay->source;
                    new = relay_copy (relay);
                    INFO1 ("relay details changed on \"%s\", restarting", new->localmount);
                    existing_relay->new_details = new;
                    if (source && source->client)
                        source->client->schedule_ms = 0;
                }
                *existing_p = existing_relay->next; /* leave client to free structure */
                new->next = new_list;
                new_list = new;
                break;
            }
            else
                existing_p = &existing_relay->next;
            existing_relay = *existing_p;
        }
        if (existing_relay == NULL)
        {
            /* new one, copy and insert */
            existing_relay = relay_copy (relay);
            existing_relay->next = new_list;
            new_list = existing_relay;
            relay_install (existing_relay);
        }
        relay = relay->next;
    }
    return new_list;
}


/* update the relay_list with entries from new_relay_list. Any new relays
 * are added to the list, and any not listed in the provided new_relay_list
 * are shutdown
 */
static void update_relays (relay_server **relay_list, relay_server *new_relay_list)
{
    relay_server *active_relays, *cleanup_relays = new_relay_list;
    worker_t *worker = NULL;

    if (relay_list)
    {
        thread_mutex_lock (&(config_locks()->relay_lock));
        active_relays = update_relay_set (relay_list, new_relay_list);
        cleanup_relays = *relay_list;
        *relay_list = active_relays;
        thread_mutex_unlock (&(config_locks()->relay_lock));
    }
    while (cleanup_relays)
    {
        relay_server *to_release = cleanup_relays;
        source_t *source = to_release->source;

        cleanup_relays = to_release->next;
        if (source && source->client)
        {
            INFO1 ("relay shutdown request on \"%s\"", to_release->localmount);
            source->client->schedule_ms = 0;
        }
        to_release->cleanup = 1;
    }
    worker = workers;
    while (worker)
    {
        worker_wakeup (worker);
        worker = worker->next;
    }
}


#ifdef HAVE_CURL
struct master_conn_details
{
    char *server;
    int port;
    int ssl_port;
    int send_auth;
    int on_demand;
    int previous;
    int ok;
    int max_interval;
    char *buffer;
    char *username;
    char *password;
    char *bind;
    char *server_id;
    char *args;
    relay_server *new_relays;
};


/* process a single HTTP header from streamlist response */
static size_t streamlist_header (void *ptr, size_t size, size_t nmemb, void *stream)
{
    size_t passed_len = size*nmemb;
    char *eol = memchr (ptr, '\r', passed_len);
    struct master_conn_details *master = stream;

    /* drop EOL chars if any */
    if (eol)
        *eol = '\0';
    else
    {
        eol = memchr (ptr, '\n', passed_len);
        if (eol)
            *eol = '\0';
        else
            return -1;
    }
    if (strncmp (ptr, "HTTP", 4) == 0)
    {
        int respcode = 0;
        if (sscanf (ptr, "HTTP%*s %d OK", &respcode) == 1 && respcode == 200)
            master->ok = 1;  // needed if resetting master relays ???
        else
            WARN1 ("Failed response from master \"%s\"", (char*)ptr);
    }
    DEBUG1 ("header is %s", ptr);
    return passed_len;
}


/* process mountpoint list from master server. This may be called multiple
 * times so watch for the last line in this block as it may be incomplete
 */
static size_t streamlist_data (void *ptr, size_t size, size_t nmemb, void *stream)
{
    struct master_conn_details *master = stream;
    size_t passed_len = size*nmemb;
    size_t len = passed_len;
    char *buffer = ptr, *buf = ptr;
    int prev = 0;

    if (master->ok == 0)
        return passed_len;
    if (master->previous)
    {
        char *eol = memchr (ptr, '\n', passed_len < 150 ? passed_len : 150);
        if (eol == NULL)
        {
            if (passed_len > 150 || master->previous > 200)
            {
                WARN1 ("long line received for append, ignoring %d", passed_len);
                return (master->ok = 0);
            }
            buffer = realloc (master->buffer, len + 1);
            if (buffer == NULL) return 0;
            master->buffer = buffer;
            memcpy (master->buffer + master->previous, ptr, passed_len);
            master->buffer [len] = '\0';
            master->previous = len;
            return passed_len;
        }
        // just fill out enough for 1 entry
        len = (eol - buffer) + 1 + master->previous;
        buffer = realloc (master->buffer, len + 1);
        if (buffer == NULL) return 0;
        master->buffer = buffer;
        prev = len - master->previous;
        memcpy (buffer+master->previous, ptr, prev);
        buffer [len] = '\0';
        buf = buffer;
    }

    while (len)
    {
        int offset;
        char *eol = strchr (buf, '\n');
        if (eol)
        {
            offset = (eol - buf) + 1;
            *eol = '\0';
            eol = strchr (buf, '\r');
            if (eol) *eol = '\0';
        }
        else
        {
            /* incomplete line, the rest may be in the next read */
            master->buffer = calloc (1, len + 1);
            memcpy (master->buffer, buf, len);
            master->previous = len;
            break;
        }

        if (*buf == '/')
        {
            relay_server *r = calloc (1, sizeof (relay_server));
            relay_server_master *m = calloc (1, sizeof (relay_server_master));

            DEBUG1 ("read from master \"%s\"", buf);
            m->ip = (char *)xmlStrdup (XMLSTR(master->server));
            m->port = master->port;
            if (master->bind)
                m->bind = (char *)xmlStrdup (XMLSTR(master->bind));
            m->mount = (char *)xmlStrdup (XMLSTR(buf));
            m->timeout = 4;
            r->masters = m;
            if (strncmp (buf, "/admin/streams?mount=/", 22) == 0)
                r->localmount = (char *)xmlStrdup (XMLSTR(buf+21));
            else
                r->localmount = (char *)xmlStrdup (XMLSTR(buf));
            r->mp3metadata = 1;
            r->on_demand = master->on_demand;
            r->interval = master->max_interval;
            r->running = 1;
            if (master->send_auth)
            {
                r->username = (char *)xmlStrdup (XMLSTR(master->username));
                r->password = (char *)xmlStrdup (XMLSTR(master->password));
            }
            r->next = master->new_relays;
            master->new_relays = r;
        }
        else
            DEBUG1 ("skipping \"%s\"", buf);
        buf += offset;
        len -= offset;
        if (len == 0 && prev)
        {
            buf = ptr + prev;
            len =  passed_len - prev;
            free (master->buffer);
            master->buffer = NULL;
            master->previous = 0;
            prev = 0;
        }
    }
    return passed_len;
}


/* retrieve streamlist from master server. The streamlist can be retrieved
 * from an SSL port if curl is capable and the config is aware of the port
 * to use
 */
static void *streamlist_thread (void *arg)
{
    struct master_conn_details *master = arg;
    CURL *handle;
    const char *protocol = "http";
    int port = master->port;
    char error [CURL_ERROR_SIZE];
    char url [1024], auth [100];

    DEBUG0 ("checking master stream list");
    if (master->ssl_port)
    {
        protocol = "https";
        port = master->ssl_port;
    }
    snprintf (auth, sizeof (auth), "%s:%s", master->username, master->password);
    snprintf (url, sizeof (url), "%s://%s:%d/admin/streams%s",
            protocol, master->server, port, master->args);
    handle = curl_easy_init ();
    curl_easy_setopt (handle, CURLOPT_USERAGENT, master->server_id);
    curl_easy_setopt (handle, CURLOPT_URL, url);
    curl_easy_setopt (handle, CURLOPT_HEADERFUNCTION, streamlist_header);
    curl_easy_setopt (handle, CURLOPT_HEADERDATA, master);
    curl_easy_setopt (handle, CURLOPT_WRITEFUNCTION, streamlist_data);
    curl_easy_setopt (handle, CURLOPT_WRITEDATA, master);
    curl_easy_setopt (handle, CURLOPT_USERPWD, auth);
    curl_easy_setopt (handle, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt (handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt (handle, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt (handle, CURLOPT_TIMEOUT, 120L);
    if (master->bind)
        curl_easy_setopt (handle, CURLOPT_INTERFACE, master->bind);

    master->ok = 0;
    if (curl_easy_perform (handle) != 0 || master->ok == 0)
    {
        /* fall back to traditional request */
        INFO0 ("/admin/streams failed trying streamlist");
        snprintf (url, sizeof (url), "%s://%s:%d/admin/streamlist.txt%s",
                protocol, master->server, port, master->args);
        curl_easy_setopt (handle, CURLOPT_URL, url);
        if (curl_easy_perform (handle) != 0)
            WARN2 ("Failed URL access \"%s\" (%s)", url, error);
    }
    if (master->ok)     /* merge retrieved relays */
        update_relays (&global.master_relays, master->new_relays);
    while (master->new_relays)
        master->new_relays = config_clear_relay (master->new_relays);

    curl_easy_cleanup (handle);
    free (master->server);
    free (master->username);
    free (master->password);
    free (master->buffer);
    free (master->server_id);
    free (master->args);
    free (master);
    streamlister = 0;
    return NULL;
}
#endif


static void update_from_master (ice_config_t *config)
{
#ifdef HAVE_CURL
    struct master_conn_details *details;

    if (config->master_password == NULL || config->master_server == NULL ||
            config->master_server_port == 0)
        return;
    if (streamlister) return;
    streamlister = 1;
    details = calloc (1, sizeof (*details));
    details->server = strdup (config->master_server);
    details->port = config->master_server_port; 
    details->ssl_port = config->master_ssl_port; 
    details->username = strdup (config->master_username);
    details->password = strdup (config->master_password);
    details->send_auth = config->master_relay_auth;
    details->bind = (config->master_bind) ? strdup (config->master_bind) : NULL;
    details->on_demand = config->on_demand;
    details->server_id = strdup (config->server_id);
    details->max_interval = config->master_update_interval;
    if (config->master_redirect)
    {
        details->args = malloc (4096);
        snprintf (details->args, 4096, "?rserver=%s&rport=%d&interval=%d",
                config->hostname, config->port, config->master_update_interval);
    }
    else
        details->args = strdup ("");

    thread_create ("streamlist", streamlist_thread, details, THREAD_DETACHED);
#endif
}


static void update_master_as_slave (ice_config_t *config)
{
    redirect_host *redirect;

    if (config->master_server == NULL || config->master_redirect == 0 || config->max_redirects == 0)
         return;

    thread_rwlock_wlock (&slaves_lock);
    redirect = find_slave_host (config->master_server, config->master_server_port);
    if (redirect == NULL)
    {
        INFO2 ("adding master %s:%d", config->master_server, config->master_server_port);
        redirector_add (config->master_server, config->master_server_port, 0);
    }
    else
        redirect->next_update += config->master_update_interval;
    thread_rwlock_unlock (&slaves_lock);
}


static void slave_startup (void)
{
    ice_config_t *config = config_get_config();

#ifdef HAVE_GETRLIMIT
    struct rlimit rlimit;
    if (getrlimit (RLIMIT_NOFILE, &rlimit) == 0)
    {
        if (rlimit.rlim_cur < rlimit.rlim_max)
        {
            long old = rlimit.rlim_cur;
            rlimit.rlim_cur = rlimit.rlim_max;
            if (setrlimit (RLIMIT_NOFILE, &rlimit) < 0)
                rlimit.rlim_cur = old;
        }
        WARN1 ("process has %ld max file descriptor limit", (long)rlimit.rlim_cur);
    }
    if (getrlimit (RLIMIT_CORE, &rlimit) == 0)
    {
        if (rlimit.rlim_cur < rlimit.rlim_max)
        {
            rlimit.rlim_cur = rlimit.rlim_max;
            setrlimit (RLIMIT_CORE, &rlimit);
        }
    }
#endif

    update_settings = 0;
    update_all_mounts = 0;

    redirector_setup (config);
    update_master_as_slave (config);
    stats_global (config);
    workers_adjust (config->workers_count);
    yp_initialize (config);
    config_release_config();

    source_recheck_mounts (1);
    connection_thread_startup();
}

static void _slave_thread(void)
{
    slave_startup();

    while (1)
    {
        struct timespec current;

        thread_get_timespec (&current);
        /* re-read xml file if requested */
        if (global . schedule_config_reread)
        {
            event_config_read ();
            global . schedule_config_reread = 0;
        }

        global_add_bitrates (global.out_bitrate, 0L, THREAD_TIME_MS(&current));
        if (global.new_connections_slowdown)
            global.new_connections_slowdown--;
        if (global.new_connections_slowdown > 30)
            global.new_connections_slowdown = 30;

        if (global.running != ICE_RUNNING)
            break;

        if (streamlist_check <= current.tv_sec)
        {
            ice_config_t *config = config_get_config();

            streamlist_check = current.tv_sec + config->master_update_interval;
            update_master_as_slave (config);

            update_from_master (config);

            update_relays (&global.relays, config->relay);

            config_release_config();
        }

        if (update_settings)
        {
            source_recheck_mounts (update_all_mounts);
            update_settings = 0;
            update_all_mounts = 0;
            if (restart_connection_thread)
            {
                connection_thread_startup();
                restart_connection_thread = 0;
            }
        }
        stats_global_calc();
        thread_sleep (1000000);
    }
    connection_thread_shutdown();
    fserve_running = 0;
    INFO0 ("shutting down current relays");
    update_relays (&global.relays, NULL);
    update_relays (&global.master_relays, NULL);
    global.relays = NULL;
    global.master_relays = NULL;
    redirector_clearall();

    INFO0 ("Slave thread shutdown complete");
}


relay_server *slave_find_relay (relay_server *relays, const char *mount)
{
    while (relays)
    {
        if (strcmp (relays->localmount, mount) == 0)
            break;
        relays = relays->next;
    }
    return relays;
}


/* drop all redirection details.
 */
void redirector_clearall (void)
{
    thread_rwlock_wlock (&slaves_lock);
    while (redirectors)
    {
        redirect_host *current = redirectors;
        redirectors = current->next;
        INFO2 ("removing %s:%d", current->server, current->port);
        free (current->server);
        free (current);
    }
    global.redirect_count = 0;
    thread_rwlock_unlock (&slaves_lock);
}


void redirector_setup (ice_config_t *config)
{
    redirect_host *redir = config->redirect_hosts;

    thread_rwlock_wlock (&slaves_lock);
    while (redir)
    {
        redirector_add (redir->server, redir->port, 0);
        redir = redir->next;
    }
    thread_rwlock_unlock (&slaves_lock);
}


/* Add new redirectors or update any existing ones
 */
void redirector_update (client_t *client)
{
    redirect_host *redirect;
    const char *rserver = httpp_get_query_param (client->parser, "rserver");
    const char *value;
    int rport = 0, interval = 0;

    if (rserver==NULL) return;
    value = httpp_get_query_param (client->parser, "rport");
    if (value == NULL) return;
    rport = atoi (value);
    if (rport <= 0) return;
    value = httpp_get_query_param (client->parser, "interval");
    if (value == NULL) return;
    interval = atoi (value);
    if (interval < 5) return;

    thread_rwlock_wlock (&slaves_lock);
    redirect = find_slave_host (rserver, rport);
    if (redirect == NULL)
    {
        ice_config_t *config = config_get_config();
        unsigned int allowed = config->max_redirects;

        config_release_config();

        if (global.redirect_count < allowed)
            redirector_add (rserver, rport, interval);
        else
            INFO2 ("redirect to slave limit reached (%d, %d)", global.redirect_count, allowed);
    }
    else
    {
        DEBUG2 ("touch update on %s:%d", redirect->server, redirect->port);
        redirect->next_update = time(NULL) + interval;
    }
    thread_rwlock_unlock (&slaves_lock);
}



/* search list of redirectors for a matching entry, lock must be held before
 * invoking this function
 */
static redirect_host *find_slave_host (const char *server, int port)
{
    redirect_host *redirect = redirectors;
    while (redirect)
    {
        if (strcmp (redirect->server, server) == 0 && redirect->port == port)
            break;
        redirect = redirect->next;
    }
    return redirect;
}


static void redirector_add (const char *server, int port, int interval)
{
    redirect_host *redirect = calloc (1, sizeof (redirect_host));
    if (redirect == NULL)
        abort();
    redirect->server = strdup (server);
    redirect->port = port;
    if (interval == 0)
        redirect->next_update = (time_t)0;
    else
        redirect->next_update = time(NULL) + interval;
    redirect->next = redirectors;
    redirectors = redirect;
    global.redirect_count++;
    INFO3 ("slave (%d) at %s:%d added", global.redirect_count,
            redirect->server, redirect->port);
}

static relay_server *get_relay_details (client_t *client)
{
    relay_server *relay = client->shared_data;
    if (relay && relay->new_details)
    {
        relay_server *old_details = relay;

        thread_mutex_lock (&(config_locks()->relay_lock));
        INFO1 ("Detected change in relay details for %s", relay->localmount);
        client->shared_data = relay->new_details;
        relay = client->shared_data;
        relay->source = old_details->source;
        old_details->source = NULL;
        config_clear_relay (old_details);

        thread_mutex_unlock (&(config_locks()->relay_lock));
    }
    return relay;
}


static void relay_reset (relay_server *relay)
{
    relay_server_master *server = relay->masters;

    source_clear_source (relay->source);
    for (; server; server = server->next)
       server->skip = 0;
    INFO1 ("servers to be retried on %s", relay->localmount);
}


static int relay_read (client_t *client)
{
    relay_server *relay = get_relay_details (client);
    source_t *source = relay->source;

    thread_mutex_lock (&source->lock);
    if (source_running (source))
    {
        if (relay->cleanup) relay->running = 0;
        if (relay->running == 0)
            source->flags &= ~SOURCE_RUNNING;
        if (relay->on_demand && source->listeners == 0 && source->format->read_bytes > 1000000)
            source->flags &= ~SOURCE_RUNNING;
        return source_read (source);
    }
    if ((source->flags & SOURCE_TERMINATING) == 0)
    {
        /* this section is for once through code */
        int fallback = 1;
        if (client->connection.con_time)
        {
            if (relay->running && relay->in_use)
                fallback = 0;
            if (client->worker->current_time.tv_sec - client->connection.con_time < 60)
            {
                /* force a server skip if a stream cannot be maintained for 1 min */
                WARN1 ("stream for %s died too quickly, skipping server for now", relay->localmount);
                if (relay->in_use) relay->in_use->skip = 1;
            }
            else
                relay_reset (relay); // spent some time on this so give other servers a chance
            if (source->flags & SOURCE_TIMEOUT)
            {
                WARN1 ("stream for %s timed out, skipping server for now", relay->localmount);
                if (relay->in_use) relay->in_use->skip = 1;
            }
        }
        /* don't pause listeners if relay shutting down */
        if (relay->running == 0)
            source->flags &= ~SOURCE_PAUSE_LISTENERS;
        // fallback listeners unless relay is to be retried
        source_shutdown (source, fallback);
    }
    if (source->termination_count && source->termination_count <= source->listeners)
    {
        client->schedule_ms = client->worker->time_ms + 150;
        if (client->worker->current_time.tv_sec - client->timer_start > 2)
        {
            client->schedule_ms += 400;
            WARN3 ("counts are %lu and %lu (%s)", source->termination_count, source->listeners, source->mount);
        }
        else
            DEBUG3 ("counts are %lu and %lu (%s)", source->termination_count, source->listeners, source->mount);
        thread_mutex_unlock (&source->lock);
        return 0;
    }
    DEBUG1 ("all listeners have now been checked on %s", relay->localmount);
    if (client->connection.con_time)
    {
        global_lock();
        global.sources--;
        stats_event_args (NULL, "sources", "%d", global.sources);
        global_unlock();
        global_reduce_bitrate_sampling (global.out_bitrate);
    }
    client->timer_start = 0;
    client->parser = NULL;
    free (source->fallback.mount);
    source->fallback.mount = NULL;
    source->flags &= ~(SOURCE_TERMINATING|SOURCE_LISTENERS_SYNC);
    if (relay->cleanup)
    {
        connection_close (&client->connection);
        if (source->listeners)
        {
            INFO1 ("listeners on terminating relay %s, rechecking", relay->localmount);
            source->termination_count = source->listeners;
            source->flags &= ~SOURCE_PAUSE_LISTENERS;
            source->flags |= SOURCE_LISTENERS_SYNC;
            thread_mutex_unlock (&source->lock);
            return 0; /* listeners may be paused, recheck and let them leave this stream */
        }
        INFO1 ("shutting down relay %s", relay->localmount);
        stats_event_args (source->mount, "listeners", "%lu", source->listeners);
        thread_mutex_unlock (&source->lock);
        stats_event (relay->localmount, NULL, NULL);
        slave_update_all_mounts();
        return -1;
    }
    client->ops = &relay_startup_ops;
    do {
        if (relay->running)
        {
            if (client->connection.con_time && relay->in_use)
            {
                INFO1 ("standing by to restart relay on %s", relay->localmount);
                if (relay->on_demand && source->listeners == 0)
                    relay_reset (relay);
                stats_event (relay->localmount, NULL, NULL);
                client->ops = &relay_init_ops;
                break;
            }
            client->schedule_ms = client->worker->time_ms + (relay->interval * 1000);
            INFO2 ("standing by to restart relay on %s in %d seconds", relay->localmount, relay->interval);
        }
        else
        {
            INFO1 ("Relay %s is disabled", relay->localmount);
            client->schedule_ms = client->worker->time_ms + 3600000;
        }
        source->flags &= ~SOURCE_ON_DEMAND;
        stats_set_args (source->stats, "listeners", "%lu", source->listeners);
        relay_reset (relay);
        stats_event (relay->localmount, NULL, NULL);
        slave_update_all_mounts();
    } while (0);
    client->connection.con_time = 0;
    source->stats = 0;

    thread_mutex_unlock (&source->lock);
    connection_close (&client->connection);
    return 0;
}


static void relay_release (client_t *client)
{
    relay_server *relay = client->shared_data;
    DEBUG2("freeing relay %s (%p)", relay->localmount, relay);
    if (relay->source)
        source_free_source (relay->source);
    relay->source = NULL;
    config_clear_relay (relay);
    client_destroy (client);
}


static int relay_initialise (client_t *client)
{
    relay_server *relay = get_relay_details (client);

    if (relay->source == NULL)  /* new relay, so set up a source if we can */
    {
        source_t *source = source_reserve (relay->localmount, 0);
        if (source == NULL)
        {
            INFO1 ("new relay but source \"%s\" exists, waiting", relay->localmount);
            client->schedule_ms = client->worker->time_ms + 2000;
            return 0;
        }
        relay->source = source;
        source->client = client;
    }
    do
    {
        if (global.running != ICE_RUNNING)  break;
        if (relay->running)
        {
            if (relay->on_demand)
            {
                ice_config_t *config;
                mount_proxy *mountinfo;
                source_t *source = relay->source;

                thread_mutex_lock (&source->lock);
                config = config_get_config();
                mountinfo = config_find_mount (config, source->mount);
                source->flags |= SOURCE_ON_DEMAND;
                source_update_settings (config, source, mountinfo);
                thread_mutex_unlock (&source->lock);
                config_release_config();
                slave_update_all_mounts();
                stats_set_flags (source->stats, "listener_connections", "0", STATS_COUNTERS);
            }
            break;
        }
        client->schedule_ms = client->worker->time_ms + 10000000;
        return 0;
    } while(0);
    client->ops = &relay_startup_ops;
    return client->ops->process (client);
}


static int relay_startup (client_t *client)
{
    relay_server *relay = get_relay_details (client);
    worker_t *worker = client->worker;

    if (relay->cleanup)
    {
        /* listeners may be still on, do a recheck */
        relay->running = 0;
        DEBUG1 ("cleanup detected on %s", relay->localmount);
    }
    if (relay->running == 0)
    {
        if (relay->source == NULL)
            return -1;
        client->ops = &relay_client_ops;
        client->schedule_ms = worker->time_ms + 20;
        return 0;
    }
    if (global.running != ICE_RUNNING)  /* wait for cleanup */
    {
        client->schedule_ms = client->worker->time_ms + 50;
        return 0;
    }
    if (relay->on_demand)
    {
        source_t *source = relay->source;
        int fallback_def = 0, start_relay = source->listeners; // 0 or non-zero
        mount_proxy *mountinfo = config_find_mount (config_get_config(), source->mount);

        source->flags |= SOURCE_ON_DEMAND;
        if (mountinfo && mountinfo->fallback_mount)
        {
            source_t *fallback;

            avl_tree_rlock (global.source_tree);
            fallback = source_find_mount (mountinfo->fallback_mount);
            fallback_def = 1;
            if (fallback)
            {
                if (strcmp (fallback->mount, source->mount) != 0 && fallback->listeners)
                    start_relay = 1;
                avl_tree_unlock (global.source_tree);
            }
            else
            {
                fbinfo finfo;

                avl_tree_unlock (global.source_tree);
                finfo.flags = FS_FALLBACK;
                finfo.mount = (char *)mountinfo->fallback_mount;
                finfo.fallback = NULL;

                // need to check for listeners on the fallback
                if (fserve_query_count (&finfo) > 0)
                    start_relay = 1; // force a start if there is fallback defined
            }
        }
        config_release_config();
        if (start_relay == 0)
        {
            client->schedule_ms = worker->time_ms + (fallback_def ? (relay->interval*1000) : 60000);
            return 0;
        }
        INFO1 ("starting on-demand relay %s", relay->localmount);
    }

    /* limit the number of relays starting up at the same time */
    thread_spin_lock (&relay_start_lock);
    if (relays_connecting > 3)
    {
        thread_spin_unlock (&relay_start_lock);
        client->schedule_ms = worker->time_ms + 200;
        if (global.new_connections_slowdown < 5)
            global.new_connections_slowdown++;
        return 0;
    }
    relays_connecting++;
    thread_spin_unlock (&relay_start_lock);

    client->flags &= ~CLIENT_ACTIVE;
    thread_create ("Relay Thread", start_relay_stream, client, THREAD_DETACHED);
    return 0;
}

