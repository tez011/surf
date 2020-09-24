#pragma once
#include <array>
#include "config.h"
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <vector>
#include "lru.h"
namespace fs = std::filesystem;

class audio_tag {
public:
	enum sval {
		TRACK_UUID = 0,
		ALBUM_UUID,
		FORMAT,
		BITRATE,
		DURATION,
		SAMPLERATE,
		TITLE,
		DISC,
		TRACK_NUM,
		ARTISTSTR,
		ALBUMARTISTSTR,
		ALBUM_TITLE,
		DATE_YEAR,
		DATE_MONTH,
		DATE_DAY,
		SVAL_MAX
	};

	enum lval {
		ARTIST_NAMES = 0,
		ARTIST_UUIDS,
		ALBUMARTIST_NAMES,
		ALBUMARTIST_UUIDS,
		LVAL_MAX
	};

	std::array<std::string, SVAL_MAX> stag;
	std::array<std::vector<std::string>, LVAL_MAX> ltag;

	int populate(const fs::path& item);
};

class db_connection {
private:
	sqlite3 *db;
public:
	db_connection(const std::string& uri);
	~db_connection();

	inline sqlite3 *handle() const { return db; };
};

class mediadb {
private:
	class tccache : public lru<fs::path> {
	public:
		tccache(size_t sz) : lru<fs::path>(sz) {};
		void evict(const fs::path& evicted_value) override
		{
			fs::remove(evicted_value);
		}
	};

	enum prep_stmt_type {
		INSERT_ARTISTS,
		INSERT_ALBUMS,
		INSERT_TRACKS,
		INSERT_TRACKARTISTS,
		INSERT_ALBUMARTISTS,
		PREP_STMT_MAX
	};

	fs::path media_path, cache_path;
	tccache cache;
	std::map<std::string, std::chrono::system_clock::time_point> mod_times;

	mediadb(const mediadb& o) = delete;
	void init_db(sqlite3*);
	void init_prepped_inserts(sqlite3*, sqlite3_stmt**);
	void scan_file(const db_connection& dbc, const fs::path& path, sqlite3_stmt** stmt);

public:
	mediadb(const std::string& media_path, const std::string& cache_path, size_t cache_size);
	inline db_connection dbconn() { return db_connection((media_path / APP_NAME ".db").string()); };

	void scan_path(const fs::path& path);
	std::pair<std::string, bool> get_cached_transcode(const std::string& track_uuid, int quality);
	std::chrono::system_clock::time_point latest_mod_time() const;
};

std::vector<std::string> tokenize(const std::string& input, const char *delimiters, bool should_trim = true);
