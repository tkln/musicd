/*
 * This file is part of musicd.
 * Copyright (C) 2011 Konsta Kokkinen <kray@tsundere.fi>
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
#include "url.h"

#include "log.h"
#include "strings.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>


static size_t
write_memory_function(void *data, size_t size, size_t nmemb, void *opaque)
{
  size_t realsize = size * nmemb;
  string_t *buf = (string_t *)opaque;
  
  string_nappend(buf, data, realsize);
  
  return realsize;
}

char *url_fetch(const char *url)
{
  string_t *buf = string_new();
  CURL *curl;
  char errorbuf[CURL_ERROR_SIZE];
  CURLcode result;

  curl = curl_easy_init();
  
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorbuf);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_function);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);

  musicd_log(LOG_DEBUG, "url", "fetching '%s'", url);

  result = curl_easy_perform(curl);
  if (result) {
    musicd_log(LOG_ERROR, "url", "fetching '%s' failed: %s", url, errorbuf);
    string_free(buf);
    buf = NULL;
  }
  
  curl_easy_cleanup(curl);
  return buf ? string_release(buf) : NULL;
}

char *url_escape(const char *string)
{
  if (!string) {
    return NULL;
  }
  return curl_escape(string, strlen(string));
}

char *url_escape_location(const char *server, const char *location)
{
  if (!server || !location) {
    return NULL;
  }
  char *encoded, *result;
  
  encoded = curl_escape(location, strlen(location));
  result = stringf("%s/%s", server, encoded);
  curl_free(encoded);
  return result;
}

char *url_fetch_escaped_location(const char* server, const char* location)
{
  char *url, *result;
  
  url = url_escape_location(server, location);
  if (!url) {
    return NULL;
  }

  result = url_fetch(url);
  free(url);
  return result;
}

