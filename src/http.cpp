#include "http.h"
#include <iostream>
#include "picohttpparser.h"

static const char *status_code_name(int code)
{
	switch (code) {
		case 100: return "Continue";
		case 101: return "Switching Protocols";
		case 200: return "OK";
		case 201: return "Created";
		case 202: return "Accepted";
		case 204: return "No Content";
		case 205: return "Reset Content";
		case 206: return "Partial Content";
		case 300: return "Multiple Choices";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 303: return "See Other";
		case 304: return "Not Modified";
		case 307: return "Temporary Redirect";
		case 308: return "Permanent Redirect";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 409: return "Conflict";
		case 416: return "Range Not Satisfiable";
		case 429: return "Too Many Requests";
		case 451: return "Unavailable For Legal Reasons";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 502: return "Bad Gateway";
		case 503: return "Service Unavailable";
		case 505: return "HTTP Version Not Supported";
		default: return nullptr;
	}
}

std::string http_server::format_time(const time_t& tm)
{
	std::stringstream ss;
	ss << std::put_time(std::localtime(&tm), "%a, %d %b %Y %H:%M:%S %Z");
	return ss.str();
}

static std::string url_decode(const std::string& raw)
{
	std::string decoded;
	decoded.reserve(raw.size());
	for (auto c = raw.begin(); c != raw.end(); ) {
		std::string hb;
		if (*c == '%') {
			hb.assign(c + 1, c + 3);
			decoded.push_back(static_cast<char>(std::stoul(hb, nullptr, 16)));
			c += 3;
		} else {
			decoded.push_back(*c);
			c++;
		}
	}
	return decoded;
}

void http_server::session::reset()
{
	request.headers.clear();
	request.params.clear();
	response.headers.clear();
	parse_buf_len = 0;
	response.status_code = 0;
	response.header_written = false;

	start();
}

void http_server::session::start()
{
	std::vector<char> sbuf(sbuf_size);
	int sbuf_off = 0;
	while (true) {
		ssize_t r = socket_.read(sbuf.data() + sbuf_off, sbuf_size - sbuf_off);
		if (r > 0) {
			const char *method, *path;
			struct phr_header headers[128];
			size_t mlen, plen, n_headers = 128;
			int minor_ver, pret;
			sbuf_off += r;
			pret = phr_parse_request(sbuf.data(), sbuf_off, &method, &mlen, &path, &plen, &minor_ver, headers, &n_headers, 0);
			if (pret > 0) {
				request.method = std::string(method, mlen);
				request.minor_version = minor_ver;
				for (int i = 0; i < n_headers; i++) {
					std::string key(headers[i].name, headers[i].name_len);
					std::transform(key.begin(), key.end(), key.begin(), ::tolower);
					request.headers[key] = std::string(headers[i].value, headers[i].value_len);
				}

				std::string full_path(path, plen);
				size_t qp_start = full_path.find('?');
				if (qp_start == std::string::npos) {
					request.url = std::move(full_path);
				} else {
					size_t qp_end = full_path.find_last_of('#');
					std::string qparams = qp_end == std::string::npos ? full_path.substr(qp_start + 1) : full_path.substr(qp_start + 1, qp_end - qp_start);
					request.url = full_path.substr(0, qp_start);

					auto entries = tokenize(qparams, "&");
					for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
						size_t split_loc = it->find('=', 0);
						if (split_loc == std::string::npos)
							continue;

						std::string key = url_decode(it->substr(0, split_loc));
						request.params[key] = url_decode(it->substr(split_loc + 1));
					}
				}

				server->pick_route(this);
			} else if (pret == -1) {
				std::cerr << "httpreadfail parse" << std::endl;
				socket_.close();
				return;
			} else if (sbuf_off >= sbuf_size) {
				std::cerr << "httpreadfail oom" << std::endl;
				socket_.close();
				return;
			}
		} else {
			if (r == -1)
				std::cerr << "httpreadfail io : " << socket_.last_error_str() << std::endl;
			socket_.close();
			return;
		}
	}
}

bool http_server::session::set_status_code(int code)
{
	if (status_code_name(code) == nullptr)
		return false;

	response.status_code = code;
	return true;
}

void http_server::session::write_headers()
{
	if (response.status_code == 0)
		throw std::invalid_argument("cannot write a response without a valid status");
	if (response.header_written)
		return;

	std::stringstream ss;
	ss << "HTTP/1." << request.minor_version << " " << response.status_code << " " << status_code_name(response.status_code) << "\r\n"
		<< "Date: " << format_time(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) << "\r\n"
		<< "Server: surf-mt/0.0.1\r\n"
		<< "Connection: keep-alive\r\n";
	for (auto it = response.headers.begin(); it != response.headers.end(); ++it)
		ss << it->first << ": " << it->second << "\r\n";
	ss << "\r\n";

	socket_.write(ss.str());
	response.header_written = true;
}

void http_server::session::write(const char *data, size_t length)
{
	if (response.header_written == false)
		write_headers();

	socket_.write(data, length);
}
