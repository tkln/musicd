/*
 * This file is part of musicd.
 * Copyright (C) 2012 Konsta Kokkinen <kray@tsundere.fi>
 * 
 * Musicd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Musicd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Musicd.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "query.h"

#include "db.h"
#include "log.h"
#include "strings.h"

#include <stdbool.h>

static const char *field_names[QUERY_FIELD_ALL] = {
  "",
  "id",
  "trackid",
  "artistid",
  "albumid",
  "title",
  "artist",
  "album",
  "track",
  "duration",
};

/* All id fields. */
static bool id_fields[QUERY_FIELD_ALL + 1] = {
  false,
  true,
  true,
  true,
  true,
  false,
  false,
  false,
  false,
  false,
  false
};

query_field_t query_field_from_string(const char *string)
{
  int i;
  for (i = 1; i < QUERY_FIELD_ALL; ++i) {
    if (!strcmp(string, field_names[i])) {
      return i;
    }
  }
  if (!strcmp(string, "all") || !strcmp(string, "search")) {
    /* Special case */
    return QUERY_FIELD_ALL;
  }
  return QUERY_FIELD_NONE;
}


struct query_format {
  const char *body;
  const char **maps;
};

static const char *track_maps[QUERY_FIELD_ALL + 1] = {
  NULL,
  "tracks.rowid",
  "tracks.rowid",
  "tracks.artist",
  "tracks.album",
  "tracks.title",
  "artists.name",
  "albums.name",
  "tracks.track",
  "tracks.duration",
  /* Special case... */
  "(COALESCE(tracks.title, '') || COALESCE(artists.name, '') || COALESCE(albums.name, ''))",
};
static struct query_format track_query = {
  "SELECT tracks.rowid AS trackid, urls.path AS url, tracks.track AS track, tracks.title AS title, tracks.artist AS artistid, artists.name AS artist, tracks.album AS albumid, albums.name AS album, tracks.start AS start, tracks.duration AS duration FROM tracks JOIN urls ON tracks.url = urls.rowid LEFT OUTER JOIN artists ON tracks.artist = artists.rowid LEFT OUTER JOIN albums ON tracks.album = albums.rowid",
  track_maps
};

static const char *artist_maps[QUERY_FIELD_ALL + 1] = {
  NULL,
  "artists.rowid",
  NULL,
  "artists.rowid",
  NULL,
  NULL,
  "artists.name",
  NULL,
  NULL,
  NULL,
  /* Special case... */
  "(COALESCE(artists.name, ''))",
};
static struct query_format artist_query = {
  "SELECT artists.rowid AS artistid, artists.name AS artist FROM artists",
  artist_maps
};

static const char *album_maps[QUERY_FIELD_ALL + 1] = {
  NULL,
  "albums.rowid",
  NULL,
  NULL,
  "albums.rowid",
  NULL,
  NULL,
  "albums.name",
  NULL,
  NULL,
  /* Special case... */
  "(COALESCE(albums.name, ''))",
};
static struct query_format album_query = {
  "SELECT albums.rowid AS albumid, albums.name AS album FROM albums",
  album_maps
};

struct query {
  struct query_format *format;

  sqlite3_stmt *stmt;

  char *filters[QUERY_FIELD_ALL + 1];
  int64_t limit;
  int64_t offset;

  string_t *order;
};

static query_t *query_new()
{
  query_t *query = malloc(sizeof(query_t));
  memset(query, 0, sizeof(query_t));
  query->order = string_new();
  query->limit = -1;  
  return query;
}

query_t *query_tracks_new()
{
  query_t *query = query_new();
  query->format = &track_query;
  return query;
}

query_t *query_artists_new()
{
  query_t *query = query_new();
  query->format = &artist_query;
  return query;
}

query_t *query_albums_new()
{
  query_t *query = query_new();
  query->format = &album_query;
  return query;
}

void query_close(query_t *query)
{
  int i;

  sqlite3_finalize(query->stmt);
  for (i = 0; i <= QUERY_FIELD_ALL; ++i) {
    free(query->filters[i]);
  }
  string_free(query->order);
  free(query);
}

void query_filter(query_t *query, query_field_t field,
                      const char *filter)
{
  string_t *string;
  if (!filter) {
    query->filters[field] = NULL;
    return;
  }
  if (!id_fields[field]) {
    query->filters[field] = stringf("%%%s%%", filter);
    return;
  }

  /* The field is an id field. Ensure the filter is a comma-separated list of
   * decimal numbers. */
  string = string_new();
  for (; *filter != '\0'; ++filter) {
    if (*filter == ',' || (*filter >= '0' && *filter <= '9')) {
      string_push_back(string, *filter);
    }
  }
  query->filters[field] = string_release(string);
}

void query_limit(query_t *query, int64_t limit)
{
  query->limit = limit;
}

void query_offset(query_t *query, int64_t offset)
{
  query->offset = offset;
}

void query_sort(query_t *query, query_field_t field,
                        bool descending)
{
  if (!query->format->maps[field]) {
    /* Not valid field for this query format, ignore. */
    return;
  }
  if (string_size(query->order) > 0) {
    string_append(query->order, ", ");
  }
  string_appendf(query->order, "%s COLLATE NOCASE %s",
                               query->format->maps[field],
                               descending ? "DESC" : "ASC");
}

int query_sort_from_string(query_t *query, const char *sort)
{
  const char *end;
  char *name;
  bool descending;
  query_field_t field;

  while (*sort != '\0') {
    if (*sort == '-') {
      descending = true;
      ++sort;
    } else {
      descending = false;
    }

    for (end = sort; *end != ',' && *end != '\0'; ++end) { }

    name = strextract(sort, end);
    field = query_field_from_string(name);
    free(name);

    if (field == QUERY_FIELD_NONE) {
      /* Not a valid field name */
      return -1;
    }

    query_sort(query, field, descending);

    sort = end;

    if (*sort == ',') {
      ++sort;
    }
  }
  return 0;
}

/* Generates SQL for the WHERE clause. */
static char *build_filters(query_t *query)
{
  int i;
  string_t *sql = string_new();

  bool join = false;
  for (i = 1; i <= QUERY_FIELD_ALL; ++i) {
    if (!query->filters[i] || !query->format->maps[i]) {
      continue;
    }

    if (!join) {
      string_appendf(sql, "WHERE ");
      join = true;
    } else {
      string_appendf(sql, " AND ");
    }

    if (!id_fields[i]) {
      string_appendf(sql, "%s LIKE ?", query->format->maps[i]);
    } else {
      /* Id-field means it can be a comma-separated list of decimal numbers.
       * The filter is validated when it is set, so we can add it to the query
       * directly. */
      string_appendf(sql, "%s IN (%s)", query->format->maps[i], query->filters[i]);
    }
  }
  return string_release(sql);
}

/* Bind filters from query to stmt */
void bind_filters(query_t *query, sqlite3_stmt *stmt)
{
  int i, n;
  for (i = 1, n = 1; i <= QUERY_FIELD_ALL; ++i) {
    if (query->filters[i] && !id_fields[i]) {
      sqlite3_bind_text(stmt, n, query->filters[i], -1, NULL);
      ++n;
    }
  }
}

static sqlite3_stmt *start(query_t *query, const char *body)
{
  string_t *sql = string_new();
  char *where = build_filters(query);
  sqlite3_stmt *stmt;
  
  string_appendf(sql, "%s %s", body, where);
  free(where);

  if (string_size(query->order) > 0) {
    string_appendf(sql, " ORDER BY %s", string_string(query->order));
  }

  if (query->limit > 0 || query->offset > 0) {
    string_appendf(sql, " LIMIT %" PRId64 " OFFSET %" PRId64 "", query->limit, query->offset);
  }

  musicd_log(LOG_DEBUG, "query", "%s", string_string(sql));

  if (sqlite3_prepare_v2(db_handle(),
                        string_string(sql), -1,
                        &stmt, NULL)!= SQLITE_OK) {
    musicd_log(LOG_ERROR, "query", "can't prepare '%s': %s",
               string_string(sql), db_error());
    string_free(sql);
    return NULL;
  }
  string_free(sql);

  bind_filters(query, stmt);

  return stmt;
}

int query_start(query_t *query)
{
  query->stmt = start(query, query->format->body);
  if (!query->stmt) {
    return -1;
  }

  return 0;
}

int query_tracks_next(query_t *query, track_t *track)
{
  int result;
  sqlite3_stmt *stmt;

  if (!query) {
    return -1;
  }

  stmt = query->stmt;

  result = sqlite3_step(stmt);
  if (result == SQLITE_DONE) {
    return 1;
  } else if (result != SQLITE_ROW) {
    musicd_log(LOG_ERROR, "query",
               "query_tracks_next: sqlite3_step failed");
    return -1;
  }

  track->id = sqlite3_column_int64(stmt, 0);
  track->path = (char *)sqlite3_column_text(stmt, 1);
  track->track = sqlite3_column_int(stmt, 2);
  track->title = (char *)sqlite3_column_text(stmt, 3);
  track->artistid = sqlite3_column_int64(stmt, 4);
  track->artist = (char *)sqlite3_column_text(stmt, 5);
  track->albumid = sqlite3_column_int64(stmt, 6);
  track->album = (char *)sqlite3_column_text(stmt, 7);
  track->start = sqlite3_column_int(stmt, 8);
  track->duration = sqlite3_column_int(stmt, 9);

  return 0;
}

int query_artists_next(query_t *query, query_artist_t *artist)
{
  int result;
  sqlite3_stmt *stmt;

  if (!query) {
    return -1;
  }

  stmt = query->stmt;

  result = sqlite3_step(stmt);
  if (result == SQLITE_DONE) {
    return 1;
  } else if (result != SQLITE_ROW) {
    musicd_log(LOG_ERROR, "query",
               "query_artists_next: sqlite3_step failed");
    return -1;
  }

  artist->artistid = sqlite3_column_int64(stmt, 0);
  artist->artist = (char *)sqlite3_column_text(stmt, 1);

  return 0;
}

int query_albums_next(query_t *query, query_album_t *album)
{
  int result;
  sqlite3_stmt *stmt;

  if (!query) {
    return -1;
  }

  stmt = query->stmt;

  result = sqlite3_step(stmt);
  if (result == SQLITE_DONE) {
    return 1;
  } else if (result != SQLITE_ROW) {
    musicd_log(LOG_ERROR, "query",
               "query_albums_next: sqlite3_step failed");
    return -1;
  }

  album->albumid = sqlite3_column_int64(stmt, 0);
  album->album = (char *)sqlite3_column_text(stmt, 1);

  return 0;
}
