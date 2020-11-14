#pragma once
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <sockpp/tcp_acceptor.h>
#include <string>
#include <thread>
#include "mediadb.h"

using json = nlohmann::json;

class http_server {
public:
	class session {
	private:
		static constexpr int sbuf_size = 8192;
		std::array<char, sbuf_size> sbuf;
		http_server *server;
		sockpp::tcp_socket socket_;

		std::string body_data;
		size_t full_body_length;

		struct {
			int minor_version;
			std::string method, url;
			std::map<std::string, std::string> headers, params;
		} request;

		struct {
			int status_code;
			std::map<std::string, std::string> headers;
			bool header_written = false;
		} response;

		void reset();
	public:
		session(http_server *server, sockpp::tcp_socket&& sock) : server(server), socket_(std::move(sock))
		{
		}

		inline sockpp::tcp_socket& socket()
		{
			return socket_;
		}

		inline const std::string& request_method() const { return request.method; };
		inline const std::string& request_path() { return request.url; };
		inline const std::map<std::string, std::string>& request_headers() { return request.headers; };
		inline const std::map<std::string, std::string>& request_params() { return request.params; };
		inline const std::string& request_body() const { return body_data; };
		inline bool request_slurped() const { return body_data.size() == full_body_length; };

		std::optional<std::string> request_header(const std::string& key) {
			auto it = request.headers.find(key);
			if (it == request.headers.end())
				return std::nullopt;
			else
				return it->second;
		}

		std::optional<std::string> request_param(const std::string& key) {
			auto it = request.params.find(key);
			if (it == request.params.end())
				return std::nullopt;
			else
				return it->second;
		}

		void start();
		bool set_status_code(int code);
		inline void set_response_header(const std::string& header, const std::string& value) { response.headers[header] = value; };
		inline void clear_response_headers() { response.headers.clear(); };
		void write_headers();
		void write(const char *data, size_t length);
		void serve_error(int status_code, const std::string& msg);
	};

protected:
	sockpp::tcp_acceptor acc;
public:
	http_server(unsigned short port) : acc(port)
	{
		if (!acc)
			throw std::runtime_error("accept: " + acc.last_error_str());
	}

	static std::string format_time(const time_t& tm);
	virtual void pick_route(http_server::session* session) = 0;
};

class surf_server : public http_server {
private:
	mediadb& mdb;
	std::vector<std::thread> threads;
	std::mutex mtx;
	std::condition_variable cond;
	std::queue<sockpp::tcp_socket> sockets;
	bool stop;

	/* GET requests, return JSON, can be used in multiget */
	void api_v1_albums(http_server::session* sn);
	void api_v1_artists(http_server::session* sn);
	void api_v1_tracks(http_server::session* sn, const std::string& sort);
	void api_v1_album(http_server::session* sn, const std::string& album_uuid);
	void api_v1_coverart(http_server::session* sn, const std::string& album_uuid);
	void api_v1_plists(http_server::session* session);
	void api_v1_plist_GET(http_server::session* sn, const std::string& plist_uuid);

	/* POST */
	void api_v1_plist_insert(http_server::session* sn, const std::string& plist_uuid);
	void api_v1_plist_reorder(http_server::session* sn, const std::string& plist_uuid);
	void api_v1_plist_remove(http_server::session* sn, const std::string& plist_uuid);

	/* GET */
	void api_v1_search(http_server::session* sn, const std::string& q);
	void api_v1_stream(http_server::session* sn, const std::string& track_uuid);

	void api_v1_stream_cached(http_server::session* sn, const std::string& path_to_tc);
	void api_v1_transcode(http_server::session* sn, const std::string& track_uuid, const std::string& track_path, int quality);

	void api_v1_plist_DELETE(http_server::session* sn, const std::string& plist_uuid);
	void api_v1_plist_PUT(http_server::session* sn, const std::string& plist_uuid);

	bool check_mdb_modified_date(http_server::session* sess);
	void write_json(http_server::session* sn, const json& doc);

public:
	surf_server(mediadb& mdb, unsigned short port);
	~surf_server();

	void accept();
	void pick_route(http_server::session* session) override;
	void run();
};
