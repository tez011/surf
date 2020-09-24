#include "config.h"
#include "ffmpeg.h"
#include "mediadb.h"
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <fstream>
#include <highwayhash/highwayhash.h>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
const highwayhash::HHKey hashkey HH_ALIGNAS(32) = {0, 22, 69, 49};
static const char *COMMON_DELIMS = ",|;/";

static std::string trim(const std::string& s)
{
	std::string t = s;
	auto pred = [](char c) { return !std::isspace<char>(c, std::locale::classic()); };
	t.erase(std::find_if(t.rbegin(), t.rend(), pred).base(), t.end());
	t.erase(t.begin(), std::find_if(t.begin(), t.end(), pred));
	return t;
}

std::vector<std::string> tokenize(const std::string& input, const char *delimiters, bool should_trim)
{
	size_t pos = 0, num_delimiters = strlen(delimiters);
	std::vector<std::string> tokens;
	while (true) {
		size_t split_loc = std::string::npos;
		for (unsigned i = 0; i < num_delimiters; i++) {
			size_t p = input.find(delimiters[i], pos);
			if (split_loc == std::string::npos)
				split_loc = p;
			else if (p != std::string::npos && p < split_loc)
				split_loc = p;
		}
		if (split_loc == std::string::npos) {
			std::string token = input.substr(pos);
			if (should_trim)
				token = trim(token);
			tokens.push_back(token);
			return tokens;
		}
		if (split_loc != pos) {
			std::string token = input.substr(pos, split_loc - pos);
			if (should_trim)
				token = trim(token);
			tokens.push_back(token);
		}
		pos = split_loc + 1;
	}
}

static std::optional<fs::path> get_best_coverart(const fs::path& path)
{
	fs::path pdir = path.parent_path();
	fs::directory_options walk_opts = fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied;
	std::set<fs::path> images, covers;
	for (auto& p: fs::directory_iterator(pdir, walk_opts)) {
		if (p.is_regular_file()) {
			std::string fname = p.path().filename().generic_string(), ext = p.path().extension().generic_string();
			std::transform(fname.begin(), fname.end(), fname.begin(), ::tolower);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
				images.insert(p.path());
				if (fname == "cover" || fname == "folder")
					covers.insert(p.path());
			}
		}
	}
	if (covers.size() > 0)
		return covers.begin()->string();
	else if (images.size() > 0)
		return images.begin()->string();
	else
		return std::nullopt;
}

static bool av_dict_multiget(AVDictionary *dict, std::initializer_list<std::string> keys, std::string& out)
{
	for (auto it = keys.begin(); it != keys.end(); ++it) {
		AVDictionaryEntry* e = av_dict_get(dict, it->c_str(), nullptr, 0);
		if (e != nullptr) {
			out = e->value;
			return true;
		}
	}
	return false;
}

int audio_tag::populate(const fs::path& p)
{
	AVFormatContext* ctx = nullptr;
	AVDictionary* dict = nullptr;
	AVDictionaryEntry* ent = nullptr;
	AVCodec *codec;
	std::string raw_artist_names, tagval;
	int err = 0;

	if ((err = avformat_open_input(&ctx, p.c_str(), nullptr, nullptr)) < 0)
		return err;
	if ((err = avformat_find_stream_info(ctx, nullptr)) < 0)
		return err;

	int first_audio_stream = -1;
	for (size_t i = 0; i < ctx->nb_streams && first_audio_stream == -1; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			first_audio_stream = i;
	}
	if (first_audio_stream == -1) {
		err = AVERROR_STREAM_NOT_FOUND;
		goto cleanup;
	}
	if ((codec = avcodec_find_decoder(ctx->streams[first_audio_stream]->codecpar->codec_id)) == nullptr) {
		err = AVERROR_DECODER_NOT_FOUND;
		goto cleanup;
	}

	if (av_dict_count(ctx->metadata) == 0)
		// Tags are present in the stream metadata.
		dict = ctx->streams[first_audio_stream]->metadata;
	else
		dict = ctx->metadata;

	if (av_dict_multiget(dict, {"TITLE"}, stag[sval::TITLE]) == false ||
		av_dict_multiget(dict, {"ALBUM"}, stag[sval::ALBUM_TITLE]) == false ||
		av_dict_multiget(dict, {"ARTIST"}, stag[sval::ARTISTSTR]) == false ||
		av_dict_multiget(dict, {"ARTISTS", "ARTIST"}, raw_artist_names) == false ||
		av_dict_multiget(dict, {"album_artist", "ALBUMARTIST", "ARTIST"}, stag[sval::ALBUMARTISTSTR]) == false) {
		err = AVERROR_OPTION_NOT_FOUND;
		goto cleanup;
	}

	if (av_dict_multiget(dict, {"MUSICBRAINZ_TRACKID", "MusicBrainz Release Track Id"}, tagval)) {
		tagval.erase(std::remove(tagval.begin(), tagval.end(), '-'), tagval.end());
		std::transform(tagval.begin(), tagval.end(), tagval.begin(), ::tolower);
		stag[sval::TRACK_UUID] = tagval;
	} else {
		char buffer[16384], res_str[33];
		highwayhash::HHResult128 res;
		highwayhash::HHStateT<HH_TARGET> state(hashkey);

		std::ifstream fin(p, std::ifstream::binary);
		while (fin.read(buffer, 16384)) {
			std::streamsize sz = fin.gcount();
			const size_t remainder = sz & (sizeof(highwayhash::HHPacket) - 1),
				truncated = sz & ~(sizeof(highwayhash::HHPacket) - 1);
			for (size_t off = 0; off < truncated; off += sizeof(highwayhash::HHPacket)) {
				state.Update(*reinterpret_cast<const highwayhash::HHPacket *>(buffer + off));
			}
			if (remainder != 0)
				state.UpdateRemainder(buffer + truncated, remainder);
		}
		state.Finalize(&res);
		snprintf(res_str, 33, "%016" PRIx64 "%016" PRIx64, res[1], res[0]);
		stag[sval::TRACK_UUID] = res_str;
	}

	if (av_dict_multiget(dict, {"MUSICBRAINZ_RELEASEGROUPID",
					"MusicBrainz Release Group Id",
					"MUSICBRAINZ_ALBUMID",
					"MusicBrainz Album Id"}, tagval)) {
		tagval.erase(std::remove(tagval.begin(), tagval.end(), '-'), tagval.end());
		std::transform(tagval.begin(), tagval.end(), tagval.begin(), ::tolower);
		stag[sval::ALBUM_UUID] = tagval;
	} else {
		std::string buffer;
		char res_str[33];
		highwayhash::HHResult128 res;
		highwayhash::HHStateT<HH_TARGET> state(hashkey);

		av_dict_multiget(dict, {"ALBUM_ARTIST", "ALBUMARTIST"}, buffer);
		if ((ent = av_dict_get(dict, "ALBUM", nullptr, 0)) != nullptr)
			buffer += ent->value;
		highwayhash::HighwayHashT(&state, buffer.data(), buffer.length(), &res);
		snprintf(res_str, 33, "%016" PRIx64 "%016" PRIx64, res[1], res[0]);
		stag[sval::ALBUM_UUID] = res_str;
	}

	ltag[lval::ARTIST_NAMES] = tokenize(raw_artist_names, COMMON_DELIMS);
	if (av_dict_multiget(dict, {"MUSICBRAINZ_ARTISTID", "MusicBrainz Artist Id"}, tagval)) {
		tagval.erase(std::remove(tagval.begin(), tagval.end(), '-'), tagval.end());
		std::transform(tagval.begin(), tagval.end(), tagval.begin(), ::tolower);
		ltag[lval::ARTIST_UUIDS] = tokenize(tagval, COMMON_DELIMS);
	} else {
		const std::vector<std::string>& artist_names = ltag[lval::ARTIST_NAMES];
		std::vector<std::string> artist_uuids;
		artist_uuids.resize(artist_names.size());
		std::transform(artist_names.begin(), artist_names.end(), artist_uuids.begin(), [](const std::string& name)->std::string {
			char res_str[33];
			highwayhash::HHResult128 res;
			highwayhash::HHStateT<HH_TARGET> state(hashkey);
			highwayhash::HighwayHashT(&state, name.data(), name.length(), &res);
			snprintf(res_str, 33, "%016" PRIx64 "%016" PRIx64, res[1], res[0]);
			return res_str;
		});
		ltag[lval::ARTIST_UUIDS] = artist_uuids;
	}

	ltag[lval::ALBUMARTIST_NAMES] = tokenize(stag[sval::ALBUMARTISTSTR], COMMON_DELIMS);
	if (av_dict_multiget(dict, {"MUSICBRAINZ_ALBUMARTISTID", "MusicBrainz Album Artist Id"}, tagval)) {
		tagval.erase(std::remove(tagval.begin(), tagval.end(), '-'), tagval.end());
		std::transform(tagval.begin(), tagval.end(), tagval.begin(), ::tolower);
		ltag[lval::ALBUMARTIST_UUIDS] = tokenize(tagval, COMMON_DELIMS);
	} else {
		const std::vector<std::string>& album_artist_names = ltag[lval::ALBUMARTIST_NAMES];
		std::vector<std::string> album_artist_uuids;
		album_artist_uuids.resize(album_artist_names.size());
		std::transform(album_artist_names.begin(), album_artist_names.end(), album_artist_uuids.begin(), [](const std::string& name) {
			char res_str[33];
			highwayhash::HHResult128 res;
			highwayhash::HHStateT<HH_TARGET> state(hashkey);
			highwayhash::HighwayHashT(&state, name.data(), name.length(), &res);
			snprintf(res_str, 33, "%016" PRIx64 "%016" PRIx64, res[1], res[0]);
			return std::string(res_str);
		});
		ltag[lval::ALBUMARTIST_UUIDS] = album_artist_uuids;
	}

	if (av_dict_multiget(dict, {"date", "originaldate", "year", "originalyear", "TORY"}, tagval)) {
		auto sdate = tokenize(tagval, "-", false);
		std::transform(sdate.begin(), sdate.end(), sdate.begin(), [](const std::string& tok) {
			return std::all_of(tok.begin(), tok.end(), ::isdigit) ? tok : "0";
		});
		for (int i = 0; i < 3; i++)
			stag[sval::DATE_YEAR + i] = i < sdate.size() ? sdate[i] : "0";
	} else {
		for (int i = 0; i < 3; i++)
			stag[sval::DATE_YEAR + i] = "0";
	}

	if (av_dict_multiget(dict, {"disc"}, tagval)) {
		size_t splitter = tagval.find('/');
		stag[sval::DISC] = splitter == std::string::npos ? tagval : tagval.substr(0, splitter);
	} else
		stag[sval::DISC] = "0";
	if (av_dict_multiget(dict, {"track"}, tagval)) {
		size_t splitter = tagval.find('/');
		stag[sval::TRACK_NUM] = splitter == std::string::npos ? tagval : tagval.substr(0, splitter);
	} else
		stag[sval::TRACK_NUM] = "0";

	stag[sval::FORMAT] = codec->name;
	stag[sval::BITRATE] = std::to_string(ctx->bit_rate);
	stag[sval::DURATION] = std::to_string(1000 * ctx->duration / AV_TIME_BASE);

cleanup:
	avformat_close_input(&ctx);
	return err;
}

void mediadb::scan_file(const db_connection& dbc, const fs::path& path, sqlite3_stmt** stmt)
{
	int rc;
	audio_tag atag;
	if ((rc = atag.populate(path)) != 0) {
		std::cerr << "scan skip populate " << path << " : " << av_err2str(rc) << std::endl;
		return;
	}

	if (atag.ltag[audio_tag::lval::ARTIST_NAMES].size() != atag.ltag[audio_tag::lval::ARTIST_UUIDS].size()) {
		std::cerr << "scan skip artist_uuid_mismatch " << path << " : "
			<< atag.ltag[audio_tag::lval::ARTIST_UUIDS].size() << " UUIDs, "
			<< atag.ltag[audio_tag::lval::ARTIST_NAMES].size() << " names." << std::endl;
		return;
	}
	if (atag.ltag[audio_tag::lval::ALBUMARTIST_NAMES].size() != atag.ltag[audio_tag::lval::ALBUMARTIST_UUIDS].size()) {
		std::cerr << "scan skip album_artist_uuid_mismatch " << path << " : "
			<< atag.ltag[audio_tag::lval::ARTIST_UUIDS].size() << " UUIDs, "
			<< atag.ltag[audio_tag::lval::ARTIST_NAMES].size() << " names." << std::endl;
		return;
	}

	// Insert artists.
	for (int h = 0; h < 2; h++) {
		for (auto i = 0; i < atag.ltag[audio_tag::lval::ARTIST_NAMES + h].size(); i++) {
			sqlite3_bind_text(stmt[INSERT_ARTISTS], 1, atag.ltag[audio_tag::lval::ARTIST_UUIDS][i].c_str(), -1, SQLITE_STATIC);
			sqlite3_bind_text(stmt[INSERT_ARTISTS], 2, atag.ltag[audio_tag::lval::ARTIST_NAMES][i].c_str(), -1, SQLITE_STATIC);
			do { rc = sqlite3_step(stmt[INSERT_ARTISTS]); } while (rc == SQLITE_BUSY);
			if (rc != SQLITE_DONE) {
				std::cerr << "scan fail INSERT_ARTISTS " << atag.ltag[audio_tag::lval::ARTIST_UUIDS][i]
					<< " : \"" << atag.ltag[audio_tag::lval::ARTIST_NAMES][i] << "\" "
					<< sqlite3_errmsg(dbc.handle()) << std::endl;
				abort();
			}
			sqlite3_reset(stmt[INSERT_ARTISTS]);
		}
	}

	// Insert album.
	sqlite3_bind_text(stmt[INSERT_ALBUMS], 1, atag.stag[audio_tag::sval::ALBUM_UUID].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt[INSERT_ALBUMS], 2, atag.stag[audio_tag::sval::ALBUM_TITLE].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt[INSERT_ALBUMS], 3, atag.stag[audio_tag::sval::ALBUMARTISTSTR].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt[INSERT_ALBUMS], 5, atoi(atag.stag[audio_tag::sval::DATE_YEAR].c_str()));
	sqlite3_bind_int(stmt[INSERT_ALBUMS], 6, atoi(atag.stag[audio_tag::sval::DATE_MONTH].c_str()));
	sqlite3_bind_int(stmt[INSERT_ALBUMS], 7, atoi(atag.stag[audio_tag::sval::DATE_DAY].c_str()));

	auto coverart = get_best_coverart(path);
	if (coverart.has_value())
		sqlite3_bind_text(stmt[INSERT_ALBUMS], 4, coverart->c_str(), -1, SQLITE_STATIC);
	else
		sqlite3_bind_null(stmt[INSERT_ALBUMS], 4);

	do { rc = sqlite3_step(stmt[INSERT_ALBUMS]); } while (rc == SQLITE_BUSY);
	if (rc != SQLITE_DONE) {
		std::cerr << "scan fail INSERT_ALBUMS " << atag.stag[audio_tag::sval::ALBUM_UUID]
			<< " : \"" << atag.stag[audio_tag::sval::ALBUM_TITLE] << "\" "
			<< sqlite3_errmsg(dbc.handle()) << std::endl;
		abort();
	}
	sqlite3_reset(stmt[INSERT_ALBUMS]);

	// Insert track.
	sqlite3_bind_text(stmt[INSERT_TRACKS], 1, atag.stag[audio_tag::sval::TRACK_UUID].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt[INSERT_TRACKS], 2, atag.stag[audio_tag::sval::FORMAT].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt[INSERT_TRACKS], 3, atoi(atag.stag[audio_tag::sval::BITRATE].c_str()));
	sqlite3_bind_int64(stmt[INSERT_TRACKS], 4, atoll(atag.stag[audio_tag::sval::DURATION].c_str()));
	sqlite3_bind_text(stmt[INSERT_TRACKS], 5, atag.stag[audio_tag::sval::TITLE].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt[INSERT_TRACKS], 6, atoll(atag.stag[audio_tag::sval::TRACK_NUM].c_str()));
	sqlite3_bind_int(stmt[INSERT_TRACKS], 7, atoll(atag.stag[audio_tag::sval::DISC].c_str()));
	sqlite3_bind_text(stmt[INSERT_TRACKS], 8, atag.stag[audio_tag::sval::ARTISTSTR].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt[INSERT_TRACKS], 9, atag.stag[audio_tag::sval::ALBUM_UUID].c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt[INSERT_TRACKS], 10, path.c_str(), -1, SQLITE_STATIC);
	do { rc = sqlite3_step(stmt[INSERT_TRACKS]); } while (rc == SQLITE_BUSY);
	if (rc != SQLITE_DONE) {
		std::cerr << "scan fail INSERT_TRACKS " << atag.stag[audio_tag::sval::TRACK_UUID]
			<< " : \"" << atag.stag[audio_tag::sval::TITLE] << "\" "
			<< sqlite3_errmsg(dbc.handle()) << std::endl;
		abort();
	}
	sqlite3_reset(stmt[INSERT_TRACKS]);

	// Insert album artists and track artists.
	for (size_t i = 0; i < atag.ltag[audio_tag::lval::ALBUMARTIST_UUIDS].size(); i++) {
		sqlite3_bind_text(stmt[INSERT_ALBUMARTISTS], 1, atag.stag[audio_tag::sval::ALBUM_UUID].c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt[INSERT_ALBUMARTISTS], 2, atag.ltag[audio_tag::lval::ALBUMARTIST_UUIDS][i].c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt[INSERT_ALBUMARTISTS], 3, i + 1);
		do { rc = sqlite3_step(stmt[INSERT_ALBUMARTISTS]); } while (rc == SQLITE_BUSY);
		if (rc != SQLITE_DONE) {
			std::cerr << "scan fail INSERT_ALBUMARTISTS "
				<< atag.stag[audio_tag::sval::ALBUM_UUID] << " " << atag.ltag[audio_tag::lval::ALBUMARTIST_UUIDS][i]
				<< " : \"" << atag.stag[audio_tag::sval::TITLE] << "\" \"" << atag.ltag[audio_tag::lval::ALBUMARTIST_UUIDS][i] << "\" "
				<< sqlite3_errmsg(dbc.handle()) << std::endl;
			abort();
		}
		sqlite3_reset(stmt[INSERT_ALBUMARTISTS]);
	}

	for (size_t i = 0; i < atag.ltag[audio_tag::lval::ARTIST_UUIDS].size(); i++) {
		sqlite3_bind_text(stmt[INSERT_TRACKARTISTS], 1, atag.stag[audio_tag::sval::TRACK_UUID].c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt[INSERT_TRACKARTISTS], 2, atag.ltag[audio_tag::lval::ARTIST_UUIDS][i].c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt[INSERT_TRACKARTISTS], 3, i + 1);
		do { rc = sqlite3_step(stmt[INSERT_TRACKARTISTS]); } while (rc == SQLITE_BUSY);
		if (rc != SQLITE_DONE) {
			std::cerr << "scan fail INSERT_TRACKARTISTS "
				<< atag.stag[audio_tag::sval::TRACK_UUID] << " " << atag.ltag[audio_tag::lval::ARTIST_UUIDS][i]
				<< " : \"" << atag.stag[audio_tag::sval::TITLE] << "\" \"" << atag.ltag[audio_tag::lval::ARTIST_UUIDS][i] << "\" "
				<< sqlite3_errmsg(dbc.handle()) << std::endl;
			abort();
		}
		sqlite3_reset(stmt[INSERT_TRACKARTISTS]);
	}
}
