#include "http.h"
#include <fstream>

void surf_server::api_v1_albums(http_server::session* sn)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	std::string last_uuid = "";
	int rc;
	json current;

	if ((rc = sqlite3_prepare_v2(dbc.handle(),
		"SELECT A.UUID, A.TITLE, A.ARTISTSTR, A.YEAR, A.MONTH, A.DAY, R.UUID AS ARTIST_UUID, R.NAME AS ARTIST_NAME, COUNT(T.TITLE) AS NUM_TRACKS, SUM(T.DURATION)/60000 AS TOTAL_DURATION "
		"FROM ALBUMS A "
		"INNER JOIN ALBUMARTISTS AS AR ON A.UUID = AR.ALBUM "
		"INNER JOIN ARTISTS AS R ON R.UUID = AR.ARTIST "
		"INNER JOIN TRACKS AS T ON T.ALBUM = A.UUID "
		"GROUP BY A.UUID, A.TITLE, A.YEAR, A.MONTH, A.DAY, R.UUID, R.NAME "
		"ORDER BY A.ARTISTSTR, A.YEAR, A.MONTH, A.DAY, A.TITLE, AR.RANK",
		-1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/albums SQL");

	json resp = json::array();
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through /api/v1/albums SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		const char *album_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		if (album_uuid == nullptr)
			throw std::runtime_error("invariant violated: found a null album UUID");

		if (album_uuid == last_uuid) {
			current["artists"].push_back({
				{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
				{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
			});
		} else {
			if (last_uuid.size() > 0)
				resp.push_back(current);
			current = {
				{"uuid", album_uuid},
				{"title", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
				{"artist_sort", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
				{"year", sqlite3_column_int(stmt, 3)},
				{"month", sqlite3_column_int(stmt, 4)},
				{"day", sqlite3_column_int(stmt, 5)},
				{"num_tracks", sqlite3_column_int(stmt, 8)},
				{"total_duration", sqlite3_column_int(stmt, 9)},
				{"artists", {{
					{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
					{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
				}}},
			};
			last_uuid = album_uuid;
		}
	}
	resp.push_back(current);
	sqlite3_finalize(stmt);
	write_json(sn, resp);
}

void surf_server::api_v1_artists(http_server::session* sn)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	std::string last_uuid = "";
	int rc, total_tracks;
	json current;

	if ((rc = sqlite3_prepare_v2(dbc.handle(),
		"SELECT R.UUID, R.NAME, T.ALBUM, AR.ALBUM IS NOT NULL AS OWNED, COUNT(T.UUID) AS TRACKS "
		"FROM ARTISTS R "
		"LEFT JOIN ALBUMARTISTS AR ON AR.ARTIST = R.UUID "
		"INNER JOIN TRACKARTISTS TR ON TR.ARTIST = R.UUID "
		"INNER JOIN TRACKS T ON TR.TRACK = T.UUID "
		"WHERE AR.ALBUM IS NULL OR AR.ALBUM = T.ALBUM "
		"GROUP BY R.UUID, R.NAME, T.ALBUM, AR.ALBUM "
		"ORDER BY R.NAME",
		-1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/artists SQL");

	json resp = json::array();
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through /api/v1/artists SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		const char *artist_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		if (artist_uuid == nullptr)
			throw std::runtime_error("invariant violated: found a null artist UUID");

		if (artist_uuid != last_uuid) {
			if (last_uuid.size() > 0) {
				current["total_tracks"] = total_tracks;
				resp.push_back(current);
			}

			current = {
				{"uuid", artist_uuid},
				{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
				{"albums", json::array()},
				{"appearances", json::array()},
			};
			total_tracks = 0;
			last_uuid = artist_uuid;
		}

		const char *slot = sqlite3_column_int(stmt, 3) ? "albums" : "appearances";
		current[slot].push_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)));
		total_tracks += sqlite3_column_int(stmt, 4);
	}
	current["total_tracks"] = total_tracks;
	resp.push_back(current);
	sqlite3_finalize(stmt);
	write_json(sn, resp);
}

void surf_server::api_v1_tracks(http_server::session* sn, const std::string& _sort)
{
	// Transform/sanitize sort parameters.
	auto sort = tokenize(_sort, ",");
	for (auto it = sort.begin(); it != sort.end(); ++it) {
		if (*it == "album_artist")
			*it = "A.ARTISTSTR";
		else if (*it == "album_date")
			*it = "A.YEAR,A.MONTH,A.DAY";
		else if (*it == "album_title")
			*it = "A.TITLE";
		else if (*it == "track_number")
			*it = "T.DISC,T.TRACK";
		else if (*it == "track_title")
			*it = "T.TITLE";
		else if (*it == "track_artist")
			*it = "T.ARTISTSTR";
		else
			sn->serve_error(400, "Bad 'sort' parameter\r\n");
	}

	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	std::string last_uuid = "";
	int rc;
	json current;
	std::ostringstream oss;

	oss << "SELECT T.UUID, T.DURATION, T.TITLE, T.DISC, T.TRACK, A.UUID AS ALBUM_UUID, A.TITLE AS ALBUM, R.UUID AS ARTIST_UUID, R.NAME AS ARTIST "
		"FROM TRACKS T "
		"INNER JOIN ALBUMS A ON A.UUID = T.ALBUM "
		"INNER JOIN TRACKARTISTS TR ON TR.TRACK = T.UUID "
		"INNER JOIN ARTISTS R ON TR.ARTIST = R.UUID "
		"ORDER BY ";
	for (auto it = sort.begin(); it != sort.end(); ++it) {
		oss << *it << ',';
	}
	oss << "TR.RANK";
	if ((rc = sqlite3_prepare_v2(dbc.handle(), oss.str().c_str(), -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/tracks SQL");

	json resp = json::array();
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through /api/v1/tracks SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		const char *track_uuid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
		if (track_uuid == nullptr)
			throw std::runtime_error("invariant violated: found a null track UUID");

		if (track_uuid == last_uuid) {
			current["artists"].push_back({
				{"uuid", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7))},
				{"name", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8))},
			});
		} else {
			if (last_uuid.size() > 0)
				resp.push_back(current);

			current = {
				{"uuid", track_uuid},
				{"duration", sqlite3_column_int(stmt, 1)},
				{"title", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))},
				{"disc", sqlite3_column_int(stmt, 3)},
				{"track", sqlite3_column_int(stmt, 4)},
				{"album", {
					{"uuid", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5))},
					{"title", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6))},
				}},
				{"artists", {{
					{"uuid", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7))},
					{"name", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8))},
				}}},
			};
			last_uuid = track_uuid;
		}
	}
	resp.push_back(current);
	sqlite3_finalize(stmt);
	write_json(sn, resp);
}

void surf_server::api_v1_album(http_server::session* sn, const std::string& album_uuid)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	int rc;
	json resp;

	if ((rc = sqlite3_prepare_v2(dbc.handle(),
		"SELECT A.UUID, A.TITLE, A.ARTISTSTR, A.YEAR, A.MONTH, A.DAY, R.UUID AS ARTIST_UUID, R.NAME AS ARTIST_NAME, SUM(T.DURATION)/60000 AS TOTAL_DURATION "
		"FROM ALBUMS A "
		"INNER JOIN ALBUMARTISTS AS AR ON A.UUID = AR.ALBUM "
		"INNER JOIN ARTISTS AS R ON R.UUID = AR.ARTIST "
		"INNER JOIN TRACKS AS T ON T.ALBUM = A.UUID "
		"WHERE A.UUID = ? "
		"GROUP BY A.UUID, A.TITLE, A.YEAR, A.MONTH, A.DAY, R.UUID, R.NAME "
		"ORDER BY AR.RANK",
		-1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/album SQL");
	sqlite3_bind_text(stmt, 1, album_uuid.c_str(), -1, SQLITE_STATIC);

	bool first_row = true;
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through /api/v1/albums SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		const char *album_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		if (album_uuid == nullptr)
			throw std::runtime_error("invariant violated: found a null album UUID");

		if (first_row) {
			resp = {
				{"uuid", album_uuid},
				{"title", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
				{"artist_sort", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
				{"year", sqlite3_column_int(stmt, 3)},
				{"month", sqlite3_column_int(stmt, 4)},
				{"day", sqlite3_column_int(stmt, 5)},
				{"total_duration", sqlite3_column_int(stmt, 8)},
				{"artists", {{
					{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
					{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
				}}},
			};
		} else {
			resp["artists"].push_back({
				{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
				{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
			});
		}
	}
	sqlite3_finalize(stmt);
	if (first_row) {
		// no album was found
		sn->set_status_code(404);
		sn->set_response_header("Cache-Control", "no-store");
		sn->set_response_header("Content-type", "text/plain; charset=utf-8");
		sn->set_response_header("Content-Length", "11");
		return sn->write("Not Found\r\n", 11);
	}

	std::string last_uuid = "";
	json current;

	if ((rc = sqlite3_prepare_v2(dbc.handle(),
		"SELECT T.UUID, T.DURATION, T.TITLE, T.DISC, T.TRACK, R.UUID AS ARTIST_UUID, R.NAME AS ARTIST "
		"FROM TRACKS T "
		"INNER JOIN ALBUMS A ON A.UUID = T.ALBUM "
		"INNER JOIN TRACKARTISTS TR ON TR.TRACK = T.UUID "
		"INNER JOIN ARTISTS R ON TR.ARTIST = R.UUID "
		"WHERE A.UUID = ? "
		"ORDER BY T.DISC, T.TRACK, T.TITLE, TR.RANK",
		-1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/album SQL");
	sqlite3_bind_text(stmt, 1, album_uuid.c_str(), -1, SQLITE_STATIC);

	resp["tracks"] = json::array();
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through /api/v1/tracks SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		const char *track_uuid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
		if (track_uuid == nullptr)
			throw std::runtime_error("invariant violated: found a null track UUID");

		if (track_uuid == last_uuid) {
			current["artists"].push_back({
				{"uuid", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5))},
				{"name", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6))},
			});
		} else {
			if (last_uuid.size() > 0)
				resp["tracks"].push_back(current);

			current = {
				{"uuid", track_uuid},
				{"duration", sqlite3_column_int(stmt, 1)},
				{"title", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))},
				{"disc", sqlite3_column_int(stmt, 3)},
				{"track", sqlite3_column_int(stmt, 4)},
				{"artists", {{
					{"uuid", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5))},
					{"name", reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6))},
				}}},
			};
			last_uuid = track_uuid;
		}
	}
	resp["tracks"].push_back(current);
	sqlite3_finalize(stmt);
	write_json(sn, resp);
}

void surf_server::api_v1_coverart(http_server::session* sn, const std::string& album_uuid)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	std::optional<std::string> coverart_path;
	int rc;

	if ((rc = sqlite3_prepare_v2(dbc.handle(), "SELECT COVERART FROM ALBUMS WHERE UUID = ? LIMIT 1", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/coverart SQL");
	sqlite3_bind_text(stmt, 1, album_uuid.c_str(), -1, SQLITE_STATIC);
	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	else if (rc == SQLITE_DONE)
		coverart_path = std::nullopt;
	else if (rc == SQLITE_ROW)
		coverart_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
	else
		throw std::runtime_error("could not step through /api/v1/coverart SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
	sqlite3_finalize(stmt);

	if (!coverart_path.has_value())
		return sn->serve_error(404, "Not Found\r\n");

	std::string ext = coverart_path->substr(coverart_path->find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	if (ext == "jpg")
		ext = "jpeg";
	else if (ext != "png" && ext != "jpeg")
		ext = "xyz";

	std::ifstream ps(coverart_path.value(), std::ios_base::in | std::ios_base::ate | std::ios_base::binary);
	if (ps) {
		sn->set_status_code(200);
		sn->set_response_header("Content-type", "image/" + ext);
		sn->set_response_header("Cache-Control", "public; max-age=31536000");
		sn->set_response_header("Last-Modified", http_server::format_time(std::chrono::system_clock::to_time_t(mdb.latest_mod_time())));
		sn->set_response_header("Content-length", std::to_string(ps.tellg()));
		ps.clear();
		ps.seekg(0);

		std::vector<char> pbuf(8192);
		while (!ps.eof()) {
			ps.read(pbuf.data(), pbuf.size());
			sn->write(pbuf.data(), ps.gcount());
		}
	} else {
		sn->serve_error(500, "Failed to open coverart " + album_uuid + ".");
	}
}
