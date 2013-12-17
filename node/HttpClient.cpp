/*
 * ZeroTier One - Global Peer to Peer Ethernet
 * Copyright (C) 2012-2013  ZeroTier Networks LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include <utility>
#include <algorithm>

#include "Constants.hpp"
#include "HttpClient.hpp"
#include "Thread.hpp"
#include "Utils.hpp"
#include "NonCopyable.hpp"
#include "Defaults.hpp"

#ifdef __UNIX_LIKE__
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif

namespace ZeroTier {

const std::map<std::string,std::string> HttpClient::NO_HEADERS;

#ifdef __UNIX_LIKE__

// The *nix implementation calls 'curl' externally rather than linking to it.
// This makes it an optional dependency that can be avoided in tiny systems
// provided you don't want to have automatic software updates... or want to
// do them via another method.

#ifdef __APPLE__
// TODO: get proxy configuration
#endif

// Paths where "curl" may be found on the system
#define NUM_CURL_PATHS 5
static const char *CURL_PATHS[NUM_CURL_PATHS] = { "/usr/bin/curl","/bin/curl","/usr/local/bin/curl","/usr/sbin/curl","/sbin/curl" };

// Maximum message length
#define CURL_MAX_MESSAGE_LENGTH (1024 * 1024 * 64)

// Internal private thread class that performs request, notifies handler,
// and then commits suicide by deleting itself.
class P_Req : NonCopyable
{
public:
	P_Req(const char *method,const std::string &url,const std::map<std::string,std::string> &headers,unsigned int timeout,void (*handler)(void *,int,const std::string &,bool,const std::string &),void *arg) :
		_url(url),
		_headers(headers),
		_timeout(timeout),
		_handler(handler),
		_arg(arg)
	{
		_myThread = Thread::start(this);
	}

	void threadMain()
	{
		char *curlArgs[1024];
		char buf[16384];
		fd_set readfds,writefds,errfds;
		struct timeval tv;

		std::string curlPath;
		for(int i=0;i<NUM_CURL_PATHS;++i) {
			if (Utils::fileExists(CURL_PATHS[i])) {
				curlPath = CURL_PATHS[i];
				break;
			}
		}
		if (!curlPath.length()) {
			_handler(_arg,-1,_url,false,"unable to locate 'curl' binary in /usr/bin, /bin, /usr/local/bin, /usr/sbin, or /sbin");
			delete this;
			return;
		}

		if (!_url.length()) {
			_handler(_arg,-1,_url,false,"cannot fetch empty URL");
			delete this;
			return;
		}

		curlArgs[0] = const_cast <char *>(curlPath.c_str());
		curlArgs[1] = const_cast <char *>("-D");
		curlArgs[2] = const_cast <char *>("-"); // append headers before output
		int argPtr = 3;
		std::vector<std::string> headerArgs;
		for(std::map<std::string,std::string>::const_iterator h(_headers.begin());h!=_headers.end();++h) {
			headerArgs.push_back(h->first);
			headerArgs.back().append(": ");
			headerArgs.back().append(h->second);
		}
		for(std::vector<std::string>::iterator h(headerArgs.begin());h!=headerArgs.end();++h) {
			if (argPtr >= (1024 - 4)) // leave room for terminating NULL and URL
				break;
			curlArgs[argPtr++] = const_cast <char *>("-H");
			curlArgs[argPtr++] = const_cast <char *>(h->c_str());
		}
		curlArgs[argPtr++] = const_cast <char *>(_url.c_str());
		curlArgs[argPtr] = (char *)0;

		int curlStdout[2];
		int curlStderr[2];
		::pipe(curlStdout);
		::pipe(curlStderr);

		long pid = (long)vfork();
		if (pid < 0) {
			// fork() failed
			::close(curlStdout[0]);
			::close(curlStdout[1]);
			::close(curlStderr[0]);
			::close(curlStderr[1]);
			_handler(_arg,-1,_url,false,"unable to fork()");
			delete this;
			return;
		} else if (pid > 0) {
			// fork() succeeded, in parent process
			::close(curlStdout[1]);
			::close(curlStderr[1]);
			fcntl(curlStdout[0],F_SETFL,O_NONBLOCK);
			fcntl(curlStderr[0],F_SETFL,O_NONBLOCK);

			int exitCode = -1;
			unsigned long long timesOutAt = Utils::now() + ((unsigned long long)_timeout * 1000ULL);
			bool timedOut = false;
			bool tooLong = false;
			for(;;) {
				FD_ZERO(&readfds);
				FD_ZERO(&writefds);
				FD_ZERO(&errfds);
				FD_SET(curlStdout[0],&readfds);
				FD_SET(curlStderr[0],&readfds);
				FD_SET(curlStdout[0],&errfds);
				FD_SET(curlStderr[0],&errfds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				select(std::max(curlStdout[0],curlStderr[0])+1,&readfds,&writefds,&errfds,&tv);

				if (FD_ISSET(curlStdout[0],&readfds)) {
					int n = (int)::read(curlStdout[0],buf,sizeof(buf));
					if (n > 0) {
						_body.append(buf,n);
						// Reset timeout when data is read...
						timesOutAt = Utils::now() + ((unsigned long long)_timeout * 1000ULL);
					} else if (n < 0)
						break;
					if (_body.length() > CURL_MAX_MESSAGE_LENGTH) {
						::kill(pid,SIGKILL);
						tooLong = true;
						break;
					}
				}
				if (FD_ISSET(curlStderr[0],&readfds))
					::read(curlStderr[0],buf,sizeof(buf));
				if (FD_ISSET(curlStdout[0],&errfds)||FD_ISSET(curlStderr[0],&errfds))
					break;

				if (Utils::now() >= timesOutAt) {
					::kill(pid,SIGKILL);
					timedOut = true;
					break;
				}

				if (waitpid(pid,&exitCode,WNOHANG) > 0) {
					for(;;) {
						// Drain output...
						int n = (int)::read(curlStdout[0],buf,sizeof(buf));
						if (n <= 0)
							break;
						else {
							_body.append(buf,n);
							if (_body.length() > CURL_MAX_MESSAGE_LENGTH) {
								tooLong = true;
								break;
							}
						}
					}
					pid = 0;
					break;
				}
			}

			if (pid > 0)
				waitpid(pid,&exitCode,0);

			::close(curlStdout[0]);
			::close(curlStderr[0]);

			if (timedOut)
				_handler(_arg,-1,_url,false,"connection timed out");
			else if (tooLong)
				_handler(_arg,-1,_url,false,"response too long");
			else if (exitCode)
				_handler(_arg,-1,_url,false,"connection failed (curl returned non-zero exit code)");
			else {
				unsigned long idx = 0;

				// Grab status line and headers, which will prefix output on
				// success and will end with an empty line.
				std::vector<std::string> headers;
				headers.push_back(std::string());
				while (idx < _body.length()) {
					char c = _body[idx++];
					if (c == '\n') {
						if (!headers.back().length()) {
							headers.pop_back();
							break;
						} else headers.push_back(std::string());
					} else if (c != '\r')
						headers.back().push_back(c);
				}
				if (headers.empty()||(!headers.front().length())) {
					_handler(_arg,-1,_url,false,"HTTP response empty");
					delete this;
					return;
				}

				// Parse first line -- HTTP status code and response
				size_t scPos = headers.front().find(' ');
				if (scPos == std::string::npos) {
					_handler(_arg,-1,_url,false,"invalid HTTP response (no status line)");
					delete this;
					return;
				}
				++scPos;
				unsigned int rcode = Utils::strToUInt(headers.front().substr(scPos,3).c_str());
				if ((!rcode)||(rcode > 999)) {
					_handler(_arg,-1,_url,false,"invalid HTTP response (invalid response code)");
					delete this;
					return;
				}

				// Serve up the resulting data to the handler
				if (rcode == 200)
					_handler(_arg,rcode,_url,false,_body.substr(idx));
				else if ((scPos + 4) < headers.front().length())
					_handler(_arg,rcode,_url,false,headers.front().substr(scPos+4));
				else _handler(_arg,rcode,_url,false,"(no status message from server)");
			}

			delete this;
			return;
		} else {
			// fork() succeeded, in child process
			::dup2(curlStdout[1],STDOUT_FILENO);
			::close(curlStdout[1]);
			::dup2(curlStderr[1],STDERR_FILENO);
			::close(curlStderr[1]);
			::execv(curlPath.c_str(),curlArgs);
			::exit(-1); // only reached if execv() fails
		}
	}

	const std::string _url;
	std::string _body;
	std::map<std::string,std::string> _headers;
	unsigned int _timeout;
	void (*_handler)(void *,int,const std::string &,bool,const std::string &);
	void *_arg;
	Thread _myThread;
};

HttpClient::Request HttpClient::_do(
	const char *method,
	const std::string &url,
	const std::map<std::string,std::string> &headers,
	unsigned int timeout,
	void (*handler)(void *,int,const std::string &,bool,const std::string &),
	void *arg)
{
	return (HttpClient::Request)(new P_Req(method,url,headers,timeout,handler,arg));
}

#endif

} // namespace ZeroTier