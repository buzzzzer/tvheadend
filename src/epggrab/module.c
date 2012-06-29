/*
 *  EPG Grabber - module functions
 *  Copyright (C) 2012 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/un.h>

#include "tvheadend.h"
#include "settings.h"
#include "htsmsg.h"
#include "htsmsg_xml.h"
#include "service.h"
#include "channels.h"
#include "spawn.h"
#include "file.h"
#include "epg.h"
#include "epggrab.h"
#include "epggrab/private.h"

/* **************************************************************************
 * Generic module routines
 * *************************************************************************/

epggrab_module_t *epggrab_module_create
  ( epggrab_module_t *skel, const char *id, const char *name )
{
  assert(skel);
  
  /* Setup */
  skel->id   = strdup(id);
  skel->name = strdup(name);

  /* Insert */
  assert(!epggrab_module_find_by_id(id));
  LIST_INSERT_HEAD(&epggrab_modules, skel, link);

  return skel;
}

/* **************************************************************************
 * Channel related routines
 * *************************************************************************/

void epggrab_module_ch_save ( void *_m, epggrab_channel_t *ch )
{
  int i;
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_t *a;
  epggrab_module_t *mod = _m;

  if (ch->name)
    htsmsg_add_str(m, "name", ch->name);
  if (ch->icon)
    htsmsg_add_str(m, "icon", ch->icon);
  if (ch->channel)
    htsmsg_add_u32(m, "channel", ch->channel->ch_id);
  if (ch->sid) {
    a = htsmsg_create_list();
    i = 0;
    while (ch->sid[i]) {
      htsmsg_add_u32(a, NULL, ch->sid[i]);
      i++;
    }
    htsmsg_add_msg(m, "sid", a);
  }
  if (ch->sname) {
    a = htsmsg_create_list();
    i = 0;
    while (ch->sname[i]) {
      htsmsg_add_str(a, NULL, ch->sname[i]);
      i++;
    }
    htsmsg_add_msg(m, "sname", a);
  }
  if (ch->number)
    htsmsg_add_u32(m, "number", ch->number);

  hts_settings_save(m, "epggrab/%s/channels/%s", mod->id, ch->id);
}

void epggrab_module_ch_add ( void *m, channel_t *ch )
{
  epggrab_channel_t *egc;
  epggrab_module_int_t *mod = m;
  RB_FOREACH(egc, mod->channels, link) {
    if (epggrab_channel_link(egc, ch)) {
      epggrab_module_ch_save(mod, egc);
      break;
    }
  }
}

void epggrab_module_ch_rem ( void *m, channel_t *ch )
{
  epggrab_channel_t *egc;
  epggrab_module_int_t *mod = m;
  RB_FOREACH(egc, mod->channels, link) {
    if (egc->channel == ch) {
      egc->channel = NULL;
      epggrab_module_ch_save(mod, egc);
      break;
    }
  }
}

void epggrab_module_ch_mod ( void *mod, channel_t *ch )
{
  return epggrab_module_ch_add(mod, ch);
}


#if 0

static void epggrab_module_ch_load 
  ( epggrab_module_t *mod, htsmsg_t *m, const char *id )
{
  int save = 0, i;
  const char *str;
  uint32_t u32;
  htsmsg_t *a;
  htsmsg_field_t *f;

  epggrab_channel_t *ch = epggrab_module_channel_find(mod, id, 1, &save);

  if ((str = htsmsg_get_str(m, "name")))
    ch->name = strdup(str);
  if ((str = htsmsg_get_str(m, "icon")))
    ch->icon = strdup(str);
  if ((a = htsmsg_get_list(m, "sid"))) {
    i = 0;
    HTSMSG_FOREACH(f, a) i++;
    ch->sid_cnt = i;
    ch->sid     = calloc(i, sizeof(uint16_t));
    i = 0;
    HTSMSG_FOREACH(f, a) {
      ch->sid[i] = (uint16_t)f->hmf_s64;
      i++;
    }
  }
  if ((a = htsmsg_get_list(m, "sname"))) {
    i = 0;
    HTSMSG_FOREACH(f, a) i++;
    ch->sname = calloc(i+1, sizeof(char*));
    i = 0;
    HTSMSG_FOREACH(f, a) {
      ch->sname[i] = strdup(f->hmf_str);
      i++;
    }
  }
  if(!htsmsg_get_u32(m, "number", &u32))
    ch->number = u32;

  if (!htsmsg_get_u32(m, "channel", &u32))
    ch->channel = channel_find_by_identifier(u32);
}

void epggrab_module_channels_load ( epggrab_module_t *mod )
{
  htsmsg_t *m, *e;
  htsmsg_field_t *f;

  if ((m = hts_settings_load("epggrab/%s/channels", mod->id))) {
    HTSMSG_FOREACH(f, m) {
      if ((e = htsmsg_get_map_by_field(f)))
        epggrab_module_channel_load(mod, e, f->hmf_name);
    }
    htsmsg_destroy(m);
  }
}

epggrab_channel_t *epggrab_module_ch_find
  ( epggrab_module_t *mod, const char *id, int create, int *save ) 
{
  epggrab_channel_t *ec;
  static epggrab_channel_t *skel = NULL;

  if (!mod || !mod->channels ) return NULL;

  if ( !skel ) skel = calloc(1, sizeof(epggrab_channel_t));
  skel->id  = (char*)id;
  skel->mod = mod;

  /* Find */
  if (!create) {
    ec = RB_FIND(mod->channels, skel, link, _ch_id_cmp);

  /* Create (if required) */
  } else {
    ec = RB_INSERT_SORTED(mod->channels, skel, link, _ch_id_cmp);
    if ( ec == NULL ) {
      skel->id = strdup(skel->id);
      ec       = skel;
      skel     = NULL;
      *save    = 1;
    }
  }

  return ec;
}
#endif

/* **************************************************************************
 * Internal module routines
 * *************************************************************************/

epggrab_module_int_t *epggrab_module_int_create
  ( epggrab_module_int_t *skel,
    const char *id, const char *name, const char *path,
    char* (*grab) (void*m),
    int (*parse) (void *m, htsmsg_t *data, epggrab_stats_t *sta),
    htsmsg_t* (*trans) (void *mod, char *data),
    epggrab_channel_tree_t *channels )
{
  /* Allocate data */
  if (!skel) skel = calloc(1, sizeof(epggrab_module_int_t));
  
  /* Pass through */
  epggrab_module_create((epggrab_module_t*)skel, id, name);

  /* Int data */
  skel->type     = EPGGRAB_INT;
  skel->path     = strdup(path);
  skel->channels = channels;
  skel->grab     = grab  ?: epggrab_module_grab_spawn;
  skel->trans    = trans ?: epggrab_module_trans_xml;
  skel->parse    = parse;
  if (channels) {
    skel->ch_add = epggrab_module_ch_add;
    skel->ch_rem = epggrab_module_ch_rem;
    skel->ch_mod = epggrab_module_ch_mod;
  }

  return skel;
}

char *epggrab_module_grab_spawn ( void *m )
{ 
  int        outlen;
  char       *outbuf;
  epggrab_module_int_t *mod = m;

  /* Debug */
  tvhlog(LOG_INFO, mod->id, "grab %s", mod->path);

  /* Grab */
  outlen = spawn_and_store_stdout(mod->path, NULL, &outbuf);
  if ( outlen < 1 ) {
    tvhlog(LOG_ERR, "pyepg", "no output detected");
    return NULL;
  }

  return outbuf;
}



htsmsg_t *epggrab_module_trans_xml ( void *m,  char *c )
{
  htsmsg_t *ret;
  char     errbuf[100];

  if (!c) return NULL;

  /* Extract */
  ret = htsmsg_xml_deserialize(c, errbuf, sizeof(errbuf));
  if (!ret)
    tvhlog(LOG_ERR, "pyepg", "htsmsg_xml_deserialize error %s", errbuf);
  return ret;
}

/* **************************************************************************
 * External module routines
 * *************************************************************************/

/*
 * Socket handler
 */
static void _epggrab_socket_handler ( epggrab_module_ext_t *mod, int s )
{
  size_t outlen;
  char *outbuf;
  time_t tm1, tm2;
  htsmsg_t *data = NULL;

  /* Grab/Translate */
  time(&tm1);
  outlen = file_readall(s, &outbuf);
  if (outlen) data = mod->trans(mod, outbuf);
  time(&tm2);

  /* Process */
  if ( data ) {
    tvhlog(LOG_INFO, mod->id, "grab took %d seconds", tm2 - tm1);
    epggrab_module_parse(mod, data);

  /* Failed */
  } else {
    tvhlog(LOG_ERR, mod->id, "failed to read data");
  }
}


/*
 * External (socket) grab thread
 */
static void *_epggrab_socket_thread ( void *p )
{
  int s;
  epggrab_module_ext_t *mod = (epggrab_module_ext_t*)p;
  tvhlog(LOG_INFO, mod->id, "external socket enabled");
  
  while ( mod->enabled && mod->sock ) {
    tvhlog(LOG_DEBUG, mod->id, "waiting for connection");
    s = accept(mod->sock, NULL, NULL);
    if (s <= 0) continue;
    tvhlog(LOG_DEBUG, mod->id, "got connection %d", s);
    _epggrab_socket_handler(mod, s);
  }
  tvhlog(LOG_DEBUG, mod->id, "terminated");
  return NULL;
}

/*
 * Enable socket module
 */
int epggrab_module_enable_socket ( void *m, uint8_t e )
{
  pthread_t      tid;
  pthread_attr_t tattr;
  struct sockaddr_un addr;
  epggrab_module_ext_t *mod = (epggrab_module_ext_t*)m;
  assert(mod->type == EPGGRAB_EXT);
  
  /* Ignore */
  if ( mod->enabled == e ) return 0;

  /* Disable */
  if (!e) {
    shutdown(mod->sock, SHUT_RDWR);
    close(mod->sock);
    unlink(mod->path);
    mod->sock = 0;
  
  /* Enable */
  } else {
    unlink(mod->path); // just in case!

    mod->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(mod->sock);

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, mod->path, 100);
    if ( bind(mod->sock, (struct sockaddr*)&addr,
             sizeof(struct sockaddr_un)) != 0 ) {
      tvhlog(LOG_ERR, mod->id, "failed to bind socket");
      close(mod->sock);
      mod->sock = 0;
      return 0;
    }

    if ( listen(mod->sock, 5) != 0 ) {
      tvhlog(LOG_ERR, mod->id, "failed to listen on socket");
      close(mod->sock);
      mod->sock = 0;
      return 0;
    }

    tvhlog(LOG_DEBUG, mod->id, "starting socket thread");
    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &tattr, _epggrab_socket_thread, mod);
  }
  mod->enabled = e;
  return 1;
}

/*
 * Create a module
 */
epggrab_module_ext_t *epggrab_module_ext_create
  ( epggrab_module_ext_t *skel,
    const char *id, const char *name, const char *sockid,
    int (*parse) (void *m, htsmsg_t *data, epggrab_stats_t *sta),
    htsmsg_t* (*trans) (void *mod, char *data),
    epggrab_channel_tree_t *channels )
{
  char path[512];

  /* Allocate data */
  if (!skel) skel = calloc(1, sizeof(epggrab_module_ext_t));
  
  /* Pass through */
  snprintf(path, 512, "%s/epggrab/%s.sock",
           hts_settings_get_root(), sockid);
  epggrab_module_int_create((epggrab_module_int_t*)skel,
                            id, name, path,
                            NULL, parse, trans,
                            channels);

  /* Local */
  skel->type   = EPGGRAB_EXT;
  skel->enable = epggrab_module_enable_socket;

  return skel;
}

/* **************************************************************************
 * OTA module functions
 * *************************************************************************/

epggrab_module_ota_t *epggrab_module_ota_create
  ( epggrab_module_ota_t *skel,
    const char *id, const char *name,
    void (*start) (epggrab_module_ota_t*m,
                   struct th_dvb_mux_instance *tdmi),
    int (*enable) (void *m, uint8_t e ),
    epggrab_channel_tree_t *channels )
{
  if (!skel) skel = calloc(1, sizeof(epggrab_module_ota_t));
  
  /* Pass through */
  epggrab_module_create((epggrab_module_t*)skel, id, name);

  /* Setup */
  skel->type   = EPGGRAB_OTA;
  skel->enable = enable;
  skel->start  = start;

  return skel;
}
