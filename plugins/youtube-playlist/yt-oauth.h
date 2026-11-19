#pragma once

#include <util/dstr.h>
#include <stdbool.h>

/* YouTube Data API access with the user's own Google OAuth client.
 *
 * Credentials (client id/secret, refresh token) are stored in the module
 * config directory (oauth.json, mode 0600).  HTTP goes through the system
 * curl; secrets are passed through 0600 temporary files, never on the
 * command line.  The browser login itself is done by youtube-playlist-ui,
 * which talks to this module through global procedures:
 *
 *   youtube_playlist_oauth_status(out string json)
 *       {"configured", "logged_in", "client_id", "channel"}
 *   youtube_playlist_oauth_set_client(in string client_id, in string client_secret)
 *   youtube_playlist_oauth_exchange(in string code, in string verifier,
 *                                   in string redirect_uri,
 *                                   out bool ok, out string error)
 *   youtube_playlist_oauth_logout()
 *   youtube_playlist_my_playlists(out bool ok, out string json, out string error)
 *       {"playlists": [{"id", "title", "count", "privacy"}]}
 */

void yt_oauth_init(void);
void yt_oauth_free(void);

bool yt_oauth_logged_in(void);

/* Expand a playlist through the API.  On success `lines` receives
 * "<video url>\t<duration seconds>\t<title>\n" per public/unlisted video
 * (same format as the yt-dlp output used by the fetcher). */
bool yt_api_expand_playlist(const char *playlist_id, struct dstr *lines);

/* Extract the playlist id from a URL with list=..., or NULL. (bfree) */
char *yt_playlist_id_from_url(const char *url);
