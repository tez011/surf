#include "http.h"
#include <iomanip>
#include <regex>
#include <sstream>

surf_server::surf_server(mediadb& mdb, unsigned short port) :
	http_server(port), mdb(mdb), stop(false)
{
	const int num_threads = std::thread::hardware_concurrency() * 8 / 5;
	threads.reserve(num_threads);
	for (int i = 0; i < num_threads; i++) {
		threads.emplace_back(&surf_server::accept, this);
	}
}

surf_server::~surf_server()
{
	stop = true;
	cond.notify_all();
	for (std::thread& th : threads)
		th.join();
}

void surf_server::accept()
{
	std::unique_lock<std::mutex> lck(mtx, std::defer_lock);
	while (true) {
		lck.lock();
		cond.wait(lck, [&]()->bool { return !sockets.empty() || stop; });
		if (stop)
			return;

		http_server::session sess(this, std::move(sockets.front()));
		sockets.pop();
		lck.unlock();
		sess.start();
	}
}

void surf_server::run()
{
	while (true) {
		sockpp::tcp_socket sock = acc.accept();
		if (sock) {
			std::unique_lock<std::mutex> lck(mtx);
			sockets.push(std::move(sock));
			cond.notify_one();
		} else {
			std::cerr << "accept fail : " << acc.last_error_str() << std::endl;
		}
	}
}

void surf_server::pick_route(http_server::session* sn)
{
	std::smatch sm;
	if (sn->request_path() == "/api/v1/albums") {
		if (check_mdb_modified_date(sn) == false)
			api_v1_albums(sn);
	} else if (sn->request_path() == "/api/v1/artists") {
		if (check_mdb_modified_date(sn) == false)
			api_v1_artists(sn);
	} else if (sn->request_path() == "/api/v1/tracks") {
		if (check_mdb_modified_date(sn) == false)
			api_v1_tracks(sn, sn->request_param("sort").value_or("album_artist,album_date,album_title,track_number,track_title"));
	} else if (std::regex_match(sn->request_path(), sm, std::regex("/api/v1/album/([^/]*)"))) {
		if (check_mdb_modified_date(sn) == false)
			api_v1_album(sn, sm[1]);
	} else if (std::regex_match(sn->request_path(), sm, std::regex("/api/v1/coverart/([^/]*)"))) {
		if (check_mdb_modified_date(sn) == false)
			api_v1_coverart(sn, sm[1]);
	} else if (sn->request_path() == "/api/v1/search" && sn->request_param("q")) {
		if (check_mdb_modified_date(sn) == false)
			api_v1_search(sn, sn->request_param("q").value());
	} else if (std::regex_match(sn->request_path(), sm, std::regex("/api/v1/stream/([^/]*)"))) {
		if (check_mdb_modified_date(sn) == false) {
			int quality = 6;
			auto qs = sn->request_param("q");
			if (qs.has_value()) {
				try {
					quality = std::stoul(qs.value());
				} catch (...) {
					quality = -1;
				}
			}
			api_v1_stream(sn, sm[1], quality);
		}
	} else {
		sn->set_status_code(404);
		sn->set_response_header("Content-type", "text/plain; charset=utf-8");
		sn->set_response_header("Content-Length", "11");
		sn->write("Not Found\r\n", 11);
	}
}

bool surf_server::check_mdb_modified_date(http_server::session* sn)
{
	auto ims = sn->request_header("if-modified-since");
	if (ims.has_value()) {
		time_t lmt = std::chrono::system_clock::to_time_t(mdb.latest_mod_time());
		std::tm ims_v;
		std::stringstream ss(ims.value());
		ss >> std::get_time(&ims_v, "%a, %d %b %Y %H:%M:%S %Z");
		if (difftime(mktime(&ims_v), lmt) >= 0) {
			sn->set_status_code(304);
			sn->set_response_header("Cache-Control", "public; must-revalidate");
			sn->set_response_header("Last-Modified", http_server::format_time(lmt));
			sn->write_headers();
			return true;
		}
	}
	return false;
}

void surf_server::write_json(http_server::session* sn, const json& doc)
{
	std::string s = doc.dump();
	time_t lmt = std::chrono::system_clock::to_time_t(mdb.latest_mod_time());

	sn->set_status_code(200);
	sn->set_response_header("Cache-Control", "public; max-age=86400");
	sn->set_response_header("Content-type", "application/json");
	sn->set_response_header("Content-length", std::to_string(s.length()));
	sn->set_response_header("Last-Modified", http_server::format_time(lmt));
	sn->write(s.c_str(), s.length());
}

void http_server::session::serve_error(int status_code, const std::string& msg)
{
	set_status_code(status_code);
	response.headers.clear();
	response.headers["Cache-Control"] = "no-store";
	response.headers["Content-type"] = "text/plain; charset=utf-8";
	response.headers["Content-length"] = std::to_string(msg.length());
	write(msg.c_str(), msg.length());
}
