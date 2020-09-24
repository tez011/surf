#include "config.h"
#include "ffmpeg.h"
#include "ini.h"
#include "mediadb.h"
#include "http.h"
#include <cstring>
#include <iostream>
#include <regex>
#include <sago/platform_folders.h>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <uv.h>
#ifdef _WIN32
#include <Windows.h>
#include <winerror.h>
#else
#include <pwd.h>
#include <signal.h>
#include <unistd.h>
#endif

static void ignore_broken_pipes(void)
{
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
}

typedef struct {
	std::string media_dir;
	int port, cache_size;
} inidata;

static int ini_parser(void* user, const char* section, const char* name, const char* value)
{
	#define MATCH(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)
	inidata *cfg = reinterpret_cast<inidata *>(user);
	if (MATCH("net", "port"))
		cfg->port = atoi(value);
	else if (MATCH("media", "path"))
		cfg->media_dir = value;
	else if (MATCH("media", "cache_size"))
		cfg->cache_size = atoi(value);
	else
		return 0;

	return 1;
}

int main(int argc, char **argv)
{
	std::string config_path = sago::getConfigHome() + "/" ORG_NAME "/" APP_NAME "/config.ini",
		cache_path = sago::getCacheDir() + "/" ORG_NAME "/" APP_NAME "/";

	int rc;
	inidata cfg;
	const char *env;

	if ((env = std::getenv("SURF_PORT")) == nullptr)
		cfg.port = 27440;
	else
		cfg.port = atoi(env);
	if ((env = std::getenv("SURF_MAX_CACHE")) == nullptr)
		cfg.cache_size = 64;
	else
		cfg.cache_size = atoi(env);
	if ((env = std::getenv("SURF_MEDIA")) != nullptr)
		cfg.media_dir = env;

	int ini_parsed = ini_parse(config_path.c_str(), ini_parser, &cfg);
	if (cfg.media_dir == "") {
		sago::PlatformFolders p;
		cfg.media_dir = p.getMusicFolder();
	}

	std::cout << "Configuration: (" << (ini_parsed < 0 ? "unable to load" : "loaded") << " from " << config_path << ")" << std::endl
		<< "\tport:\t\t" << cfg.port << std::endl
		<< "\tcache size:\t" << cfg.cache_size << std::endl
		<< "\tpath:\t\t" << cfg.media_dir << std::endl;

	av_log_set_level(AV_LOG_ERROR);
	sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
	ignore_broken_pipes();

	mediadb md(cfg.media_dir, cache_path, cfg.cache_size);
	md.scan_path(cfg.media_dir);

	surf_server server(md, cfg.port);
	std::cout << "Now accepting new connections." << std::endl;
	server.run();

	return 0;
}
