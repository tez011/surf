#include "config.h"
#include "mediadb.h"
#include <sstream>

/* I want RAII! */
db_connection::db_connection(const std::string& uri)
	: db(nullptr)
{
	if (sqlite3_open(uri.c_str(), &db) != SQLITE_OK)
		throw std::runtime_error("could not open database at " + uri + ": " + sqlite3_errmsg(db));

	sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
	sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);
}

db_connection::~db_connection()
{
	sqlite3_close(db);
}

mediadb::mediadb(const std::string& media_path, const std::string& cache_path, size_t cache_size)
	: media_path(media_path), cache_path(cache_path), cache(cache_size)
{
	fs::path db_path = this->media_path / (APP_NAME ".db");
	fs::create_directories(media_path);
	fs::create_directories(cache_path);

	// Check the database for validity.
	sqlite3* db = nullptr;
	sqlite3_stmt* stmt = nullptr;
	int rc, db_version = 0;
	if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK)
		throw std::runtime_error("could not open database at " + db_path.string() + ": " + sqlite3_errmsg(db));
	if ((rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS SURF_DB_META (VERSION INTEGER)", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create table SURF_DB_META: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_prepare_v2(db, "SELECT VERSION FROM SURF_DB_META", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare SQL to check database validity: " + std::string(sqlite3_errstr(rc)));

	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
		db_version = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

	if (db_version == 0)
		init_db(db);

	sqlite3_close(db);
}

void mediadb::init_db(sqlite3* db)
{
	int rc;
	if ((rc = sqlite3_exec(db, "DELETE FROM SURF_DB_META", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not truncate table SURF_DB_META: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db, "INSERT INTO SURF_DB_META (VERSION) VALUES (1)", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not fill table SURF_DB_META: " + std::string(sqlite3_errstr(rc)));

	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS ARTISTS ("
		"UUID TEXT PRIMARY KEY NOT NULL,"
		"NAME TEXT NOT NULL)", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create artists table: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS ALBUMS ("
		"UUID TEXT PRIMARY KEY NOT NULL,"
		"TITLE TEXT NOT NULL,"
		"ARTISTSTR TEXT,"
		"COVERART TEXT,"
		"YEAR MEDIUMINT,"
		"MONTH TINYINT,"
		"DAY TINYINT)", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create albums table: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS TRACKS ("
		"UUID TEXT PRIMARY KEY NOT NULL,"
		"FORMAT TEXT,"
		"BITRATE UNSIGNED INT NOT NULL,"
		"DURATION BIGINT NOT NULL,"
		"TITLE TEXT NOT NULL,"
		"TRACK MEDIUMINT,"
		"DISC MEDIUMINT,"
		"ARTISTSTR TEXT,"
		"ALBUM TEXT NOT NULL REFERENCES ALBUMS(UUID),"
		"LOCATION TEXT UNIQUE NOT NULL)", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create tracks table: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS TRACKARTISTS ("
		"TRACK TEXT NOT NULL REFERENCES TRACKS(UUID),"
		"ARTIST TEXT NOT NULL REFERENCES ARTISTS(UUID),"
		"RANK INTEGER,"
		"UNIQUE(TRACK, ARTIST))", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create track-artists table: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS ALBUMARTISTS ("
		"ALBUM TEXT NOT NULL REFERENCES ALBUMS(UUID),"
		"ARTIST TEXT NOT NULL REFERENCES ARTISTS(UUID),"
		"RANK INTEGER,"
		"UNIQUE(ALBUM, ARTIST))", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create album-artists table: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS PLAYLISTS ("
		"UUID TEXT PRIMARY KEY NOT NULL,"
		"NAME TEXT NOT NULL)", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create playlists table: " + std::string(sqlite3_errstr(rc)));
	if ((rc = sqlite3_exec(db,
	"CREATE TABLE IF NOT EXISTS PLAYLISTTRACKS ("
		"PLAYLIST TEXT NOT NULL REFERENCES PLAYLISTS(UUID) ON DELETE CASCADE,"
		"RANK UNSIGNED INT NOT NULL,"
		"TRACK TEXT NOT NULL REFERENCES TRACKS(UUID),"
		"UNIQUE(PLAYLIST, RANK))", nullptr, nullptr, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not create playlist-tracks table: " + std::string(sqlite3_errstr(rc)));
}

void mediadb::init_prepped_inserts(sqlite3* db, sqlite3_stmt** stmt)
{
	int rc;
	if ((rc = sqlite3_prepare_v3(db,
		"INSERT INTO ARTISTS (UUID, NAME) VALUES (?, ?) ON CONFLICT DO NOTHING",
		-1, SQLITE_PREPARE_PERSISTENT, stmt + INSERT_ARTISTS, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare INSERT_ARTISTS SQL");
	if ((rc = sqlite3_prepare_v3(db,
		"INSERT INTO ALBUMS (UUID, TITLE, ARTISTSTR, COVERART, YEAR, MONTH, DAY) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
		"ON CONFLICT(UUID) DO UPDATE SET TITLE = ?2, ARTISTSTR = ?3, COVERART = ?4, YEAR = ?5, MONTH = ?6, DAY = ?7 "
		"WHERE UUID = ?1",
		-1, SQLITE_PREPARE_PERSISTENT, stmt + INSERT_ALBUMS, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare INSERT_ALBUMS SQL");
	if ((rc = sqlite3_prepare_v3(db,
		"INSERT INTO TRACKS "
		"(UUID, FORMAT, BITRATE, DURATION, TITLE, TRACK, DISC, ARTISTSTR, ALBUM, LOCATION) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
		"ON CONFLICT(UUID) DO UPDATE SET "
			"FORMAT = ?2, BITRATE = ?3, DURATION = ?4, TITLE = ?5, TRACK = ?6,"
			"DISC = ?7, ARTISTSTR = ?8, ALBUM = ?9, LOCATION = ?10 "
		"WHERE UUID = ?1",
		-1, SQLITE_PREPARE_PERSISTENT, stmt + INSERT_TRACKS, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare INSERT_TRACKS SQL");
	if ((rc = sqlite3_prepare_v3(db,
		"INSERT INTO TRACKARTISTS (TRACK, ARTIST, RANK) VALUES (?, ?, ?) ON CONFLICT DO NOTHING",
		-1, SQLITE_PREPARE_PERSISTENT, stmt + INSERT_TRACKARTISTS, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare INSERT_TRACKARTISTS SQL");
	if ((rc = sqlite3_prepare_v3(db,
		"INSERT INTO ALBUMARTISTS (ALBUM, ARTIST, RANK) VALUES (?, ?, ?) ON CONFLICT DO NOTHING",
		-1, SQLITE_PREPARE_PERSISTENT, stmt + INSERT_ALBUMARTISTS, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare INSERT_ALBUMARTISTS SQL");
}

void mediadb::scan_path(const fs::path& _path)
{
	db_connection dbc = dbconn();
	fs::path root = fs::canonical(_path);
	sqlite3_stmt* prep_stmt[PREP_STMT_MAX];

	mod_times[root] = std::chrono::system_clock::now();
	init_prepped_inserts(dbc.handle(), prep_stmt);
	sqlite3_exec(dbc.handle(), "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
	if (fs::is_directory(root)) {
		fs::directory_options walk_opts = fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied;
		for (auto& p : fs::recursive_directory_iterator(root, walk_opts)) {
			if (p.is_regular_file()) {
				std::string fname = p.path().filename().string();
				if (fname.length() > 0 && fname[0] != '.' && fname != APP_NAME ".db")
					scan_file(dbc, p.path(), prep_stmt);
			}
		}
	} else {
		scan_file(dbc, root, prep_stmt);
	}

	sqlite3_exec(dbc.handle(), "END TRANSACTION", nullptr, nullptr, nullptr);
	for (int i = 0; i < PREP_STMT_MAX; i++)
		sqlite3_finalize(prep_stmt[i]);
}

std::optional<std::string> mediadb::get_track_path(const std::string& track_uuid)
{
	db_connection dbc = dbconn();
	sqlite3_stmt *stmt = nullptr;
	std::optional<std::string> track_path;
	int rc;

	if ((rc = sqlite3_prepare_v2(dbc.handle(), "SELECT LOCATION FROM TRACKS WHERE UUID = ? LIMIT 1", -1, &stmt, nullptr)) != SQLITE_OK)
		throw std::runtime_error("could not prepare track retrieval SQL");
	sqlite3_bind_text(stmt, 1, track_uuid.c_str(), -1, SQLITE_STATIC);
	do { rc = sqlite3_step(stmt); } while (rc == SQLITE_BUSY);
	if (rc == SQLITE_MISUSE)
		throw std::runtime_error("sqlite misuse at " __FILE__ "@" + std::to_string(__LINE__) + ".");
	else if (rc == SQLITE_DONE)
		track_path = std::nullopt;
	else if (rc == SQLITE_ROW)
		track_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
	else
		throw std::runtime_error("could not step through track retrieval SQL: " + std::string(sqlite3_errmsg(dbc.handle())));
	sqlite3_finalize(stmt);
	return track_path;
}

std::pair<std::string, bool> mediadb::get_cached_transcode(const std::string& track_uuid, int quality)
{
	std::stringstream ss;
	ss << track_uuid << '.' << quality << ".mp3";
	fs::path cache_loc = fs::absolute(cache_path / ss.str());
	bool is_ok = fs::exists(cache_loc) && fs::file_size(cache_loc) > 0;

	cache.put(cache_loc);
	return { cache_loc.string(), is_ok };
}

std::chrono::system_clock::time_point mediadb::latest_mod_time() const
{
	auto it = mod_times.begin();
	std::chrono::system_clock::time_point m = it->second;
	++it;

	for (; it != mod_times.end(); ++it)
		m = std::max(m, it->second);

	return m;
}
