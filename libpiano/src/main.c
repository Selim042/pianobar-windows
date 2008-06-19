/*
Copyright (c) 2008 Lars-Dominik Braun

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "const.h"
#include "main.h"
#include "piano.h"
#include "http.h"
#include "xml.h"
#include "crypt.h"
#include "config.h"

/*	more "secure" free version; only use this function, not original free ()
 *	in this library
 *	@public no!!!
 *	@param free this pointer
 *	@param zero n bytes; 0 disables zeroing (for strings with unknown size,
 *			e.g.)
 */
void PianoFree (void *ptr, size_t size) {
	if (ptr != NULL) {
		if (size > 0) {
			/* avoid reuse of freed memory */
			memset ((char *) ptr, 0, size);
		}
		free (ptr);
	}
}

/*	initialize piano handle, set up curl handle and settings; note: you
 *	_must_ init curl and libxml2 using curl_global_init (CURL_GLOBAL_SSL)
 *	and xmlInitParser (), you also _must_ cleanup their garbage on your own!
 *	@param piano handle
 *	@return nothing
 */
void PianoInit (PianoHandle_t *ph) {
	memset (ph, 0, sizeof (*ph));
	ph->curlHandle = curl_easy_init ();
	/* FIXME: 64-bit may make this hack useless */
	snprintf (ph->routeId, sizeof (ph->routeId), "%07liP", time (NULL)>>8);
	curl_easy_setopt (ph->curlHandle, CURLOPT_USERAGENT, PACKAGE_STRING);
}

/*	free complete search result
 *	@public yes
 *	@param search result
 */
void PianoDestroySearchResult (PianoSearchResult_t *searchResult) {
	PianoArtist_t *curArtist, *lastArtist;
	PianoSong_t *curSong, *lastSong;

	curArtist = searchResult->artists;
	while (curArtist != NULL) {
		PianoFree (curArtist->name, 0);
		PianoFree (curArtist->musicId, 0);
		lastArtist = curArtist;
		curArtist = curArtist->next;
		PianoFree (lastArtist, sizeof (*lastArtist));
	}

	curSong = searchResult->songs;
	while (curSong != NULL) {
		PianoFree (curSong->title, 0);
		PianoFree (curSong->artist, 0);
		PianoFree (curSong->musicId, 0);
		lastSong = curSong;
		curSong = curSong->next;
		PianoFree (lastSong, sizeof (*lastSong));
	}
}

/*	free single station
 *	@public yes
 *	@param station
 */
void PianoDestroyStation (PianoStation_t *station) {
	PianoFree (station->name, 0);
	PianoFree (station->id, 0);
	memset (station, 0, sizeof (station));
}

/*	free complete station list
 *	@param piano handle
 */
void PianoDestroyStations (PianoHandle_t *ph) {
	PianoStation_t *curStation, *lastStation;

	curStation = ph->stations;
	while (curStation != NULL) {
		lastStation = curStation;
		curStation = curStation->next;
		PianoDestroyStation (lastStation);
		PianoFree (lastStation, sizeof (*lastStation));
	}
	ph->stations = NULL;
}

/* FIXME: copy & waste */
/*	free _all_ elements of playlist
 *	@param piano handle
 *	@return nothing
 */
void PianoDestroyPlaylist (PianoHandle_t *ph) {
	PianoSong_t *curSong, *lastSong;

	curSong = ph->playlist;
	while (curSong != NULL) {
		PianoFree (curSong->audioUrl, 0);
		PianoFree (curSong->artist, 0);
		PianoFree (curSong->focusTraitId, 0);
		PianoFree (curSong->matchingSeed, 0);
		PianoFree (curSong->musicId, 0);
		PianoFree (curSong->title, 0);
		PianoFree (curSong->userSeed, 0);
		lastSong = curSong;
		curSong = curSong->next;
		PianoFree (lastSong, sizeof (*lastSong));
	}
	ph->playlist = NULL;
}

/*	frees the whole piano handle structure; this will _not_ cleanup curl's
 *	internal garbage, you have to call curl_global_cleanup () and
 *	xmlCleanupParser () for libxml2
 *	@param piano handle
 *	@return nothing
 */
void PianoDestroy (PianoHandle_t *ph) {
	curl_easy_cleanup (ph->curlHandle);
	PianoFree (ph->user.webAuthToken, 0);
	PianoFree (ph->user.authToken, 0);
	PianoFree (ph->user.listenerId, 0);

	PianoDestroyStations (ph);
	PianoDestroyPlaylist (ph);
	memset (ph, 0, sizeof (*ph));
}

/*	authenticates user
 *	@param piano handle
 *	@param username (utf-8 encoded)
 *	@param password (plaintext, utf-8 encoded)
 */
PianoReturn_t PianoConnect (PianoHandle_t *ph, char *user, char *password) {
	char url[PIANO_URL_BUFFER_SIZE];
	char *requestStr = PianoEncryptString ("<?xml version=\"1.0\"?>"
			"<methodCall><methodName>misc.sync</methodName>"
			"<params></params></methodCall>");
	char *retStr, requestStrPlain[10000];
	PianoReturn_t ret;

	/* sync (is the return value used by pandora? for now: ignore result) */
	snprintf (url, sizeof (url), PIANO_RPC_URL "rid=%s&method=sync",
			ph->routeId);
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	/* authenticate */
	snprintf (requestStrPlain, sizeof (requestStrPlain), 
			"<?xml version=\"1.0\"?><methodCall>"
			"<methodName>listener.authenticateListener</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"</params></methodCall>", time (NULL), user, password);
	requestStr = PianoEncryptString (requestStrPlain);
	snprintf (url, sizeof (url), PIANO_SECURE_RPC_URL "rid=%s"
			"&method=authenticateListener", ph->routeId);
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseUserinfo (ph, retStr);

	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	return ret;
}

/*	get all stations for authenticated user (so: PianoConnect needs to
 *	be run before)
 *	@param piano handle filled with some authentication data by PianoConnect
 */
PianoReturn_t PianoGetStations (PianoHandle_t *ph) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr;
	PianoReturn_t ret;

	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>station.getStations</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken);
	requestStr = PianoEncryptString (xmlSendBuf);
	snprintf (url, sizeof (url), PIANO_RPC_URL
			"rid=%s&lid=%s&method=getStations", ph->routeId,
			ph->user.listenerId);
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseStations (ph, retStr);
	PianoFree (retStr, 0);
	PianoFree (requestStr, 0);

	return ret;
}

/*	get next songs for station (usually four tracks)
 *	@param piano handle
 *	@param station id
 */
PianoReturn_t PianoGetPlaylist (PianoHandle_t *ph, char *stationId) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr;
	PianoReturn_t ret;

	/* FIXME: remove static numbers */
	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>playlist.getFragment</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>15941546</string></value></param>"
			"<param><value><string>181840822</string></value></param>"
			"<param><value><string></string></value></param>"
			"<param><value><string></string></value></param>"
			"<param><value><string>aacplus</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			stationId);
	requestStr = PianoEncryptString (xmlSendBuf);
	snprintf (url, sizeof (url), PIANO_RPC_URL
			"rid=%s&lid=%s&method=getFragment&arg1=%s&arg2=15941546"
			"&arg3=181840822&arg4=&arg5=&arg6=aacplus", ph->routeId,
			ph->user.listenerId, stationId);
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParsePlaylist (ph, retStr);
	PianoFree (retStr, 0);
	PianoFree (requestStr, 0);
	return ret;
}

/*	love or ban track (you cannot remove your rating, so PIANO_RATE_NONE is
 *	not allowed)
 *	@public yes
 *	@param piano handle
 *	@param track will be added to this stations loved tracks list
 *	@param rate this track
 *	@param your rating
 */
PianoReturn_t PianoRateTrack (PianoHandle_t *ph, PianoStation_t *station,
		PianoSong_t *song, PianoSongRating_t rating) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr;
	PianoReturn_t ret = PIANO_RET_ERR;

	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>station.addFeedback</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><boolean>%i</boolean></value></param>"
			"<param><value><boolean>0</boolean></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			station->id, song->musicId, song->matchingSeed, song->userSeed,
			/* sometimes focusTraitId is not set, dunno why yet */
			(song->focusTraitId == NULL) ? "" : song->focusTraitId,
			(rating == PIANO_RATE_LOVE) ? 1 : 0);
	requestStr = PianoEncryptString (xmlSendBuf);
	snprintf (url, sizeof (url), PIANO_RPC_URL
			"rid=%s&lid=%s&method=addFeedback&arg1=%s&arg2=%s"
			"&arg3=%s&arg4=%s&arg5=%s&arg6=%s&arg7=false", ph->routeId,
			ph->user.listenerId, station->id, song->musicId,
			song->matchingSeed, song->userSeed,
			(song->focusTraitId == NULL) ? "" : song->focusTraitId,
			(rating == PIANO_RATE_LOVE) ? "true" : "false");
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseSimple (retStr);

	if (ret == PIANO_RET_OK) {
		song->rating = rating;
	}

	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	return ret;
}

/*	rename station (on the server and local)
 *	@public yes
 *	@param piano handle
 *	@param change this stations name
 *	@param new name
 *	@return
 */
PianoReturn_t PianoRenameStation (PianoHandle_t *ph, PianoStation_t *station,
		char *newName) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr, *urlencodedNewName, *xmlencodedNewName;
	PianoReturn_t ret = PIANO_RET_ERR;

	xmlencodedNewName = PianoXmlEncodeString (newName);
	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>station.setStationName</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			station->id, xmlencodedNewName);
	requestStr = PianoEncryptString (xmlSendBuf);

	urlencodedNewName = curl_easy_escape (ph->curlHandle, newName, 0);
	snprintf (url, sizeof (url), PIANO_RPC_URL "rid=%s&lid=%s"
			"&method=setStationName&arg1=%s&arg2=%s", ph->routeId,
			ph->user.listenerId, station->id, urlencodedNewName);
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseSimple (retStr);
	
	if (ret == PIANO_RET_OK) {
		PianoFree (station->name, 0);
		station->name = strdup (newName);
	}

	curl_free (urlencodedNewName);
	PianoFree (xmlencodedNewName, 0);
	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	return ret;
}

/*	delete station
 *	@public yes
 *	@param piano handle
 *	@param station you want to delete
 */
PianoReturn_t PianoDeleteStation (PianoHandle_t *ph, PianoStation_t *station) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr;
	PianoReturn_t ret = PIANO_RET_ERR;

	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>station.removeStation</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			station->id);
	requestStr = PianoEncryptString (xmlSendBuf);

	snprintf (url, sizeof (url), PIANO_RPC_URL "rid=%s&lid=%s"
			"&method=removeStation&arg1=%s", ph->routeId, ph->user.listenerId,
			station->id);
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseSimple (retStr);

	if (ret == PIANO_RET_OK) {
		/* delete station from local station list */
		PianoStation_t *curStation = ph->stations, *lastStation = NULL;
		while (curStation != NULL) {
			if (curStation == station) {
				printf ("deleting station\n");
				if (lastStation != NULL) {
					lastStation->next = curStation->next;
				} else {
					/* first station in list */
					ph->stations = curStation->next;
				}
				PianoDestroyStation (curStation);
				PianoFree (curStation, sizeof (*curStation));
			}
			lastStation = curStation;
			curStation = curStation->next;
		}
	}

	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	return ret;
}

/*	search for music (artist or track), needed to create new station; don't
 *	forget to free the search result; beware! searchResult will be nulled
 *	by PianoXmlParseSearch
 *	@public yes
 *	@param piano handle
 *	@param utf-8 search string
 *	@param return search result
 */
PianoReturn_t PianoSearchMusic (PianoHandle_t *ph, char *searchStr,
		PianoSearchResult_t *searchResult) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr, *xmlencodedSearchStr, *urlencodedSearchStr;
	PianoReturn_t ret;

	xmlencodedSearchStr = PianoXmlEncodeString (searchStr);
	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>music.search</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			xmlencodedSearchStr);
	requestStr = PianoEncryptString (xmlSendBuf);

	urlencodedSearchStr = curl_easy_escape (ph->curlHandle, searchStr, 0);
	snprintf (url, sizeof (url), PIANO_RPC_URL "rid=%s&lid=%s&"
			"method=search&arg1=%s", ph->routeId, ph->user.listenerId,
			urlencodedSearchStr);
	
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseSearch (retStr, searchResult);

	curl_free (urlencodedSearchStr);
	PianoFree (xmlencodedSearchStr, 0);
	PianoFree (retStr, 0);
	PianoFree (requestStr, 0);

	return ret;
}

/*	create new station on server
 *	@public yes
 *	@param piano handle
 *	@param music id from artist or track, you may obtain one by calling
 *			PianoSearchMusic
 */
PianoReturn_t PianoCreateStation (PianoHandle_t *ph, char *musicId) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr;
	PianoReturn_t ret;

	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>station.createStation</methodName>"
			"<params><param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>mi%s</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			musicId);
	requestStr = PianoEncryptString (xmlSendBuf);

	snprintf (url, sizeof (url), PIANO_RPC_URL "rid=%s&lid=%s"
			"&method=createStation&arg1=mi%s", ph->routeId,
			ph->user.listenerId, musicId);
	
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseCreateStation (ph, retStr);

	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	return ret;
}

/* FIXME: update station data instead of replacing them */
/*	add more music to existing station; multithreaded apps beware! this alters
 *	station data, don't forget to lock the station pointer you passed to this
 *	function
 *	@public yes
 *	@param piano handle
 *	@param add music to this station
 *	@param music id; can be obtained with PianoSearchMusic ()
 */
PianoReturn_t PianoStationAddMusic (PianoHandle_t *ph,
		PianoStation_t *station, char *musicId) {
	char xmlSendBuf[10000], url[PIANO_URL_BUFFER_SIZE];
	char *requestStr, *retStr;
	PianoReturn_t ret;

	snprintf (xmlSendBuf, sizeof (xmlSendBuf), "<?xml version=\"1.0\"?>"
			"<methodCall><methodName>station.addSeed</methodName><params>"
			"<param><value><int>%li</int></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"<param><value><string>%s</string></value></param>"
			"</params></methodCall>", time (NULL), ph->user.authToken,
			station->id, musicId);
	requestStr = PianoEncryptString (xmlSendBuf);

	snprintf (url, sizeof (url), PIANO_RPC_URL "rid=%s&lid=%s"
			"&method=addSeed&arg1=%s&arg2=%s", ph->routeId,
			ph->user.listenerId, station->id, musicId);
	
	PianoHttpPost (ph->curlHandle, url, requestStr, &retStr);
	ret = PianoXmlParseAddSeed (ph, retStr, station);

	PianoFree (requestStr, 0);
	PianoFree (retStr, 0);

	return ret;
}