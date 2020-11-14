#include "http.h"

void surf_server::api_v1_plists(http_server::session* sn)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	int rc;

	if ((rc = sqlite3_prepare_v2(dbc.handle(), "SELECT UUID, NAME FROM PLAYLISTS ORDER BY NAME", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/plists SQL");

	json resp = json::array();
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through /api/v1/plists SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		resp.push_back({
			{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))},
			{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))}
		});
	}
	sqlite3_finalize(stmt);
	write_json(sn, resp);
}

void surf_server::api_v1_plist_GET(http_server::session* sn, const std::string& plist_uuid)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	int rc;
	json resp;

	if ((rc = sqlite3_prepare_v2(dbc.handle(), "SELECT NAME FROM PLAYLISTS WHERE UUID = ? LIMIT 1", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare GET /api/v1/plist/ SQL to get name");
	sqlite3_bind_text(stmt, 1, plist_uuid.c_str(), -1, SQLITE_STATIC);

	std::string pl_name;
	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	else if (rc == SQLITE_DONE)
		sn->serve_error(404, "Not Found\r\n");
	else if (rc == SQLITE_ROW)
		resp["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
	else
		throw std::runtime_error("could not step through GET /api/v1/plist/ SQL to get name: " + std::string(sqlite3_errmsg(dbc.handle())));
	sqlite3_finalize(stmt);
	if (rc != SQLITE_ROW)
		return;

	json current;
	std::string last_uuid = "";
	resp["tracks"] = json::array();
	if ((rc = sqlite3_prepare_v2(dbc.handle(),
		"SELECT T.UUID, T.DURATION, T.TITLE, T.DISC, T.TRACK, R.UUID AS ARTIST_UUID, R.NAME AS ARTIST, A.UUID AS ALBUM_UUID, A.TITLE AS ALBUM "
		"FROM TRACKS T "
		"INNER JOIN ALBUMS A ON A.UUID = T.ALBUM "
		"INNER JOIN TRACKARTISTS TR ON TR.TRACK = T.UUID "
		"INNER JOIN ARTISTS R ON TR.ARTIST = R.UUID "
		"INNER JOIN PLAYLISTTRACKS PLT ON PLT.TRACK = T.UUID "
		"WHERE PLT.PLAYLIST = ? "
		"ORDER BY PLT.RANK",
		-1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare /api/v1/plist SQL");
	sqlite3_bind_text(stmt, 1, plist_uuid.c_str(), -1, SQLITE_STATIC);

	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_BUSY) {
			continue;
		} else if (rc == SQLITE_MISUSE) {
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
		} else if (rc != SQLITE_ROW) {
			throw std::runtime_error("could not step through GET /api/v1/plist SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
		}

		const char* track_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		if (track_uuid == nullptr)
			throw std::runtime_error("invariant violated: found a null track UUID");

		if (track_uuid == last_uuid) {
			current["artists"].push_back({
				{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))},
				{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
			});
		} else {
			if (last_uuid.size() > 0)
				resp["tracks"].push_back(current);

			current = {
				{"uuid", track_uuid},
				{"duration", sqlite3_column_int(stmt, 1)},
				{"title", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
				{"disc", sqlite3_column_int(stmt, 3)},
				{"track", sqlite3_column_int(stmt, 4)},
				{"album", {
					{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
					{"title", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8))},
				}},
				{"artists", {{
					{"uuid", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))},
					{"name", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
				}}},
			};
			last_uuid = track_uuid;
		}
	}
	resp["tracks"].push_back(current);
	sqlite3_finalize(stmt);
	write_json(sn, resp);
}

void surf_server::api_v1_plist_insert(http_server::session* sn, const std::string& plist_uuid)
{
	sn->serve_error(501, "Not implemented\r\n");
}

void surf_server::api_v1_plist_reorder(http_server::session* sn, const std::string& plist_uuid)
{
	sn->serve_error(501, "Not implemented\r\n");
}

void surf_server::api_v1_plist_remove(http_server::session* sn, const std::string& plist_uuid)
{
	sn->serve_error(501, "Not implemented\r\n");
}

void surf_server::api_v1_plist_PUT(http_server::session* sn, const std::string& plist_uuid)
{
	int content_length;
	try {
		content_length = std::stoul(sn->request_header("content-length").value());
	} catch (...) {
		content_length = 0;
	}
	if (content_length == 0)
		return sn->serve_error(400, "Bad Request: no body present\r\n");

	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	int rc;

	if ((rc = sqlite3_prepare_v2(dbc.handle(), "DELETE FROM PLAYLISTTRACKS WHERE PLAYLIST = ?", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare PUT.1 /api/v1/plist SQL");
	sqlite3_bind_text(stmt, 1, plist_uuid.c_str(), -1, SQLITE_STATIC);

	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	sqlite3_finalize(stmt);

	if (sn->request_param("name").has_value()) {
		std::string pl_name = sn->request_param("name").value();
		if ((rc = sqlite3_prepare_v2(dbc.handle(),
			"INSERT INTO PLAYLISTS (UUID, NAME) VALUES (?1, ?2) "
			"ON CONFLICT(UUID) DO UPDATE SET NAME = ?2 WHERE UUID = ?1",
			-1, &stmt, nullptr)) != SQLITE_OK)
			throw std::runtime_error("could not prepare PUT.2 /api/v1/plist SQL");
		sqlite3_bind_text(stmt, 1, plist_uuid.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, pl_name.c_str(), -1, SQLITE_STATIC);
		do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
		if (rc == SQLITE_MISUSE)
			throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	}

	std::ostringstream ss;
	auto items = tokenize(sn->request_body(), ",\n");
	ss << "INSERT INTO PLAYLISTTRACKS (PLAYLIST, RANK, TRACK) VALUES ";
	for (int i = 0; i < items.size(); i++) {
		if (i != 0)
			ss << ",";
		ss << "(?,?,?)";
	}
	if ((rc = sqlite3_prepare_v2(dbc.handle(), ss.str().c_str(), -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare PUT.3 /api/v1/plist SQL");
	for (int i = 0; i < items.size(); i++) {
		sqlite3_bind_text(stmt, 3 * i + 1, plist_uuid.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 3 * i + 2, i + 1);
		sqlite3_bind_text(stmt, 3 * i + 3, items[i].c_str(), -1, SQLITE_STATIC);
	}

	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	sqlite3_finalize(stmt);
	if (rc == SQLITE_DONE)
		write_json(sn, items);
	else
		sn->serve_error(400, "Bad Request\r\n");
}

void surf_server::api_v1_plist_DELETE(http_server::session* sn, const std::string& plist_uuid)
{
	db_connection dbc = mdb.dbconn();
	sqlite3_stmt *stmt = nullptr;
	int rc;
	if ((rc = sqlite3_prepare_v2(dbc.handle(), "DELETE FROM PLAYLISTTRACKS WHERE PLAYLIST = ?", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare DELETE /api/v1/plist SQL");
	sqlite3_bind_text(stmt, 1, plist_uuid.c_str(), -1, SQLITE_STATIC);

	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	sqlite3_finalize(stmt);

	if ((rc = sqlite3_prepare_v2(dbc.handle(), "DELETE FROM PLAYLISTS WHERE UUID = ?", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare DELETE /api/v1/plist SQL");
	sqlite3_bind_text(stmt, 1, plist_uuid.c_str(), -1, SQLITE_STATIC);

	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	sqlite3_finalize(stmt);

	sn->serve_error(200, "Playlist deleted.\r\n");
}
